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
 * Configuration for AGWPE upstreams (agwpe.conf).  Each upstream is an
 * AGWPE server (Direwolf, AGWPE on Windows, extmodem, ...) reachable
 * over TCP.  It is used by the ax25netd daemon to expose the upstreams
 * as a single AGWPE port to local clients.
 *
 * The reserved upstream name AGWPE_LOOP_NAME ("loop") is virtual: it
 * enables the local loopback port instead of naming a radio.  No host
 * or tcp port is needed for it, so the host and port fields are left
 * empty.
 */

#ifndef	_NETAX25_AGWPE_CONFIG_H
#define	_NETAX25_AGWPE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define	AGWPE_UPSTREAM_NAME_MAX	24
/* Unix socket paths can be up to sun_path (108) plus a leading "/".  */
#define	AGWPE_UPSTREAM_HOST_MAX	128
#define	AGWPE_AUTH_NAME_MAX	24
#define	AGWPE_AUTH_PASS_MAX	128

/* Reserved upstream name enabling the virtual local loopback port.  */
#define	AGWPE_LOOP_NAME		"loop"

/*
 * Reserved shadow file target for the general client authentication of
 * an ax25netd instance: credentials that any AGWPE client connecting to
 * the local server port (local applications and peer ax25netd boxes in
 * a chain) must present.  Not tied to AGWPE_LOOP_NAME: client login is
 * a property of the daemon itself, independent of whether the virtual
 * loop upstream is configured.
 */
#define	AGWPE_AUTH_TARGET	"ax25netd"

/* Loop port client authentication modes, selected by the "auth"
 * directive in agwpe.conf.  The directive asks how much authentication
 * to require, so "yes" means every client must log in.  The default
 * when the directive is absent is AGWPE_AUTH_EXTERN.  */
#define	AGWPE_AUTH_OFF		0	/* trust every client, no login */
#define	AGWPE_AUTH_EXTERN	1	/* trust loopback peers (127.0.0.1,
					 * ::1), require a login from any
					 * other peer */
#define	AGWPE_AUTH_ALWAYS	2	/* require a login from everyone */

/* Ownership of the loop port unix domain socket, selected by the "group"
 * directive.  AGWPE_GROUP_DEFAULT gives the socket to the daemon's run
 * user (uid and primary gid after the privilege drop), so the daemon's
 * own user and group can connect.  AGWPE_GROUP_NAMED restricts access
 * to a single group (e.g. "hams", see ax25-tools/ax25/axspawn.conf),
 * AGWPE_GROUP_ALL makes the socket world accessible.  */
#define	AGWPE_GROUP_DEFAULT	0
#define	AGWPE_GROUP_NAMED	1
#define	AGWPE_GROUP_ALL		2

struct agwpe_upstream {
	char			name[AGWPE_UPSTREAM_NAME_MAX];
	char			host[AGWPE_UPSTREAM_HOST_MAX];
	int			tcp_port;
	int			virtual;	/* virtual loop upstream */

	/* Optional AGWPE login credentials for this upstream, normally
	 * filled in from agwpe_shadow.conf.  */
	char			user[AGWPE_AUTH_NAME_MAX];
	char			pass[AGWPE_AUTH_PASS_MAX];
};

/* One credential record: either an upstream name or AGWPE_LOOP_NAME for
 * loop port clients.  */
struct agwpe_auth {
	char			user[AGWPE_AUTH_NAME_MAX];
	char			pass[AGWPE_AUTH_PASS_MAX];
};

struct agwpe_config {
	struct agwpe_upstream	*upstreams;
	int			count;

	/* Loop port client authentication mode: one of the AGWPE_AUTH_*
	 * constants.  In AGWPE_AUTH_EXTERN (the default) and
	 * AGWPE_AUTH_ALWAYS mode clients connecting from a non-loopback
	 * address must log in with credentials from agwpe_shadow.conf
	 * before they may issue any other frame.  */
	int			auth;

	/* Automatic digipeater resolution ("autoroute yes|no" in
	 * agwpe.conf): when set (the default) the daemon asks the ax25rtd
	 * route cache for a learned path on the target port before it
	 * sends a connect that names no digipeaters.  A connect with an
	 * explicit digipeater path ('v') is never touched.  */
	int			autoroute;

	/* Loop port client credentials, from agwpe_shadow.conf.  */
	struct agwpe_auth	*clients;
	int			nclients;

	/* Loop port unix domain socket.  An empty path disables it; the
	 * AGWPE protocol itself is unchanged, only the transport differs.
	 * This is not configured in agwpe.conf: the daemon fills it from
	 * the shared ax25common.conf ("loop socket"), overridable on the
	 * command line.  */
	char			socket_path[108];

	/* Whether the TCP loop listener is enabled at all.  With "tcp
	 * no" the unix socket is the only way in.  From ax25common.conf
	 * ("loop tcp"), overridable on the command line.  */
	int			tcp_enabled;

	/* Ownership of the unix socket: one of the AGWPE_GROUP_*
	 * constants; group_name names the group for AGWPE_GROUP_NAMED (a
	 * name or a numeric gid).  From ax25common.conf ("loop group"),
	 * overridable on the command line.  */
	int			group_mode;
	char			group_name[64];
};

/*
 * Read the upstream list from path (typically "agwpe.conf" below
 * AX25_SYSCONFDIR).  Returns 0 on success, -1 on error.  The caller
 * must call agwpe_config_free().
 */
extern int agwpe_config_load(const char *path, struct agwpe_config *cfg);

/*
 * Read credentials from path (typically "agwpe_shadow.conf").  Lines
 * are "<target> <user> <password>" where target is an upstream name
 * from agwpe.conf or AGWPE_LOOP_NAME for loop port clients.  Returns 0
 * on success (including a missing file, which simply leaves the
 * credentials empty) and -1 on parse errors.  The file is expected to
 * be mode 0600 and owned by root.
 */
extern int agwpe_config_load_shadow(const char *path, struct agwpe_config *cfg);

extern void agwpe_config_free(struct agwpe_config *cfg);

#ifdef __cplusplus
}
#endif

#endif
