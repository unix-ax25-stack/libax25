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
 * Userspace AF_AX25 sockets over the AGWPE protocol.
 *
 * Platforms without a native kernel AX.25 stack (BSD, SysV, macOS, ...)
 * have no AF_AX25 address family.  This module provides the socket API
 * the AX.25 tools use on top of the AGWPE client protocol.  Each AF_AX25
 * socket() is backed by a pipe: the file descriptor handed to the
 * application is the read end, so read(), select(), poll() and fcntl()
 * work unchanged.  bind(), connect(), send() and close() are translated
 * into AGWPE register/connect/data/disconnect frames carried over a
 * single TCP connection to the configured AGWPE server.
 *
 * The module exports the socket API from libax25 itself.  Because the
 * AX.25 tools link libax25 before libSystem, their socket()/bind()/
 * connect()/... references bind to these implementations on macOS/BSD.
 * On Linux, where the kernel provides AF_AX25, this module is not built
 * and the tools bind to libSystem exactly as before.
 */
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <net/if.h>
#include <netdb.h>

#include "netax25/ax25.h"
#include "wampes.h"
#include "netax25/axlib.h"
#include "netax25/axconfig.h"
#include "netax25/agwpe.h"
#include "netax25/agwpe_client.h"
#include "netax25/axmon.h"
#include "axsock_real.h"
#include "agwpe_sock.h"

#define	AXSOCK_DEFAULT_HOST	"127.0.0.1"
#define	AXSOCK_DEFAULT_PORT	8100
#define	AXSOCK_MAX_SOCK		128
#define	AXSOCK_CONNECT_TIMEOUT	60

int axsock_debug = 0;

/*
 * Backend selection.
 *
 * On a platform with a native kernel AX.25 stack (Linux, HAVE_KERNEL_AX25)
 * AF_AX25 and AF_PACKET/SOCK_PACKET sockets are normally passed straight
 * through to the kernel (real_socket) and this shim stays out of the way.
 * Where libax25 was also built to serve them from userspace
 * (--enable-userspace-ax25) the AXSOCK_BACKEND environment variable forces
 * one or the other per process:
 *
 *   AXSOCK_BACKEND=kernel   all AX.25 goes to the kernel stack
 *   AXSOCK_BACKEND=agwpe    all AX.25 goes to AGWPE via the shim
 *   AXSOCK_BACKEND=wampes   all AX.25 goes to a WAMPES node (see wampes.c)
 *
 * Without the variable the backend is decided once per process (cached
 * for the lifetime of the program, so a long running service probes at
 * most once): on Linux a single real_socket(AF_AX25, ...) probe checks
 * whether the kernel provides AF_AX25 at all and selects 'kernel' if it
 * does, 'agwpe' if not (EAFNOSUPPORT and friends).  On platforms without
 * a native stack there is nothing to probe - AF_AX25 as defined by the bundled
 * headers may even collide with a real address family there - so the
 * backend is always 'agwpe'.  'kernel' is only meaningful where
 * HAVE_KERNEL_AX25 is defined.
 *
 * This is a stop-gap.  Because socket() must return an fd before the
 * callsign (and therefore the port) is known at bind()/connect(), the
 * backend cannot be derived per port here.  The eventual replacement is
 * socket caching: socket() hands back a placeholder fd, bind() resolves
 * the callsign against axports and the port's device name (the 'agwpe-'
 * prefix marks an AGWPE port) to a backend and dup2()s the real (kernel
 * or AGWPE socketpair) fd over the placeholder, keeping the visible fd
 * number stable.  That removes the need for AXSOCK_BACKEND entirely and
 * lets kernel and AGWPE ports coexist in one process.
 */static int	axsock_backend = -1;	/* -1 undecided, 0 = agwpe, 1 = kernel */

/*
 * The descriptor number belongs to whoever handed it out, which is this file:
 * socket() returned it and the application knows nothing else about it.  So
 * the two operations that change what stands behind it live here, and not in
 * either backend.
 *
 * They were in wampes.c, because a node was the only thing that ever took a
 * descriptor over - AGWPE only ever built one, at socket() time.  That meant
 * one backend knew how to reach inside the other, which is the wrong way
 * round for something neither of them owns.  With the primitives here, a
 * backend says "let go" and "put this behind the number" and knows nothing
 * about who held it before.
 */

/*
 * Put newfd behind fd, keeping the number the application holds.  newfd is
 * consumed either way; on failure fd is untouched and errno is the dup2()
 * one.
 */
