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
 * ax25netd listens on the loop port, local AX.25 services (ax25tcpd, the
 * AGWPE shim) connect to it.  The endpoint is configured once, in
 * this file, so server and client sides cannot drift apart:
 *
 *	loop socket <path|no>	unix domain socket of the loop port
 *				(default: AX25COMMON_SOCKET_DEFAULT,
 *				which configure sets to
 *				/var/run/ax25/sockets/ax25netd.sock)
 *	loop tcp <port|yes|no>	TCP loop port (default: no)
 *	loop group <name|gid|all|default>
 *				who may connect to the unix socket, and
 *				which mode the socket itself gets
 *				(default: all, i.e. every local account;
 *				"default" is the daemon's run user and
 *				its primary group)
 *	loop mode <octal>	mode of the directory holding the socket
 *				(default: AX25COMMON_MODE_DEFAULT, i.e.
 *				1775 - other accounts may reach it)
 *
 * The file is read by both daemons; a missing file is not an error and
 * leaves the defaults in place.
 */

#ifndef	_NETAX25_AXCOMMON_H
#define	_NETAX25_AXCOMMON_H

#include <sys/types.h>		/* mode_t */

#include <netax25/agwpe_config.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	AX25COMMON_SOCKET_MAX	108
#define	AX25COMMON_GROUP_MAX	64
#define	AX25COMMON_TCP_DEFAULT	8200

/*
 * 1775: the group the directory belongs to may create and remove entries,
 * any other account may read and enter it, and the sticky bit keeps one
 * daemon from deleting another's socket.  Wide on purpose - a station
 * that installed this wants its programs to reach the loop port, and the
 * decision about who may hold the radio is taken with "loop group".
 */
#define	AX25COMMON_MODE_DEFAULT	01775

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

	/* Mode of the directory the socket lives in, as an octal mode_t.
	 * The daemons create it when it is missing, so this is what a
	 * fresh install ends up with.  */
	mode_t			loop_mode;
};

/*
 * Read the shared loop port configuration from path (typically
 * "ax25common.conf" below AX25_SYSCONFDIR).  Returns 0 on success; a
 * missing file is not an error and leaves the defaults in place.
 * Returns -1 on parse errors.
 */
extern int ax25common_config_load(const char *path, struct ax25common *cfg);

/*
 * Where ax25common.conf is looked for by default, for a caller that has
 * no reason to want another path - the AGWPE shim, which follows the loop
 * port rather than configuring it.
 */
extern const char *ax25common_default_config(void);

#ifdef __cplusplus
}
#endif

#endif
