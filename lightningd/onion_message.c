#include "config.h"
#include <ccan/mem/mem.h>
#include <common/blindedpath.h>
#include <common/configdir.h>
#include <common/json_command.h>
#include <common/json_param.h>
#include <common/type_to_string.h>
#include <connectd/connectd_wiregen.h>
#include <lightningd/channel.h>
#include <lightningd/onion_message.h>
#include <lightningd/peer_control.h>
#include <lightningd/plugin_hook.h>
#include <lightningd/subd.h>
#include <sodium/randombytes.h>

struct onion_message_hook_payload {
	/* Optional */
	struct blinded_path *reply_path;
	struct pubkey *our_alias;
	struct tlv_onionmsg_tlv *om;
};

static void json_add_blindedpath(struct json_stream *stream,
				 const char *fieldname,
				 const struct blinded_path *path)
{
	json_object_start(stream, fieldname);
	json_add_pubkey(stream, "first_node_id", &path->first_node_id);
	json_add_pubkey(stream, "blinding", &path->blinding);
	json_array_start(stream, "hops");
	for (size_t i = 0; i < tal_count(path->path); i++) {
		json_object_start(stream, NULL);
		json_add_pubkey(stream, "blinded_node_id",
				&path->path[i]->blinded_node_id);
		json_add_hex_talarr(stream, "encrypted_recipient_data",
				    path->path[i]->encrypted_recipient_data);
		json_object_end(stream);
	};
	json_array_end(stream);
	json_object_end(stream);
}

static void onion_message_serialize(struct onion_message_hook_payload *payload,
				    struct json_stream *stream,
				    struct plugin *plugin)
{
	json_object_start(stream, "onion_message");
	if (payload->our_alias)
		json_add_pubkey(stream, "our_alias", payload->our_alias);

	if (payload->reply_path)
		json_add_blindedpath(stream, "reply_blindedpath",
				     payload->reply_path);

	if (payload->om->invoice_request)
		json_add_hex_talarr(stream, "invoice_request",
				    payload->om->invoice_request);
	if (payload->om->invoice)
		json_add_hex_talarr(stream, "invoice", payload->om->invoice);

	if (payload->om->invoice_error)
		json_add_hex_talarr(stream, "invoice_error",
				    payload->om->invoice_error);

	json_array_start(stream, "unknown_fields");
	for (size_t i = 0; i < tal_count(payload->om->fields); i++) {
		if (payload->om->fields[i].meta)
			continue;
		json_object_start(stream, NULL);
		json_add_u64(stream, "number", payload->om->fields[i].numtype);
		json_add_hex(stream, "value",
			     payload->om->fields[i].value,
			     payload->om->fields[i].length);
		json_object_end(stream);
	}
	json_array_end(stream);

	json_object_end(stream);
}

static void
onion_message_hook_cb(struct onion_message_hook_payload *payload STEALS)
{
	/* plugin_hook_continue checks the "result"; anything other than continue
	 * just stops. */
	tal_free(payload);
}

/* Two hooks, because it's critical we only accept blinding if we expect that
 * exact blinding key.  Otherwise, we can be probed using old blinded paths. */
REGISTER_PLUGIN_HOOK(onion_message_blinded,
		     plugin_hook_continue,
		     onion_message_hook_cb,
		     onion_message_serialize,
		     struct onion_message_hook_payload *);

REGISTER_PLUGIN_HOOK(onion_message_ourpath,
		     plugin_hook_continue,
		     onion_message_hook_cb,
		     onion_message_serialize,
		     struct onion_message_hook_payload *);