int axsock_replace(int fd, int newfd)
{
	int save;

	if (dup2(newfd, fd) < 0) {
		save = errno;
		close(newfd);
		errno = save;
		return -1;
	}
	close(newfd);
	return 0;
}

/*
 * Leave something inert behind fd: an unbound unix socket, which answers
 * nothing and is readable by nobody.  For the moment between taking a
 * descriptor away from one backend and giving it to another, where the number
 * must stay valid because the application still holds it.
 */
int axsock_placeholder(int fd)
{
	int ph;

	if ((ph = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	return axsock_replace(fd, ph);
}

int axsock_backend_now(void)
{
	const char *b;

	if (axsock_backend >= 0)
		return axsock_backend;

	b = getenv("AXSOCK_BACKEND");
	if (b != NULL) {
		if (strcmp(b, "kernel") == 0)
			axsock_backend = 1;
		else if (strcmp(b, "agwpe") == 0)
			axsock_backend = 0;
		else if (strcmp(b, "wampes") == 0)
			/* Not one of this function's answers - wampes.c reads
			 * the same variable and takes the call before anybody
			 * asks here.  Named all the same, so that the one
			 * value both of them understand does not get reported
			 * as a typo on every run.
			 */
			axsock_backend = 0;
		else
			fprintf(stderr, "axsock: unknown AXSOCK_BACKEND \"%s\" "
				"(kernel|agwpe|wampes), using default\n", b);
	}

	if (axsock_backend < 0) {
#ifdef HAVE_KERNEL_AX25
		/* Once per process: does the kernel answer AF_AX25 at all
		 * (module loaded)?  If it does not, there is no native
		 * stack to fall back to and AGWPE it is.  Linux says
		 * EAFNOSUPPORT for an address family nobody registered,
		 * which is the everyday case here - no ax25 module - and
		 * the one this used to get wrong; EPROTONOSUPPORT and
		 * EPFNOSUPPORT are the other ways a system says the same
		 * thing.  Any other failure (e.g. EPERM) keeps the kernel
		 * selected and the caller's real socket() surfaces the
		 * error unchanged. */
		int fd = real_socket(AF_AX25, SOCK_SEQPACKET, 0);

		if (fd >= 0) {
			real_close(fd);
			axsock_backend = 1;
		} else if (errno == EAFNOSUPPORT ||
			   errno == EPROTONOSUPPORT ||
			   errno == EPFNOSUPPORT) {
			axsock_backend = 0;
		} else {
			axsock_backend = 1;
		}
#else
		axsock_backend = 0;
#endif
	}
	return axsock_backend;
}

#ifndef __APPLE__
/*
 * The real calls, fetched once.  Declared in axsock_real.h, defined here:
 * this is the file that stands in front of them.
 */
int	(*real_socket)(int, int, int);
int	(*real_close)(int);
int	(*real_bind)(int, const struct sockaddr *, socklen_t);
int	(*real_connect)(int, const struct sockaddr *, socklen_t);
ssize_t	(*real_send)(int, const void *, size_t, int);
ssize_t	(*real_sendto)(int, const void *, size_t, int,
			       const struct sockaddr *, socklen_t);
ssize_t	(*real_recv)(int, void *, size_t, int);
ssize_t	(*real_recvfrom)(int, void *, size_t, int,
				 struct sockaddr *, socklen_t *);
ssize_t	(*real_write)(int, const void *, size_t);
ssize_t	(*real_read)(int, void *, size_t);
int	(*real_shutdown)(int, int);
int	(*real_setsockopt)(int, int, int, const void *, socklen_t);
int	(*real_getsockopt)(int, int, int, void *, socklen_t *);
int	(*real_ioctl)(int, unsigned long, void *);
int	(*real_getpeername)(int, struct sockaddr *, socklen_t *);
int	(*real_getsockname)(int, struct sockaddr *, socklen_t *);
int	(*real_listen)(int, int);
int	(*real_accept)(int, struct sockaddr *, socklen_t *);
#endif

/*
 * The real calls, resolved lazily instead of in a constructor.  A library
 * constructor runs in the dynamic linker's initfini order, which is driven
 * by the dependency graph: a constructor of a deeper dependency (e.g.
 * libcap-ng behind libpam/libaudit) can run before this one and call one
 * of the interposed syscalls (close(2), ...) while the real_* pointers are
 * still NULL, crashing at the fall-through.  Resolving on first use makes
 * the order irrelevant.
 *
 * Resolved exactly once, deadlock-free: a plain "if (real_socket == NULL)"
 * gate lets two threads walk into the init together, and one of them can
 * then see real_socket set while real_close is still NULL and fall through
 * on an empty pointer.  pthread_once() blocks the second thread until the
 * first is done, which is the point of the exercise.
 */
static pthread_once_t axsock_real_once = PTHREAD_ONCE_INIT;

static void axsock_real_init(void)
{
	axsock_debug = getenv("AXSOCK_DEBUG") ? 1 : 0;

#ifndef __APPLE__
	/* Not on macOS: there the real calls are reached by calling them, and
	 * dlsym() would hand back this library's own - see the note at the top
	 * about what interposing rewrites. */
	real_socket = dlsym(RTLD_NEXT, "socket");
	real_bind = dlsym(RTLD_NEXT, "bind");
	real_connect = dlsym(RTLD_NEXT, "connect");
	real_send = dlsym(RTLD_NEXT, "send");
	real_sendto = dlsym(RTLD_NEXT, "sendto");
	real_recv = dlsym(RTLD_NEXT, "recv");
	real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
	real_write = dlsym(RTLD_NEXT, "write");
	real_read = dlsym(RTLD_NEXT, "read");
	real_close = dlsym(RTLD_NEXT, "close");
	real_shutdown = dlsym(RTLD_NEXT, "shutdown");
	real_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
	real_getsockopt = dlsym(RTLD_NEXT, "getsockopt");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	real_getpeername = dlsym(RTLD_NEXT, "getpeername");
	real_getsockname = dlsym(RTLD_NEXT, "getsockname");
	real_listen = dlsym(RTLD_NEXT, "listen");
	real_accept = dlsym(RTLD_NEXT, "accept");
#endif
}

#define AXSOCK_NEED_REAL()				\
	do {						\
		pthread_once(&axsock_real_once,		\
			     axsock_real_init);		\
	} while (0)

int AXSOCK_ENTRY(socket)(int domain, int type, int protocol)
{
	int fd;

	AXSOCK_NEED_REAL();
	{
		int ret;

		if (agwpe_socket_packet(domain, type, protocol, &ret))
			return ret;
	}

	if (domain != AF_AX25)
		return real_socket(domain, type, protocol);

	/* One chooser, not two.  The protocol id was said here and nowhere
	 * else - bind() is too late to ask - so it is noted afterwards, on
	 * whichever descriptor the chooser handed back, instead of the
	 * choice being written out a second time for protocol != 0.
	 */
	if (wampes_enabled())
		fd = wampes_socket(type);
	else if (axsock_backend_now() == 1)
		/* Kernel backend: hand AF_AX25 to the kernel stack unchanged.
		 * The resulting fd is not tracked in the axsock table, so
		 * every other interceptor falls through to its real_*
		 * counterpart. */
		fd = real_socket(domain, type, protocol);
	else
		fd = agwpe_socket(type, protocol);
	if (fd >= 0 && protocol != 0)
		wampes_note_protocol(fd, protocol);
	return fd;
}

int AXSOCK_ENTRY(bind)(int fd, const struct sockaddr *addr, socklen_t len)
{
	int ret;

	AXSOCK_NEED_REAL();
	/* bind() is the first moment the port is known, and therefore the
	 * first moment the backend can be chosen per port rather than per
	 * process.  A socket made in socket() is handed over here if the port
	 * turns out to belong to a WAMPES node - which is why WAMPES is asked
	 * before the AGWPE table, not after it.
	 */
	if (wampes_bind(fd, addr, len, &ret))
		return ret;
	if (agwpe_bind(fd, addr, len, &ret))
		return ret;
	return real_bind(fd, addr, len);
}

int AXSOCK_ENTRY(connect)(int fd, const struct sockaddr *addr, socklen_t len)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (wampes_connect(fd, addr, len, &ret))
		return ret;
	if (agwpe_connect(fd, addr, len, &ret))
		return ret;
	return real_connect(fd, addr, len);
}

ssize_t AXSOCK_ENTRY(send)(int fd, const void *buf, size_t len, int flags)
{
	ssize_t ret;

	AXSOCK_NEED_REAL();
	if (agwpe_send(fd, buf, len, &ret))
		return ret;
	return real_send(fd, buf, len, flags);
}

ssize_t AXSOCK_ENTRY(sendto)(int fd, const void *buf, size_t len, int flags,
	       const struct sockaddr *to, socklen_t tolen)
{
	ssize_t ret;

	AXSOCK_NEED_REAL();
	if (wampes_sendto(fd, buf, len, flags, to, tolen, &ret))
		return ret;
	if (agwpe_sendto(fd, buf, len, to, tolen, &ret))
		return ret;
	return real_sendto(fd, buf, len, flags, to, tolen);
}

ssize_t AXSOCK_ENTRY(write)(int fd, const void *buf, size_t len)
{
	ssize_t ret;

	AXSOCK_NEED_REAL();
	if (agwpe_write(fd, buf, len, &ret))
		return ret;
	return real_write(fd, buf, len);
}

ssize_t AXSOCK_ENTRY(recv)(int fd, void *buf, size_t len, int flags)
{
	ssize_t ret;

	AXSOCK_NEED_REAL();
	if (agwpe_recv(fd, buf, len, &ret))
		return ret;
	return real_recv(fd, buf, len, flags);
}

ssize_t AXSOCK_ENTRY(recvfrom)(int fd, void *buf, size_t len, int flags,
		 struct sockaddr *addr, socklen_t *addrlen)
{
	ssize_t ret;

	AXSOCK_NEED_REAL();
	if (wampes_recvfrom(fd, buf, len, flags, addr, addrlen, &ret))
		return ret;
	if (agwpe_recvfrom(fd, buf, len, addr, addrlen, &ret))
		return ret;
	return real_recvfrom(fd, buf, len, flags, addr, addrlen);
}

int AXSOCK_ENTRY(shutdown)(int fd, int how)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (agwpe_shutdown(fd, how, &ret))
		return ret;
	return real_shutdown(fd, how);
}

