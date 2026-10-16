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
 * WAMPES backend for the AF_AX25 shim - internal interface to axsock.c.
 *
 * The three that answer "was this mine, and what should the caller return"
 * take a *ret and give 1 when they handled it, 0 when the descriptor belongs
 * to somebody else.  That keeps the hooks in axsock.c to one line each.
 */

#ifndef _WAMPES_H
#define _WAMPES_H

#include <sys/socket.h>

int wampes_configured(void);
int wampes_enabled(void);

int wampes_socket(int type);
void wampes_note_protocol(int fd, int protocol);
int wampes_bind(int fd, const struct sockaddr *addr, socklen_t len, int *ret);
int wampes_connect(int fd, const struct sockaddr *addr, socklen_t len,
		   int *ret);
int wampes_listen(int fd, int *ret);
int wampes_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int *ret);
int wampes_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen,
		       int *ret);
int wampes_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen,
		       int *ret);
int wampes_setsockopt(int fd, int level, int optname, int *ret);
int wampes_ioctl(int fd, unsigned long request, void *arg, int *ret);

ssize_t wampes_sendto(int fd, const void *buf, size_t len, int flags,
		      const struct sockaddr *addr, socklen_t alen,
		      ssize_t *ret);
ssize_t wampes_recvfrom(int fd, void *buf, size_t len, int flags,
			struct sockaddr *addr, socklen_t *alen,
			ssize_t *ret);
int wampes_close(int fd);

/*
 * The raw monitor of a node, for axmon.c.  Not the AF_AX25 shim: this opens a
 * connection of its own to the node's monitor socket and reads the node's own
 * records - see axmon.c and WAMPES' remote_net.c for the framing.  The
 * descriptor is a plain libc socket, so it is closed with close() and not
 * through this header.
 *
 * protocol is in network byte order, as socket() wants it: ETH_P_AX25 for what
 * arrives on the node's ports, ETH_P_ALL to add what the node sends.
 *
 * port names an axports entry of a WAMPES node ("wampes:hf"), which also says
 * which interface to watch; NULL or "" means the node WAMPES_SOCKET names and
 * every interface of it.  A port that belongs to no node here, or a node whose
 * address is not a unix socket, is not a monitor to open.
 *
 * Returns 1 with the descriptor in *fdp, 0 when this is not a WAMPES monitor,
 * -1 with errno set when it is one and could not be opened.
 */
int wampes_mon_open(int protocol, const char *port, int *fdp);

/*
 * One whole record off a monitor descriptor, taken apart: the node writes
 *
 *	[4 byte big-endian frame length][interface name][NUL][frame]
 *
 * and the caller is handed one payload in the shape a kernel packet socket
 * gives - [KISS data byte][frame] - with the interface name in sa_data as
 * "<node>:<interface>".  Returns the payload length, 0 at end of the stream
 * with errno cleared, or -1 with errno set (E2BIG if the frame does not fit
 * buflen, ECONNRESET if the node went away in the middle of one).
 */
ssize_t wampes_mon_read(int fd, void *buf, size_t buflen,
			struct sockaddr *sa, socklen_t *asize);

/* Forget a descriptor the monitor layer closed, so its node name cannot be
 * read back for a later socket that happens to land on the same number. */
int wampes_mon_close(int fd);

#endif
