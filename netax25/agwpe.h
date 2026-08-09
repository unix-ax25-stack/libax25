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
 * This file describes the AGWPE TCP/IP client protocol, as published in
 * the "AGWPE TCP/IP API Tutorial" by Ing. Pedro E. Colla (LU7DID) and
 * George Rossopoulos (SV2AGW).  Servers which speak this protocol include
 * AGWPE on Windows, and Direwolf (which implements a compatible subset).
 *
 * Every message consists of a 36 byte header followed by an optional
 * data area whose length is given by the (little endian on the wire)
 * data_len field.
 */

#ifndef	_NETAX25_AGWPE_H
#define	_NETAX25_AGWPE_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	AGWPE_HEADER_LEN	36
#define	AGWPE_MAX_CALL		10
#define	AGWPE_MAX_DIGIS		8

/* The wire protocol identifies the radio ports starting at zero.  */
#define	AGWPE_PORT_DEFAULT	8000
#define	AGWPE_PORT_MAX		100

/* Reserved port of the virtual "loop" upstream, which ax25netd serves
 * locally instead of connecting to a radio.  */
#define	AGWPE_PORT_LOOP		255

struct agwpe_s {
	unsigned char	port;		/* 0 = first port, 1 = second, ... */
	unsigned char	reserved1;
	unsigned char	reserved2;
	unsigned char	reserved3;

	unsigned char	datakind;	/* the frame code, usually a letter */
	unsigned char	reserved4;
	unsigned char	pid;
	unsigned char	reserved5;

	char		call_from[AGWPE_MAX_CALL];
	char		call_to[AGWPE_MAX_CALL];

	uint32_t	data_len;	/* little endian on the wire */
	uint32_t	user_reserved;
};

/* Frames sent by an application to the server.  */
#define	AGWPE_CMD_LOGIN			'P'
#define	AGWPE_CMD_REGISTER		'X'
#define	AGWPE_CMD_UNREGISTER		'x'
#define	AGWPE_CMD_PORT_INFO		'G'
#define	AGWPE_CMD_PORT_CAPAB		'g'
#define	AGWPE_CMD_VERSION		'R'
#define	AGWPE_CMD_HEARD			'H'
#define	AGWPE_CMD_MONITOR		'm'
#define	AGWPE_CMD_UNPROTO		'M'
#define	AGWPE_CMD_UNPROTO_VIA		'V'
#define	AGWPE_CMD_CONNECT		'C'
#define	AGWPE_CMD_CONNECT_VIA		'v'
#define	AGWPE_CMD_CONNECT_PID		'c'
#define	AGWPE_CMD_DATA			'D'
#define	AGWPE_CMD_DISCONNECT		'd'
#define	AGWPE_CMD_RAW			'K'
#define	AGWPE_CMD_RAW_MONITOR		'k'
#define	AGWPE_CMD_OUTSTANDING_PORT	'y'
#define	AGWPE_CMD_OUTSTANDING_CONN	'Y'

/* Private extension of ax25netd: register a listening socket on the
 * virtual loop port.  Only meaningful on AGWPE_PORT_LOOP.  */
#define	AGWPE_CMD_LISTEN		'L'

/* Private extension of ax25netd: connection control.  The frame names
 * the connection with the usual port / call_from / call_to fields and
 * carries one command in its data area.  Used by axctl and axkill to
 * adjust or terminate an established session without the application
 * that owns it having to do anything.
 *
 *   data[0] = AGWPE_CTL_KILL	drop the connection (no further data)
 *   data[0] = AGWPE_CTL_PARAM	set a parameter:
 *     data[1]  scope (AGWPE_CTL_SCOPE_*)
 *     data[2]  parameter (AGWPE_CTL_PARAM_*)
 *     data[3..6] value, little endian
 */
#define	AGWPE_CMD_CTL			'Q'

