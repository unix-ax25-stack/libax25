/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 1997-1999 Jonathan Naylor, Tomi Manninen, Jean-Paul Roubelat
 * and Alan Cox.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
/*
 * Shared configuration of the ax25netd loop port (ax25common.conf).
 *
 * ax25netd listens on the loop port, local AX.25 services (ax25tcpd,
 * the AGWPE shim) connect to it.  The endpoint is configured once, in
 * this file, so server and client sides cannot drift apart:
 *
 *	loop socket <path|no>	unix domain socket of the loop port
 *				(default: none)
 *	loop tcp <port|yes|no>	TCP loop port (default: yes, 8100)
 *	loop group <name|gid|all>
 *				who may connect to the unix socket
 *				(default: the daemon's run user and its
 *				primary group)
 *
 * The file is read by both daemons; a missing file is not an error and
 * leaves the defaults in place.
 */

#ifndef	_NETAX25_AXCOMMON_H
#define	_NETAX25_AXCOMMON_H

#include <netax25/agwpe_config.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	AX25COMMON_SOCKET_MAX	108
#define	AX25COMMON_GROUP_MAX	64
#define	AX25COMMON_TCP_DEFAULT	8100

struct ax25common {
	/* Unix domain socket of the loop port; empty disables it.  */
	char			loop_socket[AX25COMMON_SOCKET_MAX];

	/* Whether the TCP loop listener is enabled, and on which port.  */
	int			loop_tcp_enabled;
	int			loop_tcp_port;

	/* Ownership of the unix socket: one of the AGWPE_GROUP_*
	 * constants; group_name names the group for AGWPE_GROUP_NAMED.  */
	int			group_mode;
	char			group_name[AX25COMMON_GROUP_MAX];
};

/*
 * Read the shared loop port configuration from path (typically
 * "ax25common.conf" below AX25_SYSCONFDIR).  Returns 0 on success; a
 * missing file is not an error and leaves the defaults in place.
 * Returns -1 on parse errors.
 */
extern int ax25common_config_load(const char *path, struct ax25common *cfg);

#ifdef __cplusplus
}
#endif

#endif
