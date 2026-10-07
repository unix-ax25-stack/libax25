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
 * The AGWPE backend: AF_AX25 sockets served from userspace over an AGWPE
 * server, whether that is an ax25netd(8) or a direwolf.
 *
 * It was written inside axsock.c, which is the file that stands in front of
 * the libc calls, and the two grew into each other.  They are apart now, and
 * they touch in two places and no others: the eighteen agwpe_* functions in
 * agwpe_sock.h, which axsock.c calls, and the real_* pointers in
 * axsock_real.h, which this file calls.  The socket table, the lock, the
 * AGWPE client and the threads around them do not appear in either header,
 * which is the point: a boundary that exported the mutex would not be one.
 *
 * The other backend, a WAMPES node, sits in wampes.c and is asked first -
 * see the entry points in axsock.c.
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
#include "netax25/agwpe_config.h"
#include "netax25/axcommon.h"
#include "axsock_real.h"
#include "agwpe_sock.h"
#include "pathnames.h"

#define	AXSOCK_MAX_SOCK		128
#define	AXSOCK_CONNECT_TIMEOUT	60
#ifndef SIOCGSTAMP
#define	SIOCGSTAMP	0x8906
#endif

/* ax25-apps/listen opens a PF_PACKET/SOCK_PACKET socket to capture raw
 * AX.25 frames.  There is no packet socket on macOS; intercept it here
 * and back it with a monitor socket fed from the AGWPE 'K' raw frames.
 */
#ifndef PF_PACKET
#define	PF_PACKET	17
#endif
#ifndef AF_PACKET
#define	AF_PACKET	PF_PACKET
#endif
#ifndef SOCK_PACKET
#define	SOCK_PACKET	10
#endif

#ifndef SIOCGIFHWADDR
#define	SIOCGIFHWADDR	0x8927
#endif

/* The hardware address member of struct ifreq is named ifr_hwaddr on
 * Linux and ifr_addr on the BSDs and macOS, where this shim builds.  */
#if defined(__linux__)
#define	AXSOCK_IFR_HWADDR	ifr_hwaddr
#else
#define	AXSOCK_IFR_HWADDR	ifr_addr
#endif

enum axsock_state {
	AXSOCK_NEW,
	AXSOCK_CONNECTING,
	AXSOCK_CONNECTED
};

struct axsock_sock {
	int			fd;	/* app end handed to the application */
	int			peer;	/* router end: inbound data is written
					 * here, outbound data read from here */
	int			type;
	enum axsock_state	state;
	int			connect_err;
	char			local[AGWPE_MAX_CALL];
	char			remote[AGWPE_MAX_CALL];
	unsigned char		port;
	unsigned char		pid;
	int			listening;
	int			raw;	/* SOCK_PACKET monitor: receives every
					 * raw AX.25 frame from the upstreams */
	char			bound[16];	/* raw monitor: SOCK_PACKET bind
					   device name, '' = all ports */
	int			registered;	/* call registered with the server */
	unsigned char		*pend;	/* inbound bytes the peer end would
					 * not take yet, in order */
	size_t			plen;	/* how many of them are waiting */
	size_t			pcap;	/* how much pend can hold */
	int			peer_eof;	/* the session ended with the
						 * queue not yet empty: close
						 * the router end once it is */
	int			port_named;	/* bind named the port itself, in
					 * the digipeater slot - connect()
					 * must not talk it over */
	int			port_known;	/* the port number on this socket
					 * means a port.  It does not when
					 * nothing was known at bind time and
					 * the number is only the first port
					 * such a socket has always used, and
					 * a monitor must not read a name into
					 * that one.  It is the answer to the
					 * question a port number on the wire
					 * cannot answer by itself.  */
	char			portname[32];	/* the name the bind asked for,
					 * kept because a port that no
					 * upstream serves still gets frames
					 * to ax25netd, and this is the only
					 * half of the answer that can be
					 * printed afterwards: the number
					 * alone is not the name and cannot
					 * be read back as one.  32, since an
					 * axports name is not bounded by
					 * anything the protocol imposes and
					 * a name cut here would be a name
					 * that is in no file */
	int			has_peer_thread; /* accepted: a reader thread
						   * forwards outbound data and
						   * owns the teardown */
	struct axsock_sock	*pending;	/* queued inbound connections */
	struct axsock_sock	*pend_next;	/* link within the pending list */
	struct axsock_sock	*next;		/* global socket list */
};

/*
 * Recursive: the AGWPE client layer is part of this dylib and its socket
 * calls route back through these wrappers.  Those fds are never axsock
 * fds, but the lock is re-taken while an outer axsock call already holds
 * it (e.g. connect() -> axsock_ensure_locked() -> TCP connect()).
 *
 * The lock protects the socket table and the AGWPE client lifecycle.  It
 * is a data lock, never a blocking-I/O lock: every frame send takes a
 * client reference, snapshots its arguments and releases the lock before
 * touching the (blocking) AGWPE TCP socket, then re-acquires it to drop
 * the reference, and the reader thread connects and resolves names with
 * no lock at all (see axsock_link_up).  Recursion therefore only ever
 * re-enters around short critical sections, which also bounds what a signal
 * handler calling close() can be made to wait for.
 *
 * What that buys is the property a WAMPES node depends on: a host whose
 * ax25netd is down and whose node is up keeps working.  Every interposed
 * send() and close() of the node's own traffic passes through this lock on
 * its way to being recognised as not ours, so a reader thread that held it
 * across a connect - or across the backoff sleeps a missing server causes -
 * would put the node's throughput behind the availability of a program it
 * does not use.  A monitor that cannot be fed is a monitor that says so;
 * a connector that is blocked is a radio that is down.
 */
static pthread_mutex_t	axsock_lock;
static pthread_cond_t	axsock_cond = PTHREAD_COND_INITIALIZER;

static struct axsock_sock	*axsock_list;
static int			axsock_nsock;
static int			axsock_nraw;	/* open SOCK_PACKET monitors */
static int			axsock_nuisub;	/* datagram sockets off the
						   * loop port: the UI
						   * subscription is for
						   * the connection, so it
						   * has to be (re)sent
						   * while any of them is
						   * open */
static unsigned char		axsock_mon_mask = AGWPE_MONMASK_ALL;
static int			axsock_npending; /* sockets with a queue, so the
					  * reader knows to come back */


static agwpe_client_t		*axsock_agwpe;
static pthread_t		axsock_thread;
static int			axsock_up;

/* Whether axsock_reader() exists.  It owns the connection: it brings the link
 * up, pumps it, and brings it back when it goes away, so a monitor that is
 * only waiting in poll() does not have to notice anything.  The application
 * side starts it and waits for the link (axsock_ensure_locked()).
 */
static int			axsock_thread_alive;

/* Why the last attempt to bring the link up failed, for the thread that is
 * waiting to be told.  Written and read under axsock_lock and nowhere else:
 * errno belongs to whoever set it, and the thread that made the attempt is
 * asleep in its backoff when the answer is wanted. */
static int			axsock_err;

/* Seconds axsock_ensure_locked() waits for the link before giving up.  Long
 * enough that a server which is being restarted under a running program is
 * waited out rather than refused; short enough that a server which is not
 * coming back is not waited on forever. */
#define AXSOCK_ENSURE_TIMEOUT	10

/*
 * Say to a server that has just come up what the raw monitor stream is for
 * and what it may leave out.  One place, because there are three moments that
 * need it - the first monitor socket opening, a raw feed starting on a
 * registered socket, and the link coming back - and a fourth one that is only
 * remembered because somebody wrote it down once: raw monitoring is a toggle
 * on the connection, so a new connection starts with it off, and a monitor
 * whose toggle was lost with the old link stayed silent for the rest of the
 * process's life.  A mask sent from one place and not the others would fail
 * the same way and more quietly, because a lost mask only shows up as frames
 * that are longer than the program asked for.
 *
 * Nothing goes out when there is no monitor: both live on the connection, and
 * a connection with neither is one that was asked for nothing.
 */
static void axsock_mon_state_locked(void)
{
	if (axsock_agwpe == NULL || axsock_nraw == 0)
		return;
	agwpe_client_raw_toggle(axsock_agwpe);
	agwpe_client_mon_mask(axsock_agwpe, axsock_mon_mask);
}

/* Client lifetime across sends that run without axsock_lock (see
 * axsock_client_acquire): a sender snapshots axsock_agwpe and bumps the
 * reference, so ensure_locked() must not free a client still in flight.
 * It instead retires it to axsock_retired, and once every reference is
 * gone axsock_client_release() reaps them all.  Guarded by axsock_lock.
 */
static unsigned int		axsock_client_refs;
static agwpe_client_t		*axsock_retired[4];
static int			axsock_nretired;

/* Every call this process registered with the server, with a reference
 * count so that several sockets sharing one call do not unregister it
 * while one of them still needs it.  */
struct axsock_reg {
	char			call[AGWPE_MAX_CALL];
	unsigned char		port;
	int			refs;
	int			listener;	/* registered via 'L' */
};

static struct axsock_reg	axsock_registered[16];
static int			axsock_nregistered;

static const char		*axsock_host;
static int			axsock_port;
static char			axsock_hostbuf[AX25COMMON_SOCKET_MAX];

static struct axsock_sock *axsock_find_locked(int fd)
{
	struct axsock_sock *s;

	for (s = axsock_list; s != NULL; s = s->next)
		if (s->fd == fd)
			return s;
	return NULL;
}

/*
 * Fast path for the hot interposers (close, write): when no AGWPE-backed
 * socket exists at all, skip the mutex and the table walk.  axsock_nsock
 * is mutated only under axsock_lock, so this relaxed load either sees the
 * current count or a value one mutation stale; it can never tear.  The
 * only way it misleads is a thread reading a stale zero just as the very
 * first shim socket is created by another thread, and even then both
 * misroutes degrade gracefully: close() just drops the descriptor (the
 * peer reader thread still sees EOF and tears the session down) and
 * write() delivers the bytes into the socketpair, from which the peer
 * reader forwards them as the same 'D' frame the slow path would send.
 */
static inline int axsock_may_have_sock(void)
{
	return __atomic_load_n(&axsock_nsock, __ATOMIC_RELAXED) != 0;
}

/*
 * Take a reference on the AGWPE client so a frame can be sent without
 * holding axsock_lock.  Called with the lock held; returns NULL when the
 * link is down, in which case the caller reports ENOTCONN.  The caller
 * must pair this with axsock_client_release() after its send.
 */
static agwpe_client_t *axsock_client_acquire(void)
{
	if (!axsock_up || axsock_agwpe == NULL)
		return NULL;
	axsock_client_refs++;
	return axsock_agwpe;
}

/*
 * Drop a client reference taken by axsock_client_acquire.  Called with
 * the lock held.  Reaps every client retired by axsock_client_retire()
 * as soon as it is the last outstanding reference.
 */
static void axsock_client_release(void)
{
	if (axsock_client_refs > 0)
		axsock_client_refs--;
	if (axsock_client_refs == 0 && axsock_nretired > 0) {
		int i;

		for (i = 0; i < axsock_nretired; i++)
			agwpe_client_free(axsock_retired[i]);
		axsock_nretired = 0;
	}
}

/*
 * Copy a 10 byte AGWPE header field into a NUL terminated string.
 * Callsigns are normalized to upper case as AX.25 requires.
 */
static void axsock_copy_call(char *dst, const char src[AGWPE_MAX_CALL])
{
	size_t n = 0;

	while (n < AGWPE_MAX_CALL - 1 && src[n] != '\0')
		n++;
	memcpy(dst, src, n);
	dst[n] = '\0';
	for (size_t i = 0; i < n; i++)
		dst[i] = (char)toupper((unsigned char)dst[i]);
}

/*
 * True if the sockaddr carries an AX.25 address.  The struct sockaddr_ax25
 * follows the Linux layout (family in the first byte, no leading sa_len),
 * so the family is read from the ax25 struct, not from the generic
 * struct sockaddr whose family sits one byte in on BSD derived systems.
 */
static int axsock_is_ax25(const struct sockaddr *addr, socklen_t len)
{
	const struct sockaddr_ax25 *sa = (const struct sockaddr_ax25 *)addr;

	return addr != NULL && len >= sizeof(struct sockaddr_ax25) &&
	       sa->sax25_family == AF_AX25;
}

/*
 * The port table learned from the AGWPE server's 'G' (port info) reply.
 * Each entry is one flat AGWPE port with the name of the upstream that
 * owns it.  The table is cached for the life of the process: the flat
 * numbering (upstream index * 16 + channel) is designed to survive netd
 * and upstream restarts, and ax25netd_agwpe.conf is read once at startup anyway.
 */
struct axsock_gport {
	unsigned char	port;		/* flat AGWPE port byte */
	char		up[24];		/* upstream name (ax25netd_agwpe.conf) */
};

#define	AXSOCK_GPORTS	(AGWPE_PORT_LOOP + 1)	/* flat ports 0..255 */
static struct axsock_gport	axsock_gports[AXSOCK_GPORTS];
static int			axsock_gnports = -1;	/* -1 = not fetched */
static time_t			axsock_gports_asked;	/* last ask, for the
							 * retry below */
#define AXSOCK_GPORTS_RETRY	30	/* seconds before asking again */

