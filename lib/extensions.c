/*
 * Copyright (C) 2001-2016 Free Software Foundation, Inc.
 * Copyright (C) 2015-2017 Red Hat, Inc.
 *
 * Author: Nikos Mavrogiannopoulos, Simon Josefsson
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

/* Functions that relate to the TLS hello extension parsing.
 * Hello extensions are packets appended in the TLS hello packet, and
 * allow for extra functionality.
 */

#include "gnutls_int.h"
#include "extensions.h"
#include "errors.h"
#include "ext/max_record.h"
#include <ext/server_name.h>
#include <ext/srp.h>
#include <ext/heartbeat.h>
#include <ext/session_ticket.h>
#include <ext/safe_renegotiation.h>
#include <ext/signature.h>
#include <ext/safe_renegotiation.h>
#include <ext/ecc.h>
#include <ext/status_request.h>
#include <ext/ext_master_secret.h>
#include <ext/supported_versions.h>
#include <ext/post_handshake.h>
#include <ext/srtp.h>
#include <ext/alpn.h>
#include <ext/dumbfw.h>
#include <ext/key_share.h>
#include <ext/etm.h>
#include "extv.h"
#include <num.h>

static void
unset_ext_data(gnutls_session_t session, const struct hello_ext_entry_st *, unsigned idx);

static int ext_register(hello_ext_entry_st * mod);
static void unset_resumed_ext_data(gnutls_session_t session, const struct hello_ext_entry_st *, unsigned idx);

static hello_ext_entry_st const *extfunc[MAX_EXT_TYPES+1] = {
	&ext_mod_max_record_size,
	&ext_mod_ext_master_secret,
	&ext_mod_supported_versions,
	&ext_mod_post_handshake,
	&ext_mod_etm,
#ifdef ENABLE_OCSP
	&ext_mod_status_request,
#endif
	&ext_mod_server_name,
	&ext_mod_sr,
#ifdef ENABLE_SRP
	&ext_mod_srp,
#endif
#ifdef ENABLE_HEARTBEAT
	&ext_mod_heartbeat,
#endif
#ifdef ENABLE_SESSION_TICKETS
	&ext_mod_session_ticket,
#endif
	&ext_mod_supported_ecc,
	&ext_mod_supported_ecc_pf,
	&ext_mod_sig,
	&ext_mod_key_share,
#ifdef ENABLE_DTLS_SRTP
	&ext_mod_srtp,
#endif
#ifdef ENABLE_ALPN
	&ext_mod_alpn,
#endif
	/* This must be the last extension registered.
	 */
	&ext_mod_dumbfw,
	NULL
};

static const hello_ext_entry_st *
_gnutls_ext_ptr(gnutls_session_t session, extensions_t id, gnutls_ext_parse_type_t parse_type)
{
	unsigned i;
	const hello_ext_entry_st *e;

	for (i=0;i<session->internals.rexts_size;i++) {
		if (session->internals.rexts[i].gid == id) {
			e = &session->internals.rexts[i];
			goto done;
		}
	}

	for (i = 0; extfunc[i] != NULL; i++) {
		if (extfunc[i]->gid == id) {
			e = extfunc[i];
			goto done;
		}
	}

	return NULL;
done:
	if (parse_type == GNUTLS_EXT_ANY || e->parse_type == parse_type) {
		return e;
	} else {
		return NULL;
	}
}


/**
 * gnutls_ext_get_name:
 * @ext: is a TLS extension numeric ID
 *
 * Convert a TLS extension numeric ID to a printable string.
 *
 * Returns: a pointer to a string that contains the name of the
 *   specified cipher, or %NULL.
 **/
const char *gnutls_ext_get_name(unsigned int ext)
{
	size_t i;

	for (i = 0; extfunc[i] != NULL; i++)
		if (extfunc[i]->tls_id == ext)
			return extfunc[i]->name;

	return NULL;
}

static unsigned tls_id_to_gid(gnutls_session_t session, unsigned tls_id)
{
	unsigned i;

	for (i=0; i < session->internals.rexts_size; i++) {
		if (session->internals.rexts[i].tls_id == tls_id)
			return session->internals.rexts[i].gid;
	}

	for (i = 0; extfunc[i] != NULL; i++) {
		if (extfunc[i]->tls_id == tls_id)
			return extfunc[i]->gid;
	}

	return 0;
}