void handle_onionmsg_to_us(struct lightningd *ld, const u8 *msg)
{
	struct onion_message_hook_payload *payload;
	u8 *submsg;
	struct secret *self_id;
	size_t submsglen;
	const u8 *subptr;

	payload = tal(tmpctx, struct onion_message_hook_payload);
	payload->our_alias = tal(payload, struct pubkey);

	if (!fromwire_connectd_got_onionmsg_to_us(payload, msg,
						 payload->our_alias,
						 &self_id,
						 &payload->reply_path,
						 &submsg)) {
		log_broken(ld->log, "bad got_onionmsg_tous: %s",
			   tal_hex(tmpctx, msg));
		return;
	}

#if DEVELOPER
	if (ld->dev_ignore_modern_onion)
		return;
#endif

	/* If there's no self_id, or it's not correct, ignore alias: alias
	 * means we created the path it's using. */
	if (!self_id || !secret_eq_consttime(self_id, &ld->onion_reply_secret))
		payload->our_alias = tal_free(payload->our_alias);
	tal_free(self_id);

	submsglen = tal_bytelen(submsg);
	subptr = submsg;
	payload->om = fromwire_tlv_onionmsg_tlv(payload, &subptr, &submsglen);
	if (!payload->om) {
		log_broken(ld->log, "bad got_onionmsg_tous om: %s",
			   tal_hex(tmpctx, msg));
		return;
	}
	tal_free(submsg);

	/* Make sure connectd gets this right. */
	log_debug(ld->log, "Got onionmsg%s%s",
		  payload->our_alias ? " via-ourpath": "",
		  payload->reply_path ? " reply_path": "");

	/* We'll free this on return */
	tal_steal(ld, payload);
	if (payload->our_alias)
		plugin_hook_call_onion_message_ourpath(ld, NULL, payload);
	else
		plugin_hook_call_onion_message_blinded(ld, NULL, payload);
}

struct onion_hop {
	struct pubkey node;
	u8 *tlv;
};

static struct command_result *param_onion_hops(struct command *cmd,
					       const char *name,
					       const char *buffer,
					       const jsmntok_t *tok,
					       struct onion_hop **hops)
{
	size_t i;
	const jsmntok_t *t;

	if (tok->type != JSMN_ARRAY || tok->size == 0)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "%s must be an (non-empty) array", name);

	*hops = tal_arr(cmd, struct onion_hop, tok->size);
	json_for_each_arr(i, t, tok) {
		const char *err;

		err = json_scan(cmd, buffer, t, "{id:%,tlv:%}",
				JSON_SCAN(json_to_pubkey, &(*hops)[i].node),
				JSON_SCAN_TAL(tmpctx, json_tok_bin_from_hex,
					      &(*hops)[i].tlv));
		if (err)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "%s[%zu]: %s", name, i, err);
	}
	return NULL;
}

static struct command_result *json_sendonionmessage(struct command *cmd,
						    const char *buffer,
						    const jsmntok_t *obj UNNEEDED,
						    const jsmntok_t *params)
{
	struct onion_hop *hops;
	struct node_id *first_id;
	struct pubkey *blinding;
	struct sphinx_path *sphinx_path;
	struct onionpacket *op;
	struct secret *path_secrets;
	size_t onion_size;

	if (!param(cmd, buffer, params,
		   p_req("first_id", param_node_id, &first_id),
		   p_req("blinding", param_pubkey, &blinding),
		   p_req("hops", param_onion_hops, &hops),
		   NULL))
		return command_param_failed();

	if (!feature_offered(cmd->ld->our_features->bits[NODE_ANNOUNCE_FEATURE],
			     OPT_ONION_MESSAGES))
		return command_fail(cmd, LIGHTNINGD,
				    "experimental-onion-messages not enabled");

	/* Sanity check first; connectd doesn't bother telling us if peer
	 * can't be reached. */
	if (!peer_by_id(cmd->ld, first_id))
		return command_fail(cmd, LIGHTNINGD, "Unknown first peer");

	/* Create an onion which encodes this. */
	sphinx_path = sphinx_path_new(cmd, NULL);
	for (size_t i = 0; i < tal_count(hops); i++)
		sphinx_add_hop(sphinx_path, &hops[i].node, hops[i].tlv);

	/* BOLT-onion-message #4:
	 * - SHOULD set `len` to 1366 or 32834.
	 */
	if (sphinx_path_payloads_size(sphinx_path) <= ROUTING_INFO_SIZE)
		onion_size = ROUTING_INFO_SIZE;
	else
		onion_size = 32768;

	op = create_onionpacket(tmpctx, sphinx_path, onion_size, &path_secrets);
	if (!op)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Creating onion failed (tlvs too long?)");

	subd_send_msg(cmd->ld->connectd,
		      take(towire_connectd_send_onionmsg(NULL, first_id,
					serialize_onionpacket(tmpctx, op),
					blinding)));

	return command_success(cmd, json_stream_success(cmd));
}