static int axsock_read_full(int fd, void *buf, size_t len)
{
	unsigned char *p = (unsigned char *)buf;

	while (len > 0) {
		ssize_t n = real_read(fd, p, len);

		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static void axsock_ports_parse(const unsigned char *data, size_t len)
{
	char *s, *tok, *save;
	int n = 0;

	if (len == 0)
		return;
	s = strndup((const char *)data, len);
	if (s == NULL)
		return;

	for (tok = strtok_r(s, ";", &save); tok != NULL && n < AXSOCK_GPORTS;
	     tok = strtok_r(NULL, ";", &save)) {
		const char *sp, *colon;
		int pnum;

		if (strncmp(tok, "Port", 4) != 0)
			continue;	/* leading count token */
		pnum = atoi(tok + 4);
		if (pnum < 1 || pnum > AGWPE_PORT_LOOP + 1)
			continue;
		axsock_gports[n].port = (unsigned char)(pnum - 1);
		sp = strchr(tok, ' ');
		if (sp == NULL)
			continue;
		while (*sp == ' ')
			sp++;

		/* The upstream name is the part before the first ':'.  */
		colon = strchr(sp, ':');
		if (colon != NULL) {
			size_t ulen = (size_t)(colon - sp);

			if (ulen >= sizeof(axsock_gports[n].up))
				ulen = sizeof(axsock_gports[n].up) - 1;
			memcpy(axsock_gports[n].up, sp, ulen);
			axsock_gports[n].up[ulen] = '\0';
		} else {
			strncpy(axsock_gports[n].up, sp,
				sizeof(axsock_gports[n].up) - 1);
			axsock_gports[n].up[sizeof(axsock_gports[n].up) - 1] =
				'\0';
		}
		n++;
	}
	free(s);
	axsock_gnports = n;
}

/*
 * Ask the AGWPE server (netd) for its port table.  Uses a short lived
 * TCP connection of its own so that it works before the main AGWPE
 * session exists (bind() picks the port) and does not disturb it; all
 * socket calls go through the real_* pointers, so the call is safe with
 * the axsock lock held.  Returns 0 on success (table cached), -1 on
 * error (failures are retried on the next call).
 */

/*
 * Where the AGWPE server is, worked out once.
 *
 * AXSOCK_HOST decides the transport, and it wins: a leading '/' is taken
 * as given, anything else is a TCP host.  That is also how a station
 * points the shim at a radio program instead of an ax25netd.
 *
 * Without it we read ax25common.conf - the same file ax25netd reads - and
 * use whatever loop port it serves there.  That is the point of the file:
 * server and client cannot drift apart.  It also has to be so, because
 * the loop port is a unix socket by default and 127.0.0.1:8200 is not
 * what answers then.
 *
 * AXSOCK_PORT is a TCP port and is only looked at when the host turned
 * out to be a TCP host.  Setting it alone does not throw the configured
 * socket away - otherwise a port set for some other reason would
 * disconnect a socket that was there for a reason.
 *
 * A missing or unreadable ax25common.conf is not an error; what the load
 * leaves in place is the built-in default, which is the socket path
 * ax25netd uses when it is not told otherwise.  Nothing is invented here.
 */
static void axsock_resolve_server(void)
{
	static int done;
	struct ax25common com;
	const char *host, *portstr;
	int tcp_port = 0;

	if (done)
		return;
	done = 1;

	/*
	 * Nothing is resolved to a built-in address.  The loop socket of
	 * ax25common.conf carries AX25COMMON_SOCKET_DEFAULT from axcommon.c
	 * even when the file is not there at all, so a unix socket is what a
	 * host that says nothing gets - which is what ax25netd listens on
	 * unless it is told otherwise.  The host stays NULL where the file
	 * turns that socket off and no TCP loop listener replaces it: a
	 * library with no server to talk to says so once and keeps off the
	 * wire, rather than offering a 127.0.0.1 that nobody asked for.
	 */
	axsock_host = NULL;
	axsock_port = 0;

	host = getenv("AXSOCK_HOST");
	if (host != NULL && host[0] != '\0') {
		axsock_host = host;
	} else if (ax25common_config_load(ax25common_default_config(),
					   &com) == 0) {
		if (com.loop_socket[0] != '\0') {
			strncpy(axsock_hostbuf, com.loop_socket,
				sizeof(axsock_hostbuf) - 1);
			axsock_hostbuf[sizeof(axsock_hostbuf) - 1] = '\0';
			axsock_host = axsock_hostbuf;
		} else if (com.loop_tcp_enabled) {
			/* "loop socket no" together with a loop tcp listener:
			 * the server is reachable over TCP and the file says
			 * which port.  The host is the loopback because a
			 * listener of this file has no address of its own to
			 * be reached on - ax25netd binds it there. */
			axsock_host = AXSOCK_LOOPBACK_HOST;
			tcp_port = com.loop_tcp_port;
		}
	}

	if (axsock_host == NULL)
		return;
	if (axsock_host[0] == '/') {
		/* A path carries no port.  Keeping one here would be ignored
		 * everywhere it is read, and a port that a caller can set and
		 * that changes nothing is worse than one it cannot. */
		axsock_port = 0;
		return;
	}

	/* The port belongs to the host: an AXSOCK_PORT names one explicitly,
	 * and otherwise it is the one the file gave with the listener.  There
	 * is no port to fall back on, because there is no host to fall back
	 * on either - a TCP host without a port is a missing configuration,
	 * not an invitation to guess one. */
	portstr = getenv("AXSOCK_PORT");
	if (portstr != NULL && portstr[0] != '\0')
		tcp_port = atoi(portstr);
	if (tcp_port <= 0 || tcp_port > 65535)
		tcp_port = 0;
	axsock_port = tcp_port;
}

/*
 * The endpoint both backends connect to, resolved exactly once by
 * axsock_resolve_server() so the AGWPE client here and the monitor mirror in
 * wampes.c can never disagree about it.  *port is meaningless when the host
 * is a socket path, because a path carries no port.  NULL when nothing names
 * one - see AXSOCK_LOOPBACK_HOST in agwpe_sock.h for why there is no
 * built-in answer - and a caller that cannot do without one says so instead
 * of inventing it.
 */
const char *axsock_server_endpoint(int *port)
{
	axsock_resolve_server();
	if (port != NULL)
		*port = axsock_port;
	return axsock_host;
}

/*
 * The endpoint as text, for a message.  Never an empty string: a report that
 * says where the server should be and prints nothing has told the reader
 * nothing at all, which is how a working monitor came to look broken.
 */
const char *axsock_server_name(void)
{
	axsock_resolve_server();
	if (axsock_host == NULL)
		return "(no AGWPE server is configured)";
	return axsock_host;
}

/*
 * Connect the AGWPE client to the configured server.  A leading '/'
 * in AXSOCK_HOST selects a unix domain socket (a path), anything else
 * a TCP host:port.  The unix socket is gated by its file permissions,
 * so an ax25netd configured with 'socket' and 'group hams' is only
 * reachable by members of that group.
 */
static int axsock_transport_connect(agwpe_client_t *c)
{
	axsock_resolve_server();
	if (axsock_host == NULL)
		return -1;
	if (axsock_host[0] == '/')
		return agwpe_client_connect_unix(c, axsock_host);
	return agwpe_client_connect_host(c, axsock_host, axsock_port);
}

static int axsock_ports_fetch(void)
{
	const char *user, *pass;
	struct addrinfo hints, *res, *ai;
	char service[16];
	struct agwpe_s hdr;
	struct timeval tv;
	int fd = -1;

	if (axsock_gnports >= 0)
		return 0;

	/* A server that does not answer the query must not be asked again
	 * for every frame that arrives: the ask is a connect and a round
	 * trip, and the name a frame is reported under is wanted far more
	 * often than the table behind it can change.  Wait a while before
	 * asking again, so a server that starts answering is still picked
	 * up on its own.  */
	if (axsock_gports_asked != 0) {
		time_t now = time(NULL);

		if (now - axsock_gports_asked < AXSOCK_GPORTS_RETRY)
			return -1;
	}
	axsock_gports_asked = time(NULL);

	axsock_resolve_server();
	if (axsock_host == NULL)
		return -1;

	snprintf(service, sizeof(service), "%d", axsock_port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (axsock_host[0] == '/') {
		struct sockaddr_un sa;

		if (strlen(axsock_host) >= sizeof(sa.sun_path))
			return -1;
		fd = real_socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			memset(&sa, 0, sizeof(sa));
			sa.sun_family = AF_UNIX;
			strncpy(sa.sun_path, axsock_host,
				sizeof(sa.sun_path) - 1);
			if (real_connect(fd, (struct sockaddr *)&sa,
					 SUN_LEN(&sa)) != 0) {
				real_close(fd);
				fd = -1;
			}
		}
	} else if (getaddrinfo(axsock_host, service, &hints, &res) != 0) {
		return -1;
	} else {
		for (ai = res; ai != NULL; ai = ai->ai_next) {
			fd = real_socket(ai->ai_family, ai->ai_socktype,
					 ai->ai_protocol);
			if (fd < 0)
				continue;
			if (real_connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
				break;
			real_close(fd);
			fd = -1;
		}
		freeaddrinfo(res);
	}
	if (fd < 0)
		return -1;

	/* netd may hold a 'G' request until every upstream reported its
	 * channels (deferred up to 10 s); be generous here, but not
	 * too: without the table the positional mapping is the fallback.  */
	tv.tv_sec = 3;
	tv.tv_usec = 0;
	real_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	user = getenv("AXSOCK_USER");
	pass = getenv("AXSOCK_PASSWORD");
	if (user != NULL && user[0] != '\0') {
		unsigned char data[510];

		memset(data, 0, sizeof(data));
		strncpy((char *)data, user, 254);
		data[254] = '\0';
		strncpy((char *)data + 255,
			(pass != NULL) ? pass : "", 254);
		data[509] = '\0';
		agwpe_header_init(&hdr, 0, AGWPE_CMD_LOGIN, 0, NULL, NULL,
				  sizeof(data));
		if (real_send(fd, &hdr, AGWPE_HEADER_LEN, MSG_NOSIGNAL) !=
		    AGWPE_HEADER_LEN ||
		    real_send(fd, data, sizeof(data), MSG_NOSIGNAL) !=
		    (ssize_t)sizeof(data)) {
			real_close(fd);
			return -1;
		}
	}

	agwpe_header_init(&hdr, 0, AGWPE_CMD_PORT_INFO, 0, NULL, NULL, 0);
	if (real_send(fd, &hdr, AGWPE_HEADER_LEN, MSG_NOSIGNAL) ==
	    AGWPE_HEADER_LEN) {
		struct agwpe_s r;

		if (axsock_read_full(fd, &r, AGWPE_HEADER_LEN) == 0) {
			uint32_t dlen = agwpe_netle2host(r.data_len);

			if (dlen <= 16 * 1024) {
				unsigned char *data = malloc(dlen);

				if (data != NULL) {
					if (axsock_read_full(fd, data, dlen) == 0)
						axsock_ports_parse(data, dlen);
					free(data);
				}
			}
		}
	}
	real_close(fd);
	return (axsock_gnports >= 0) ? 0 : -1;
}

/* The flat port of channel "chan" of the upstream named "base".  The
 * channels of one upstream are contiguous in the 'G' reply, starting at
 * its first channel, so channel N is first + N.  Returns -1 when the
 * upstream or that channel is not reported.  */
static int axsock_gport_channel(const char *base, int chan)
{
	int i, baseport = -1;

	for (i = 0; i < axsock_gnports; i++)
		if (strcasecmp(axsock_gports[i].up, base) == 0) {
			baseport = axsock_gports[i].port;
			break;
		}
	if (baseport < 0)
		return -1;
	if (chan == 0)
		return baseport;
	for (i = 0; i < axsock_gnports; i++)
		if (strcasecmp(axsock_gports[i].up, base) == 0 &&
		    axsock_gports[i].port == baseport + chan)
			return baseport + chan;
	return -1;
}

/* Strip the conventional "agwpe-" namespace marker from an axports
 * interface name, so the remaining name can be matched against the
 * upstream name in the 'G' reply (the name in ax25netd_agwpe.conf).  The prefix
 * keeps the virtual AGWPE interface names distinct from kernel AX.25
 * interfaces on systems with both.  */
static const char *axsock_strip_prefix(const char *name)
{
	if (strncasecmp(name, "agwpe-", 6) == 0)
		name += 6;
	return name;
}

/*
 * Map a callsign onto the AGWPE port number.
 *
 * The port numbers are flat (netd: upstream index * 16 + channel), so
 * they can no longer be guessed from the position of the axports entry.
 * Instead the port table is learned from the netd 'G' reply:
 *
 *   - the reserved interface name "loop" is the virtual loopback
 *     upstream and always uses AGWPE_PORT_LOOP;
 *   - an interface name with a ":N" suffix (e.g. "agwpe-direwolf2:4")
 *     selects channel N of the upstream of the same name, i.e. the
 *     (N+1)-th radio interface of that upstream (the name before the
 *     ":" selects the upstream, the suffix the channel inside it);
 *   - a plain interface name selects the first channel of the upstream
 *     that carries the same name in the 'G' reply;
 *   - an optional "agwpe-" prefix on the interface name is a namespace
 *     marker (the virtual interface name in axports) and is stripped
 *     before matching against the upstream name.
 *
 * When the 'G' table is not reachable the old positional mapping is
 * used as a fallback: the position of the first axports entry whose
 * address matches is the upstream index, so the flat port is
 * position * 16 + channel.  A bound call with an SSID for which no
 * entry exists falls back to the loop interface: its base call serves
 * every SSID.  Falls back to port 0.
 */
static int axsock_call_base_equal(const char *a, const char *b);

static int agwpe_local_upstream(const char *base);
static int axsock_port_of_entry(const char *entry);

/*
 * Is the AGWPE server this shim talks to on this machine?
 *
 * That decides where an upstream's index may be read from.  On this host
 * the ax25netd_agwpe.conf in front of us is the server's own file, and the position
 * of an upstream in it is the index AGWPE numbers its ports from.  A server
 * elsewhere has an ax25netd_agwpe.conf of its own, which this one need not be, so
 * nothing here says anything about it.
 */
static int axsock_server_local(void)
{
	static int cached = -1;
	const char *host;
	struct addrinfo hints, *res, *ai;
	int local = 0;

	if (cached >= 0)
		return cached;

	axsock_resolve_server();
	host = axsock_host;
	if (host == NULL) {
		/* No server named, so nothing can tell us whether it would
		 * have been this machine.  Saying no is the safe answer:
		 * an upstream index read from the wrong file is worse than
		 * one that is not read at all. */
		cached = 0;
		return cached;
	}
	if (host[0] == '/') {
		/* A unix socket is on this machine by definition.  */
		cached = 1;
		return cached;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res) == 0) {
		for (ai = res; ai != NULL; ai = ai->ai_next) {
			if (ai->ai_family == AF_INET &&
			    (ntohl(((struct sockaddr_in *)ai->ai_addr)
				    ->sin_addr.s_addr) >> 24) == 127) {
				local = 1;
				break;
			}
			if (ai->ai_family == AF_INET6 &&
			    IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)
						   ai->ai_addr)->sin6_addr)) {
				local = 1;
				break;
			}
		}
		freeaddrinfo(res);
	}
	cached = local;
	return cached;
}

/* The axports entry that owns this callsign, and the port that entry is on.
 *
 * One walk answers both, because a bind used to ask both and got two answers
 * that did not have to agree.  axsock_bind_port() wanted the name for a
 * diagnostic and the number for the wire, and looked each up on its own: two
 * walks of the port table, and - since the reload behind each of them rebuilt
 * that table - two chances to disagree.  They did.  A bind that had resolved
 * "loop" through the first was refused by the second with "no backend serves
 * the port 'loop'", and only where a kernel socket was in the way, because
 * that is the one path where the shim's reader thread is live while the
 * application binds.
 *
 * The two answers still answer different questions, so they can still
 * disagree: the number is asked a second question - which upstream serves this
 * entry - and that one is allowed to fail.  An entry no ax25netd_agwpe.conf upstream serves is
 * exactly the case worth being able to see: the number is -1 and the bind is
 * refused, while a monitor on the raw stream still sees frames, because those
 * frames got to ax25netd and ax25netd is what puts them on the wire.
 *
 * The name is what goes out, when the caller has somewhere to put it, and it
 * stays empty when there is no entry.  A source callsign that is nobody's port
 * is not a misconfiguration - it is what a program that only transmits looks
 * like - so there is nothing to report about it.
 *
 * Printing the port number there would say less and mislead.  The number is
 * this machine's answer to "which upstream index", neither axports nor
 * ax25netd_agwpe.conf ever wrote it down, and it does not read back as a name: one
 * upstream's channels share its stride, so a number cannot tell channel 0
 * from channel 5.  The name is what the operator wrote and can go and fix.
 *
 * Exact match first, then the base-call fallback, and in that order for both
 * answers: the entry has to be the same one whichever of the two found it, or
 * a monitor could name an entry that the port it is reporting did not come
 * from.
 *
 * Not axsock_copy_call(): a port name is written the way axports spells it,
 * and uppercasing would print RADIO0 where the operator wrote radio0.  A
 * callsign has no spelling to lose, which is why that one can fold.
 */

/* Copy a port name, and mark it if it did not fit.
 *
 * Truncating quietly would be worse than not copying it at all: the cut
 * string is not the name in any file, so a monitor would print it as if it
 * were one, and an operator who copied it into ax25netd_agwpe.conf would get a
 * port that does not exist.  A tilde says the label is cut, which is the
 * one thing about it the reader can act on.  Returns the length written,
 * terminator not counted.
 */
static size_t axsock_copy_port_name(char *dst, size_t dstlen, const char *src)
{
	size_t n;

	n = strlen(src);
	if (n >= dstlen) {
		n = dstlen - 1;
		if (n > 0) {
			memcpy(dst, src, n - 1);
			dst[n - 1] = '~';
		}
	} else {
		memcpy(dst, src, n);
	}
	dst[n] = '\0';
	return n;
}

static int axsock_port_found(const char *entry, char *name, size_t namelen)
{
	if (name != NULL && namelen > 0)
		(void)axsock_copy_port_name(name, namelen, entry);

	return axsock_port_of_entry(entry);
}

static int axsock_port_lookup(const char *call, char *name, size_t namelen)
{
	char *entry, *addr;

	if (name != NULL && namelen > 0)
		name[0] = '\0';
	if (call == NULL || call[0] == '\0')
		return -1;

	ax25_config_ports_ensure();

	for (entry = ax25_config_get_next(NULL); entry != NULL;
	     entry = ax25_config_get_next(entry)) {
		addr = ax25_config_get_addr(entry);
		if (addr != NULL && strcasecmp(addr, call) == 0)
			return axsock_port_found(entry, name, namelen);
	}
	for (entry = ax25_config_get_next(NULL); entry != NULL;
	     entry = ax25_config_get_next(entry)) {
		addr = ax25_config_get_addr(entry);
		if (addr != NULL && axsock_call_base_equal(addr, call))
			return axsock_port_found(entry, name, namelen);
	}

	if (axsock_debug)
		fprintf(stderr, "axsock: no axports entry has the callsign '%s', "
			"so no port of it can be named\n", call);

	return -1;
}

/* The AGWPE port of the entry that owns this callsign, or -1 when nothing
 * can say which port that is.  A callsign that is in no entry at all is -1
 * as well: a caller that named no port has nothing to go on and has to
 * hear that rather than be handed the first one.  */
static int axsock_port_for(const char *call)
{
	return axsock_port_lookup(call, NULL, 0);
}

/*
 * The AGWPE port of one axports entry.
 *
 * The server's port list decides, because the numbering belongs to the
 * server: AGWPE gives the channels of upstream i the numbers i*16+c, and
 * which i a given upstream has is not written down in axports.  The list
 * is one command away whenever the server is a real AGWPE server.
 *
 * It is not there when the server does not answer it - a stub in front of
 * the socket, a server that has not finished coming up, or a test rig with
 * nothing behind it at all.  When the server is on this machine then
 * ax25netd_agwpe.conf is the server's own file and the position of an upstream in it
 * is that index, so i*16+c is exact and no server is needed for it.  A
 * server elsewhere cannot be answered for that way, and then there is
 * nothing left but to refuse.
 *
 * Refusing is the point of the whole arrangement.  The guess that stood
 * here took the position of the entry in axports for the index of the
 * upstream, which is a different number: it agreed while an upstream had
 * one entry, and from the second entry on it was out by a whole stride, so
 * the second channel of the first radio went out on a port that belongs to
 * the second radio.  It did so quietly.  A frame on the wrong frequency is
 * the one mistake in this file that cannot be taken back, and nothing at
 * the sending end of it says so.
 *
 * channel and upstream are for ax25_port_info(), which has to tell a caller
 * which channel of which upstream it is looking at: the flat port alone
 * belongs to the server and says nothing to anybody reading a config file.
 * Both are optional, and the shim itself asks for neither.
 */
static int axsock_port_of_entry_ex(const char *entry, int *channel,
				   char *upstream, size_t uplen)
{
	char base[24];
	char upname[24];
	const char *colon;
	int idx, p, up;
	size_t blen, ulen;

	/* Optional ":N" suffix selects a channel of the named upstream,
	 * not a second upstream: "direwolf:1" is channel 1 of the upstream
	 * "direwolf", the way AGWPE numbers the channels of one server.
	 *
	 * A suffix that is not a number is a refusal and not channel 0.
	 * It used to fall through to idx = -1, which means channel 0, so
	 * "direwolf:x" went out on the first radio of the upstream - a
	 * typo in a config file turned into frames on a frequency nobody
	 * asked for, and the only sign of it was that the port number
	 * matched the one for "direwolf".  ax25tcpd has always refused this
	 * spelling, which is why the two disagreed about it.
	 */
	idx = -1;
	colon = strrchr(entry, ':');
	if (colon != NULL && colon != entry && colon[1] != '\0') {
		char *end;
		long v;

		errno = 0;
		v = strtol(colon + 1, &end, 10);
		if (errno != 0 || *end != '\0' || end == colon + 1 ||
		    v < 0 || v > 15) {
			if (axsock_debug)
				fprintf(stderr, "axsock: '%s': a channel is a "
					"number, 0 to 15\n", entry);
			return -1;
		}
		idx = (int)v;
		blen = (size_t)(colon - entry);
		if (blen >= sizeof(base))
			blen = sizeof(base) - 1;
		memcpy(base, entry, blen);
		base[blen] = '\0';
	} else {
		strncpy(base, entry, sizeof(base) - 1);
		base[sizeof(base) - 1] = '\0';
	}

	/* The namespace marker comes off before anything is looked up, and
	 * not only for the upstream table: "agwpe-loop" is the loop port as
	 * much as "loop" is, because the marker is what keeps the virtual
	 * AGWPE names apart from kernel AX.25 ones and says nothing about
	 * which upstream is meant.
	 *
	 * It used to be tested against the marked name, so "agwpe-loop"
	 * skipped the check below and fell through to the position of
	 * "loop" in ax25netd_agwpe.conf - a port of the second upstream,
	 * not 255.  agwpe_owns_port() strips the marker first and so has
	 * always said that name was ours, which made the two disagree
	 * about the port of a port that was claimed.
	 */
	{
		const char *stripped = axsock_strip_prefix(base);

		ulen = strlen(stripped);
		if (ulen >= sizeof(upname))
			ulen = sizeof(upname) - 1;
		memcpy(upname, stripped, ulen);
		upname[ulen] = '\0';
	}

	/* The reserved loop interface is the virtual loopback upstream.  */
	if (strcasecmp(upname, "loop") == 0) {
		if (channel != NULL)
			*channel = 0;
		if (upstream != NULL && uplen > 0)
			snprintf(upstream, uplen, "%s", "loop");
		return AGWPE_PORT_LOOP;
	}

	/* An upstream has sixteen channels and no more, so a suffix beyond
	 * that names nothing: i*16+c for c of 99 is a port of the seventh
	 * upstream, and letting the arithmetic produce one is the very thing
	 * this function refuses to do.  Refused above, where the suffix is
	 * read, so that the two cannot disagree about what a channel is. */
	if (axsock_ports_fetch() == 0) {
		p = axsock_gport_channel(upname, (idx >= 0) ? idx : 0);
		if (p >= 0) {
			int i;

			if (channel != NULL)
				*channel = (idx >= 0) ? idx : 0;
			/* From the table rather than from the name as typed:
			 * the entry that carries the port is the one whose
			 * upstream name the operator would write in
			 * ax25netd_agwpe.conf, and an "agwpe-" prefix is
			 * the marker that keeps the two name spaces apart. */
			if (upstream != NULL && uplen > 0) {
				upstream[0] = '\0';
				for (i = 0; i < axsock_gnports; i++)
					if (axsock_gports[i].port == p) {
						snprintf(upstream, uplen, "%s",
							 axsock_gports[i].up);
						break;
					}
			}
			return p;
		}
	}

	if (axsock_server_local()) {
		up = agwpe_local_upstream(upname);
		if (up >= 0) {
			if (channel != NULL)
				*channel = (idx >= 0) ? idx : 0;
			/* The name ax25netd_agwpe.conf gave it, which is what
			 * matched above; there is no table to read it from
			 * when the server did not answer. */
			if (upstream != NULL && uplen > 0)
				snprintf(upstream, uplen, "%s", upname);
			return up * 16 + (idx >= 0 ? idx : 0);
		}
	}

	return -1;
}