void _gnutls_extension_list_add_sr(gnutls_session_t session)
{
	_gnutls_extension_list_add(session, &ext_mod_sr, 1);
}

typedef struct hello_ext_ctx_st {
	gnutls_session_t session;
	gnutls_ext_flags_t msg;
	gnutls_ext_parse_type_t parse_type;
	const hello_ext_entry_st *ext; /* used during send */
} hello_ext_ctx_st;

static
int hello_ext_parse(void *_ctx, uint16_t tls_id, const uint8_t *data, int data_size)
{
	hello_ext_ctx_st *ctx = _ctx;
	gnutls_session_t session = ctx->session;
	const hello_ext_entry_st *ext;
	unsigned id;
	int ret;

	id = tls_id_to_gid(session, tls_id);
	if (id == 0) { /* skip */
		return 0;
	}

	if (session->security_parameters.entity == GNUTLS_CLIENT) {
		if ((ret =
		     _gnutls_extension_list_check(session, id)) < 0) {
			_gnutls_debug_log("EXT[%p]: Received unexpected extension '%s/%d'\n", session,
					gnutls_ext_get_name(tls_id), (int)tls_id);
			gnutls_assert();
			return ret;
		}
	}

	ext = _gnutls_ext_ptr(session, id, ctx->parse_type);
	if (ext == NULL || ext->recv_func == NULL) {
		_gnutls_handshake_log
		    ("EXT[%p]: Ignoring extension '%s/%d'\n", session,
		     gnutls_ext_get_name(tls_id), tls_id);
		return 0;
	}

	if ((ext->validity & ctx->msg) == 0) {
		_gnutls_debug_log("EXT[%p]: Received unexpected extension (%s/%d) for '%s'\n", session,
				  gnutls_ext_get_name(tls_id), (int)tls_id,
				  ext_msg_validity_to_str(ctx->msg));
		return gnutls_assert_val(GNUTLS_E_RECEIVED_ILLEGAL_EXTENSION);
	}

	if (session->security_parameters.entity == GNUTLS_SERVER) {
		ret = _gnutls_extension_list_add(session, ext, 1);
		if (ret == 0)
			return gnutls_assert_val(GNUTLS_E_RECEIVED_ILLEGAL_EXTENSION);
	}

	_gnutls_handshake_log
	    ("EXT[%p]: Parsing extension '%s/%d' (%d bytes)\n",
	     session, gnutls_ext_get_name(tls_id), tls_id,
	     data_size);

	if ((ret = ext->recv_func(session, data, data_size)) < 0) {
		gnutls_assert();
		return ret;
	}

	return 0;
}

int
_gnutls_parse_extensions(gnutls_session_t session,
			 gnutls_ext_flags_t msg,
			 gnutls_ext_parse_type_t parse_type,
			 const uint8_t * data, int data_size)
{
	int ret;
	hello_ext_ctx_st ctx;

	ctx.session = session;
	ctx.msg = msg;
	ctx.parse_type = parse_type;

	ret = _gnutls_extv_parse(&ctx, hello_ext_parse, data, data_size);
	if (ret < 0)
		return gnutls_assert_val(ret);

	return 0;
}

