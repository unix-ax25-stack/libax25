/* LIBAX25 - Library for AX.25 programs
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */
#ifndef AGWPE_SOCK_H
#define AGWPE_SOCK_H

/*
 * The AGWPE backend, as the interception layer sees it.
 *
 * Every one of these answers the same question: is this descriptor mine?  It
 * returns 0 for one that is not, and the entry point falls through to the
 * real call; 1 for one that is, with the answer in *ret.  socket() is the
 * exception on both counts - there is no descriptor yet, so it builds rather
 * than answers - and agwpe_socket_packet() is its monitor half.
 *
 * That is the whole surface.  The socket table, the lock, the AGWPE client
 * and the threads around them stay behind it, and nothing here hands out a
 * pointer to any of them.  That is what makes this a boundary rather than
 * just a second file: a mutex in a header would be neither.
 */

#include <sys/socket.h>

/* The AGWPE server these backends feed or read.  AXSOCK_HOST and AXSOCK_PORT
 * name it: a leading slash in the host is a unix socket path, anything else
 * is a TCP host with that port.  Both backends - agwpe_sock.c talking to it,
 * and the wampes.c monitor mirror pushing frames into it - resolve it the same
 * way, so the answer lives here once.
 *
 * There is no built-in endpoint.  Where nothing names one, the question is
 * answered by not answering it: a library that fell back on 127.0.0.1 said
 * "no ax25netd at 127.0.0.1" on a machine whose ax25netd listens on a unix
 * socket and is right there, and that was read as the diagnosis of a program
 * that was working.  A missing name is a configuration fact and is reported
 * as one; guessing it is what made it invisible.
 *
 * The port belongs to the host that is named: AXSOCK_PORT for a TCP host out
 * of the environment, loop_tcp_port from ax25common.conf for a TCP loop
 * listener that file enables.  A path carries no port, so none is asked for.
 */

/* The address a TCP loop listener out of ax25common.conf is reached on.  It
 * has no address of its own to be reached on and ax25netd binds it there, so
 * this is where the file's "loop tcp" sends a client - not a guess, and only
 * reached when the file has asked for a listener. */
#define	AXSOCK_LOOPBACK_HOST	"127.0.0.1"

int agwpe_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int *ret);
int agwpe_bind(int fd, const struct sockaddr *addr, socklen_t len, int *ret);
int agwpe_close(int fd, int *ret);
int agwpe_connect(int fd, const struct sockaddr *addr, socklen_t len, int *ret);
int agwpe_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen, int *ret);
int agwpe_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen, int *ret);
int agwpe_getsockopt(int fd, int level, void *optval, socklen_t *optlen, int *ret);
int agwpe_ioctl(int fd, unsigned long request, void *arg, int *ret);
int agwpe_listen(int fd, int *ret);
int agwpe_recv(int fd, void *buf, size_t len, ssize_t *ret);
int agwpe_recvfrom(int fd, void *buf, size_t len, struct sockaddr *addr, socklen_t *addrlen, ssize_t *ret);
int agwpe_send(int fd, const void *buf, size_t len, ssize_t *ret);
int agwpe_sendto(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t tolen, ssize_t *ret);
int agwpe_setsockopt(int fd, int level, int optname, int *ret);
int agwpe_shutdown(int fd, int how, int *ret);
int agwpe_socket(int type, int protocol);
int agwpe_socket_packet(int domain, int type, int protocol, int *ret);
int agwpe_write(int fd, const void *buf, size_t len, ssize_t *ret);

/*
 * A monitor descriptor without asking which backend this process was given.
 *
 * agwpe_socket_packet() is socket()'s answer to a program that asked for a
 * packet socket, and one descriptor is what such a program has room for.  This
 * is the same socket for a caller that means to read from both sources: a
 * kernel AX.25 stack and an ax25netd can be heard at the same time, and on a
 * host that has both, the monitor that takes only one of them is missing the
 * other's ports without saying so.  axmon_open() is the caller; it opens the
 * kernel socket next to this one.
 *
 * Always returns 1 with the descriptor in *ret, or 1 with -1 when there is
 * neither a server nor a node to refuse quietly.
 *
 * mask is AGWPE_MONMASK_*: what the program wants the raw frames to carry.
 * It is one value for the connection, which is the program's, and it is sent
 * when the first monitor socket exists and again whenever the link comes back.
 */
int agwpe_mon_open(int protocol, unsigned char mask, int *ret);

/*
 * And the way out.  A descriptor made here changes hands when bind() finds
 * the port belongs to a node: the other backend asks what kind of socket it
 * is and then takes it away.  Two calls, one direction, and the only edge
 * between the two backends themselves.
 */
int agwpe_socktype(int fd);
int agwpe_forget(int fd);

/* The one resolution of the endpoint both backends use, so the monitor
 * mirror in wampes.c reaches the same ax25netd the client here does.  Without
 * it the mirror keeps its own idea of the address and never finds a node that
 * serves a loop socket.  *port is ignored when the returned host is a path.
 * NULL where nothing names a server. */
const char *axsock_server_endpoint(int *port);

/* The same endpoint, for a message: never NULL and never an empty string. */
const char *axsock_server_name(void);

/* Named agwpe_* since the split, because that is what they are: this
 * backend letting go.  They used to be axsock_*, from when everything lived
 * in one file and the prefix said nothing. */

#endif /* AGWPE_SOCK_H */