static int axsock_port_of_entry(const char *entry)
{
	return axsock_port_of_entry_ex(entry, NULL, NULL, 0);
}

/*
 * What this backend knows about a port name, for ax25_port_info().
 *
 * The same test agwpe_bind() makes, so the two cannot disagree about which
 * ports are ours: the kernel's first, because a bind naming one of those was
 * answered by socket() and does not belong to a userspace backend however
 * much the server behind it might also be able to send.
 *
 * No ax25_config_ports_ensure() before axsock_port_of_entry_ex(), which
 * fetches the server's list and reads ax25netd_agwpe.conf by itself.  Asking
 * for a read here would make every question cost a file read, and a
 * diagnostic tool asks the whole table in a loop.
 */
static int agwpe_port_hook(const char *name, int *port, int *channel,
			   char *upstream, size_t uplen)
{
	int p;

	if (name == NULL || *name == '\0')
		return 0;
	if (ax25_config_port_is_kernel(name))
		return 0;

	p = axsock_port_of_entry_ex(name, channel, upstream, uplen);
	if (p < 0)
		return 0;
	if (port != NULL)
		*port = p;

	return 1;
}

/*
 * Which port a bind names.  The callsign in the address is the source; the
 * port is named by the first digipeater slot, which is what ax25_aton()
 * builds from an axports entry and what every program in the suite passes.
 * Only when no slot is given does the source callsign have to answer for
 * both, and it can answer only when it is itself a port's callsign.
 *
 * Reading the source alone was wrong in the case that matters most.  A
 * service callsign is not in axports - that is the whole point of one - so
 * it mapped to port 0, and a listener bound to the port it had named ended
 * up on the first port instead.  It went unnoticed because every test used
 * a callsign that was in axports, where the two answers agree.
 *
 * wampes.c arrives at the same rule in port_of_bind().  They are two
 * lookups because the results are different things - a flat AGWPE port
 * number against a node's interface name - not because the question
 * differs.
 */

/*
 * Did this bind name a port?
 *
 * It did when the first digipeater slot is filled: that is where every
 * program in the suite puts the callsign of the axports entry it means
 * (call(1) resolves the name the user typed with ax25_config_get_addr()
 * and passes the result here), and it is what makes the name recoverable
 * at all - the address carries a callsign, not a name.  A bind without
 * that slot named no port, and the two cases are not the same question:
 * a program is free to bind a source callsign that is nobody's port, and
 * the kernel may well have an interface with it even though axports does
 * not list one.  Refusing those would break kernel AX.25 for every
 * callsign outside the port table, which is most of them.
 */
static int axsock_addr_names_port(const struct sockaddr *addr, socklen_t len)
{
	const struct full_sockaddr_ax25 *fsa =
		(const struct full_sockaddr_ax25 *) addr;

	return len >= sizeof(struct full_sockaddr_ax25) &&
	       fsa->fsa_ax25.sax25_ndigis > 0;
}

static int axsock_bind_port(const struct sockaddr *addr,
			    socklen_t len, const char *local,
			    int *named, int *known, char *name, size_t namelen)
{
	const struct full_sockaddr_ax25 *fsa =
		(const struct full_sockaddr_ax25 *) addr;
	int p;

	if (name != NULL && namelen > 0)
		name[0] = '\0';
	if (known != NULL)
		*known = 0;

	if (axsock_addr_names_port(addr, len)) {
		*named = 1;
		/* A port was named, so it has to be a port.  If the entry
		 * says a channel the server does not list, that is a
		 * mistake in axports and this is where it is caught.  The
		 * name goes out with the number: a number alone cannot be
		 * read back as a name once the port does not exist, and a
		 * monitor that then has nothing to print says less than
		 * the one line that got the frame onto the wire.  One
		 * lookup for both - see axsock_port_lookup().  */
		return axsock_port_lookup(ax25_ntoa(&fsa->fsa_digipeater[0]),
					  name, namelen);
	}

	*named = 0;
	/* Nothing named a port.  A source callsign that is itself a port's
	 * still says which one; anything else has nothing to go on, and the
	 * first port is what such a socket has always used.
	 *
	 * The first port and no port are the same number on the wire, and
	 * that is worth being careful about further down: a frame that goes
	 * out on 0 because nothing was known is not a frame of port 0, and a
	 * monitor must not read a name into it.  port_known() says which of
	 * the two this is, and axsock_port_name() is asked only when it does.
	 */
	p = axsock_port_lookup(local, name, namelen);
	*known = (p >= 0);
	return (p >= 0) ? p : 0;
}

/* The name to report a raw frame on, into sa_data.
 *
 * axsock_port_name() is the right answer when it has one: it is the entry
 * that is actually on the port number, so a monitor on one port of a
 * two-channel upstream is told which channel it is looking at.  It answers
 * nothing for a port number that no axports entry is on, and that is not a
 * corner but the ordinary result of an upstream that ax25netd_agwpe.conf does not
 * list - and the raw stream still carries its frames, because they got to
 * ax25netd, which is what put them on the wire.
 *
 * s->portname is the answer for that case: the name the bind asked for, kept
 * since the bind.  It is the one half of the answer that means something to
 * the operator, who can go and put the upstream in ax25netd_agwpe.conf, and it is
 * not recoverable later - the number does not say which entry wanted it, and
 * for a port no upstream serves there is no index to compute one from.
 *
 * With neither, sa_data stays as the memset left it.  A caller that has
 * nothing to print is not a mistake to paper over: a source callsign that is
 * nobody's port is what a program that only transmits looks like, and
 * listen(1) prints that as no name rather than inventing one.
 */
static const char *axsock_port_name(unsigned char port);

static void axsock_raw_name(struct axsock_sock *s, unsigned char port,
			    struct sockaddr *sa, size_t datalen)
{
	const char *name = NULL;

	/*
	 * Only a port that is known to be a port may be read back as a name.
	 * When the bind could not determine one, the number that went on
	 * the wire is the first port such a socket has always used, and it is
	 * indistinguishable there from channel 0 of the first upstream.  An
	 * axports entry that happens to sit on that number would then be
	 * reported for a frame that never went near it, which is worse than
	 * saying nothing: an operator who reads a name off a monitor takes
	 * it as the answer to "which port was this on".
	 *
	 * The loop port is the one exception, and it is not a corner: 255 is
	 * reserved by the protocol for exactly this upstream, so its number
	 * names it as unambiguously as any axports entry could, whether or
	 * not the monitor was bound and whether or not a loop line exists.
	 * Every frame that crossed a WAMPES node lands on it, and it is the
	 * port the mirror itself sends on, so leaving it unnamed is what
	 * made a mirrored frame show up as "?" instead of "loop:".
	 */
	if (s->port_known || port == AGWPE_PORT_LOOP)
		name = axsock_port_name(port);
	if (name == NULL)
		name = s->portname;
	if (name == NULL || datalen == 0)
		return;

	/* Marked when it does not fit, which for an axports name is a
	 * configuration fact rather than a hope: nothing bounds them.  The
	 * frame is still real either way, and the callsign on it is still the
	 * whole of the answer.  */
	axsock_copy_port_name(sa->sa_data, datalen, name);
}

static const char *axsock_port_name(unsigned char port)
{
	char *name;

	ax25_config_ports_ensure();
	for (name = ax25_config_get_next(NULL); name != NULL;
	     name = ax25_config_get_next(name))
		if (axsock_port_of_entry(name) == port)
			return name;

	/* The loopback port is the one name that needs no entry in axports.
	 * axsock_port_of_entry() hands "loop" the number AGWPE_PORT_LOOP
	 * without asking anybody, so the reverse lookup has to be able to
	 * hand the number its name back - otherwise the two disagree about
	 * a port whose name is not in question.  A file that has no loop
	 * line is perfectly usable, since nothing needs the line to put
	 * frames on the port, and every frame that crossed a WAMPES node
	 * arrives on it.  A loop line, if there is one, was found above and
	 * wins.  */
	if (port == AGWPE_PORT_LOOP)
		return "loop";

	return NULL;
}

/* Does a raw monitor socket want frames from the given flat AGWPE port?
 * Unbound monitors accept everything; a monitor bound by listen(1) -p
 * matches the name the frame would be reported under (its AGWPE/axports
 * port name), or the axports device name for that port.  */
static int axsock_raw_match(struct axsock_sock *s, unsigned char port)
{
	const char *name;

	if (s->bound[0] == '\0')
		return 1;

	name = axsock_port_name(port);
	if (name == NULL)
		return 0;
	if (strcasecmp(name, s->bound) == 0)
		return 1;
	name = ax25_config_get_dev((char *)name);
	if (name != NULL && strcasecmp(name, s->bound) == 0)
		return 1;
	return 0;
}

static struct axsock_sock *axsock_alloc_sock_locked(int type);
static void axsock_peer_reader_teardown(struct axsock_sock *s);

/*
 * Create the socketpair backed descriptor for a new virtual socket.
 * Returns NULL on error with errno set.  Call with the lock held.
 */
static struct axsock_sock *axsock_alloc_sock_locked(int type)
{
	struct axsock_sock *s;
	int fds[2];
	int fl;

	if (__atomic_load_n(&axsock_nsock, __ATOMIC_RELAXED) >=
	    AXSOCK_MAX_SOCK) {
		errno = EMFILE;
		return NULL;
	}

	/* A bidirectional pair so the fd handed to the application can be
	 * both read (inbound data) and written (outbound data): ax25d
	 * gives the accepted descriptor to a forked child as stdin and
	 * stdout.  */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
		return NULL;

	/* Generous buffers on both ends: the dispatch path (inbound frames)
	 * writes non-blocking into this pair, and bursts of data would
	 * otherwise hit EAGAIN and be silently dropped while the peer
	 * thread is busy forwarding to the network.  The default socketpair
	 * buffers on most systems are only a few kilobytes.
	 */
	{
		int bufsz = 1024 * 1024;
		(void)setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF,
				 &bufsz, sizeof(bufsz));
		(void)setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF,
				 &bufsz, sizeof(bufsz));
		(void)setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF,
				 &bufsz, sizeof(bufsz));
		(void)setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF,
				 &bufsz, sizeof(bufsz));
	}

	fl = fcntl(fds[1], F_GETFL);
	if (fl != -1)
		fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);
	/* The router end must not survive exec: a forked child inherits it,
	 * and as long as any copy of it is open the socketpair write side
	 * stays up, so the child would never see EOF on stdin.
	 */
	(void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);

	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		close(fds[0]);
		close(fds[1]);
		errno = ENOMEM;
		return NULL;
	}

	s->fd = fds[0];
	s->peer = fds[1];
	s->type = type;
	s->pid = AGWPE_PID_AX25;

	s->next = axsock_list;
	axsock_list = s;
	__atomic_add_fetch(&axsock_nsock, 1, __ATOMIC_RELAXED);

	return s;
}

/*
 * The protocol id belongs to the socket from the moment it is made: AX.25
 * carries it in every frame, and socket() is where the application says
 * which one it wants.  Zero means "the usual", which is text.
 */
static int axsock_new_sock(int type, int pid)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_alloc_sock_locked(type);
	if (s != NULL && pid != 0)
		s->pid = (unsigned char) pid;
	pthread_mutex_unlock(&axsock_lock);
	if (s == NULL)
		return -1;
	return s->fd;
}

/*
 * Register a call sign with the AGWPE server so that frames addressed to
 * it are delivered to this client.  listener selects the 'L' extension
 * (a listening socket on the loop port) instead of the plain 'X'.  Called
 * with the lock held.
 */
static void axsock_register_locked(const char *call, unsigned char port,
				   int listener)
{
	int i;

	for (i = 0; i < axsock_nregistered; i++) {
		if (strcasecmp(axsock_registered[i].call, call) != 0)
			continue;
		axsock_registered[i].refs++;
		if (listener)
			axsock_registered[i].listener = 1;
		goto send;
	}

	if (axsock_nregistered >= (int)(sizeof(axsock_registered) /
					sizeof(axsock_registered[0])))
		return;

	strncpy(axsock_registered[axsock_nregistered].call, call,
		AGWPE_MAX_CALL - 1);
	axsock_registered[axsock_nregistered].call[AGWPE_MAX_CALL - 1] = '\0';
	axsock_registered[axsock_nregistered].port = port;
	axsock_registered[axsock_nregistered].refs = 1;
	axsock_registered[axsock_nregistered].listener = listener;
	axsock_nregistered++;

send:
	if (listener)
		agwpe_client_listen(axsock_agwpe, port, call);
	else
		agwpe_client_register(axsock_agwpe, port, call);
}

static void axsock_unregister_locked(const char *call, unsigned char port)
{
	int i;

	for (i = 0; i < axsock_nregistered; i++) {
		if (strcasecmp(axsock_registered[i].call, call) != 0)
			continue;
		if (--axsock_registered[i].refs > 0)
			return;
		agwpe_client_unregister(axsock_agwpe, port, call);
		axsock_registered[i] = axsock_registered[axsock_nregistered - 1];
		axsock_nregistered--;
		return;
	}
}

/* Re-send every registration over a (re)established AGWPE connection.  */
static void axsock_register_all_locked(void)
{
	int i;

	for (i = 0; i < axsock_nregistered; i++) {
		if (axsock_registered[i].listener)
			agwpe_client_listen(axsock_agwpe,
					    axsock_registered[i].port,
					    axsock_registered[i].call);
		else
			agwpe_client_register(axsock_agwpe,
					      axsock_registered[i].port,
					      axsock_registered[i].call);
	}
}

/*
 * A listener bound to a call without an SSID (SSID 0) serves every SSID
 * of the same base call, mirroring the netd routing rule.
 */
static int axsock_call_has_ssid(const char *call)
{
	return strchr(call, '-') != NULL;
}

static int axsock_call_base_equal(const char *a, const char *b)
{
	const char *da = strchr(a, '-');
	const char *db = strchr(b, '-');
	size_t la = da != NULL ? (size_t)(da - a) : strlen(a);
	size_t lb = db != NULL ? (size_t)(db - b) : strlen(b);

	return la == lb && strncasecmp(a, b, la) == 0;
}

static int axsock_call_match(const char *listener, const char *target)
{
	if (strcasecmp(listener, target) == 0)
		return 1;
	if (!axsock_call_has_ssid(listener) &&
	    axsock_call_base_equal(listener, target))
		return 1;
	return 0;
}

/*
 * Route one incoming frame to the matching virtual socket.  Runs in the
 * reader thread.
 */
/*
 * Getting an inbound frame to the application, without losing any of it.
 *
 * The peer end of the socketpair is non-blocking, which it has to be: this
 * runs on the one reader thread that serves every session, and it holds
 * axsock_lock, so waiting here would not stall one session but the library -
 * every close(), connect() and send() the application makes queues behind
 * the same mutex.
 *
 * What it did instead was treat EAGAIN as an answer and throw away the rest
 * of the frame, which is the one thing that must not happen: EAGAIN says
 * nothing was taken and to come back, and on a byte-stream socketpair - what
 * macOS gives, having no SEQPACKET pair - the loss is not a missing frame
 * but a hole in the middle of the stream, which the application cannot see.
 *
 * So keep what the socket would not take and push it later, which is what
 * ax25netd does for its own clients in loop_send_client().  The reader loop
 * stops blocking for as long as anything is queued and comes back to flush;
 * with nothing queued it blocks as it always did, so the cost falls entirely
 * on the case that used to lose data.
 *
 * A ceiling is still needed, because a reader that never reads must not grow
 * this without end.  Past it the session goes, loudly - a disconnect is
 * something the application can see and act on, which is exactly what the
 * silent hole was not.
 */

#define	AXSOCK_PEND_MAX	(1 * 1024 * 1024)	/* as NETD_OUT_MAX in ax25netd */

/* Let the queue go and stop the reader coming back for it. */
static void axsock_peer_discard_locked(struct axsock_sock *s)
{
	if (s->plen > 0)
		__atomic_sub_fetch(&axsock_npending, 1, __ATOMIC_RELAXED);
	free(s->pend);
	s->pend = NULL;
	s->plen = 0;
	s->pcap = 0;
}

/* Detach the application's end from the router end.
 *
 * close() alone is not enough when a peer reader thread is blocked in
 * select()/read() on this same descriptor: another thread's close() does
 * not wake it, and while that reference stands the write side stays up,
 * so the application never sees end of file and the session outlives the
 * disconnect.  shutdown() wakes the thread and delivers EOF to the
 * application at once; close() then releases the descriptor.
 */
static void axsock_peer_shutdown_locked(struct axsock_sock *s)
{
	if (s->peer < 0)
		return;
	(void)real_shutdown(s->peer, SHUT_RDWR);
	real_close(s->peer);
	s->peer = -1;
}

static void axsock_peer_drop_locked(struct axsock_sock *s, const char *why)
{
	fprintf(stderr, "axsock: %s for %.*s - closing the session\n", why,
		AGWPE_MAX_CALL, s->remote);
	axsock_peer_discard_locked(s);
	axsock_peer_shutdown_locked(s);
	s->state = AXSOCK_NEW;
}

/* Push what is waiting.  Called with axsock_lock held; every write here is
 * non-blocking, so the lock is never held across a wait. */
static void axsock_peer_flush_locked(struct axsock_sock *s)
{
	size_t off = 0;

	if (s->plen == 0)
		return;

	/* The peer end can be closed from elsewhere - a DISCONNECT frame, or
	 * the teardown when the AGWPE link drops - and those paths do not
	 * know about this queue.  Letting it lie would leave axsock_npending
	 * standing, and the reader thread would come back every 20 ms for
	 * the rest of the process's life, looking at something nobody will
	 * ever read. */
	if (s->peer < 0) {
		axsock_peer_discard_locked(s);
		return;
	}

	while (off < s->plen) {
		ssize_t n = real_write(s->peer, s->pend + off, s->plen - off);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;			/* still full */
			axsock_peer_drop_locked(s, "the peer end is gone");
			return;
		}
		off += (size_t) n;
	}

	if (off == s->plen) {
		s->plen = 0;
		__atomic_sub_fetch(&axsock_npending, 1, __ATOMIC_RELAXED);
		if (s->peer_eof) {
			axsock_peer_shutdown_locked(s);
			s->peer_eof = 0;
		}
	} else if (off > 0) {
		memmove(s->pend, s->pend + off, s->plen - off);
		s->plen -= off;
	}
}

/*
 * The far end hung up, or the link to the server did.  Whatever already
 * arrived is still the application's to read - it was received before the
 * session ended - so the router end is closed only once the queue is empty.
 * Closing it now would throw that away, which is the same loss this file
 * just stopped making, only at the end of a session instead of the middle.
 */