static
int hello_ext_send(void *_ctx, gnutls_buffer_st *buf)
{
	hello_ext_ctx_st *ctx = _ctx;
	int ret;
	const hello_ext_entry_st *p = ctx->ext;
	gnutls_session_t session = ctx->session;
	int appended;
	size_t size_prev;

	if (unlikely(p->send_func == NULL))
		return 0;

	if (ctx->parse_type != GNUTLS_EXT_ANY
	    && p->parse_type != ctx->parse_type) {
		return 0;
	}

	if ((ctx->msg & p->validity) == 0) {
		_gnutls_handshake_log("EXT[%p]: Not sending extension (%s/%d) for '%s'\n", session,
				  p->name, (int)p->tls_id,
				  ext_msg_validity_to_str(ctx->msg));
		return 0;
	}

	/* ensure we don't send something twice (i.e, overriden extensions in
	 * client), and ensure we are sending only what we received in server. */
	ret = _gnutls_extension_list_check(session, p->gid);

	if (session->security_parameters.entity == GNUTLS_SERVER) {
		if (ret < 0) {/* not advertized */
			return 0;
		}
	} else {
		if (ret == 0) {/* already sent */
			return 0;
		}
	}


	size_prev = buf->length;

	ret = p->send_func(session, buf);
	if (ret < 0 && ret != GNUTLS_E_INT_RET_0) {
		return gnutls_assert_val(ret);
	}

	appended = buf->length - size_prev;

	/* add this extension to the extension list, to know which extensions
	 * to expect.
	 */
	if ((appended > 0 || ret == GNUTLS_E_INT_RET_0) &&
	    session->security_parameters.entity == GNUTLS_CLIENT) {

		_gnutls_extension_list_add(session, p, 0);
	}

	return ret;
}

int
_gnutls_gen_extensions(gnutls_session_t session,
		       gnutls_buffer_st * buf,
		       gnutls_ext_flags_t msg,
		       gnutls_ext_parse_type_t parse_type)
{
	int pos, ret;
	size_t i;
	hello_ext_ctx_st ctx;

	ctx.session = session;
	ctx.msg = msg;
	ctx.parse_type = parse_type;

	ret = _gnutls_extv_append_init(buf);
	if (ret < 0)
		return gnutls_assert_val(ret);

	pos = ret;

	for (i=0; i < session->internals.rexts_size; i++) {
		ctx.ext = &session->internals.rexts[i];
		ret = _gnutls_extv_append(buf, session->internals.rexts[i].tls_id,
					  &ctx, hello_ext_send);
		if (ret < 0)
			return gnutls_assert_val(ret);

		if (ret > 0)
			_gnutls_handshake_log
				    ("EXT[%p]: Sending extension %s/%d (%d bytes)\n",
				     session, ctx.ext->name, (int)ctx.ext->tls_id, ret-4);
	}

	/* send_extension() ensures we don't send duplicates, in case
	 * of overriden extensions */
	for (i = 0; extfunc[i] != NULL; i++) {
		ctx.ext = extfunc[i];
		ret = _gnutls_extv_append(buf, extfunc[i]->tls_id,
					  &ctx, hello_ext_send);
		if (ret < 0)
			return gnutls_assert_val(ret);

		if (ret > 0)
			_gnutls_handshake_log
				    ("EXT[%p]: Sending extension %s/%d (%d bytes)\n",
				     session, ctx.ext->name, (int)ctx.ext->tls_id, ret-4);
	}

	ret = _gnutls_extv_append_final(buf, pos);
	if (ret < 0)
		return gnutls_assert_val(ret);

	return 0;
}

/* Global deinit and init of global extensions */
int _gnutls_ext_init(void)
{
	return GNUTLS_E_SUCCESS;
}

void _gnutls_ext_deinit(void)
{
	unsigned i;
	for (i = 0; extfunc[i] != NULL; i++) {
		if (extfunc[i]->free_struct != 0) {
			gnutls_free((void*)extfunc[i]->name);
			gnutls_free((void*)extfunc[i]);
			extfunc[i] = NULL;
		}
	}
}

static
int ext_register(hello_ext_entry_st * mod)
{
	unsigned i = 0;

	while(extfunc[i] != NULL) {
		i++;
	}

	if (i >= MAX_EXT_TYPES-1) {
		return gnutls_assert_val(GNUTLS_E_INTERNAL_ERROR);
	}

	extfunc[i] = mod;
	extfunc[i+1] = NULL;
	return GNUTLS_E_SUCCESS;
}

