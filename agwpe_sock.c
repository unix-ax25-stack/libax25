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
#include "axsock_real.h"
#include "agwpe_sock.h"

#define	AXSOCK_DEFAULT_HOST	"127.0.0.1"
#define	AXSOCK_DEFAULT_PORT	8100
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
 * the reference.  Recursion therefore only ever re-enters around short
 * critical sections, which also bounds what a signal handler calling
 * close() can be made to wait for.
 */
static pthread_mutex_t	axsock_lock;
static pthread_cond_t	axsock_cond = PTHREAD_COND_INITIALIZER;

static struct axsock_sock	*axsock_list;
static int			axsock_nsock;
static int			axsock_nraw;	/* open SOCK_PACKET monitors */
static int			axsock_npending; /* sockets with a queue, so the
					  * reader knows to come back */


static agwpe_client_t		*axsock_agwpe;
static pthread_t		axsock_thread;
static int			axsock_up;

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
 * and upstream restarts, and agwpe.conf is read once at startup anyway.
 */
struct axsock_gport {
	unsigned char	port;		/* flat AGWPE port byte */
	char		up[24];		/* upstream name (agwpe.conf) */
};

#define	AXSOCK_GPORTS	(AGWPE_PORT_LOOP + 1)	/* flat ports 0..255 */
static struct axsock_gport	axsock_gports[AXSOCK_GPORTS];
static int			axsock_gnports = -1;	/* -1 = not fetched */

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
 * Connect the AGWPE client to the configured server.  A leading '/'
 * in AXSOCK_HOST selects a unix domain socket (a path), anything else
 * a TCP host:port.  The unix socket is gated by its file permissions,
 * so an ax25netd configured with 'socket' and 'group hams' is only
 * reachable by members of that group.
 */
static int axsock_transport_connect(agwpe_client_t *c)
{
	if (axsock_host[0] == '/')
		return agwpe_client_connect_unix(c, axsock_host);
	return agwpe_client_connect_host(c, axsock_host, axsock_port);
}