static void axsock_peer_close_locked(struct axsock_sock *s)
{
	if (s->peer < 0)
		return;
	axsock_peer_flush_locked(s);
	if (s->plen > 0 && s->peer >= 0) {
		s->peer_eof = 1;	/* the flush loop finishes the job */
		return;
	}
	axsock_peer_shutdown_locked(s);
	s->peer_eof = 0;
}

/* Everything of it or nothing lost.  Returns 0 when the session lives on,
 * whether the bytes went out or were put by; -1 when it was torn down. */
static int axsock_peer_write_locked(struct axsock_sock *s,
				    const unsigned char *data, size_t len)
{
	size_t off = 0;

	if (s->peer < 0)
		return -1;

	/* Anything already waiting goes first, or the stream would arrive
	 * out of order - which is worse than arriving late. */
	axsock_peer_flush_locked(s);
	if (s->peer < 0)
		return -1;

	if (s->plen == 0) {
		while (off < len) {
			ssize_t n = real_write(s->peer, data + off, len - off);

			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				axsock_peer_drop_locked(s,
					"the peer end is gone");
				return -1;
			}
			off += (size_t) n;
		}
		if (off == len)
			return 0;
	}

	if (s->plen + (len - off) > AXSOCK_PEND_MAX) {
		axsock_peer_drop_locked(s,
			"the application stopped reading and the queue is full");
		return -1;
	}

	if (s->plen + (len - off) > s->pcap) {
		size_t want = s->pcap ? s->pcap : 4096;
		unsigned char *nb;

		while (want < s->plen + (len - off))
			want *= 2;
		if ((nb = realloc(s->pend, want)) == NULL) {
			axsock_peer_drop_locked(s, "out of memory queueing");
			return -1;
		}
		s->pend = nb;
		s->pcap = want;
	}

	if (s->plen == 0)
		__atomic_add_fetch(&axsock_npending, 1, __ATOMIC_RELAXED);
	memcpy(s->pend + s->plen, data + off, len - off);
	s->plen += len - off;
	return 0;
}

/* Called from the reader thread between frames: whatever became writable
 * while it was waiting goes out now. */
static void axsock_flush_pending(void)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	for (s = axsock_list; s != NULL; s = s->next)
		if (s->plen > 0)
			axsock_peer_flush_locked(s);
	pthread_mutex_unlock(&axsock_lock);
}

/*
 * A connection coming up, however it is spelled.  A connect has three
 * spellings on the way out - 'C' plain, 'v' through digipeaters, 'c' with a
 * protocol id that is not text - and ax25netd hands the frame on as it got it
 * rather than normalising it to the 'C' a server would report.  Knowing only
 * 'C' meant neither a digipeated connect nor one with a pid was recognised at
 * all: the caller sat out its timeout and the callee never saw the call.
 *
 * Both directions, because the caller reads its own confirmation from the
 * same dispatch.
 */

/*
 * Our own transmissions come back on the monitor stream, and a station is not
 * told its own frames.  What must not be swallowed with them is the rest:
 *
 *   - a digipeater repeating us is the same source and a different frame, and
 *     the one an operator most wants to see: it is the proof the hop happened
 *   - the same frame heard back on another port says something about the
 *     network and is not an echo
 *
 * So the test is all three at once - same port, nothing repeated yet, and the
 * same frame - and the frame is remembered by what was said rather than by
 * the bytes, because the far end rebuilds those in its own shape.
 */

#define	AXSOCK_SENT_RING	8

static struct {
	unsigned char	port;
	unsigned char	pid;
	char		src[AGWPE_MAX_CALL];
	char		dst[AGWPE_MAX_CALL];
	uint32_t	hash;
	int		used;
} axsock_sent[AXSOCK_SENT_RING];
static int axsock_sent_at;

static uint32_t agwpe_hash(const unsigned char *p, size_t n)
{
	uint32_t h = 2166136261u;	/* FNV-1a */

	while (n-- > 0) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

/* Called under the lock, from the unproto send path. */
static void agwpe_note_sent_locked(unsigned char port, unsigned char pid,
				   const char *src, const char *dst,
				   const void *buf, size_t len)
{
	int i = axsock_sent_at;

	axsock_sent[i].port = port;
	axsock_sent[i].pid = pid;
	snprintf(axsock_sent[i].src, sizeof(axsock_sent[i].src), "%s", src);
	snprintf(axsock_sent[i].dst, sizeof(axsock_sent[i].dst), "%s", dst);
	axsock_sent[i].hash = agwpe_hash(buf, len);
	axsock_sent[i].used = 1;
	axsock_sent_at = (i + 1) % AXSOCK_SENT_RING;
}

static int agwpe_was_ours_locked(unsigned char port, unsigned char pid,
				 const char *src, const char *dst,
				 const unsigned char *info, size_t ilen,
				 int repeated)
{
	uint32_t h;
	int i;

	if (repeated)
		return 0;		/* somebody repeated it: not an echo */
	h = agwpe_hash(info, ilen);
	for (i = 0; i < AXSOCK_SENT_RING; i++) {
		if (!axsock_sent[i].used || axsock_sent[i].port != port ||
		    axsock_sent[i].pid != pid || axsock_sent[i].hash != h)
			continue;
		if (strcasecmp(axsock_sent[i].src, src) != 0 ||
		    strcasecmp(axsock_sent[i].dst, dst) != 0)
			continue;
		axsock_sent[i].used = 0;	/* one echo per transmission */
		if (axsock_debug)
			fprintf(stderr, "axsock: monitor copy of our own %s>%s on port %u - not delivered\n",
				src, dst, port);
		return 1;
	}
	return 0;
}

/*
 * A UI frame on its way to a datagram socket.
 *
 * The socketpair is a byte stream - macOS has no SEQPACKET pair - so a frame
 * needs a length in front of it or two of them arriving together become one.
 * And recvfrom() has to name the sender, which the bytes themselves do not,
 * so the callsign travels with the length.  The raw monitor solves the same
 * problem the same way (netax25/axmon.h); this is its small sibling.
 *
 *	[4 bytes, payload length, big endian][10 bytes, source callsign][payload]
 */

#define	AXSOCK_UI_HDR	(4 + AGWPE_MAX_CALL)

static void agwpe_ui_deliver_locked(struct axsock_sock *s, const char *from,
				    const unsigned char *data, size_t len)
{
	unsigned char hdr[AXSOCK_UI_HDR];
	unsigned char *rec;
	size_t n;

	if (len > AXMON_FRAME_MAX)
		return;			/* nothing here could read it */

	hdr[0] = (unsigned char)(len >> 24);
	hdr[1] = (unsigned char)(len >> 16);
	hdr[2] = (unsigned char)(len >> 8);
	hdr[3] = (unsigned char) len;

	/* The field is zeroed first, so the terminator does not have to
	 * come from the copy, and a loop is used instead of strncpy()
	 * because that is what GCC complains about: strncpy() writes no
	 * terminator of its own, so -Wstringop-truncation fires on it even
	 * here, where the memset before it has already made the result
	 * correct.  The loop takes at most AGWPE_MAX_CALL - 1 bytes and
	 * leaves the zero the memset put into byte AGWPE_MAX_CALL - 1, and
	 * it cannot read past AGWPE_MAX_CALL - 1 bytes of from - which is
	 * what matters, because from is a callsign field that came off the
	 * wire and need not be terminated at all. */
	memset(hdr + 4, 0, AGWPE_MAX_CALL);
	for (n = 0; n < AGWPE_MAX_CALL - 1 && from[n] != '\0'; n++)
		hdr[4 + n] = (unsigned char) from[n];

	/* One write, so the queue in axsock_peer_write_locked() can never
	 * hold half a record: a reader that has the length must be able to
	 * get the payload. */
	if ((rec = malloc(sizeof(hdr) + len)) == NULL)
		return;
	memcpy(rec, hdr, sizeof(hdr));
	if (len > 0)
		memcpy(rec + sizeof(hdr), data, len);
	(void) axsock_peer_write_locked(s, rec, sizeof(hdr) + len);
	free(rec);
}

static int agwpe_is_connect(unsigned char kind)
{
	return kind == AGWPE_DK_CONNECT ||
	       kind == AGWPE_CMD_CONNECT_VIA ||
	       kind == AGWPE_CMD_CONNECT_PID;
}

static void axsock_dispatch(agwpe_client_t *c, const struct agwpe_s *hdr,
			    const unsigned char *data, size_t len)
{
	struct axsock_sock *s;
	int found = 0;

	(void)c;

	pthread_mutex_lock(&axsock_lock);
	if (axsock_debug)
		fprintf(stderr, "axsock dispatch: kind=%c from='%.*s' to='%.*s' len=%zu\n",
			hdr->datakind, AGWPE_MAX_CALL, hdr->call_from,
			AGWPE_MAX_CALL, hdr->call_to, len);

	/* A raw frame ('K') belongs to every monitor socket, whatever its
	 * calls.  The data is a KISS encapsulated packet: direwolf prefixes
	 * each frame with a channel byte that doubles as the KISS data
	 * marker (channel << 4, low nibble 0), which ax25-apps/listen
	 * strips again.  */
	if (hdr->datakind == AGWPE_DK_RAW) {
		for (s = axsock_list; s != NULL; s = s->next) {
			unsigned char fbuf[AXMON_HDR_LEN + AXMON_FRAME_MAX];
			ssize_t n;

			if (!s->raw || s->peer < 0)
				continue;
			if (!axsock_raw_match(s, hdr->port))
				continue;
			/* Deliver each frame as one headed unit (see
			 * netax25/axmon.h): the monitor socketpair is a
			 * byte stream, and a stream merges frames that
			 * arrive back to back, which would make listen(1)
			 * decode past the end of the first frame.  The
			 * port rides along in the header - one value per
			 * socket cannot name them, because the frame an
			 * application reads is not the frame written
			 * last once two channels are busy at once.  */
			if (len == 0 || len > AXMON_FRAME_MAX)
				continue;	/* monitor cannot show it */
			fbuf[0] = (unsigned char)(len >> 24);
			fbuf[1] = (unsigned char)(len >> 16);
			fbuf[2] = (unsigned char)(len >> 8);
			fbuf[3] = (unsigned char)len;
			fbuf[4] = hdr->port;
			memcpy(fbuf + AXMON_HDR_LEN, data, len);
			n = real_write(s->peer, fbuf, AXMON_HDR_LEN + len);
			if (n != (ssize_t)(AXMON_HDR_LEN + len))
				continue;	/* monitor fell behind: drop */
			s->port = hdr->port;
		}
		/*
		 * Only the monitors are fed from here.  A datagram socket
		 * used to be fed from here as well, which worked and was
		 * wrong in two ways at once: it had to turn the raw monitor
		 * stream on for its connection to get here, and then every
		 * frame of every session on every port of this server was
		 * duplicated into that connection for it to pick one out of.
		 * What it wanted is delivered now, over the UI subscription
		 * (agwpe_client_uisub) and the 'M' frames ax25netd(8) sends
		 * in answer - the way the loop port has always done it.
		 */
		goto out;
	}

	/*
	 * Matched on the callsigns, not on a descriptor - which means a
	 * socket the application has closed can still be in this list and
	 * still carry the pair a new one is using.  It has to be: ax25d(8)
	 * hands the accepted descriptor to a forked child as stdin and
	 * stdout, closes its own copy, and the child reads on through the
	 * dup2()'d ones, so an entry with fd == -1 is a live session.
	 *
	 * What keeps the two apart is the order: a new socket goes on the
	 * front of the list, so the newest holder of a pair is found first,
	 * and that is the right precedence anyway - a session opened after
	 * another supersedes it.
	 */
	for (s = axsock_list; s != NULL; s = s->next) {
		if (strncasecmp(s->local, hdr->call_to, AGWPE_MAX_CALL) != 0)
			continue;

		if (s->state == AXSOCK_CONNECTING &&
		    strncasecmp(s->remote, hdr->call_from, AGWPE_MAX_CALL) == 0) {
			if (agwpe_is_connect(hdr->datakind)) {
				s->state = AXSOCK_CONNECTED;
				pthread_cond_broadcast(&axsock_cond);
			} else if (hdr->datakind == AGWPE_DK_DISCONNECT) {
				/*
				 * The refusal carries its reason in the text,
				 * which is the only place AGWPE has for one.
				 * "Nobody answered" and "that pair is already
				 * connected" are different things to a caller
				 * and deserve different answers - the node
				 * backend has always told them apart, saying
				 * EADDRINUSE for busy.
				 */
				s->connect_err =
					(data != NULL && len > 0 &&
					 memmem(data, len, "BUSY", 4) != NULL)
					? EADDRINUSE : ECONNREFUSED;
				s->state = AXSOCK_NEW;
				pthread_cond_broadcast(&axsock_cond);
			}
			found = 1;
			break;
		}

		if (s->state == AXSOCK_CONNECTED &&
		    strncasecmp(s->remote, hdr->call_from, AGWPE_MAX_CALL) == 0) {
			if (hdr->datakind == AGWPE_DK_DATA && s->peer >= 0) {
				/* Whole or queued, never half: see
				 * axsock_peer_write_locked() above.  A hard
				 * error there has already closed the router
				 * end, which wakes the peer reader thread -
				 * it owns the teardown. */
				if (axsock_peer_write_locked(s, data, len)
				    != 0) {
					found = 1;
					break;
				}
			} else if (hdr->datakind == AGWPE_DK_DISCONNECT) {
				axsock_peer_close_locked(s);
				s->state = AXSOCK_NEW;
			}
			found = 1;
			break;
		}
	}

	/*
	 * An incoming UI frame.  ax25netd routes one to the client that
	 * registered the destination callsign and hands it on as it
	 * arrived, so it reaches us as 'M' or 'V'.  That is the loop port's
	 * own mechanism, and since the UI subscription (see
	 * agwpe_client_uisub) it is how a radio port works as well - which
	 * is what lets a datagram socket off the loop port read its UI
	 * frames without the raw monitor stream being turned on for this
	 * connection.
	 */
	if (!found && (hdr->datakind == AGWPE_CMD_UNPROTO ||
		       hdr->datakind == AGWPE_CMD_UNPROTO_VIA)) {
		unsigned char want = hdr->pid ? hdr->pid : AGWPE_PID_AX25;
		struct axsock_sock *best = NULL;

		for (s = axsock_list; s != NULL; s = s->next) {
			if (s->type != SOCK_DGRAM || s->local[0] == '\0' ||
			    s->peer < 0)
				continue;
			if (strcasecmp(s->local, hdr->call_to) != 0 &&
			    !axsock_call_match(s->local, hdr->call_to))
				continue;
			if (s->pid == want) {
				best = s;
				break;
			}
			if (best == NULL)
				best = s;	/* the pid is a preference */
		}
		if (best != NULL) {
			/*
			 * Our own UI frames come back addressed to us when
			 * we digipeated them, and a station is not told its
			 * own frames.  Tested here rather than in the loop
			 * port's own path, which did not have to: a frame
			 * that loop port hands back can only be one this
			 * client just sent to somebody else, whereas off the
			 * loop port the address can be our own.
			 */
			if (!agwpe_was_ours_locked(hdr->port,
						   hdr->pid ? hdr->pid : AGWPE_PID_AX25,
						   hdr->call_from, hdr->call_to,
						   data, len, 0))
				agwpe_ui_deliver_locked(best, hdr->call_from,
							data, len);
			found = 1;
		}
		goto out;
	}

	if (!found && agwpe_is_connect(hdr->datakind)) {
		/* Inbound connect: queue it on a listening socket.  An
		 * exact call match wins; a listener bound to a call with
		 * SSID 0 serves any SSID of the same base call.
		 */
		/*
		 * The pid decides as much as the callsign does.  On the air
		 * one link carries frames of several protocol ids, and a
		 * service listening for NET/ROM and one listening for text
		 * are two different listeners on one callsign - which is how
		 * the node backend has always treated it.
		 *
		 * Here the sorting has to happen on this side: AGWPE
		 * registers a callsign with 'X' and 'X' carries no pid, so
		 * the server hands its one owner everything addressed to it
		 * and leaves the choosing to the client.  Two processes
		 * therefore cannot divide one callsign by pid - only one of
		 * them owns it at the server.
		 *
		 * A plain 'C' carries no pid either, so a zero means text,
		 * the same reading session_pid() uses in ax25netd.
		 *
		 * And the pid is a preference, not a filter.  Matching it
		 * exactly and dropping the rest would lose calls that used
		 * to arrive - from an AGWPE client that fills the field
		 * differently, or simply because the only listener here is
		 * the one that was always taking them.  So: the right pid
		 * first, and a listener without a claim on this pid after
		 * that, which is what happened before there was a pid at
		 * all.
		 */
		unsigned char want = hdr->pid ? hdr->pid : AGWPE_PID_AX25;
		int pass;

		s = NULL;
		for (pass = 0; pass < 4 && s == NULL; pass++) {
			int exact = (pass % 2) == 0;	/* call: exact, then base */
			int bypid = pass < 2;		/* pid: matching, then any */

			for (s = axsock_list; s != NULL; s = s->next) {
				if (!s->listening)
					continue;
				if (bypid && s->pid != want)
					continue;
				if (exact) {
					if (strcasecmp(s->local,
						       hdr->call_to) == 0)
						break;
				} else if (axsock_call_match(s->local,
							     hdr->call_to)) {
					break;
				}
			}
			if (s != NULL && !bypid && s->pid != want) {
				static int said;

				if (!said) {
					said = 1;
					fprintf(stderr,
						"axsock: no listener for pid 0x%02x on %.*s - giving the call to the one for 0x%02x\n",
						want, AGWPE_MAX_CALL,
						hdr->call_to, s->pid);
				}
			}
		}
		if (axsock_debug)
			fprintf(stderr, "axsock: inbound connect to '%.*s' -> listener %s (listening=%d peer=%d)\n",
				AGWPE_MAX_CALL, hdr->call_to,
				s != NULL ? s->local : "(none)",
				s != NULL ? s->listening : -1,
				s != NULL ? s->peer : -1);
		if (s != NULL) {
			struct axsock_sock *n;

			n = axsock_alloc_sock_locked(SOCK_SEQPACKET);
			if (n != NULL) {
				axsock_copy_call(n->local, hdr->call_to);
				axsock_copy_call(n->remote, hdr->call_from);
				n->port = s->port;
				n->state = AXSOCK_CONNECTED;
				n->pend_next = s->pending;
				s->pending = n;

				/* Wake up a select()/poll() on the listening
				 * socket: one marker byte per pending
				 * connection, drained again in accept().
				 */
				if (s->peer >= 0) {
					unsigned char m = 0;
					ssize_t nw;

					nw = real_write(s->peer, &m, 1);
					if (axsock_debug)
						fprintf(stderr,
							"axsock: marker write peer=%d (read-end %d) -> %zd\n",
							s->peer, s->fd, nw);
				}
			}
		}
	}
out:
	pthread_mutex_unlock(&axsock_lock);
}

static int axsock_link_up(agwpe_client_t **cp, int *errp);
static void axsock_link_publish(agwpe_client_t *c);

/*
 * Background thread: bring the AGWPE connection up, wait for frames from the
 * server and dispatch them, and bring it back up when it goes away.
 *
 * It is the one owner of the connection.  That is the whole point of it being
 * a loop rather than a straight line: it used to return when the link was
 * lost, and nothing in the library ever started it again - a program that
 * only reads (listen(1), mheardd(8), ax25mond(8)) has no call path that
 * brings the link back, so after one "systemctl restart ax25netd" every
 * monitor in the suite was waiting on a socket whose reader had gone away,
 * with a zero-length read to show for it.  Restarting every service by hand
 * was the workaround.
 *
 * Sessions still get EOF when the link goes: a connection that cannot be
 * carried is a connection that ended, and a program that is transferring has
 * to hear that rather than sit in read() until its process is killed.
 */
static void *axsock_reader(void *arg)
{
	sigset_t set;
	unsigned backoff = 0;
	int up = 0;

	(void)arg;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	for (;;) {
		int wait;

		if (!up) {
			unsigned nap = 0;
			agwpe_client_t *c = NULL;
			int e = 0;

			/*
			 * Connecting happens without axsock_lock.  It is
			 * the one slow thing this library does - a name
			 * that has to be resolved, a TCP connect that
			 * waits out its timeout - and holding the global
			 * lock across it would stop every other socket
			 * call in the process, including the ones of a
			 * WAMPES node that is working perfectly well and
			 * has nothing to do with whether ax25netd is up.
			 * The lock protects the table and the client, not
			 * the way to the server; the connector must not pay
			 * for a monitor's best effort.
			 */
			if (axsock_link_up(&c, &e) == 0) {
				pthread_mutex_lock(&axsock_lock);
				axsock_link_publish(c);
				up = 1;
				backoff = 0;
				pthread_cond_broadcast(&axsock_cond);
				pthread_mutex_unlock(&axsock_lock);
				if (axsock_debug)
					fprintf(stderr, "axsock: link to the "
						"server is up again\n");
				continue;
			}

			/*
			 * Nobody waiting for the link gets its answer by
			 * the backoff ending, so the waiters have to be
			 * released here, on both answers - a caller that
			 * waits out its full timeout for an answer that
			 * has already come is ten seconds of nothing.
			 */
			pthread_mutex_lock(&axsock_lock);
			axsock_err = e;
			pthread_cond_broadcast(&axsock_cond);
			pthread_mutex_unlock(&axsock_lock);

			if (axsock_debug && backoff == 0)
				fprintf(stderr, "axsock: no server at %s: %s\n",
					axsock_server_name(), strerror(e));

			nap = 1u << (backoff < 5 ? backoff : 5);
			if (backoff < 5)
				backoff++;
			sleep(nap);
			continue;
		}

		/* Blocking wait as before while nothing is queued.  With a
		 * queue there is a second thing to wait for - the peer end
		 * becoming writable - and it is not on this select, so come
		 * back regularly instead.  The cost falls only on the case
		 * that used to lose the data. */
		wait = __atomic_load_n(&axsock_npending, __ATOMIC_RELAXED)
			? 20 : -1;

		if (agwpe_client_pump(axsock_agwpe, wait) < 0) {
			struct axsock_sock *s;

			pthread_mutex_lock(&axsock_lock);
			axsock_up = 0;
			for (s = axsock_list; s != NULL; s = s->next) {
				if (s->state == AXSOCK_CONNECTING) {
					s->connect_err = ECONNRESET;
					s->state = AXSOCK_NEW;
				} else if (s->state == AXSOCK_CONNECTED) {
					/* Not axsock_peer_close_locked(): this
					 * thread is the one that would come
					 * back to finish the queue, and it is
					 * about to leave the pump.  Push
					 * what fits, let the rest go, and
					 * close - a socket that never reaches
					 * EOF would be worse than a short
					 * one. */
					axsock_peer_flush_locked(s);
					if (s->plen > 0)
						fprintf(stderr,
							"axsock: link to the server lost with %zu bytes still undelivered to %.*s\n",
							s->plen,
							AGWPE_MAX_CALL,
							s->remote);
					axsock_peer_discard_locked(s);
					axsock_peer_shutdown_locked(s);
					s->state = AXSOCK_NEW;
				}
			}
			/* The port table belongs to the server and does not
			 * survive it: after a restart it can name other
			 * upstreams, or other channels of the same one, so
			 * asking for it again is the only way to find out.
			 * Leaving the old numbers in place sent frames on
			 * a port that now belongs to a different radio. */
			axsock_gnports = -1;
			pthread_cond_broadcast(&axsock_cond);
			pthread_mutex_unlock(&axsock_lock);
			up = 0;
			continue;
		}
		if (__atomic_load_n(&axsock_npending, __ATOMIC_RELAXED))
			axsock_flush_pending();
	}

	/*
	 * Only reached if the loop above ever leaves, which it cannot: every
	 * failure either sleeps and continues or comes back up.  Here because
	 * the compiler is right that the function promises a return value and
	 * would otherwise have to take the promise on trust.
	 */
	return NULL;
}

/*
 * Drop the current AGWPE client (called with the lock held, on the way
 * to establishing a new one).  A client that a sender is still using
 * outside the lock is retired instead of freed; the last reference
 * holder reaps it in axsock_client_release().  If the retired list is
 * full the client is freed anyway - reconnects must not be blocked on a
 * leak far rarer than the list can ever be asked to absorb.
 */
static void axsock_client_retire(void)
{
	if (axsock_client_refs == 0 || axsock_nretired >= 4)
		agwpe_client_free(axsock_agwpe);
	else
		axsock_retired[axsock_nretired++] = axsock_agwpe;
	axsock_agwpe = NULL;
}

/*
 * Build the AGWPE connection.  Called with no lock held.
 *
 * Resolve the server, connect, log in.  All of it before the client is
 * published, and none of it under axsock_lock, for the reason the caller
 * gives: this is the slow part, the part that waits for a name and for a
 * TCP handshake, and a connector on the other backend must not be made to
 * wait for it.  On a host whose ax25netd is down and whose WAMPES node is
 * up, that is the whole difference between a working AX.25 link and a
 * process whose every socket call queues behind a connect that goes nowhere.
 *
 * On success *cp is the new client, still private to this thread: nothing
 * else can reach it and axsock_up is still 0, so nothing is sent on it
 * before axsock_link_publish().  On failure *errp says why and *cp stays
 * NULL.
 */
static int axsock_link_up(agwpe_client_t **cp, int *errp)
{
	const struct agwpe_client_cb cb = {
		.raw_frame = axsock_dispatch,
	};
	agwpe_client_t *c;

	*cp = NULL;
	*errp = ECONNREFUSED;

	axsock_resolve_server();

	c = agwpe_client_new(&cb, NULL);
	if (c == NULL) {
		*errp = ENOMEM;
		return -1;
	}

	if (axsock_transport_connect(c) != 0) {
		int e = agwpe_client_err(c);

		agwpe_client_free(c);
		*errp = (e != 0) ? e : ECONNREFUSED;
		return -1;
	}

	/*
	 * If the server requires login (ax25netd started with 'auth'),
	 * present AXSOCK_USER/AXSOCK_PASSWORD now; everything else the
	 * server receives before a successful login is ignored.
	 */
	{
		const char *user = getenv("AXSOCK_USER");
		const char *pass = getenv("AXSOCK_PASSWORD");

		if (user != NULL && user[0] != '\0')
			agwpe_client_login(c, user,
					   (pass != NULL) ? pass : "");
	}

	*cp = c;
	return 0;
}

/*
 * Take a connection built by axsock_link_up() into use.  Called with the lock
 * held, which it does not release; the client is consumed either way.
 *
 * The work the reader used to be told to do by whoever started it: put the new
 * client in place of the old one, then re-register everything this process had
 * registered with the server before.  A reconnect runs the same path, which is
 * what makes the 'k' toggle and the registrations come back by themselves -
 * both are state of the connection, and a new connection starts without
 * either.
 *
 * Registering does write to the connection, so this is the one place left that
 * touches a blocking socket under the lock.  It is a frame of a few bytes into
 * a connection that was just accepted, which is the one moment a write cannot
 * block: the peer's receive window is there for exactly this.  The connect
 * that could take seconds, and the backoff sleeps that follow it when there is
 * no server at all, are outside - see axsock_link_up() and axsock_reader().
 */
static void axsock_link_publish(agwpe_client_t *c)
{
	if (axsock_up) {
		/* Cannot happen: only this thread builds a connection and
		 * it builds one at a time.  Better to drop the new one
		 * than to leave two, though. */
		agwpe_client_free(c);
		return;
	}

	if (axsock_agwpe != NULL)
		axsock_client_retire();

	axsock_agwpe = c;
	axsock_up = 1;
	axsock_err = 0;

	axsock_register_all_locked();
	/*
	 * The UI subscription is state of the connection too, and a new
	 * connection starts without it - the same mistake the 'k' toggle
	 * was, and with the same consequence: the datagram socket keeps
	 * its registration, so it looks healthy, and receives nothing,
	 * because the 'M' frames it is now waiting for are never sent.
	 */
	if (axsock_nuisub > 0)
		agwpe_client_uisub(c);
	/*
	 * Raw monitoring is a toggle on the connection, and a new
	 * connection starts with it off.  axsock_nraw still counts the
	 * monitor sockets that never closed, so the guards at the
	 * socket() and bind() sites do not send the toggle again, and
	 * register_all_locked() only walks the registrations.  Without
	 * this, one lost link to the server left every monitor silent
	 * for the rest of the process's life: the link came back, the
	 * 'k' never did, and no 'K' frame arrived to hand out.  The
	 * payload mask goes with it, for the same reason and with the
	 * same mistake waiting - see axsock_mon_state_locked().
	 */
	axsock_mon_state_locked();
}

/*
 * Start axsock_reader() if it is not already running.  Called with the lock
 * held.  Does not wait for anything: it only makes sure there is somebody who
 * will bring the link up, now or after the next backoff.
 */
static int axsock_reader_start_locked(void)
{
	if (axsock_thread_alive)
		return 0;

	axsock_thread_alive = 1;
	if (pthread_create(&axsock_thread, NULL, axsock_reader, NULL) != 0) {
		axsock_thread_alive = 0;
		errno = EAGAIN;
		return -1;
	}
	return 0;
}

/*
 * Make sure the AGWPE connection is up.  Called with the lock held.
 *
 * Starts axsock_reader() if it is not running and then waits for it to have
 * the link.  The reader owns the connection, so this cannot connect itself
 * without two threads racing to own one socket - and waiting is what lets a
 * program whose only business is receiving (listen(1), mheardd(8),
 * ax25mond(8)) have a link restored under it without asking.
 *
 * Used where the link is not optional: a connect(2) or a listen(2) that has
 * nothing to carry without it.  A monitor asks for less and gets less - see
 * agwpe_mon_open(), which starts the reader and hands out a quiet descriptor
 * rather than making the program that wants to watch the air wait for it.
 */
static int axsock_ensure_locked(void)
{
	if (axsock_up)
		return 0;

	if (axsock_reader_start_locked() != 0)
		return -1;

	while (!axsock_up) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += AXSOCK_ENSURE_TIMEOUT;
		if (pthread_cond_timedwait(&axsock_cond, &axsock_lock, &ts) != 0)
			break;
	}

	if (!axsock_up) {
		errno = (axsock_err != 0) ? axsock_err : ECONNREFUSED;
		return -1;
	}
	return 0;
}