/* Packing of extension data (for use in resumption) */
static int pack_extension(gnutls_session_t session, const hello_ext_entry_st *extp,
			  gnutls_buffer_st *packed)
{
	int ret;
	int size_offset;
	int cur_size;
	gnutls_ext_priv_data_t data;
	int rval = 0;

	ret =
	    _gnutls_ext_get_session_data(session, extp->gid,
					 &data);
	if (ret >= 0 && extp->pack_func != NULL) {
		BUFFER_APPEND_NUM(packed, extp->gid);

		size_offset = packed->length;
		BUFFER_APPEND_NUM(packed, 0);

		cur_size = packed->length;

		ret = extp->pack_func(data, packed);
		if (ret < 0) {
			gnutls_assert();
			return ret;
		}

		rval = 1;
		/* write the actual size */
		_gnutls_write_uint32(packed->length - cur_size,
				     packed->data + size_offset);
	}

	return rval;
}

int _gnutls_ext_pack(gnutls_session_t session, gnutls_buffer_st *packed)
{
	unsigned int i;
	int ret;
	int total_exts_pos;
	int n_exts = 0;
	const struct hello_ext_entry_st *ext;

	total_exts_pos = packed->length;
	BUFFER_APPEND_NUM(packed, 0);

	for (i = 0; i <= GNUTLS_EXTENSION_MAX_VALUE; i++) {
		if (session->internals.used_exts & (1<<i)) {

			ext = _gnutls_ext_ptr(session, i, GNUTLS_EXT_ANY);
			if (ext == NULL)
				continue;

			ret = pack_extension(session, ext, packed);
			if (ret < 0)
				return gnutls_assert_val(ret);

			if (ret > 0)
				n_exts++;
		}
	}

	_gnutls_write_uint32(n_exts, packed->data + total_exts_pos);

	return 0;
}

static void
_gnutls_ext_set_resumed_session_data(gnutls_session_t session,
				     extensions_t id,
				     gnutls_ext_priv_data_t data)
{
	int i;
	const struct hello_ext_entry_st *ext;

	ext = _gnutls_ext_ptr(session, id, GNUTLS_EXT_ANY);

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (session->internals.ext_data[i].id == id
		    || (!session->internals.ext_data[i].resumed_set && !session->internals.ext_data[i].set)) {

			if (session->internals.ext_data[i].resumed_set != 0)
				unset_resumed_ext_data(session, ext, i);

			session->internals.ext_data[i].id = id;
			session->internals.ext_data[i].resumed_priv = data;
			session->internals.ext_data[i].resumed_set = 1;
			return;
		}
	}
}

int _gnutls_ext_unpack(gnutls_session_t session, gnutls_buffer_st * packed)
{
	int i, ret;
	gnutls_ext_priv_data_t data;
	int max_exts = 0;
	extensions_t id;
	int size_for_id, cur_pos;
	const struct hello_ext_entry_st *ext;

	BUFFER_POP_NUM(packed, max_exts);
	for (i = 0; i < max_exts; i++) {
		BUFFER_POP_NUM(packed, id);
		BUFFER_POP_NUM(packed, size_for_id);

		cur_pos = packed->length;

		ext = _gnutls_ext_ptr(session, id, GNUTLS_EXT_ANY);
		if (ext == NULL || ext->unpack_func == NULL) {
			gnutls_assert();
			return GNUTLS_E_PARSING_ERROR;
		}

		ret = ext->unpack_func(packed, &data);
		if (ret < 0) {
			gnutls_assert();
			return ret;
		}

		/* verify that unpack read the correct bytes */
		cur_pos = cur_pos - packed->length;
		if (cur_pos /* read length */  != size_for_id) {
			gnutls_assert();
			return GNUTLS_E_PARSING_ERROR;
		}

		_gnutls_ext_set_resumed_session_data(session, id, data);
	}

	return 0;

      error:
	return ret;
}

static void
unset_ext_data(gnutls_session_t session, const struct hello_ext_entry_st *ext, unsigned idx)
{
	if (session->internals.ext_data[idx].set == 0)
		return;

	if (ext && ext->deinit_func && session->internals.ext_data[idx].priv != NULL)
		ext->deinit_func(session->internals.ext_data[idx].priv);
	session->internals.ext_data[idx].set = 0;
}

void
_gnutls_ext_unset_session_data(gnutls_session_t session,
				extensions_t id)
{
	int i;
	const struct hello_ext_entry_st *ext;

	ext = _gnutls_ext_ptr(session, id, GNUTLS_EXT_ANY);

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (session->internals.ext_data[i].id == id) {
			unset_ext_data(session, ext, i);
			return;
		}
	}
}