int AXSOCK_ENTRY(close)(int fd)
{
	int ret;

	AXSOCK_NEED_REAL();

	/* A WAMPES placeholder that never reached connect() - after connect()
	 * there is nothing of ours left to forget.  It answers nothing, so it
	 * is not asked in the same breath as the others: whatever it held is
	 * released and the descriptor is still closed below. */
	wampes_close(fd);

	if (agwpe_close(fd, &ret))
		return ret;
	return real_close(fd);
}

int AXSOCK_ENTRY(listen)(int fd, int backlog)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (wampes_listen(fd, &ret))
		return ret;
	if (agwpe_listen(fd, &ret))
		return ret;
	return real_listen(fd, backlog);
}

int AXSOCK_ENTRY(accept)(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (axsock_debug)
		fprintf(stderr, "axsock: accept(fd=%d) called\n", fd);

	if (wampes_accept(fd, addr, addrlen, &ret))
		return ret;
	if (agwpe_accept(fd, addr, addrlen, &ret))
		return ret;
	return real_accept(fd, addr, addrlen);
}

/*
 * The option table, and it belongs here rather than to either backend:
 * both ask the same question about the same option, and the one time they
 * answered it separately they answered it differently.
 */
/* SOL_AX25 options already warned about (setsockopt: once per process
 * and option, see axsock_setsockopt).  Guarded by axsock_lock.  */