/*
 * Send one data frame on an established connection.  The AGWPE TCP
 * connection is blocking, so the send is done without axsock_lock - a
 * full TCP buffer must not stall every other socket call in the
 * process.  The client pointer is snapshotted and referenced under the
 * lock (so ensure_locked() cannot free it), all per-socket fields are
 * copied to locals, and the lock is dropped only for the write itself.
 * Called with the lock held, returns with it held.
 */
static ssize_t axsock_send_data(struct axsock_sock *s,
				const void *buf, size_t len)
{
	unsigned char port, pid;
	char local[AGWPE_MAX_CALL], remote[AGWPE_MAX_CALL];
	agwpe_client_t *cl;
	int rc;

	if (s->state != AXSOCK_CONNECTED) {
		errno = ENOTCONN;
		return -1;
	}
	cl = axsock_client_acquire();
	if (cl == NULL) {
		errno = ENOTCONN;
		return -1;
	}

	/* Snapshot everything the send needs before the lock goes down;
	 * another thread may close the socket and free s meanwhile.  */
	port = s->port;
	pid = s->pid;
	memcpy(local, s->local, AGWPE_MAX_CALL);
	memcpy(remote, s->remote, AGWPE_MAX_CALL);

	pthread_mutex_unlock(&axsock_lock);
	rc = agwpe_client_send_data(cl, port, pid, local, remote,
				    (const unsigned char *)buf, (int)len);
	pthread_mutex_lock(&axsock_lock);
	axsock_client_release();

	if (rc != 0) {
		errno = (agwpe_client_err(cl) != 0) ?
			agwpe_client_err(cl) : EIO;
		return -1;
	}
	return (ssize_t)len;
}

/*
 * Send a DGRAM (unproto) frame, releasing axsock_lock around the TCP
 * write for the same reason as axsock_send_data.  Called with the lock
 * held, returns with it held.  digis may be NULL / ndigis 0.
 */
static int axsock_send_unproto(struct axsock_sock *s, const char *target,
			       const char *const *digis, int ndigis,
			       const void *buf, size_t len)
{
	unsigned char port, pid;
	char local[AGWPE_MAX_CALL];
	agwpe_client_t *cl;
	int rc;

	cl = axsock_client_acquire();
	if (cl == NULL) {
		errno = ENOTCONN;
		return -1;
	}

	port = s->port;
	pid = s->pid;
	memcpy(local, s->local, AGWPE_MAX_CALL);

	/* Remembered before the lock goes, because the ring is under it: the
	 * copy that comes back on the monitor stream is told from somebody
	 * else's traffic by this - see agwpe_was_ours_locked().  Noting a
	 * frame the send then fails to deliver costs nothing: the entry is
	 * overwritten in eight frames, and until then it could only swallow
	 * an identical frame from the same pair on the same port. */
	agwpe_note_sent_locked(port, pid, local, target, buf, len);

	pthread_mutex_unlock(&axsock_lock);

	if (ndigis > 0)
		rc = agwpe_client_send_unproto_via(cl, port, pid, local,
						   target, digis, ndigis,
						   (const unsigned char *)buf,
						   (int)len);
	else
		rc = agwpe_client_send_unproto(cl, port, pid, local, target,
					       (const unsigned char *)buf,
					       (int)len);
	pthread_mutex_lock(&axsock_lock);
	axsock_client_release();

	if (rc != 0) {
		errno = (agwpe_client_err(cl) != 0) ?
			agwpe_client_err(cl) : EIO;
		return -1;
	}
	return 0;
}

static void axsock_disconnect_locked(struct axsock_sock *s)
{
	if (s->state == AXSOCK_CONNECTED && axsock_up &&
	    axsock_agwpe != NULL)
		agwpe_client_disconnect(axsock_agwpe, s->port,
					s->local, s->remote);
	s->state = AXSOCK_NEW;
}

/*
 * Tear down a connected socket whose peer socketpair lost all its
 * readers (the application closed its end or a remote disconnect tore
 * the session down): tell ax25netd about the disconnect and release
 * the router end of the socketpair.  The application still owns its
 * end (s->fd), so only s->peer is closed here.
 */