static void unset_resumed_ext_data(gnutls_session_t session, const struct hello_ext_entry_st *ext, unsigned idx)
{
	if (session->internals.ext_data[idx].resumed_set == 0)
		return;

	if (ext && ext->deinit_func && session->internals.ext_data[idx].resumed_priv) {
		ext->deinit_func(session->internals.ext_data[idx].resumed_priv);
	}
	session->internals.ext_data[idx].resumed_set = 0;
}

/* Deinitializes all data that are associated with TLS extensions.
 */
void _gnutls_ext_free_session_data(gnutls_session_t session)
{
	unsigned int i;
	const struct hello_ext_entry_st *ext;

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (!session->internals.ext_data[i].set && !session->internals.ext_data[i].resumed_set)
			continue;

		ext = _gnutls_ext_ptr(session, session->internals.ext_data[i].id, GNUTLS_EXT_ANY);

		unset_ext_data(session, ext, i);
		unset_resumed_ext_data(session, ext, i);
	}
}

/* This function allows an extension to store data in the current session
 * and retrieve them later on. We use functions instead of a pointer to a
 * private pointer, to allow API additions by individual extensions.
 */
void
_gnutls_ext_set_session_data(gnutls_session_t session, extensions_t id,
			     gnutls_ext_priv_data_t data)
{
	unsigned int i;
	const struct hello_ext_entry_st *ext;

	ext = _gnutls_ext_ptr(session, id, GNUTLS_EXT_ANY);

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (session->internals.ext_data[i].id == id ||
		    (!session->internals.ext_data[i].set && !session->internals.ext_data[i].resumed_set)) {

			if (session->internals.ext_data[i].set != 0) {
				unset_ext_data(session, ext, i);
			}
			session->internals.ext_data[i].id = id;
			session->internals.ext_data[i].priv = data;
			session->internals.ext_data[i].set = 1;
			return;
		}
	}
}

int
_gnutls_ext_get_session_data(gnutls_session_t session,
			     extensions_t id, gnutls_ext_priv_data_t * data)
{
	int i;

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (session->internals.ext_data[i].set != 0 &&
		    session->internals.ext_data[i].id == id)
		{
			*data =
			    session->internals.ext_data[i].priv;
			return 0;
		}
	}
	return GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE;
}

int
_gnutls_ext_get_resumed_session_data(gnutls_session_t session,
				     extensions_t id,
				     gnutls_ext_priv_data_t * data)
{
	int i;

	for (i = 0; i < MAX_EXT_TYPES; i++) {
		if (session->internals.ext_data[i].resumed_set != 0
		    && session->internals.ext_data[i].id == id) {
			*data =
			    session->internals.ext_data[i].resumed_priv;
			return 0;
		}
	}
	return GNUTLS_E_INVALID_REQUEST;
}

/**
 * gnutls_ext_register:
 * @name: the name of the extension to register
 * @id: the numeric TLS id of the extension
 * @parse_type: the parse type of the extension (see gnutls_ext_parse_type_t)
 * @recv_func: a function to receive the data
 * @send_func: a function to send the data
 * @deinit_func: a function deinitialize any private data
 * @pack_func: a function which serializes the extension's private data (used on session packing for resumption)
 * @unpack_func: a function which will deserialize the extension's private data
 *
 * This function will register a new extension type. The extension will remain
 * registered until gnutls_global_deinit() is called. If the extension type
 * is already registered then %GNUTLS_E_ALREADY_REGISTERED will be returned.
 *
 * Each registered extension can store temporary data into the gnutls_session_t
 * structure using gnutls_ext_set_data(), and they can be retrieved using
 * gnutls_ext_get_data().
 *
 * Any extensions registered with this function are valid for the client
 * and TLS1.2 server hello (or encrypted extensions for TLS1.3).
 *
 * This function is not thread safe.
 *
 * Returns: %GNUTLS_E_SUCCESS on success, otherwise a negative error code.
 *
 * Since: 3.4.0
 **/