static int			axsock_warned[16];
static int			axsock_nwarned;
static pthread_mutex_t	axsock_opt_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *axsock_opt_name(int optname)
{
	switch (optname) {
	case AX25_WINDOW:	return "AX25_WINDOW";
	case AX25_T1:		return "AX25_T1";
	case AX25_T2:		return "AX25_T2";
	case AX25_T3:		return "AX25_T3";
	case AX25_N2:		return "AX25_N2";
	case AX25_BACKOFF:	return "AX25_BACKOFF";
	case AX25_EXTSEQ:	return "AX25_EXTSEQ";
	case AX25_PIDINCL:	return "AX25_PIDINCL";
	case AX25_IDLE:		return "AX25_IDLE";
	case AX25_PACLEN:	return "AX25_PACLEN";
	case AX25_IPMAXQUEUE:	return "AX25_IPMAXQUEUE";
	case AX25_IAMDIGI:	return "AX25_IAMDIGI";
	case AX25_KILL:		return "AX25_KILL";
	default:		return NULL;
	}
}

/*
 * Two kinds of thing live under SOL_AX25, and they cannot be answered alike.
 *
 * Most of them are channel parameters - window, the timers, paclen - and the
 * far side owns those: a node or a direwolf takes them from its own interface
 * configuration and would ignore ours.  Saying "done" and meaning "not here"
 * costs nothing, because nothing depends on the value having arrived.
 *
 * Two of them change what the bytes mean.  AX25_PIDINCL puts the protocol id
 * in front of every frame's payload, in both directions, and AX25_IAMDIGI
 * makes the socket repeat what it hears.  Accepting those and doing nothing
 * does not cost a feature, it corrupts traffic: rsuplnk(8) and rsdwnlnk(8)
 * would send the pid as data and read data as the pid, silently and forever.
 * Neither is implementable here today - a handed-over session is a plain byte
 * stream with no room for a per-frame id - so they are refused, and both
 * programs check the return value and stop, which is the outcome to want.
 */