static int axsock_ports_fetch(void)
{
	const char *host, *portstr;
	const char *user, *pass;
	struct addrinfo hints, *res, *ai;
	char service[16];
	struct agwpe_s hdr;
	struct timeval tv;
	int fd = -1;

	if (axsock_gnports >= 0)
		return 0;

	host = getenv("AXSOCK_HOST");
	axsock_host = (host != NULL && host[0] != '\0') ?
		host : AXSOCK_DEFAULT_HOST;
	portstr = getenv("AXSOCK_PORT");
	axsock_port = (portstr != NULL && portstr[0] != '\0') ?
		atoi(portstr) : AXSOCK_DEFAULT_PORT;
	if (axsock_port <= 0 || axsock_port > 65535)
		axsock_port = AXSOCK_DEFAULT_PORT;

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
 * upstream name in the 'G' reply (the name in agwpe.conf).  The prefix
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

static unsigned char axsock_port_for(const char *call)
{
	char *name, *addr;
	unsigned char i = 0, pos = 0;
	char base[24];
	const char *entry = NULL;
	const char *colon;
	int idx;
	size_t blen;

	ax25_config_load_ports();

	/* Exact match first, then the base-call fallback: an exact entry
	 * anywhere wins over a base match earlier in the list.  */
	for (name = ax25_config_get_next(NULL); name != NULL;
	     name = ax25_config_get_next(name), i++) {
		addr = ax25_config_get_addr(name);
		if (addr != NULL && strcasecmp(addr, call) == 0) {
			entry = name;
			pos = i;
			break;
		}
	}
	if (entry == NULL) {
		i = 0;
		for (name = ax25_config_get_next(NULL); name != NULL;
		     name = ax25_config_get_next(name), i++) {
			addr = ax25_config_get_addr(name);
			if (addr != NULL && axsock_call_base_equal(addr, call)) {
				entry = name;
				pos = i;
				break;
			}
		}
	}
	if (entry == NULL)
		return 0;

	/* Optional ":N" suffix selects a channel of the named upstream,
	 * not a second upstream: "direwolf:1" is interface 1 (channel 1)
	 * of the upstream "direwolf", the way netd numbers the channels
	 * of one radio server.  */
	idx = -1;
	colon = strrchr(entry, ':');
	if (colon != NULL && colon != entry && colon[1] != '\0') {
		char *end;
		long v;

		errno = 0;
		v = strtol(colon + 1, &end, 10);
		if (errno == 0 && *end == '\0' && v >= 0 && v < 256)
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

	/* The reserved loop interface is the virtual loopback upstream.  */
	if (strcasecmp(base, "loop") == 0)
		return AGWPE_PORT_LOOP;

	if (axsock_ports_fetch() == 0) {
		int p = axsock_gport_channel(axsock_strip_prefix(base),
					     (idx >= 0) ? idx : 0);

		if (p >= 0)
			return (unsigned char)p;
	}

	/* Best effort without the table: the positional mapping extended
	 * with the channel (upstream index = position in axports).  */
	if (idx >= 0)
		return (unsigned char)(pos * 16 + idx);
	return (unsigned char)pos;
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

static unsigned char axsock_bind_port(const struct sockaddr *addr,
				      socklen_t len, const char *local,
				      int *named)
{
	const struct full_sockaddr_ax25 *fsa =
		(const struct full_sockaddr_ax25 *) addr;

	if (len >= sizeof(struct full_sockaddr_ax25) &&
	    fsa->fsa_ax25.sax25_ndigis > 0) {
		*named = 1;
		return axsock_port_for(ax25_ntoa(&fsa->fsa_digipeater[0]));
	}
	*named = 0;
	return axsock_port_for(local);
}

/* Reverse of axsock_port_for(): the configured port name for an AGWPE
 * port number, as seen in the sockaddr of a raw monitor socket.  */
static const char *axsock_port_name(unsigned char port)
{
	char *name;
	unsigned char i = 0;

	if (axsock_gnports < 0)
		(void)axsock_ports_fetch();
	if (axsock_gnports >= 0) {
		int k;

		for (k = 0; k < axsock_gnports; k++)
			if (axsock_gports[k].port == port &&
			    axsock_gports[k].up[0] != '\0')
				return axsock_gports[k].up;
	}

	ax25_config_load_ports();
	for (name = ax25_config_get_next(NULL); name != NULL;
	     name = ax25_config_get_next(name), i++) {
		if (i == port)
			return name;
	}

	if (port == AGWPE_PORT_LOOP) {
		for (name = ax25_config_get_next(NULL); name != NULL;
		     name = ax25_config_get_next(name)) {
			if (strcasecmp(name, "loop") == 0)
				return name;
		}
	}

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

static void axsock_peer_drop_locked(struct axsock_sock *s, const char *why)
{
	fprintf(stderr, "axsock: %s for %.*s - closing the session\n", why,
		AGWPE_MAX_CALL, s->remote);
	axsock_peer_discard_locked(s);
	if (s->peer >= 0) {
		real_close(s->peer);
		s->peer = -1;
	}
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
			real_close(s->peer);
			s->peer = -1;
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
	if (s->peer >= 0) {
		real_close(s->peer);
		s->peer = -1;
	}
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

	if (len > AXMON_FRAME_MAX)
		return;			/* nothing here could read it */

	hdr[0] = (unsigned char)(len >> 24);
	hdr[1] = (unsigned char)(len >> 16);
	hdr[2] = (unsigned char)(len >> 8);
	hdr[3] = (unsigned char) len;
	memset(hdr + 4, 0, AGWPE_MAX_CALL);
	strncpy((char *) hdr + 4, from, AGWPE_MAX_CALL - 1);

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
	if (getenv("AXSOCK_DEBUG"))
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
			unsigned char fbuf[AXMON_PREFIX_LEN + AXMON_FRAME_MAX];
			ssize_t n;

			if (!s->raw || s->peer < 0)
				continue;
			if (!axsock_raw_match(s, hdr->port))
				continue;
			/* Deliver each frame as one length prefixed unit
			 * (see netax25/axmon.h): the monitor socketpair is
			 * a byte stream, and a stream merges frames that
			 * arrive back to back, which would make listen(1)
			 * decode past the end of the first frame.  */
			if (len == 0 || len > AXMON_FRAME_MAX)
				continue;	/* monitor cannot show it */
			fbuf[0] = (unsigned char)(len >> 24);
			fbuf[1] = (unsigned char)(len >> 16);
			fbuf[2] = (unsigned char)(len >> 8);
			fbuf[3] = (unsigned char)len;
			memcpy(fbuf + AXMON_PREFIX_LEN, data, len);
			n = real_write(s->peer, fbuf, AXMON_PREFIX_LEN + len);
			if (n != (ssize_t)(AXMON_PREFIX_LEN + len))
				continue;	/* monitor fell behind: drop */
			s->port = hdr->port;
		}
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
	 * An incoming UI frame.  ax25netd routes one on the loop port to the
	 * client that registered the destination callsign and hands it on as
	 * it arrived, so it reaches us as 'M' or 'V'.  Against a real AGWPE
	 * server this does not happen at all - the protocol has no per
	 * callsign UI delivery, only the monitor stream - which is the other
	 * half of this and is not built yet.
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
			agwpe_ui_deliver_locked(best, hdr->call_from, data,
						len);
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
		if (getenv("AXSOCK_DEBUG"))
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
					if (getenv("AXSOCK_DEBUG"))
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

/*
 * Background thread: wait for frames from the AGWPE server and dispatch
 * them.  On connection loss every session is terminated inward.
 */
static void *axsock_reader(void *arg)
{
	sigset_t set;

	(void)arg;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	for (;;) {
		/* Blocking wait as before while nothing is queued.  With a
		 * queue there is a second thing to wait for - the peer end
		 * becoming writable - and it is not on this select, so come
		 * back regularly instead.  The cost falls only on the case
		 * that used to lose the data. */
		int wait = __atomic_load_n(&axsock_npending, __ATOMIC_RELAXED)
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
					 * about to return.  Push what fits,
					 * let the rest go, and close - a
					 * socket that never reaches EOF would
					 * be worse than a short one. */
					axsock_peer_flush_locked(s);
					if (s->plen > 0)
						fprintf(stderr,
							"axsock: link to the server lost with %zu bytes still undelivered to %.*s\n",
							s->plen,
							AGWPE_MAX_CALL,
							s->remote);
					axsock_peer_discard_locked(s);
					if (s->peer >= 0) {
						real_close(s->peer);
						s->peer = -1;
					}
					s->state = AXSOCK_NEW;
				}
			}
			pthread_cond_broadcast(&axsock_cond);
			pthread_mutex_unlock(&axsock_lock);
			return NULL;
		}
		if (__atomic_load_n(&axsock_npending, __ATOMIC_RELAXED))
			axsock_flush_pending();
	}
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
 * Make sure the AGWPE connection is up.  Called with the lock held.
 */
static int axsock_ensure_locked(void)
{
	const struct agwpe_client_cb cb = {
		.raw_frame = axsock_dispatch,
	};
	const char *host, *portstr;

	if (axsock_up)
		return 0;

	if (axsock_agwpe != NULL)
		axsock_client_retire();

	host = getenv("AXSOCK_HOST");
	axsock_host = (host != NULL && host[0] != '\0') ?
		host : AXSOCK_DEFAULT_HOST;
	portstr = getenv("AXSOCK_PORT");
	axsock_port = (portstr != NULL && portstr[0] != '\0') ?
		atoi(portstr) : AXSOCK_DEFAULT_PORT;
	if (axsock_port <= 0 || axsock_port > 65535)
		axsock_port = AXSOCK_DEFAULT_PORT;

	axsock_agwpe = agwpe_client_new(&cb, NULL);
	if (axsock_agwpe == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if (axsock_transport_connect(axsock_agwpe) != 0) {
		int e = agwpe_client_err(axsock_agwpe);

		axsock_client_retire();
		errno = (e != 0) ? e : ECONNREFUSED;
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
			agwpe_client_login(axsock_agwpe, user,
					   (pass != NULL) ? pass : "");
	}

	if (pthread_create(&axsock_thread, NULL, axsock_reader, NULL) != 0) {
		axsock_client_retire();
		errno = EAGAIN;
		return -1;
	}

	axsock_up = 1;
	axsock_register_all_locked();
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

	if (s->peer >= 0)
		real_close(s->peer);
	s->peer = -1;

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
		if (getenv("AXSOCK_DEBUG"))
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
				if (getenv("AXSOCK_DEBUG"))
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
	if (getenv("AXSOCK_DEBUG"))
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
int agwpe_socket_packet(int domain, int type, int protocol, int *ret)
{
	struct axsock_sock *s;
	int fd;

	if (type != SOCK_PACKET ||
	    (domain != AF_PACKET && domain != AF_INET))
		return 0;
	if (axsock_backend_now() == 1)
		return 0;

	/* An AGWPE server is what feeds this socket.  Ask for one first,
	 * because a machine can have both an AGWPE server and a node, and
	 * there a monitor works.
	 *
	 * Only when none answers does it matter which world we are in.
	 * WAMPES has no monitor stream at all - nothing carries a copy of
	 * every frame the way the 'K' record does - so failing there would be
	 * permanent, and a program that opens a monitor beside its real work
	 * would be taken down by it.  Hand out a descriptor that stays quiet
	 * instead: it costs that program one feature and leaves the rest
	 * working.  Say so once, on stderr, so nobody spends an evening
	 * wondering why the window is empty.
	 *
	 * With no node configured either, the refusal is a configuration
	 * fault and worth reporting as one.
	 *
	 * This is the one place the AGWPE backend looks at the other one's
	 * register, and it is not an oversight: whether a failure here should
	 * be fatal or quiet depends on whether the other world exists at all.
	 * The question belongs to neither backend alone, and it is asked in
	 * the one place where the answer changes what happens.
	 */
	pthread_mutex_lock(&axsock_lock);
	if (axsock_ensure_locked() != 0) {
		static int said;

		if (!wampes_configured()) {
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}
		s = axsock_alloc_sock_locked(type);
		if (s == NULL) {
			pthread_mutex_unlock(&axsock_lock);
			*ret = -1;
			return 1;
		}
		s->raw = 1;
		axsock_nraw++;
		fd = s->fd;
		if (!said) {
			said = 1;
			fprintf(stderr, "axsock: no monitor stream through "
				"WAMPES - this socket stays silent\n");
		}
		pthread_mutex_unlock(&axsock_lock);
		*ret = fd;
		return 1;
	}
	s = axsock_alloc_sock_locked(type);
	if (s == NULL) {
		pthread_mutex_unlock(&axsock_lock);
		*ret = -1;
		return 1;
	}
	s->raw = 1;
	/* 'k' is a toggle: enable it when the first monitor socket opens,
	 * disable it again when the last one closes.  */
	if (axsock_nraw == 0 && axsock_agwpe != NULL)
		agwpe_client_raw_toggle(axsock_agwpe);
	axsock_nraw++;
	fd = s->fd;
	pthread_mutex_unlock(&axsock_lock);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "axsock: SOCK_PACKET monitor fd=%d proto=0x%x\n",
			fd, protocol);
	*ret = fd;
	return 1;
}

int agwpe_bind(int fd, const struct sockaddr *addr, socklen_t len,
		      int *ret)
{
	const struct sockaddr_ax25 *sa;
	struct axsock_sock *s;

	pthread_mutex_lock(&axsock_lock);
	s = axsock_find_locked(fd);
	pthread_mutex_unlock(&axsock_lock);

	if (s == NULL)
		return 0;

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
		if (getenv("AXSOCK_DEBUG"))
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
	s->port = axsock_bind_port(addr, len, s->local, &s->port_named);

	/*
	 * A datagram socket says with its bind() which callsign it wants to
	 * hear, and there is no listen() coming to say it later.  So this is
	 * where it has to be announced, or nothing is ever routed here - the
	 * node backend claims it at bind() for the same reason.
	 */
	if (s->type == SOCK_DGRAM && s->local[0] != '\0' && !s->registered) {
		pthread_mutex_lock(&axsock_lock);
		if (axsock_ensure_locked() == 0) {
			axsock_register_locked(s->local, s->port, 0);
			s->registered = 1;
		}
		pthread_mutex_unlock(&axsock_lock);
	}
	if (getenv("AXSOCK_DEBUG"))
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
	if (!s->port_named)
		s->port = axsock_port_for(s->remote);

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
		if (!s->port_named)
			s->port = axsock_port_for(target);

		if (getenv("AXSOCK_DEBUG"))
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

	n = real_read(fd, buf, len);
	if (n < 0) {
		*ret = n;
		return 1;
	}
	if (addr != NULL && addrlen != NULL && s->raw) {
		struct sockaddr *sa = addr;
		const char *name;
		socklen_t want = *addrlen;

		if (want > sizeof(struct sockaddr))
			want = sizeof(struct sockaddr);
		memset(sa, 0, want);
		sa->sa_family = AF_PACKET;
		name = axsock_port_name(s->port);
		if (name != NULL)
			strncpy(sa->sa_data, name, sizeof(sa->sa_data) - 1);
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

		if (getenv("AXSOCK_DEBUG"))
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

		agwpe_header_init(&hdr, axsock_port_for(portcall),
				  AGWPE_CMD_CTL, 0, sfrom, sto, plen);
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
	axsock_nregistered = 0;
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

}