int 
gnutls_ext_register(const char *name, int id, gnutls_ext_parse_type_t parse_type,
		    gnutls_ext_recv_func recv_func, gnutls_ext_send_func send_func, 
		    gnutls_ext_deinit_data_func deinit_func, gnutls_ext_pack_func pack_func,
		    gnutls_ext_unpack_func unpack_func)
{
	hello_ext_entry_st *tmp_mod;
	int ret;
	unsigned i;
	unsigned gid = GNUTLS_EXTENSION_MAX+1;

	for (i = 0; extfunc[i] != NULL; i++) {
		if (extfunc[i]->tls_id == id)
			return gnutls_assert_val(GNUTLS_E_ALREADY_REGISTERED);

		if (extfunc[i]->gid >= gid)
			gid = extfunc[i]->gid + 1;
	}

	if (gid > GNUTLS_EXTENSION_MAX_VALUE)
		return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);

	tmp_mod = gnutls_calloc(1, sizeof(*tmp_mod));
	if (tmp_mod == NULL)
		return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);

	tmp_mod->name = gnutls_strdup(name);
	tmp_mod->free_struct = 1;
	tmp_mod->tls_id = id;
	tmp_mod->gid = gid;
	tmp_mod->parse_type = parse_type;
	tmp_mod->recv_func = recv_func;
	tmp_mod->send_func = send_func;
	tmp_mod->deinit_func = deinit_func;
	tmp_mod->pack_func = pack_func;
	tmp_mod->unpack_func = unpack_func;
	tmp_mod->validity = GNUTLS_EXT_FLAG_CLIENT_HELLO|GNUTLS_EXT_FLAG_TLS12_SERVER_HELLO|GNUTLS_EXT_FLAG_EE;

	ret = ext_register(tmp_mod);
	if (ret < 0) {
		gnutls_free((void*)tmp_mod->name);
		gnutls_free(tmp_mod);
	}
	return ret;
}

#define VALIDITY_MASK (GNUTLS_EXT_FLAG_CLIENT_HELLO|GNUTLS_EXT_FLAG_TLS12_SERVER_HELLO| \
			GNUTLS_EXT_FLAG_TLS13_SERVER_HELLO| \
			GNUTLS_EXT_FLAG_EE|GNUTLS_EXT_FLAG_CT|GNUTLS_EXT_FLAG_CR| \
			GNUTLS_EXT_FLAG_NST|GNUTLS_EXT_FLAG_HRR)

/**
 * gnutls_session_ext_register:
 * @session: the session for which this extension will be set
 * @name: the name of the extension to register
 * @id: the numeric id of the extension
 * @parse_type: the parse type of the extension (see gnutls_ext_parse_type_t)
 * @recv_func: a function to receive the data
 * @send_func: a function to send the data
 * @deinit_func: a function deinitialize any private data
 * @pack_func: a function which serializes the extension's private data (used on session packing for resumption)
 * @unpack_func: a function which will deserialize the extension's private data
 * @flags: must be zero or flags from %gnutls_ext_flags_t
 *
 * This function will register a new extension type. The extension will be
 * only usable within the registered session. If the extension type
 * is already registered then %GNUTLS_E_ALREADY_REGISTERED will be returned,
 * unless the flag %GNUTLS_EXT_FLAG_OVERRIDE_INTERNAL is specified. The latter
 * flag when specified can be used to override certain extensions introduced
 * after 3.6.0. It is expected to be used by applications which handle
 * custom extensions that are not currently supported in GnuTLS, but direct
 * support for them may be added in the future.
 *
 * Each registered extension can store temporary data into the gnutls_session_t
 * structure using gnutls_ext_set_data(), and they can be retrieved using
 * gnutls_ext_get_data().
 *
 * The validity of the extension registered can be given by the appropriate flags
 * of %gnutls_ext_flags_t. If no validity is given, then the registered extension
 * will be valid for client and TLS1.2 server hello (or encrypted extensions for TLS1.3).
 *
 * Returns: %GNUTLS_E_SUCCESS on success, otherwise a negative error code.
 *
 * Since: 3.5.5
 **/