#define	AGWPE_CTL_KILL			'K'
#define	AGWPE_CTL_PARAM			'P'

/* Scope of a parameter change.  */
#define	AGWPE_CTL_SCOPE_CONN		0	/* the addressed connection */
#define	AGWPE_CTL_SCOPE_PORT		1	/* port defaults */

/* Parameter identifiers, matching the kernel AX25_* socket constants so
 * that axctl can pass them through unchanged.  */
#define	AGWPE_CTL_PARAM_WINDOW		1
#define	AGWPE_CTL_PARAM_T1		2
#define	AGWPE_CTL_PARAM_N2		3
#define	AGWPE_CTL_PARAM_T3		4
#define	AGWPE_CTL_PARAM_T2		5
#define	AGWPE_CTL_PARAM_IDLE		9
#define	AGWPE_CTL_PARAM_PACLEN		10

/* Frames sent by the server to an application.  */
#define	AGWPE_DK_CONNECT		'C'
#define	AGWPE_DK_DATA			'D'
#define	AGWPE_DK_DISCONNECT		'd'
#define	AGWPE_DK_REGISTERED		'X'
#define	AGWPE_DK_VERSION		'R'
#define	AGWPE_DK_PORTS			'G'
#define	AGWPE_DK_CAPAB			'g'
#define	AGWPE_DK_HEARD			'H'
#define	AGWPE_DK_MON_I			'I'
#define	AGWPE_DK_MON_S			'S'
#define	AGWPE_DK_MON_U			'U'
#define	AGWPE_DK_MON_T			'T'
#define	AGWPE_DK_RAW			'K'
#define	AGWPE_DK_OUTSTANDING_PORT	'y'
#define	AGWPE_DK_OUTSTANDING_CONN	'Y'

/* Standard AX.25 PID values.  */
#define	AGWPE_PID_AX25		0xF0
#define	AGWPE_PID_NETROM	0xCF
#define	AGWPE_PID_ROSE		0xC3
#define	AGWPE_PID_TEXNET	0xC4
#define	AGWPE_PID_L3		0xCC

/* DataLen and User fields are little endian on the wire.  */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define	agwpe_host2netle(x)	( (((x) >> 24) & 0x000000ff) | (((x) >> 8) & 0x0000ff00) | (((x) << 8) & 0x00ff0000) | (((x) << 24) & 0xff000000) )
#define	agwpe_netle2host(x)	( (((x) >> 24) & 0x000000ff) | (((x) >> 8) & 0x0000ff00) | (((x) << 8) & 0x00ff0000) | (((x) << 24) & 0xff000000) )
#else
#define	agwpe_host2netle(x)	(x)
#define	agwpe_netle2host(x)	(x)
#endif

/*
 * Fill in the header fields of a frame about to be sent to the server.
 * All reserved fields are cleared, which is what the protocol requires.
 */
extern void agwpe_header_init(struct agwpe_s *hdr, unsigned char port,
			      unsigned char datakind, unsigned char pid,
			      const char *call_from, const char *call_to,
			      uint32_t data_len);

/*
 * Copy a "CALLSIGN-SSID" string into a 10 byte header field, zero padded.
 */
extern void agwpe_call_pack(char dst[AGWPE_MAX_CALL], const char *call);

/*
 * Copy a 10 byte header field into a NUL terminated "CALLSIGN-SSID" string.
 * Returns the length of the string, or -1 if the field was empty.
 */
extern int agwpe_call_unpack(char *buf, size_t buflen, const char src[AGWPE_MAX_CALL]);

/*
 * The monitored information frames (I, U and T) consist of a plain ASCII
 * decoded header line terminated by a carriage return, followed by the
 * binary information part.  This helper locates the two parts.  Returns
 * 0 on success, -1 if there is no carriage return (all text).
 */
extern int agwpe_monitor_split(const unsigned char *data, size_t len,
			       const char **text, size_t *text_len,
			       const unsigned char **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif
