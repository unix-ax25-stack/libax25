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

int wampes_enabled(void);
int axsock_forget(int fd);   /* in axsock.c */
int axsock_socktype(int fd); /* in axsock.c */
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
int wampes_setsockopt(int fd, int level, int *ret);
ssize_t wampes_sendto(int fd, const void *buf, size_t len, int flags,
		      const struct sockaddr *addr, socklen_t alen,
		      ssize_t *ret);
int wampes_close(int fd);

#endif