static void axsock_peer_reader_teardown(struct axsock_sock *s)
{
	struct axsock_sock **pp;

	if (s->state == AXSOCK_CONNECTED && axsock_up &&
	    axsock_agwpe != NULL)
		agwpe_client_disconnect(axsock_agwpe, s->port,
					s->local, s->remote);

	for (pp = &axsock_list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	__atomic_sub_fetch(&axsock_nsock, 1, __ATOMIC_RELAXED);

	axsock_peer_shutdown_locked(s);

	/*
	 * The application's end is not ours to close, and this closed it -
	 * against what the comment above has always said.  Two things came
	 * of that.  Whatever the application had not read yet went with the
	 * descriptor, so a session that ended while the reader was behind
	 * lost its tail; and the number was handed back to the process while
	 * the application still held it, so the next open() could be given
	 * the same one and the application would then be reading somebody
	 * else's file.
	 *
	 * Closing only the router end gives the application EOF once it has
	 * read what arrived, which is what a socket does.  Its own close()
	 * finds nothing in the table by then and falls through to the real
	 * one, which is right: there is nothing left here to clean up.
	 */
	axsock_peer_discard_locked(s);
	free(s);
}

/*
 * Reader thread for an accepted connection: forwards outbound data the
 * application writes into the socketpair as 'D' frames to ax25netd.
 * EOF/error on the router end means the connection is over (the child
 * closed its stdin/stdout copies, or a remote disconnect was signalled
 * by the reader thread closing s->peer) - tear the session down.
 */
static void *axsock_peer_reader(void *arg)
{
	struct axsock_sock *s = arg;
	char buf[2048];
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	for (;;) {
		fd_set rfds;
		ssize_t n;

		/* The router end is non-blocking (the reader thread writes
		 * inbound data into it), so wait for readability first.
		 */
		FD_ZERO(&rfds);
		FD_SET(s->peer, &rfds);
		if (select(s->peer + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		n = real_read(s->peer, buf, sizeof(buf));
		if (axsock_debug)
			fprintf(stderr, "axsock: peer read n=%zd errno=%d (%s)\n",
				n, n < 0 ? errno : 0,
				n < 0 ? strerror(errno) : "");
		if (n <= 0)
			break;

		/* The outbound forward is a blocking TCP write to the AGWPE
		 * server; take the client outside axsock_lock so a slow
		 * server never stalls every other socket call (including
		 * the dispatch path that writes inbound frames into this
		 * same socketpair).
		 */
		pthread_mutex_lock(&axsock_lock);
		if (s->state == AXSOCK_CONNECTED) {
			unsigned char port = s->port, pid = s->pid;
			char local[AGWPE_MAX_CALL], remote[AGWPE_MAX_CALL];
			agwpe_client_t *cl = axsock_client_acquire();

			if (cl != NULL) {
				memcpy(local, s->local, AGWPE_MAX_CALL);
				memcpy(remote, s->remote, AGWPE_MAX_CALL);
				if (axsock_debug)
					fprintf(stderr,
						"axsock: peer forward %zd bytes to %.*s\n",
						n, AGWPE_MAX_CALL, remote);
				pthread_mutex_unlock(&axsock_lock);
				(void)agwpe_client_send_data(cl, port, pid,
							     local, remote,
							     (const unsigned char *)buf,
							     (int)n);
				pthread_mutex_lock(&axsock_lock);
				axsock_client_release();
			}
		}
		pthread_mutex_unlock(&axsock_lock);
	}

	pthread_mutex_lock(&axsock_lock);
	axsock_peer_reader_teardown(s);
	pthread_mutex_unlock(&axsock_lock);

	return NULL;
}

/* Hand a freshly made socket to another backend.
 *
 * Only ever a socket that has been created and not yet used: it is not
 * connected, not registered and has no reader thread, so there is nothing to
 * tear down but the entry itself and the router end of its pair.  The
 * descriptor the application holds stays open and keeps its number, which is
 * the whole point - the application was given that number before anybody
 * could know which port it would name.
 *
 * Returns 0 when the socket was ours and has been let go, -1 otherwise.
 */

/* What kind of AX.25 socket this is, asked by another backend before it takes
 * the descriptor over.  bind() is the first moment the port is known and thus
 * the moment of the handover, but the type was decided at socket() and lives
 * only here - and a datagram socket is served quite differently from a
 * connection.  Answers -1 for a descriptor this backend does not hold.
 */
int agwpe_socktype(int fd)
{
	struct axsock_sock *s;
	int type;

	if (!axsock_may_have_sock())
		return -1;
	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	type = (s != NULL) ? s->type : -1;
	pthread_mutex_unlock(&axsock_lock);
	return type;
}

/*
 * Do the upstream names of the local ax25netd include this one?
 *
 * ax25netd_agwpe.conf is where the upstreams of an ax25netd on this machine are named,
 * and it is the file the axports entry corresponds to.  Reading it answers the
 * question for the ordinary single-machine setup without anything on the wire,
 * which matters: a bind that had to ask the server would put a TCP connection
 * and netd's three second answer time in the way of every call on a port of
 * ours, and of every program that happens to bind one.
 *
 * Read once per process, and only if the file is there - agwpe_config_load()
 * fails quietly with ENOENT when it is not, which is the normal state of a
 * machine whose server is on the other side of the network and whose answer
 * has to come from the port table instead.  A file that exists and does not
 * parse says so, as it should: it is the server's own configuration and the
 * operator wrote it.
 *
 * The path is the compiled-in one, the same default netd reads.  A netd that
 * was pointed somewhere else is not served from here - the name is simply not
 * found, and the port table answers for it, so a shim configured for a remote
 * server keeps working.
 */
/* The position of this upstream in the local ax25netd_agwpe.conf, or -1 when the
 * file has no such upstream.  That position is the index AGWPE numbers the
 * upstream's ports from, so it is worth having: it is the only way to learn
 * a port number without asking the server, and it is exact whenever the
 * server is on this machine and therefore reads this very file.  */
static int agwpe_local_upstream(const char *base)
{
	static struct agwpe_config cfg;
	static int loaded = -1;		/* -1 not read yet, 0 no, 1 read */
	int i;

	if (loaded < 0) {
		if (agwpe_config_load(CONF_AGWPE_FILE, &cfg) < 0)
			loaded = 0;		/* no file, or unreadable */
		else
			loaded = 1;
	}
	if (!loaded)
		return -1;

	for (i = 0; i < cfg.count; i++)
		if (strcasecmp(cfg.upstreams[i].name, base) == 0)
			return i;

	return -1;
}

/*
 * Does the AGWPE server serve this port?
 *
 * The kernel's ports are not up for discussion and are asked about first: a
 * bind that names one was already answered by socket(), and taking it over
 * would put frames on a radio the process never asked for.  Only the port
 * list can say that, and it can say it wrongly - "is a kernel port" is asked
 * of interfaces that were up while it was read, so a port whose TNC is
 * unplugged, or on a machine with no ax25 module loaded at all, looks exactly
 * like a port of a node.  Every port there is one.  Ownership decided that way
 * is decided by a cable, and a port meant for the local stack was handed to
 * netd and sent out of the AGWPE upstream instead.
 *
 * So the rest comes from the configuration.  The name is compared the way the
 * rest of the code compares it: an optional "agwpe-" marker and a ":N"
 * channel suffix come off first, since the prefix only ever said which
 * upstream of several was meant, and the suffix which of its channels.  The
 * virtual loopback upstream is ours by construction wherever it is named,
 * because axsock_port_for() sends it there and the two answers have to agree.
 *
 * The local ax25netd_agwpe.conf decides the ordinary case; the server's own port table
 * is asked only for a name the file does not have, which is the machine whose
 * server is elsewhere.  A name neither knows is not ours, and the descriptor
 * goes back to bind() to be refused in the kernel's words - the port is
 * either mistyped or meant for a stack that has no such port, and both are
 * better said than guessed at.
 */
static int agwpe_owns_port(const char *name)
{
	const char *base;
	char n[32];
	const char *colon;
	size_t len;

	if (ax25_config_port_is_kernel(name))
		return 0;

	colon = strrchr(name, ':');
	len = colon ? (size_t) (colon - name) : strlen(name);
	if (len == 0 || len >= sizeof(n))
		return 0;
	memcpy(n, name, len);
	n[len] = '\0';
	base = axsock_strip_prefix(n);

	if (strcasecmp(base, "loop") == 0)
		return 1;
	if (agwpe_local_upstream(base) >= 0)
		return 1;

	if (axsock_gnports < 0 && axsock_ports_fetch() != 0) {
		if (axsock_debug)
			fprintf(stderr, "axsock: no AGWPE port table at %s:%d "
				"and no upstream '%s' in %s, so port '%s' is "
				"not ours\n", axsock_server_name(), axsock_port, base,
				CONF_AGWPE_FILE, name);
		return 0;
	}

	return axsock_gport_channel(base, 0) >= 0;
}

int agwpe_forget(int fd)
{
	struct axsock_sock *s, **pp;
	int peer;

	if (!axsock_may_have_sock())
		return -1;
	pthread_mutex_lock(&axsock_lock);
	for (pp = &axsock_list; (s = *pp) != NULL; pp = &s->next)
		if (s->fd == fd)
			break;
	if (s == NULL || s->state != AXSOCK_NEW || s->registered || s->raw) {
		pthread_mutex_unlock(&axsock_lock);
		return -1;
	}
	*pp = s->next;
	__atomic_sub_fetch(&axsock_nsock, 1, __ATOMIC_RELAXED);
	peer = s->peer;
	axsock_peer_discard_locked(s);
	free(s);
	pthread_mutex_unlock(&axsock_lock);
	if (peer >= 0)
		real_close(peer);
	if (axsock_debug)
		fprintf(stderr, "axsock: fd=%d handed to another backend\n", fd);
	return 0;
}

/*
 * socket() is the one entry point that cannot ask "is this yours?": there
 * is no descriptor yet, and no callsign, so no port and no backend either.
 * It chooses instead, and the two backends are constructors here rather
 * than the pair of askers they are everywhere else.  What would remove the
 * exception is described in doc/TODO.md - hand out a placeholder here and
 * let bind() put the real descriptor over it - and it is not this step.
 */
int agwpe_socket(int type, int protocol)
{
	if (type != SOCK_SEQPACKET && type != SOCK_DGRAM &&
	    type != SOCK_RAW) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	return axsock_new_sock(type, protocol);
}

/*
 * The SOCK_PACKET monitor, which is the one thing socket() builds that is not
 * an AX.25 socket: every raw frame heard on any upstream is delivered to it,
 * fed from the AGWPE 'K' records the server sends once raw monitoring is on.
 *
 * Always available, whichever backend this process was given for AF_AX25.
 *
 * That is a change, and the reason is that the question this function used to
 * answer - "is the process on the kernel or on a server" - is not the question
 * a raw monitor asks.  It asks "what is being heard", and on a machine that
 * has both a kernel AX.25 stack and an ax25netd the answer is two sources, of
 * which the kernel one used to win by default and the server one was then not
 * offered at all.  So listen(1) on such a host showed the kernel's ports and
 * nothing else: no ax25netd radio, and no WAMPES traffic either, since that is
 * mirrored into ax25netd's monitor channel.
 *
 * The monitor socket is now opened here on purpose, next to the kernel one,
 * by axmon_open().  socket() still hands out one descriptor and still prefers
 * the kernel, because a program that says socket(PF_PACKET, SOCK_PACKET) has
 * asked for a packet socket and gets the one the kernel gives it; asking for
 * both is a new request and gets a new answer.
 *
 * SOCK_PACKET is the marker, not the address family: ax25-apps/listen asks for
 * AF_PACKET, ax25-tools/kiss/net2kiss for AF_INET, which is how one did this
 * before Linux 2.2 and how that program still does it.  Both mean the same
 * thing here.
 *
 * It sits on this side because everything it needs does - the table, the lock,
 * the raw counter - and while it stood in the chooser it was the last place an
 * entry point reached into the backend.
 *
 * Returns 0 for anything that is not a monitor socket, including on a kernel
 * backend, where the packet socket is real and the call belongs to the OS.
 */
int agwpe_mon_open(int protocol, unsigned char mask, int *ret)
{
	struct axsock_sock *s;
	int fd;

	/*
	 * What this program wants to see, over AGWPE_MONMASK_*, before
	 * anything opens: the state is sent the moment the first monitor
	 * socket exists, and again whenever the link comes back, so a mask
	 * that arrives after the toggle would be a mask that was not there
	 * for the frames that came first.
	 *
	 * One value per connection, not per socket, and the connection is
	 * the program's.  Two monitors in one process with two different
	 * wishes have no answer on this wire - the 'k' toggle is one bit
	 * for the same reason - so the later one is taken and both get it.
	 */
	axsock_mon_mask = (unsigned char)(mask & AGWPE_MONMASK_ALL);

	/* An AGWPE server feeds this socket.  Ask for one first, because a
	 * machine can have both an AGWPE server and a node, and there a
	 * monitor works.
	 *
	 * Neither backend can do without it now: AGWPE frames reach the
	 * monitor through ax25netd exactly as before, and WAMPES frames are
	 * pushed into the same ax25netd monitor channel by the libax25 side
	 * itself, so ax25netd has to be reachable there too.  Either way an
	 * unreachable server means the monitor cannot be fed.
	 *
	 * Still hand out a descriptor that stays quiet rather than failing,
	 * so a program that opens a monitor beside its real work is not
	 * taken down by the absence of a server.  It costs that program one
	 * feature and leaves the rest working.  With no node configured
	 * either, the refusal is a configuration fault and worth reporting
	 * as one.
	 *
	 * Neither case waits for the server.  A monitor is worth having and
	 * not worth a program that stands still for it: it starts the
	 * reader, hands out the descriptor, and starts receiving whenever
	 * the link is there - which, with the reader's backoff, is also
	 * whenever ax25netd comes back.  That is the other half of the rule
	 * this whole path follows: what a monitor wants is optional, and
	 * what a connector needs is not.  A WAMPES node on the same machine
	 * shares this library, this lock and this reader, and must keep
	 * working while the server it does not use is absent.
	 *
	 * This is the one place the AGWPE backend looks at the other one's
	 * register, and it is not an oversight: whether a failure here should
	 * be fatal or quiet depends on whether the other world exists at all.
	 * The question belongs to neither backend alone, and it is asked in
	 * the one place where the answer changes what happens.
	 */
	pthread_mutex_lock(&axsock_lock);
	if (axsock_reader_start_locked() != 0) {
		pthread_mutex_unlock(&axsock_lock);
		*ret = -1;
		return 1;
	}
	if (!axsock_up && !wampes_configured()) {
		/* Nobody to feed the monitor and nothing to feed it with:
		 * not a server, not a node.  Saying so beats a program
		 * that shows nothing for ever and says why not. */
		pthread_mutex_unlock(&axsock_lock);
		*ret = -1;
		return 1;
	}
	if (!axsock_up) {
		/* Not yet, and possibly not for a while.  The descriptor
		 * is the same one a fed monitor gets and starts working by
		 * itself when the reader gets the link up. */
		static int said;

		s = axsock_alloc_sock_locked(SOCK_PACKET);
		if (s == NULL) {
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}
		s->raw = 1;
		axsock_nraw++;
		fd = s->fd;
		if (axsock_debug && !said) {
			said = 1;
			/* Two different things, told apart because they need
			 * different answers: nothing names a server, which is
			 * a configuration fault and stays one, and a server
			 * that is named and not there yet, which the reader
			 * goes on retrying. */
			axsock_resolve_server();
			if (axsock_host == NULL)
				fprintf(stderr, "axsock: no AGWPE server is "
					"configured, so the monitor has no source - "
					"see ax25common.conf(5) or AXSOCK_HOST\n");
			else
				fprintf(stderr, "axsock: no AGWPE server at %s "
					"yet - the monitor starts receiving "
					"when there is one\n", axsock_host);
		}
		pthread_mutex_unlock(&axsock_lock);
		*ret = fd;
		return 1;
	}
	s = axsock_alloc_sock_locked(SOCK_PACKET);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		*ret = -1;
		return 1;
	}
	s->raw = 1;
	/* 'k' is a toggle: enable it when the first monitor socket opens,
	 * disable it again when the last one closes.  The payload mask
	 * travels with it.  */
	if (axsock_nraw++ == 0)
		axsock_mon_state_locked();
	fd = s->fd;
	pthread_mutex_unlock(&axsock_lock);
	if (axsock_debug)
		fprintf(stderr, "axsock: SOCK_PACKET monitor fd=%d proto=0x%x "
			"mask=0x%02x\n", fd, protocol, axsock_mon_mask);
	*ret = fd;
	return 1;
}

int agwpe_socket_packet(int domain, int type, int protocol, int *ret)
{
	if (type != SOCK_PACKET ||
	    (domain != AF_PACKET && domain != AF_INET))
		return 0;
	if (axsock_backend_now() == 1)
		return 0;
	return agwpe_mon_open(protocol, axsock_mon_mask, ret);
}

/*
 * A socket entry for a descriptor number this backend did not hand out.
 *
 * axsock_alloc_sock_locked() builds the pair and keeps the application end
 * for itself; here the application already holds a number and only the pair is
 * new.  So the pair's own application end is closed and the given number takes
 * its place, which leaves the router end - the one the dispatcher writes
 * inbound frames into and reads outbound frames from - exactly as it was.
 * Call with the lock held.
 */
static struct axsock_sock *axsock_adopt_sock_locked(int fd, int type)
{
	struct axsock_sock *s, **pp;
	int old;

	if ((s = axsock_alloc_sock_locked(type)) == NULL)
		return NULL;

	/* The pair's read end has to be put behind the number the application
	 * holds, not merely renumbered in this table.  agwpe_bind_take() has
	 * left an unbound placeholder there, and a descriptor that stands for
	 * an unbound unix socket cannot be read: read(2) answers EINVAL on it
	 * while poll(2) reports it readable all the same.  A client on a
	 * machine with a kernel stack therefore connected over AGWPE and then
	 * failed its very first read, and closed a session that was up - which
	 * is what call(1) reported, after a successful connect.
	 *
	 * The entry is renamed before the pair's end is closed, not after.
	 * axsock_replace() closes a descriptor, and close() is one of the
	 * calls this shim interposes: while s->fd still names that end, the
	 * interposed close() finds this very entry and tears it down.  The
	 * socket then leaves axsock_list while the caller carries on with it,
	 * and the application's next setsockopt() falls through to the kernel,
	 * which answers EOPNOTSUPP for a level it does not know.  On a machine
	 * that interposes through its linker (ELF) the entry must be renamed
	 * first, so that the close() has nothing to find and only the raw
	 * descriptor goes. */
	old = s->fd;
	s->fd = fd;
	if (axsock_replace(fd, old) != 0) {
		int e = errno;

		for (pp = &axsock_list; *pp != NULL; pp = &(*pp)->next)
			if (*pp == s)
				break;
		if (*pp != NULL)
			*pp = s->next;
		__atomic_sub_fetch(&axsock_nsock, 1, __ATOMIC_RELAXED);
		real_close(s->peer);	/* the read end went with the failed replace */
		axsock_peer_discard_locked(s);
		free(s);
		errno = e;
		return NULL;
	}
	return s;
}

/*
 * A bind named a port in axports and no backend turned out to serve it.
 *
 * Both shapes of that answer are here.  A socket the shim made itself has
 * its port resolved in agwpe_bind(); a descriptor a program opened on a
 * machine that has a kernel stack is only claimed in agwpe_bind_take(),
 * and the kernel's EADDRNOTAVAIL is what a program sees when it is not.
 * Only one of the two happens on any given machine, so the note has to
 * come from both or it appears on exactly the hosts that need it least.
 *
 * The kernel's answer is the documented one - a port nobody serves is
 * refused by whoever is left, and EADDRNOTAVAIL is the truth about a
 * mistyped port.  What it cannot say is why: it names the callsign, which
 * is right, and not the mistake.
 *
 * Which mistake it is, is not knowable here, and the temptation is to guess:
 * a name with a colon looks like a WAMPES "node:port" and a name without one
 * looks like an AGWPE upstream, so each can be blamed on a file.  But both
 * backends take a colon - WAMPES reads "node:port", AGWPE reads
 * "upstream:channel" - so the shape does not tell them apart, and a name
 * belongs to whichever file happens to list it.  A guess would be wrong in
 * exactly the case the operator needs reading: a typo in the base name of a
 * WAMPES port reads as an AGWPE channel, and vice versa.  So this reports
 * the checks that ran and stops there, and leaves the two files to the
 * operator, who knows which one they were writing.
 */
static void axsock_port_unserved(const struct sockaddr *addr, socklen_t len)
{
	char port[32];

	if (ax25_config_bind_port(addr, len, port, sizeof(port)) == 0) {
		/* The entry exists, so this is the one case where the name is
		 * still known.  All three backends have now said no to it, and
		 * each of those was a lookup that ran, so each can be stated. */
		fprintf(stderr,
			"axsock: no backend serves the port '%s': it is no AX.25 "
			"interface, no wampes.conf node, and no AGWPE upstream or "
			"server port.\n", port);
		return;
	}

	/* No entry at all.  The name died with the lookup that would have
	 * recovered it and only the callsign is left, so nothing can be said
	 * about its shape - and nothing was asked of the kernel or of a
	 * server either, so this must not claim to know those. */
	if (axsock_addr_names_port(addr, len))
		fprintf(stderr,
			"axsock: this bind names the port '%s', but no entry in "
			"%s has that callsign, so no port table, no WAMPES "
			"node and no AX.25 interface can match it.  A program "
			"that names a port has to name one of these.\n",
			ax25_ntoa(&((const struct full_sockaddr_ax25 *) addr)
				  ->fsa_digipeater[0]),
			CONF_AXPORTS_FILE);
}

/*
 * Take a descriptor over that socket() gave to somebody else.
 *
 * bind() is the first moment the port is known, and therefore the first moment
 * the backend can be chosen per port rather than per process - the one
 * comment in axsock.c has been promising for both backends since a node could
 * do it and the server could not.  socket() had to settle the question for
 * the whole process before the port was known, and on a machine that has a
 * kernel stack the answer is always the kernel, so a program opening an AGWPE
 * port there got a kernel socket and handed it to a kernel that has no such
 * port: bind() answered EADDRNOTAVAIL, which says nothing about where the port
 * really is.
 *
 * The port belongs to us when the kernel does not have it.  Its name comes from
 * the port list, not from the address: the address carries a callsign, and it
 * was compared with a table of names here, so an AGWPE port of the user's was
 * never recognised and bind() left the descriptor to a kernel that has no
 * such port.  ax25_config_bind_port() resolves the address to the axports
 * name, the same way wampes_bind() does it, and the verdict is the one bit the
 * port list remembered.
 */
static int agwpe_bind_take(int fd, const struct sockaddr *addr, socklen_t len,
			   struct axsock_sock **sp, int *ret)
{
	char port[32];
	int type;

	if (!axsock_is_ax25(addr, len))
		return 0;			/* not ours, and not AX.25 */

	/* Which port the address names, as a name in axports: the digipeater
	 * slot first, the source callsign where there is none, and a lazy
	 * "base:suffix" where one was resolved through the hook. */
	if (ax25_config_bind_port(addr, len, port, sizeof(port)) != 0) {
		/* Nothing in axports answers.  A program that named a port
		 * and got this far has named one that does not exist: the
		 * digipeater slot is where the suite puts the callsign of an
		 * axports entry, and no entry carries this one.  call(1) has
		 * already refused it with "invalid port setting", so a bind
		 * that arrives here is a program that named a port behind
		 * the suite's back, and a kernel that has no such interface
		 * answers EADDRNOTAVAIL for a reason that has nothing to do
		 * with the port.  Refuse it here, where the name is still
		 * known, rather than pass it on and be right by accident. */
		if (axsock_addr_names_port(addr, len)) {
			axsock_port_unserved(addr, len);
			errno = EADDRNOTAVAIL;
			*ret = -1;
			return 1;		/* ours to refuse */
		}
		/* No port was named: the program bound a source callsign
		 * that is nobody's port, which the kernel may have an
		 * interface for.  Leave that to the kernel. */
		return 0;
	}
	if (ax25_config_port_is_kernel(port)) {
		if (axsock_debug)
			fprintf(stderr, "axsock: fd=%d stays with the kernel, "
				"port '%s' is a kernel port\n", fd, port);
		return 0;
	}
	if (!agwpe_owns_port(port)) {
		axsock_port_unserved(addr, len);
		return 0;
	}

	/* What kind of socket this is was decided at socket() and the
	 * descriptor still knows; after the placeholder below nothing does.
	 * Only the two kinds this backend serves are taken - a raw or
	 * packet socket bound to a port of ours is none of our business, and
	 * the kernel's answer to it is a better one than ours would be. */
	if ((type = axsock_fdtype(fd)) != SOCK_SEQPACKET && type != SOCK_DGRAM)
		return 0;

	/* The kernel socket leaves the number now and an inert one takes its
	 * place, so that from here on the application is talking to us and
	 * not also holding a live socket for a port the server serves. */
	*ret = -1;
	if (axsock_placeholder(fd) != 0)
		return 1;

	pthread_mutex_lock(&axsock_lock);
	*sp = axsock_adopt_sock_locked(fd, type);
	pthread_mutex_unlock(&axsock_lock);
	if (*sp == NULL)
		return 1;			/* errno from the allocator */

	if (axsock_debug)
		fprintf(stderr, "axsock: fd=%d taken over for port '%s'\n",
			fd, port);
	*ret = 0;
	return 0;				/* ours now: carry on below */
}

int agwpe_bind(int fd, const struct sockaddr *addr, socklen_t len,
		      int *ret)
{
	const struct sockaddr_ax25 *sa;
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL && agwpe_bind_take(fd, addr, len, &s, ret))
		return 1;			/* taken over, or refused with *ret */
	if (s == NULL)
		return 0;			/* a port of the kernel's */

	/* SOCK_PACKET monitor (ax25-apps/listen -p, net2kiss -i): the app
	 * binds it to a device name (the name sits in sa_data; the family is
	 * AF_PACKET or, for net2kiss, AF_INET - see socket() above).  There
	 * is no packet socket to bind on macOS/BSD, so just remember the
	 * device and filter the raw stream in dispatch.  */
	if (s->raw) {
		const struct sockaddr *sa0 = (const struct sockaddr *)addr;

		if (addr == NULL || len < sizeof(*sa0) ||
		    (sa0->sa_family != AF_PACKET &&
		     sa0->sa_family != AF_INET)) {
			errno = EAFNOSUPPORT;
			*ret = -1;
			return 1;
		}
		memcpy(s->bound, sa0->sa_data,
		       sizeof(s->bound) - 1);
		s->bound[sizeof(s->bound) - 1] = '\0';
		if (axsock_debug)
			fprintf(stderr, "axsock: bind fd=%d raw dev='%s'\n",
				fd, s->bound);
		*ret = 0;
		return 1;
	}

	if (!axsock_is_ax25(addr, len)) {
		errno = EAFNOSUPPORT;
		*ret = -1;
		return 1;
	}

	sa = (const struct sockaddr_ax25 *)addr;
	axsock_copy_call(s->local, ax25_ntoa(&sa->sax25_call));
	{
		int p = axsock_bind_port(addr, len, s->local,
					 &s->port_named, &s->port_known,
					 s->portname, sizeof(s->portname));

		if (p < 0) {
			axsock_port_unserved(addr, len);
			errno = EADDRNOTAVAIL;
			*ret = -1;
			return 1;
		}
		s->port = (unsigned char)p;
	}

	/*
	 * A datagram socket says with its bind() which callsign it wants to
	 * hear, and there is no listen() coming to say it later.  So this is
	 * where it has to be announced, or nothing is ever routed here - the
	 * node backend claims it at bind() for the same reason.
	 */
	if (s->type == SOCK_DGRAM && s->local[0] != '\0' && !s->registered) {
		pthread_mutex_lock(&axsock_lock);
		if (axsock_ensure_locked() == 0) {
			/*
			 * The subscription is per connection rather than
			 * per call sign, so it is asked for here and not
			 * in axsock_register_locked(): a second socket on
			 * the same call sign would ask again for a thing
			 * the server already does.  Counted rather than
			 * flagged, so that closing the last one stops it
			 * being put back onto a connection that comes
			 * again, and sent here as well as on that
			 * connection, because a new one has it off
			 * until somebody asks.
			 *
			 * On the loop port nothing is asked for: the
			 * daemon routes those frames already, and did so
			 * before this existed.
			 */
			if (s->port != AGWPE_PORT_LOOP) {
				axsock_nuisub++;
				agwpe_client_uisub(axsock_agwpe);
			}
			axsock_register_locked(s->local, s->port, 0);
			s->registered = 1;
		}
		pthread_mutex_unlock(&axsock_lock);
	}
	if (axsock_debug)
		fprintf(stderr, "axsock: bind fd=%d local='%s' port=%d\n",
			fd, s->local, s->port);
	*ret = 0;
	return 1;
}

int agwpe_connect(int fd, const struct sockaddr *addr, socklen_t len,
			 int *ret)
{
	const struct sockaddr_ax25 *sa;
	struct axsock_sock *s;
	int err;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL)
		return 0;

	if (!axsock_is_ax25(addr, len)) {
		errno = EAFNOSUPPORT;
		*ret = -1;
		return 1;
	}
	if (s->local[0] == '\0') {
		errno = EDESTADDRREQ;
		*ret = -1;
		return 1;
	}

	sa = (const struct sockaddr_ax25 *)addr;
	axsock_copy_call(s->remote, ax25_ntoa(&sa->sax25_call));

	pthread_mutex_lock(&axsock_lock);
	if (axsock_ensure_locked() != 0) {
		pthread_mutex_unlock(&axsock_lock);
		*ret = -1;
		return 1;
	}

	/* With no port named at bind time the outgoing one follows the
	 * remote callsign, as the kernel picks the AX.25 device from the
	 * destination in ax25_connect().  A bind that did name a port has
	 * said where this goes, and saying it again from the destination
	 * can only be worse: the destination of an outgoing call is a
	 * station, and a station is not in axports, so the lookup fails
	 * and lands the call on port 0.
	 */
	if (!s->port_named) {
		int p = axsock_port_for(s->remote);

		if (p >= 0)
			s->port = (unsigned char)p;
	}

	axsock_register_locked(s->local, s->port, 0);
	s->registered = 1;
	s->state = AXSOCK_CONNECTING;
	s->connect_err = 0;

	{
		/*
		 * The digipeater path is a property of the call, not of the
		 * connection: the (source, destination) pair including the
		 * SSIDs identifies the AX.25 link.  It is carried in the
		 * connect frame ("connect via"), but it never makes the
		 * connection a different one.
		 */
		const struct full_sockaddr_ax25 *fsa =
			(const struct full_sockaddr_ax25 *)addr;
		char digi_text[AGWPE_MAX_DIGIS][11];
		const char *digis[AGWPE_MAX_DIGIS];
		int ndigis = 0, i;

		if (len >= sizeof(struct full_sockaddr_ax25) &&
		    fsa->fsa_ax25.sax25_ndigis > 0) {
			int n = fsa->fsa_ax25.sax25_ndigis;

			if (n > AX25_MAX_DIGIS)
				n = AX25_MAX_DIGIS;
			for (i = 0; i < n; i++) {
				if (ndigis >= AGWPE_MAX_DIGIS - 1)
					break;
				if (fsa->fsa_digipeater[i].ax25_call[0] == '\0')
					continue;
				strncpy(digi_text[ndigis],
					ax25_ntoa(&fsa->fsa_digipeater[i]), 11);
				digi_text[ndigis][10] = '\0';
				digis[ndigis] = digi_text[ndigis];
				ndigis++;
			}
		}

		if (ndigis > 0) {
			if (agwpe_client_connect_via(axsock_agwpe, s->port,
						     s->pid, s->local,
						     s->remote, digis,
						     ndigis) != 0) {
				errno = (agwpe_client_err(axsock_agwpe) != 0) ?
					agwpe_client_err(axsock_agwpe) : EIO;
				s->state = AXSOCK_NEW;
				pthread_mutex_unlock(&axsock_lock);
				*ret = -1;
				return 1;
			}
		} else if (agwpe_client_connect(axsock_agwpe, s->port, s->pid,
					       s->local, s->remote) != 0) {
			errno = (agwpe_client_err(axsock_agwpe) != 0) ?
				agwpe_client_err(axsock_agwpe) : EIO;
			s->state = AXSOCK_NEW;
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}
	}

	{
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += AXSOCK_CONNECT_TIMEOUT;
		while (s->state == AXSOCK_CONNECTING)
			if (pthread_cond_timedwait(&axsock_cond,
						   &axsock_lock, &ts) != 0)
				break;
	}

	if (s->state == AXSOCK_CONNECTED) {
		pthread_mutex_unlock(&axsock_lock);
		*ret = 0;
		return 1;
	}

	err = (s->connect_err != 0) ? s->connect_err : ETIMEDOUT;
	if (s->state == AXSOCK_CONNECTING)
		s->state = AXSOCK_NEW;
	pthread_mutex_unlock(&axsock_lock);
	errno = err;
	*ret = -1;
	return 1;
}