int axsock_opt_refuse(int optname)
{
	return optname == AX25_PIDINCL || optname == AX25_IAMDIGI;
}

/* Once per process and option, so a program that sets one every session does
 * not fill the log with it. */
void axsock_opt_note_ignored(int optname)
{
	const char *name;
	char num[16];
	int i, warn;

	pthread_mutex_lock(&axsock_opt_lock);
	for (i = 0; i < axsock_nwarned; i++)
		if (axsock_warned[i] == optname)
			break;
	warn = (i == axsock_nwarned);
	if (warn && axsock_nwarned <
		    (int)(sizeof(axsock_warned) / sizeof(axsock_warned[0])))
		axsock_warned[axsock_nwarned++] = optname;
	pthread_mutex_unlock(&axsock_opt_lock);

	if (!warn)
		return;
	if ((name = axsock_opt_name(optname)) == NULL) {
		snprintf(num, sizeof(num), "%d", optname);
		name = num;
	}
	if (axsock_debug)
		fprintf(stderr, "axsock: setsockopt(SOL_AX25, %s) is ignored: the "
			"channel parameters belong to the far side\n", name);
}

int AXSOCK_ENTRY(setsockopt)(int fd, int level, int optname,
	       const void *optval, socklen_t optlen)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (wampes_setsockopt(fd, level, optname, &ret))
		return ret;
	if (agwpe_setsockopt(fd, level, optname, &ret))
		return ret;
	return real_setsockopt(fd, level, optname, optval, optlen);
}

int AXSOCK_ENTRY(getsockopt)(int fd, int level, int optname,
	       void *optval, socklen_t *optlen)
{
	int ret;

	AXSOCK_NEED_REAL();
	(void)optname;

	if (agwpe_getsockopt(fd, level, optval, optlen, &ret))
		return ret;
	return real_getsockopt(fd, level, optname, optval, optlen);
}

int AXSOCK_ENTRY(ioctl)(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret;

	AXSOCK_NEED_REAL();
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (agwpe_ioctl(fd, request, arg, &ret))
		return ret;
	return real_ioctl(fd, request, arg);
}

int AXSOCK_ENTRY(getsockname)(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (wampes_getsockname(fd, addr, addrlen, &ret))
		return ret;
	if (agwpe_getsockname(fd, addr, addrlen, &ret))
		return ret;
	return real_getsockname(fd, addr, addrlen);
}

int AXSOCK_ENTRY(getpeername)(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	int ret;

	AXSOCK_NEED_REAL();
	if (wampes_getpeername(fd, addr, addrlen, &ret))
		return ret;
	if (agwpe_getpeername(fd, addr, addrlen, &ret))
		return ret;
	return real_getpeername(fd, addr, addrlen);
}

#ifdef __APPLE__
/*
 * The table dyld reads.  Each pair says "wherever anything binds to the
 * second, call the first instead" - which is how a program that was never
 * linked against this library reaches it, and how one that was keeps
 * reaching it now that the entry points no longer carry the libc names.
 */
#define AXSOCK_INTERPOSE(name)						\
	__attribute__((used)) static const struct {			\
		const void *replacement;				\
		const void *replacee;					\
	} axsock_interpose_##name					\
	__attribute__((section("__DATA,__interpose"))) = {		\
		(const void *)(unsigned long)&AXSOCK_ENTRY(name),	\
		(const void *)(unsigned long)&name			\
	}

AXSOCK_INTERPOSE(socket);
AXSOCK_INTERPOSE(bind);
AXSOCK_INTERPOSE(connect);
AXSOCK_INTERPOSE(listen);
AXSOCK_INTERPOSE(accept);
AXSOCK_INTERPOSE(send);
AXSOCK_INTERPOSE(sendto);
AXSOCK_INTERPOSE(recv);
AXSOCK_INTERPOSE(recvfrom);
AXSOCK_INTERPOSE(write);
AXSOCK_INTERPOSE(shutdown);
AXSOCK_INTERPOSE(close);
AXSOCK_INTERPOSE(setsockopt);
AXSOCK_INTERPOSE(getsockopt);
AXSOCK_INTERPOSE(ioctl);
AXSOCK_INTERPOSE(getsockname);
AXSOCK_INTERPOSE(getpeername);
#endif
