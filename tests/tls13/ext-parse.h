/*
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GnuTLS.
 *
 * GnuTLS is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "utils.h"

#define TLS_EXT_SUPPORTED_VERSIONS 43
#define TLS_EXT_POST_HANDSHAKE 49

#define SKIP16(pos, total) { \
	uint16_t _s; \
	if (pos+2 > total) fail("error0: at %d total: %d\n", pos+2, total); \
	_s = (msg->data[pos] << 8) | msg->data[pos+1]; \
	if ((size_t)(pos+2+_s) > total) fail("error1: at %d field: %d, total: %d\n", pos+2, (int)_s, total); \
	pos += 2+_s; \
	}

#define SKIP8(pos, total) { \
	uint8_t _s; \
	if (pos+1 > total) fail("error\n"); \
	_s = msg->data[pos]; \
	if ((size_t)(pos+1+_s) > total) fail("error\n"); \
	pos += 1+_s; \
	}

typedef void (*ext_parse_func)(gnutls_datum_t *extdata);

#define HANDSHAKE_SESSION_ID_POS 34

/* Returns 0 if the extension was not found, 1 otherwise.
 */
static unsigned find_client_extension(const gnutls_datum_t *msg, unsigned extnr, ext_parse_func cb)
{
	unsigned pos;

	if (msg->size < HANDSHAKE_SESSION_ID_POS)
		fail("invalid client hello\n");

	/* we expect the legacy version to be present */
	/* ProtocolVersion legacy_version = 0x0303 */
	if (msg->data[0] != 0x03) {
		fail("ProtocolVersion contains %d.%d\n", (int)msg->data[0], (int)msg->data[1]);
	}

	pos = HANDSHAKE_SESSION_ID_POS;
	/* legacy_session_id */
	SKIP8(pos, msg->size);

	/* CipherSuites */
	SKIP16(pos, msg->size);

	/* legacy_compression_methods */
	SKIP8(pos, msg->size);

	pos += 2;

	while (pos < msg->size) {
		uint16_t type;

		if (pos+4 > msg->size)
			fail("invalid client hello\n");

		type = (msg->data[pos] << 8) | msg->data[pos+1];
		pos+=2;

		success("Found client extension %d\n", (int)type);

		if (type != extnr) {
			SKIP16(pos, msg->size);
		} else { /* found */
			ssize_t size = (msg->data[pos] << 8) | msg->data[pos+1];
			gnutls_datum_t data;

			pos+=2;
			if (pos + size > msg->size) {
				fail("error in extension length (pos: %d, ext: %d, total: %d)\n", pos, (int)size, msg->size);
			}
			data.data = &msg->data[pos];
			data.size = size;
			if (cb)
				cb(&data);
			return 1;
		}
	}
	return 0;
}

#define TLS_RANDOM_SIZE 32

static unsigned find_server_extension(const gnutls_datum_t *msg, unsigned extnr, ext_parse_func cb)
{
	unsigned tls13 = 0;
	unsigned pos = 0;

	success("server hello of %d bytes\n", msg->size);
	/* we expect the legacy version to be present */
	/* ProtocolVersion legacy_version = 0x0303 */
#ifdef TLS13_FINAL_VERSION
	if (msg->data[0] != 0x03) {
#else
	if (msg->data[0] != 0x7f) {
#endif
		fail("ProtocolVersion contains %d.%d\n", (int)msg->data[0], (int)msg->data[1]);
	}

	if (msg->data[1] >= 0x04) {
		success("assuming TLS 1.3 or better hello format (seen %d.%d)\n", (int)msg->data[0], (int)msg->data[1]);
		tls13 = 1;
	}

	pos += 2+TLS_RANDOM_SIZE;

	if (!tls13) {
		/* legacy_session_id */
		SKIP8(pos, msg->size);
	}

	/* CipherSuite */
	pos += 2;

	if (!tls13) {
		/* legacy_compression_methods */
		SKIP8(pos, msg->size);
	}

	pos += 2;

	while (pos < msg->size) {
		uint16_t type;

		if (pos+4 > msg->size)
			fail("invalid server hello\n");

		type = (msg->data[pos] << 8) | msg->data[pos+1];
		pos+=2;

		success("Found server extension %d\n", (int)type);

		if (type != extnr) {
			SKIP16(pos, msg->size);
		} else { /* found */
			ssize_t size = (msg->data[pos] << 8) | msg->data[pos+1];
			gnutls_datum_t data;

			pos+=2;
			if (pos + size < msg->size) {
				fail("error in server extension length (pos: %d, total: %d)\n", pos, msg->size);
			}
			data.data = &msg->data[pos];
			data.size = size;
			if (cb)
				cb(&data);
			return 1;
		}
	}

	return 0;
}