int agwpe_send(int fd, const void *buf, size_t len, ssize_t *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}
	*ret = axsock_send_data(s, buf, len);
	pthread_mutex_unlock(&axsock_lock);
	return 1;
}

int agwpe_sendto(int fd, const void *buf, size_t len,
			const struct sockaddr *to, socklen_t tolen,
			ssize_t *ret)
{
	struct axsock_sock *s;
	ssize_t r;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}

	if (s->type == SOCK_DGRAM && axsock_is_ax25(to, tolen) &&
	    len <= INT_MAX) {
		const struct sockaddr_ax25 *sa =
			(const struct sockaddr_ax25 *)to;
		char target[AGWPE_MAX_CALL];

		axsock_copy_call(target, ax25_ntoa(&sa->sax25_call));

		/* The port a bind named stands, here as at connect().  Only
		 * a socket that named none has to find one from the target,
		 * and that lookup answers 0 for any real station - it can
		 * only recognise a callsign that is itself a port's.  A
		 * beacon addressed to a station therefore left on port 0
		 * whatever port it had been bound to.
		 */
		if (!s->port_named) {
			int p = axsock_port_for(target);

			if (p >= 0)
				s->port = (unsigned char)p;
		}

		if (axsock_debug)
			fprintf(stderr, "axsock: sendto fd=%d type=%d local='%s' port=%d target='%s' len=%zd\n",
				fd, s->type, s->local, s->port, target, len);

		/*
		 * DGRAM (unproto) sockets never go through connect(), so
		 * the AGWPE link has to be established here, lazily.
		 */
		if (axsock_ensure_locked() != 0) {
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}

		{
			const struct full_sockaddr_ax25 *fsa =
				(const struct full_sockaddr_ax25 *)to;
			char digi_text[AGWPE_MAX_DIGIS][11];
			const char *digis[AGWPE_MAX_DIGIS];
			int ndigis = 0, i;

			if (tolen >= sizeof(struct full_sockaddr_ax25) &&
			    fsa->fsa_ax25.sax25_ndigis > 0) {
				int n = fsa->fsa_ax25.sax25_ndigis;

				if (n > AX25_MAX_DIGIS)
					n = AX25_MAX_DIGIS;
				for (i = 0; i < n; i++) {
					if (ndigis >= AGWPE_MAX_DIGIS - 1)
						break;
					if (fsa->fsa_digipeater[i].ax25_call[0] == '\0')
						continue;
					strncpy(digi_text[ndigis],
						ax25_ntoa(&fsa->fsa_digipeater[i]), 11);
					digi_text[ndigis][10] = '\0';
					digis[ndigis] = digi_text[ndigis];
					ndigis++;
				}
			}

			if (axsock_send_unproto(s, target, digis, ndigis,
						buf, len) != 0) {
				pthread_mutex_unlock(&axsock_lock);
				*ret = -1;
				return 1;
			}
		}
		r = (ssize_t)len;
	} else {
		r = axsock_send_data(s, buf, len);
	}
	pthread_mutex_unlock(&axsock_lock);
	*ret = r;
	return 1;
}

/* The one on the data path, and the reason for the counter: a process that
 * never opened an AX.25 socket pays a load and a branch here, not a lock. */

int agwpe_write(int fd, const void *buf, size_t len, ssize_t *ret)
{
	struct axsock_sock *s;

	if (!axsock_may_have_sock())
		return 0;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}
	if (!axsock_up)
		(void)axsock_ensure_locked();
	*ret = axsock_send_data(s, buf, len);
	pthread_mutex_unlock(&axsock_lock);
	return 1;
}

static ssize_t agwpe_ui_read(struct axsock_sock *s, void *buf, size_t len,
			     struct sockaddr *addr, socklen_t *addrlen);

int agwpe_recv(int fd, void *buf, size_t len, ssize_t *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);
	if (s == NULL)
		return 0;

	/* Same framing as recvfrom(), only nobody asked who sent it. */
	if (s->type == SOCK_DGRAM && !s->raw) {
		*ret = agwpe_ui_read(s, buf, len, NULL, NULL);
		return 1;
	}
	/* The descriptor is readable by itself; only the flags are ours to
	 * drop, since the pipe behind it has none of them. */
	*ret = real_read(fd, buf, len);
	return 1;
}

/* Read exactly n bytes of a record.  Once the length is in hand the rest is
 * already on its way - it was written in one piece - so this cannot stall on
 * a frame that will never be completed. */
static int agwpe_read_full(int fd, unsigned char *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = real_read(fd, buf + got, n - got);

		if (r <= 0)
			return -1;
		got += (size_t) r;
	}
	return 0;
}

/*
 * One UI frame off a datagram socket, with the sender named.  Returns the
 * payload length, or -1 with errno set.  A payload longer than the caller's
 * buffer is truncated and the rest dropped, which is what a datagram socket
 * does - MSG_TRUNC would be the way to say so and nothing here asks for it.
 */
static ssize_t agwpe_ui_read(struct axsock_sock *s, void *buf, size_t len,
			     struct sockaddr *addr, socklen_t *addrlen)
{
	unsigned char hdr[AXSOCK_UI_HDR];
	unsigned char sink[256];
	size_t plen, take, left;
	int fd = s->fd;

	if (agwpe_read_full(fd, hdr, sizeof(hdr)) != 0)
		return -1;
	plen = ((size_t) hdr[0] << 24) | ((size_t) hdr[1] << 16) |
	       ((size_t) hdr[2] << 8) | (size_t) hdr[3];
	if (plen > AXMON_FRAME_MAX) {
		errno = EPROTO;
		return -1;
	}

	take = plen < len ? plen : len;
	if (take > 0 && agwpe_read_full(fd, buf, take) != 0)
		return -1;
	for (left = plen - take; left > 0; ) {
		size_t chunk = left < sizeof(sink) ? left : sizeof(sink);

		if (agwpe_read_full(fd, sink, chunk) != 0)
			return -1;
		left -= chunk;
	}

	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_ax25)) {
		struct sockaddr_ax25 *sa = (struct sockaddr_ax25 *) addr;
		char from[AGWPE_MAX_CALL + 1];

		memset(sa, 0, sizeof(*sa));
		sa->sax25_family = AF_AX25;
		snprintf(from, sizeof(from), "%.*s", AGWPE_MAX_CALL,
			 (const char *) hdr + 4);
		if (from[0] != '\0')
			ax25_aton_entry(from, sa->sax25_call.ax25_call);
		*addrlen = sizeof(struct sockaddr_ax25);
	}
	return (ssize_t) take;
}

/*
 * One frame from a raw monitor, with the header taken off.
 *
 * The header never leaves this file: the writer above and axmon_read()
 * below are both part of this library, so there is no version to keep in
 * step with anything outside it.  Splitting it here is what lets the port
 * travel with its own frame - see the writer - instead of the reader
 * guessing it from one value per socket.
 *
 * A short read in the middle is normal on a stream, and a signal in the
 * middle must not be mistaken for one: the frame is assembled across both,
 * because a reader that restarted the header would desynchronize the stream
 * for good.
 */
static ssize_t agwpe_mon_read(int fd, void *buf, size_t len,
			      unsigned char *portp)
{
	unsigned char hdr[AXMON_HDR_LEN];
	unsigned char *p = buf;
	size_t off, plen;
	ssize_t n;

	for (off = 0; off < AXMON_HDR_LEN; ) {
		n = real_read(fd, hdr + off, AXMON_HDR_LEN - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			/* Closed between frames is the end of the
			 * monitor; closed inside one is a cut frame. */
			errno = off == 0 ? EPIPE : ECONNRESET;
			return -1;
		}
		off += (size_t)n;
	}
	plen = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) |
	       ((size_t)hdr[2] << 8) | (size_t)hdr[3];
	*portp = hdr[4];
	if (axsock_debug)
		fprintf(stderr, "axsock: mon_read len=%zu port=%u buflen=%zu\n",
			plen, *portp, len);

	if (plen == 0 || plen > AXMON_FRAME_MAX) {
		/* The writer never produces either, so this is a peer we do
		 * not understand.  Nothing can resynchronize that. */
		errno = EPROTO;
		return -1;
	}
	if (plen > len) {
		/* Too small a buffer is the caller's, not the stream's:
		 * drop the frame whole so the next one still lines up. */
		for (off = 0; off < plen; ) {
			n = real_read(fd, hdr, sizeof(hdr));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				errno = ECONNRESET;
				return -1;
			}
			off += (size_t)n;
		}
		errno = E2BIG;
		return -1;
	}
	for (off = 0; off < plen; ) {
		n = real_read(fd, p + off, plen - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = ECONNRESET;
			return -1;
		}
		off += (size_t)n;
	}
	return (ssize_t)plen;
}