static const struct json_command sendonionmessage_command = {
	"sendonionmessage",
	"utility",
	json_sendonionmessage,
	"Send message to {first_id}, using {blinding}, encoded over {hops} (id, tlv)"
};
AUTODATA(json_command, &sendonionmessage_command);

static struct command_result *param_pubkeys(struct command *cmd,
					    const char *name,
					    const char *buffer,
					    const jsmntok_t *tok,
					    struct pubkey **pubkeys)
{
	size_t i;
	const jsmntok_t *t;

	if (tok->type != JSMN_ARRAY || tok->size == 0)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "%s must be an (non-empty) array", name);

	*pubkeys = tal_arr(cmd, struct pubkey, tok->size);
	json_for_each_arr(i, t, tok) {
		if (!json_to_pubkey(buffer, t, &(*pubkeys)[i]))
			return command_fail_badparam(cmd, name, buffer, t,
						     "should be a compressed pubkey");
	}
	return NULL;
}

static struct command_result *json_blindedpath(struct command *cmd,
					       const char *buffer,
					       const jsmntok_t *obj UNNEEDED,
					       const jsmntok_t *params)
{
	struct pubkey *ids;
	struct privkey first_blinding, blinding_iter;
	struct pubkey me;
	struct blinded_path *path;
	size_t nhops;
	struct json_stream *response;

	if (!param(cmd, buffer, params,
		   p_req("ids", param_pubkeys, &ids),
		   NULL))
		return command_param_failed();

	path = tal(cmd, struct blinded_path);
	nhops = tal_count(ids);

	/* Final id should be us! */
	if (!pubkey_from_node_id(&me, &cmd->ld->id))
		fatal("My id %s is invalid?",
		      type_to_string(tmpctx, struct node_id, &cmd->ld->id));

	path->first_node_id = ids[0];
	if (!pubkey_eq(&ids[nhops-1], &me))
		return command_fail(cmd, LIGHTNINGD,
				    "Final of ids must be this node (%s), not %s",
				    type_to_string(tmpctx, struct pubkey, &me),
				    type_to_string(tmpctx, struct pubkey,
						   &ids[nhops-1]));

	randombytes_buf(&first_blinding, sizeof(first_blinding));
	if (!pubkey_from_privkey(&first_blinding, &path->blinding))
		/* Should not happen! */
		return command_fail(cmd, LIGHTNINGD,
				    "Could not convert blinding to pubkey!");

	/* We convert ids into aliases as we go. */
	path->path = tal_arr(cmd, struct onionmsg_hop *, nhops);

	blinding_iter = first_blinding;
	for (size_t i = 0; i < nhops - 1; i++) {
		path->path[i] = tal(path->path, struct onionmsg_hop);
		path->path[i]->encrypted_recipient_data
			= create_enctlv(path->path[i],
					&blinding_iter,
					&ids[i],
					&ids[i+1], NULL,
					/* FIXME: Pad? */
					0,
					NULL, NULL, NULL, NULL,
					&blinding_iter,
					&path->path[i]->blinded_node_id);
	}

	/* FIXME: Add padding! */
	path->path[nhops-1] = tal(path->path, struct onionmsg_hop);
	path->path[nhops-1]->encrypted_recipient_data
		= create_final_enctlv(path->path[nhops-1],
				      &blinding_iter,
				      &ids[nhops-1],
				      /* FIXME: Pad? */
				      0,
				      &cmd->ld->onion_reply_secret,
				      NULL,
				      &path->path[nhops-1]->blinded_node_id);

	response = json_stream_success(cmd);
	json_add_blindedpath(response, "blindedpath", path);

	return command_success(cmd, response);
}

static const struct json_command blindedpath_command = {
	"blindedpath",
	"utility",
	json_blindedpath,
	"Create blinded path to us along {ids} (pubkey array ending in our id)"
};
AUTODATA(json_command, &blindedpath_command);
