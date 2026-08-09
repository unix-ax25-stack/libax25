/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 1997-1999 Jonathan Naylor, Tomi Manninen, Jean-Paul Roubelat
 * and Alan Cox.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */
/*
 * Helper functions for the AGWPE TCP/IP protocol (see netax25/agwpe.h).
 */

#include <string.h>
#include <stdio.h>

#include "netax25/agwpe.h"

void agwpe_header_init(struct agwpe_s *hdr, unsigned char port,
		       unsigned char datakind, unsigned char pid,
		       const char *call_from, const char *call_to,
		       uint32_t data_len)
{
	if (hdr == NULL)
		return;

	memset(hdr, 0, sizeof(*hdr));
	hdr->port = port;
	hdr->datakind = datakind;
	hdr->pid = pid;

	if (call_from != NULL)
		agwpe_call_pack(hdr->call_from, call_from);
	if (call_to != NULL)
		agwpe_call_pack(hdr->call_to, call_to);

	hdr->data_len = data_len;
}

void agwpe_call_pack(char dst[AGWPE_MAX_CALL], const char *call)
{
	size_t n;

	if (dst == NULL)
		return;

	memset(dst, 0, AGWPE_MAX_CALL);
	if (call == NULL)
		return;

	n = strlen(call);
	if (n > AGWPE_MAX_CALL - 1)
		n = AGWPE_MAX_CALL - 1;
	memcpy(dst, call, n);
}

int agwpe_call_unpack(char *buf, size_t buflen, const char src[AGWPE_MAX_CALL])
{
	size_t n;

	if (buf == NULL || buflen == 0)
		return -1;

	if (src == NULL || src[0] == '\0')
		return -1;

	n = 0;
	while (n < AGWPE_MAX_CALL && src[n] != '\0')
		n++;

	if (n > buflen - 1)
		n = buflen - 1;

	memcpy(buf, src, n);
	buf[n] = '\0';

	return (int)n;
}

int agwpe_monitor_split(const unsigned char *data, size_t len,
			const char **text, size_t *text_len,
			const unsigned char **payload, size_t *payload_len)
{
	const unsigned char *cr;
	size_t tlen;

	if (data == NULL || text == NULL || text_len == NULL)
		return -1;

	cr = memchr(data, '\r', len);
	if (cr == NULL) {
		/* All text, no payload.  */
		*text = (const char *)data;
		*text_len = len;
		if (payload != NULL)
			*payload = NULL;
		if (payload_len != NULL)
			*payload_len = 0;
		return -1;
	}

	tlen = (size_t)(cr - data);
	*text = (const char *)data;
	*text_len = tlen;
	if (payload != NULL)
		*payload = cr + 1;
	if (payload_len != NULL)
		*payload_len = len - tlen - 1;

	return 0;
}