int agwpe_recvfrom(int fd, void *buf, size_t len,
			struct sockaddr *addr, socklen_t *addrlen, ssize_t *ret)
{
	struct axsock_sock *s;
	ssize_t n;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL)
		return 0;

	/* A datagram socket carries a length and a sender in front of every
	 * frame - see agwpe_ui_deliver_locked() - so it is not read raw. */
	if (s->type == SOCK_DGRAM && !s->raw) {
		*ret = agwpe_ui_read(s, buf, len, addr, addrlen);
		return 1;
	}

	/* A monitor is fed by the writer in axsock_dispatch(), so it is read
	 * the same way: header first, then exactly the one frame it named. */
	if (s->raw && s->peer >= 0) {
		unsigned char port = 0;

		n = agwpe_mon_read(fd, buf, len, &port);
		if (n < 0) {
			*ret = n;
			return 1;
		}
		if (addr != NULL && addrlen != NULL) {
			struct sockaddr *sa = addr;
			socklen_t want = *addrlen;

			if (want > sizeof(struct sockaddr))
				want = sizeof(struct sockaddr);
			memset(sa, 0, want);
			sa->sa_family = AF_PACKET;
			axsock_raw_name(s, port, sa, sizeof(sa->sa_data));
			*addrlen = sizeof(struct sockaddr);
		}
		*ret = n;
		return 1;
	}

	n = real_read(fd, buf, len);
	if (n < 0) {
		*ret = n;
		return 1;
	}
	if (addr != NULL && addrlen != NULL && s->raw) {
		struct sockaddr *sa = addr;
		socklen_t want = *addrlen;

		if (want > sizeof(struct sockaddr))
			want = sizeof(struct sockaddr);
		memset(sa, 0, want);
		sa->sa_family = AF_PACKET;
		axsock_raw_name(s, s->port, sa, sizeof(sa->sa_data));
		*addrlen = sizeof(struct sockaddr);
		*ret = n;
		return 1;
	}
	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_ax25)) {
		struct sockaddr_ax25 *sa = (struct sockaddr_ax25 *)addr;

		memset(sa, 0, sizeof(*sa));
		sa->sax25_family = AF_AX25;
		if (s->remote[0] != '\0')
			ax25_aton_entry(s->remote, sa->sax25_call.ax25_call);
		*addrlen = sizeof(struct sockaddr_ax25);
	}
	*ret = n;
	return 1;
}

/*
 * The AGWPE backend, in the shape the WAMPES one has: it answers 0 for a
 * descriptor that is not its own and 1 with the answer in *ret for one that
 * is.  An entry point below then reads the same way for both backends,
 * instead of one of them being the case that falls out at the end - which is
 * what let the two drift apart, and what let a datagram socket fall through
 * to the descriptor itself and answer EISCONN.
 *
 * The table and the lock stay behind these functions.  They are what a file
 * of its own would have to export otherwise, and exporting a mutex is not a
 * module boundary.
 */

int agwpe_shutdown(int fd, int how, int *ret)
{
	struct axsock_sock *s;

	(void) how;
	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}
	axsock_disconnect_locked(s);
	pthread_mutex_unlock(&axsock_lock);
	*ret = 0;
	return 1;
}

/*
 * Fast path: when no AGWPE-backed socket exists, close() is a plain
 * descriptor release.  This also keeps a signal handler calling close() from
 * ever taking the lock (and thus from ever waiting on another thread):
 * listen's SIGINT handler closes the monitor socket this way.  The one call
 * that cannot be kept out of the lock is closing the monitor socket itself
 * while the dispatch thread is mid-write - the recursive mutex makes that
 * safe as long as the lock hold times are short, which the send-without-lock
 * paths above guarantee.
 */

int agwpe_close(int fd, int *ret)
{
	struct axsock_sock *s, **pp;
	int peer, rfd;

	if (!axsock_may_have_sock())
		return 0;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}

	/*
	 * An accepted socket: its peer reader thread forwards the child's
	 * outbound data and owns the teardown once the socketpair closes.
	 * Releasing our app-end copy keeps the child's dup2()'d copies
	 * alive; when the last one goes away the thread sees EOF.
	 *
	 * Owns it whenever the thread exists, which is not what the
	 * condition said: it also asked for the session to still be up, so
	 * a socket closed after the far end had already hung up went the
	 * other way and was freed here - while the thread still held it and
	 * would later tear it down again.  That is the abort in
	 * axsock_peer_reader_teardown() that shows up when a program takes
	 * one call after another, as ax25d(8) does, and it is older than
	 * the queue below.
	 *
	 * The state does not have to be tested for the thread to wake
	 * either: closing our copy is what puts EOF on the router end.
	 */
	if (s->has_peer_thread) {
		int rfd_app = s->fd;

		s->fd = -1;
		pthread_mutex_unlock(&axsock_lock);
		if (rfd_app >= 0)
			real_close(rfd_app);
		*ret = 0;
		return 1;
	}

	axsock_disconnect_locked(s);

	if (s->registered)
		axsock_unregister_locked(s->local, s->port);

	/*
	 * The raw monitor stream falls with the last reader of it, and a
	 * reader is a monitor socket.  It is not "the last user of raw
	 * frames": a datagram socket off the loop port is one of those, and
	 * counting it here is what left the stream running for every
	 * userland program on the machine for as long as one socket was
	 * open, even after the listen that shared it had gone.
	 *
	 * A datagram socket's own subscription is not withdrawn from the
	 * server, and there is nothing to withdraw: it is per connection,
	 * there is no frame for un-asking, and the frames it asked for are
	 * addressed to call signs this socket held.  What has to be undone
	 * is the count, so that a connection which comes back does not carry
	 * a subscription nobody in this process wants any more.
	 */
	if (s->type == SOCK_DGRAM && s->local[0] != '\0' &&
	    s->port != AGWPE_PORT_LOOP && axsock_nuisub > 0)
		axsock_nuisub--;

	if (s->raw) {
		if (--axsock_nraw == 0 && axsock_up && axsock_agwpe != NULL)
			agwpe_client_raw_toggle(axsock_agwpe);
	}

	for (pp = &axsock_list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	__atomic_sub_fetch(&axsock_nsock, 1, __ATOMIC_RELAXED);

	peer = s->peer;
	rfd = s->fd;
	s->peer = -1;
	s->fd = -1;
	pthread_mutex_unlock(&axsock_lock);

	if (peer >= 0)
		real_close(peer);
	real_close(rfd);
	axsock_peer_discard_locked(s);
	free(s);
	*ret = 0;
	return 1;
}

int agwpe_listen(int fd, int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		return 0;
	}
	/*
	 * One listener per callsign and pid, which is the rule the node
	 * enforces and answers with "already taken".  Say the same thing
	 * here, because on this side nobody else will: the server registers
	 * a callsign without a pid, so it cannot tell two claims apart, and
	 * the second listener would simply never hear anything.
	 *
	 * Only within this process.  Another process claiming the same
	 * callsign is beyond what the protocol can express.
	 */
	{
		struct axsock_sock *o;

		for (o = axsock_list; o != NULL; o = o->next)
			if (o != s && o->listening && o->pid == s->pid &&
			    o->local[0] != '\0' &&
			    strcasecmp(o->local, s->local) == 0) {
				pthread_mutex_unlock(&axsock_lock);
				errno = EADDRINUSE;
				*ret = -1;
				return 1;
			}
	}

	s->listening = 1;
	if (s->local[0] != '\0' && !s->registered) {
		int listener = (s->port == AGWPE_PORT_LOOP);

		if (axsock_debug)
			fprintf(stderr, "axsock: listen(fd=%d local=%s port=%u listener=%d)\n",
				fd, s->local, s->port, listener);
		/* Announce the listening call so inbound connects are
		 * delivered: 'L' on the loop port, plain 'X' elsewhere.
		 */
		if (axsock_ensure_locked() == 0) {
			axsock_register_locked(s->local, s->port, listener);
			s->registered = 1;
		}
	}
	pthread_mutex_unlock(&axsock_lock);
	*ret = 0;
	return 1;
}

int agwpe_accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
			int *ret)
{
	struct axsock_sock *s, *p;
	int afd;

	/*
	 * On a blocking socket accept() waits, which it did not: it answered
	 * EAGAIN whether or not O_NONBLOCK was set.  Every program in the
	 * suite selects before it accepts, so none of them ever saw it, and
	 * one that simply calls accept() got an error it had no reason to
	 * expect.  The WAMPES side has always waited here.
	 *
	 * A queued connection also writes a readiness byte to the listening
	 * descriptor - that is what makes select() work - so waiting for it
	 * to become readable is the same wait, and the lock is not held
	 * across it.
	 */
	for (;;) {
		struct pollfd pfd;
		int lfd, nb;

		pthread_mutex_lock(&axsock_lock);
		s = axsock_find_locked(fd);
		if (s == NULL) {
			pthread_mutex_unlock(&axsock_lock);
			return 0;
		}
		if (s->pending != NULL)
			break;

		nb = (fcntl(s->fd, F_GETFL, 0) & O_NONBLOCK) != 0;
		lfd = s->fd;
		pthread_mutex_unlock(&axsock_lock);

		if (nb) {
			errno = EAGAIN;
			*ret = -1;
			return 1;
		}

		pfd.fd = lfd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, -1) < 0 && errno != EAGAIN) {
			*ret = -1;	/* EINTR belongs to the caller */
			return 1;
		}
	}

	p = s->pending;
	s->pending = p->pend_next;
	afd = p->fd;

	/* Forward outbound data the application writes into the
	 * socketpair.  A forked child keeps this fd as stdin/stdout
	 * without linking libax25, so its writes cannot be intercepted
	 * here; the peer reader thread covers that.
	 */
	p->has_peer_thread = 1;

	/* Drain the readiness marker written when the connection was
	 * queued, so the listener does not stay readable.
	 */
	{
		int fl = fcntl(s->fd, F_GETFL, 0);
		unsigned char m;

		if (fl >= 0)
			(void)fcntl(s->fd, F_SETFL, fl | O_NONBLOCK);
		if (real_read(s->fd, &m, 1) < 0 && errno == EAGAIN)
			;	/* marker lost; ignore */
		if (fl >= 0)
			(void)fcntl(s->fd, F_SETFL, fl);
	}
	pthread_mutex_unlock(&axsock_lock);

	{
		pthread_t thr;

		if (pthread_create(&thr, NULL, axsock_peer_reader, p) == 0)
			pthread_detach(thr);
		else
			p->has_peer_thread = 0;
	}

	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_ax25)) {
		struct sockaddr_ax25 *sa = (struct sockaddr_ax25 *)addr;

		memset(sa, 0, sizeof(*sa));
		sa->sax25_family = AF_AX25;
		if (p->remote[0] != '\0')
			ax25_aton_entry(p->remote, sa->sax25_call.ax25_call);
		*addrlen = sizeof(struct sockaddr_ax25);
	}
	*ret = afd;
	return 1;
}

/* AX.25 option names for the stderr warning below.  */

int agwpe_setsockopt(int fd, int level, int optname, int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL)
		return 0;

	if (level == SOL_AX25) {
		if (axsock_opt_refuse(optname)) {
			errno = ENOPROTOOPT;
			*ret = -1;
			return 1;
		}
		axsock_opt_note_ignored(optname);
		*ret = 0;
		return 1;
	}
	if (level == SOL_SOCKET) {
		*ret = 0;
		return 1;
	}
	errno = ENOPROTOOPT;
	*ret = -1;
	return 1;
}

int agwpe_getsockopt(int fd, int level, void *optval, socklen_t *optlen,
			    int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);
	if (s == NULL)
		return 0;

	if (level == SOL_AX25 && optval != NULL && optlen != NULL) {
		memset(optval, 0, *optlen);
		*ret = 0;
		return 1;
	}
	errno = ENOPROTOOPT;
	*ret = -1;
	return 1;
}

/*
 * No WAMPES half: a descriptor served by a node is an ordinary socketpair
 * end, so FIONREAD and friends work on it as they are, and the node has no
 * equivalent of SIOCAX25CTLCON - axctl(8) and axkill(8) reach AGWPE only.
 */
int agwpe_ioctl(int fd, unsigned long request, void *arg, int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL)
		return 0;

	switch (request) {
	case FIONREAD:
		*ret = real_ioctl(s->fd, request, arg);
		return 1;
	case SIOCGSTAMP: {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		if (arg != NULL)
			memcpy(arg, &tv, sizeof(tv));
		*ret = 0;
		return 1;
	}
	case SIOCGIFHWADDR: {
		/* ax25-apps/listen -a (ETH_P_ALL) fetches the interface
		 * hardware address to tell AX.25 frames from bare IP
		 * frames apart.  There is no real interface here; report
		 * the AX.25 address family so every frame is decoded as
		 * AX.25.  */
		struct ifreq *ifr = (struct ifreq *)arg;

		if (ifr == NULL) {
			errno = EFAULT;
			*ret = -1;
			return 1;
		}
		memset(&ifr->AXSOCK_IFR_HWADDR, 0,
		       sizeof(ifr->AXSOCK_IFR_HWADDR));
		ifr->AXSOCK_IFR_HWADDR.sa_family = AF_AX25;
		*ret = 0;
		return 1;
	}
	case SIOCGIFFLAGS:
	case SIOCSIFFLAGS: {
		/* net2kiss reads the interface flags to put them back when it
		 * exits, and writes them only to add IFF_PROMISC (its -z
		 * option).  There is no interface here, and the AGWPE monitor
		 * is promiscuous anyway - it gets every frame the server
		 * hears.  Report an interface that is up, and accept the write
		 * without doing anything, so that the caller's save and
		 * restore cancel each other out.  */
		struct ifreq *ifr = (struct ifreq *)arg;

		if (ifr == NULL) {
			errno = EFAULT;
			*ret = -1;
			return 1;
		}
		if (request == SIOCGIFFLAGS)
			ifr->ifr_flags = IFF_UP | IFF_RUNNING;
		*ret = 0;
		return 1;
	}
	case SIOCAX25CTLCON: {
		/*
		 * axctl/axkill: adjust or terminate an established
		 * connection.  Translate the request into a 'Q' control
		 * frame for ax25netd, which owns the session state.
		 */
		struct ax25_ctl_struct *ctl = (struct ax25_ctl_struct *)arg;
		struct agwpe_s hdr;
		unsigned char payload[8];
		size_t plen;
		char portcall[16], sfrom[AGWPE_MAX_CALL + 1],
			sto[AGWPE_MAX_CALL + 1];

		if (ctl == NULL) {
			errno = EFAULT;
			*ret = -1;
			return 1;
		}

		pthread_mutex_lock(&axsock_lock);
		if (axsock_ensure_locked() != 0) {
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}

		snprintf(portcall, sizeof(portcall), "%s",
			 ax25_ntoa(&ctl->port_addr));
		snprintf(sfrom, sizeof(sfrom), "%s",
			 ax25_ntoa(&ctl->source_addr));
		snprintf(sto, sizeof(sto), "%s",
			 ax25_ntoa(&ctl->dest_addr));

		if (ctl->cmd == AX25_KILL) {
			payload[0] = AGWPE_CTL_KILL;
			plen = 1;
		} else {
			uint32_t v = agwpe_host2netle((uint32_t)ctl->arg);

			payload[0] = AGWPE_CTL_PARAM;
			payload[1] = AGWPE_CTL_SCOPE_CONN;
			payload[2] = (unsigned char)ctl->cmd;
			memcpy(payload + 3, &v, sizeof(v));
			plen = 7;
		}

		{
			/* A control frame belongs to a port.  If the address
			 * names an entry whose port cannot be resolved, say
			 * so rather than set the parameters of some other
			 * channel's link.  */
			int p = axsock_port_for(portcall);

			if (p < 0) {
				pthread_mutex_unlock(&axsock_lock);
				errno = EADDRNOTAVAIL;
				*ret = -1;
				return 1;
			}
			agwpe_header_init(&hdr, (unsigned char)p,
					  AGWPE_CMD_CTL, 0, sfrom, sto, plen);
		}
		if (agwpe_client_send_frame(axsock_agwpe, &hdr, payload) != 0) {
			int e = agwpe_client_err(axsock_agwpe);

			pthread_mutex_unlock(&axsock_lock);
			errno = (e != 0) ? e : EIO;
			*ret = -1;
			return 1;
		}
		pthread_mutex_unlock(&axsock_lock);
		*ret = 0;
		return 1;
	}
	default:
		errno = ENOTTY;
		*ret = -1;
		return 1;
	}
}

int agwpe_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen,
			     int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);
	if (s == NULL)
		return 0;

	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_ax25)) {
		struct sockaddr_ax25 *sa = (struct sockaddr_ax25 *)addr;

		memset(sa, 0, sizeof(*sa));
		sa->sax25_family = AF_AX25;
		if (s->local[0] != '\0')
			ax25_aton_entry(s->local, sa->sax25_call.ax25_call);
		*addrlen = sizeof(struct sockaddr_ax25);
		*ret = 0;
		return 1;
	}
	errno = EINVAL;
	*ret = -1;
	return 1;
}

int agwpe_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen,
			     int *ret)
{
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);
	if (s == NULL)
		return 0;

	/* No peer, no answer.  Reporting success with an empty callsign told
	 * the caller there was a station at the other end and left it to
	 * decide what an empty one means - and the callers that ask this are
	 * the ones deciding who is logging in.  The WAMPES side has always
	 * said ENOTCONN here (answer_addr() in wampes.c); say the same. */
	if (s->remote[0] == '\0') {
		errno = ENOTCONN;
		*ret = -1;
		return 1;
	}

	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_ax25)) {
		struct sockaddr_ax25 *sa = (struct sockaddr_ax25 *)addr;

		memset(sa, 0, sizeof(*sa));
		sa->sax25_family = AF_AX25;
		ax25_aton_entry(s->remote, sa->sax25_call.ax25_call);
		*addrlen = sizeof(struct sockaddr_ax25);
		*ret = 0;
		return 1;
	}
	errno = EINVAL;
	*ret = -1;
	return 1;
}

/*
 * After fork() only the calling thread survives.  A mutex or condition
 * variable another thread held at that instant stays locked in the
 * child forever, deadlocking the first shim call there (ax25d forks
 * once per connection and its child does getsockname()).  Re-initialize
 * them and forget the connection state the child no longer needs.
 */
static void axsock_atfork_child(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&axsock_lock, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_cond_init(&axsock_cond, NULL);

	axsock_list = NULL;
	axsock_nsock = 0;
	axsock_npending = 0;
	axsock_agwpe = NULL;
	axsock_up = 0;
	axsock_err = 0;
	axsock_nregistered = 0;
	/* The child inherited axsock_thread_alive, not the thread. */
	axsock_thread_alive = 0;
}


__attribute__((constructor))
static void agwpe_sock_init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&axsock_lock, &attr);
	pthread_mutexattr_destroy(&attr);

	pthread_atfork(NULL, NULL, axsock_atfork_child);

	/* Installed here and not in a lazy path because ax25_port_info() has
	 * no way to know whether the shim was built in: it is compiled out
	 * entirely without --enable-userspace-ax25, and a caller must not
	 * have to test that.  Order against wampes.c's constructor does not
	 * matter - the two set different hooks, and ax25_port_info() asks
	 * them in the order the sockets are served in, not in the order
	 * they were installed. */
	ax25_config_port_agwpe_hook = agwpe_port_hook;
}
