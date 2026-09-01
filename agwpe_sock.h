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
 * And the way out.  A descriptor made here changes hands when bind() finds
 * the port belongs to a node: the other backend asks what kind of socket it
 * is and then takes it away.  Two calls, one direction, and the only edge
 * between the two backends themselves.
 */
int agwpe_socktype(int fd);
int agwpe_forget(int fd);

/* Named agwpe_* since the split, because that is what they are: this
 * backend letting go.  They used to be axsock_*, from when everything lived
 * in one file and the prefix said nothing. */

#endif /* AGWPE_SOCK_H */
