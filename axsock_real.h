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
#ifndef AXSOCK_REAL_H
#define AXSOCK_REAL_H

/*
 * What the interception layer and the AGWPE backend share, and all of it:
 * how an entry point is named, and how to reach the call it stands in front
 * of.  Everything else crosses in one direction only, through agwpe_sock.h.
 */

#include <sys/socket.h>
#include <sys/types.h>

// axsock.c during axsock_real_init sets axsock_debug = getenv("AXSOCK_DEBUG") ? 1 : 0;
extern int axsock_debug;

/*
 * How the calls below get in front of the application, and why macOS needs
 * a second answer.
 *
 * ELF has a flat namespace: defining socket(), bind() and the rest as
 * ordinary strong symbols is enough, whether the library is linked in or
 * only preloaded, and the real call is fetched once with dlsym(RTLD_NEXT).
 *
 * Mach-O binds two-level, so a strong symbol is consulted only by whoever
 * linked against this library on purpose; an inserted library is loaded and
 * never asked.  DYLD_FORCE_FLAT_NAMESPACE was the answer to that and is not
 * honoured any more (measured on macOS 15.7.9: the library is loaded and the
 * call still goes to libSystem, with the variable and without it).  What dyld
 * does still honour is interposing - a table in __DATA,__interpose naming
 * pairs of functions, applied to every image in the process.
 *
 * Interposing brings one rule with it that decides the shape of everything
 * below: dyld leaves the bindings of the image that *provides* the
 * interposition alone, and rewrites everyone else's - including what dlsym
 * hands back, on any handle.  So the entry points must not be called socket()
 * here, or the table could not name libSystem's, and the fall-through must be
 * a plain call rather than a dlsym pointer, or the library would answer
 * itself.  Both were measured before they were written down; a dlsym
 * fall-through recurses until the stack is gone.
 *
 * The cost is that a libax25 linked statically into a program is not
 * intercepted on macOS: an interpose section in the main executable is
 * ignored, which was measured too.  Nothing in this suite links it statically.
 */
#ifdef __APPLE__
#define AXSOCK_ENTRY(name)	axsock_ep_##name
/*
 * A call from inside this library to recvfrom() does not reach the shim:
 * dyld leaves the bindings of the image that provides the interpose table
 * alone, so netax25/axmon.c - which is in that image - would read the
 * framed stream itself and hand the monitor four bytes of length in front
 * of every frame.  Naming the entry point is what puts the shim back in the
 * path, and it is why axmon_read() can leave the header to it.  Elsewhere
 * the entry point is recvfrom() itself, and the strong symbol is already
 * the shim, which is why the same line works on both.
 */
extern ssize_t	axsock_ep_recvfrom(int, void *, size_t, int,
				     struct sockaddr *, socklen_t *);
#define real_socket		socket
#define real_bind		bind
#define real_connect		connect
#define real_send		send
#define real_sendto		sendto
#define real_recv		recv
#define real_recvfrom		recvfrom
#define real_write		write
#define real_read		read
#define real_close		close
#define real_shutdown		shutdown
#define real_setsockopt		setsockopt
#define real_getsockopt		getsockopt
#define real_ioctl		ioctl
#define real_getpeername	getpeername
#define real_getsockname	getsockname
#define real_listen		listen
#define real_accept		accept
#else
#define AXSOCK_ENTRY(name)	name
extern int	(*real_socket)(int, int, int);
extern int	(*real_close)(int);
#endif

/*
 * Resolve the real calls, so a caller inside this library may use them.
 *
 * Everywhere else they are reached through an interposed entry point that
 * resolves them on the way past, and a caller there cannot tell whether it is
 * the first one in the process.  axmon.c can: it opens the kernel's own
 * packet socket with real_socket() precisely because socket() would ask the
 * shim instead, which makes it the first thing here to touch one of these -
 * and on Linux they are NULL until they are resolved.  Calling one before
 * asking is a call through an empty pointer, which is a crash with nothing on
 * stderr to say what it was about.
 */
extern void	axsock_real_ready(void);
#define AXSOCK_NEED_REAL()	axsock_real_ready()

#ifndef __APPLE__
extern int	(*real_bind)(int, const struct sockaddr *, socklen_t);
extern int	(*real_connect)(int, const struct sockaddr *, socklen_t);
extern ssize_t	(*real_send)(int, const void *, size_t, int);
extern ssize_t	(*real_sendto)(int, const void *, size_t, int,
			       const struct sockaddr *, socklen_t);
extern ssize_t	(*real_recv)(int, void *, size_t, int);
extern ssize_t	(*real_recvfrom)(int, void *, size_t, int,
				 struct sockaddr *, socklen_t *);
extern ssize_t	(*real_write)(int, const void *, size_t);
extern ssize_t	(*real_read)(int, void *, size_t);
extern int	(*real_shutdown)(int, int);
extern int	(*real_setsockopt)(int, int, int, const void *, socklen_t);
extern int	(*real_getsockopt)(int, int, int, void *, socklen_t *);
extern int	(*real_ioctl)(int, unsigned long, void *);
extern int	(*real_getpeername)(int, struct sockaddr *, socklen_t *);
extern int	(*real_getsockname)(int, struct sockaddr *, socklen_t *);
extern int	(*real_listen)(int, int);
extern int	(*real_accept)(int, struct sockaddr *, socklen_t *);
#endif

/*
 * Kernel or AGWPE, decided once per process and cached.  It belongs to the
 * chooser rather than to either backend, and the monitor socket asks it too.
 */
int axsock_backend_now(void);

/*
 * What stands behind a descriptor number, changed without changing the
 * number.  The number belongs to the interception layer - it handed it out -
 * so these live there and both backends use them.  A backend says "let go"
 * through its own interface; putting the new thing in place is not its
 * business to know about.
 */
int axsock_replace(int fd, int newfd);
int axsock_placeholder(int fd);

/*
 * SOCK_DGRAM or SOCK_SEQPACKET behind a descriptor, asked of the descriptor.
 * A backend taking a descriptor over from somebody else needs this before the
 * handover, not after: the table that answers for descriptors of ours is gone
 * by then, and the descriptor behind the number is not ours any more.
 */
int axsock_fdtype(int fd);

/*
 * Which SOL_AX25 options may be accepted and ignored, and which must be
 * refused because ignoring them would corrupt traffic rather than cost a
 * feature.  Both backends ask, and the one time they answered separately they
 * answered differently.
 */
int axsock_opt_refuse(int optname);
void axsock_opt_note_ignored(int optname);

#endif /* AXSOCK_REAL_H */