int 
gnutls_session_ext_register(gnutls_session_t session,
			    const char *name, int id, gnutls_ext_parse_type_t parse_type,
			    gnutls_ext_recv_func recv_func, gnutls_ext_send_func send_func, 
			    gnutls_ext_deinit_data_func deinit_func, gnutls_ext_pack_func pack_func,
			    gnutls_ext_unpack_func unpack_func, unsigned flags)
{
	hello_ext_entry_st tmp_mod;
	hello_ext_entry_st *exts;
	unsigned i;
	unsigned gid = GNUTLS_EXTENSION_MAX+1;

	/* reject handling any extensions which modify the TLS handshake
	 * in any way, or are mapped to an exported API. */
	for (i = 0; extfunc[i] != NULL; i++) {
		if (extfunc[i]->tls_id == id) {
			if (!(flags & GNUTLS_EXT_FLAG_OVERRIDE_INTERNAL)) {
				return gnutls_assert_val(GNUTLS_E_ALREADY_REGISTERED);
			} else if (extfunc[i]->cannot_be_overriden) {
				return gnutls_assert_val(GNUTLS_E_ALREADY_REGISTERED);
			}
			break;
		}

		if (extfunc[i]->gid >= gid)
			gid = extfunc[i]->gid + 1;
	}

	for (i=0;i<session->internals.rexts_size;i++) {
		if (session->internals.rexts[i].tls_id == id) {
			return gnutls_assert_val(GNUTLS_E_ALREADY_REGISTERED);
		}

		if (session->internals.rexts[i].gid >= gid)
			gid = session->internals.rexts[i].gid + 1;
	}

	if (gid > GNUTLS_EXTENSION_MAX_VALUE)
		return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);

	memset(&tmp_mod, 0, sizeof(hello_ext_entry_st));
	tmp_mod.free_struct = 1;
	tmp_mod.tls_id = id;
	tmp_mod.gid = gid;
	tmp_mod.parse_type = parse_type;
	tmp_mod.recv_func = recv_func;
	tmp_mod.send_func = send_func;
	tmp_mod.deinit_func = deinit_func;
	tmp_mod.pack_func = pack_func;
	tmp_mod.unpack_func = unpack_func;
	tmp_mod.validity = flags;

	if ((tmp_mod.validity & VALIDITY_MASK) == 0) {
		tmp_mod.validity = GNUTLS_EXT_FLAG_CLIENT_HELLO|GNUTLS_EXT_FLAG_TLS12_SERVER_HELLO|GNUTLS_EXT_FLAG_EE;
	}

	exts = gnutls_realloc(session->internals.rexts, (session->internals.rexts_size+1)*sizeof(*exts));
	if (exts == NULL) {
		return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);
	}

	session->internals.rexts = exts;

	memcpy(&session->internals.rexts[session->internals.rexts_size], &tmp_mod, sizeof(hello_ext_entry_st));
	session->internals.rexts_size++;

	return 0;
}

/**
 * gnutls_ext_set_data:
 * @session: a #gnutls_session_t opaque pointer
 * @tls_id: the numeric id of the extension
 * @data: the private data to set
 *
 * This function allows an extension handler to store data in the current session
 * and retrieve them later on. The set data will be deallocated using
 * the gnutls_ext_deinit_data_func.
 *
 * Since: 3.4.0
 **/
void
gnutls_ext_set_data(gnutls_session_t session, unsigned tls_id,
		    gnutls_ext_priv_data_t data)
{
	unsigned id = tls_id_to_gid(session, tls_id);
	if (id == 0)
		return;

	_gnutls_ext_set_session_data(session, id, data);
}

/**
 * gnutls_ext_get_data:
 * @session: a #gnutls_session_t opaque pointer
 * @tls_id: the numeric id of the extension
 * @data: a pointer to the private data to retrieve
 *
 * This function retrieves any data previously stored with gnutls_ext_set_data().
 *
 * Returns: %GNUTLS_E_SUCCESS on success, otherwise a negative error code.
 *
 * Since: 3.4.0
 **/
int
gnutls_ext_get_data(gnutls_session_t session,
		    unsigned tls_id, gnutls_ext_priv_data_t *data)
{
	unsigned id = tls_id_to_gid(session, tls_id);
	if (id == 0)
		return gnutls_assert_val(GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE);

	return _gnutls_ext_get_session_data(session, id, data);
}
