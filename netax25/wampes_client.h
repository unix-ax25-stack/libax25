/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 2026 Thomas Osterried <dl9sau@darc.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */
/*
 * wampes_client.h - ask a WAMPES node what it is carrying.
 *
 * The node has a "sessions" command on its service socket, and it answers
 * with one line per forwarding it runs for a program plus one line per
 * callsign a program is holding.  This is that table, as a library, so a
 * program on this side can list the sessions of a WAMPES node beside the ones
 * ax25netd carries (agwpe_client.h) in one table.
 *
 * What a row is.  A forwarding row is
 *
 *	<id> <port> <from> <to> <proto> <state> <owner> <prog> <path>
 *
 * and a claimed-callsign row is the same shape with "-" for the id and "*"
 * for the remote: the node holds the call, no link is up, and there is
 * nothing to close.  Software should not lean on that field layout beyond
 * what it prints - it is the node's own line, and this header's business is
 * to hand it over whole.
 *
 * Which node.  wampes.conf may name more than one node, each listening on its
 * own address, so a node is named by the name in that file - the part before
 * the colon of an axports entry ("wampes:hf" is port "hf" of node "wampes").
 * wampes_node_count()/wampes_node_name() enumerate them; a machine with no
 * wampes.conf has none.  WAMPES_SOCKET and the compiled-in default reach a
 * node these functions do not know, which is what the test that points one
 * program at a mock uses.
 *
 * Errors.  Every function that talks to a node returns 0 on success and -1
 * with errno set when it could not: ECONNREFUSED for a node that is not
 * there, ESRCH for a session the node does not have.  wampes_node_count() and
 * wampes_node_name() never fail.
 */

#ifndef _NETAX25_WAMPES_CLIENT_H
#define _NETAX25_WAMPES_CLIENT_H

/* How many nodes wampes.conf names, and the name of one of them.  A node is
 * named by index the way ax25_config_get_next() names ports: ask until the
 * name comes back NULL, which is also the answer for a machine with no file.
 * The name is what wampes_sessions() and wampes_close_session() take.
 */
int wampes_node_count(void);
const char *wampes_node_name(int i);

/* Read one node's session table and hand every row to cb, in the order the
 * node sent them, without the "***" line that ends the list.  The line is the
 * node's own, including its newline-less form; cb returns nonzero to stop the
 * walk early (the table is then reported as read, since the caller chose to
 * stop).
 *
 * The same node answers for every one of its ports, so the table is asked for
 * once per node and not once per port.
 */
int wampes_sessions(const char *node,
		    int (*cb)(void *arg, const char *line), void *arg);

/* Close the forwarding a node named by handle - the id in the first field of
 * a row.  A node that has no such forwarding is ESRCH, and a claim ("-") is
 * not a forwarding and is refused the same way.
 */
int wampes_close_session(const char *node, const char *handle);

/*
 * Connect to a WAMPES node service socket.
 * If port is NULL, connects to the default node.
 * Returns socket fd on success, -1 on error with errno set.
 */
int wampes_dial(const char *addr);

/* Resolve the service socket address for a WAMPES port.
 * Returns the socket path (static buffer), or NULL on error.
 */
const char *wampes_address(const char *port);

/* Resolve the command channel (netcmd) address for a WAMPES port.
 * Returns the socket path (static buffer), or NULL on error.
 * This is the command channel (netcmd) socket, not the service socket (axsock).
 * Used for commands like "param" that are only available on the command channel.
 */
const char *wampes_cmd_address(const char *port);

/* Get the interface name for a WAMPES port.
 * Returns the interface name (e.g. "hf1") or NULL if not found.
 */
const char *wampes_iface(const char *port);

/* Close a WAMPES monitor or raw socket.
 * Returns 0 on success, -1 on error.
 */
int wampes_close(int fd);

#endif
