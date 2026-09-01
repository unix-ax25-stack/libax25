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
 * axprobe - an AX.25 socket opened with libax25 nowhere in the picture.
 *
 * Everything in ax25-apps and ax25-tools is linked against the library, so
 * none of it can show whether the interception reaches a program that is
 * not: they would find AX.25 either way.  This one calls socket(), bind(),
 * connect(), listen() and accept() itself, reads nothing but axports, and is
 * built with no LDADD at all - which makes it the thing to point LD_PRELOAD
 * at.  See axsock(7).
 *
 * Headers are not linking.  It includes netax25/ax25.h for the address
 * layout, because copying that layout here would only invite it to drift.
 *
 * The two calls it makes are the ones an application makes, in the order
 * call(1) and ax25d(8) make them:
 *
 *   connect: socket, bind (source callsign, and the port in the first
 *            digipeater slot - see axports(5)), connect, then copy
 *   listen:  socket, bind, listen, accept, then copy
 */

#include <config.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>

#include "netax25/ax25.h"
#include "pathnames.h"

/*
 * The calls, either straight or looked up at run time.
 *
 * Straight is what an ordinary program does and is the default.  The lookup
 * (-d) exists because macOS binds two-level: a library inserted with
 * DYLD_INSERT_LIBRARIES is not consulted for a call site bound at link time,
 * but dlsym(RTLD_DEFAULT, ...) does find it, since it searches the loaded
 * images in order.  So -d is the mode in which the interception can be
 * exercised on macOS at all, and the difference between the two modes is
 * itself the measurement.
 */
static int (*p_socket)(int, int, int);
static int (*p_bind)(int, const struct sockaddr *, socklen_t);
static int (*p_connect)(int, const struct sockaddr *, socklen_t);
static int (*p_listen)(int, int);
static int (*p_accept)(int, struct sockaddr *, socklen_t *);
static int (*p_getsockname)(int, struct sockaddr *, socklen_t *);
static int (*p_getpeername)(int, struct sockaddr *, socklen_t *);
static ssize_t (*p_write)(int, const void *, size_t);

static int	verbose = 1;

static void *lookup(const char *name)
{
	void *f = dlsym(RTLD_DEFAULT, name);

	if (f == NULL) {
		fprintf(stderr, "axprobe: dlsym(%s): %s\n", name, dlerror());
		exit(1);
	}
	if (verbose) {
		Dl_info info;

		if (dladdr(f, &info) && info.dli_fname != NULL)
			fprintf(stderr, "axprobe: %s() from %s\n",
				name, info.dli_fname);
	}
	return f;
}

static void bind_calls(int use_dlsym)
{
	if (!use_dlsym) {
		p_socket = socket;
		p_bind = bind;
		p_connect = connect;
		p_listen = listen;
		p_accept = accept;
		p_getsockname = getsockname;
		p_getpeername = getpeername;
		p_write = write;
		return;
	}
	p_socket = lookup("socket");
	p_bind = lookup("bind");
	p_connect = lookup("connect");
	p_listen = lookup("listen");
	p_accept = lookup("accept");
	p_getsockname = lookup("getsockname");
	p_getpeername = lookup("getpeername");
	p_write = lookup("write");
}

/* Shifted ASCII, six characters and an SSID.  ax25_aton_entry() does this in
 * the library; here it has to be done by hand, since linking is the one thing
 * this program must not do. */
static int aton_entry(const char *cp, char *axp)
{
	int i, ssid = 0;

	for (i = 0; i < 6; i++) {
		char c = *cp;

		if (c == '-' || c == '\0')
			break;
		c = toupper((unsigned char)c);
		if (!isalnum((unsigned char)c))
			return -1;
		axp[i] = c << 1;
		cp++;
	}
	if (i == 0)
		return -1;
	for (; i < 6; i++)
		axp[i] = ' ' << 1;
	if (*cp == '-') {
		char *end;

		ssid = (int)strtol(cp + 1, &end, 10);
		if (*end != '\0' || ssid < 0 || ssid > 15)
			return -1;
	} else if (*cp != '\0') {
		return -1;
	}
	axp[6] = (ssid << 1) & 0x1e;
	return 0;
}

static const char *ntoa(const char *axp)
{
	static char buf[12];
	char *p = buf;
	int i, ssid;

	for (i = 0; i < 6; i++) {
		char c = (axp[i] >> 1) & 0x7f;

		if (c != ' ')
			*p++ = c;
	}
	ssid = (axp[6] >> 1) & 0x0f;
	if (ssid != 0)
		p += sprintf(p, "-%d", ssid);
	*p = '\0';
	return buf;
}

/* The callsign of an axports entry, read here rather than through
 * ax25_config_load_ports(). */
static int port_callsign(const char *file, const char *port, char *out,
			 size_t outlen)
{
	char line[256];
	FILE *fp;

	if ((fp = fopen(file, "r")) == NULL) {
		fprintf(stderr, "axprobe: %s: %s\n", file, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *name, *call, *save = NULL;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if ((name = strtok_r(line, " \t\n", &save)) == NULL)
			continue;
		if ((call = strtok_r(NULL, " \t\n", &save)) == NULL)
			continue;
		if (strcasecmp(name, port) != 0)
			continue;
		snprintf(out, outlen, "%s", call);
		fclose(fp);
		return 0;
	}
	fclose(fp);
	fprintf(stderr, "axprobe: no entry \"%s\" in %s\n", port, file);
	return -1;
}

/*
 * bind() carries two things in one address: the source callsign in
 * sax25_call, and the callsign of the axports entry in the first digipeater
 * slot, which is how the port is named.  call(1) does exactly this
 * (call.c:685); it is not a digipeater and never travels.
 */
static int bind_port(int fd, const char *portcall, const char *srccall)
{
	struct full_sockaddr_ax25 sa;

	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (aton_entry(srccall, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid callsign \"%s\"\n", srccall);
		return -1;
	}
	if (aton_entry(portcall, sa.fsa_digipeater[0].ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid port callsign \"%s\"\n",
			portcall);
		return -1;
	}
	sa.fsa_ax25.sax25_ndigis = 1;

	if (p_bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("axprobe: bind");
		return -1;
	}
	if (verbose)
		fprintf(stderr, "axprobe: bound %s on the port of %s\n",
			srccall, portcall);
	return 0;
}

static void report(int fd)
{
	struct full_sockaddr_ax25 sa;
	socklen_t len = sizeof(sa);

	memset(&sa, 0, sizeof(sa));
	if (p_getsockname(fd, (struct sockaddr *)&sa, &len) == 0)
		fprintf(stderr, "axprobe: getsockname: family %d, %s\n",
			sa.fsa_ax25.sax25_family,
			ntoa(sa.fsa_ax25.sax25_call.ax25_call));
	len = sizeof(sa);
	memset(&sa, 0, sizeof(sa));
	if (p_getpeername(fd, (struct sockaddr *)&sa, &len) == 0)
		fprintf(stderr, "axprobe: getpeername: family %d, %s\n",
			sa.fsa_ax25.sax25_family,
			ntoa(sa.fsa_ax25.sax25_call.ax25_call));
}

/* stdin to the link, the link to stdout, until either end stops. */
static int shovel(int fd)
{
	struct pollfd pfd[2];
	char buf[512];

	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;
	pfd[1].fd = fd;
	pfd[1].events = POLLIN;

	for (;;) {
		ssize_t n;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("axprobe: poll");
			return 1;
		}
		if (pfd[0].revents & (POLLIN | POLLHUP)) {
			n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n <= 0) {
				/* End of input is not the end of the link: what
				 * was sent may still be answered.  Stop looking
				 * at stdin and wait for the peer, which is what
				 * makes "echo ... | axprobe connect" show the
				 * reply. */
				pfd[0].fd = -1;
				continue;
			}
			if (p_write(fd, buf, (size_t)n) < 0) {
				perror("axprobe: write");
				return 1;
			}
		}
		if (pfd[1].revents & (POLLIN | POLLHUP)) {
			n = read(fd, buf, sizeof(buf));
			if (n < 0) {
				perror("axprobe: read");
				return 1;
			}
			if (n == 0) {
				fprintf(stderr, "axprobe: peer closed\n");
				return 0;
			}
			if (write(STDOUT_FILENO, buf, (size_t)n) < 0)
				return 1;
		}
	}
}

static void usage(void);

/*
 * Several sessions from one program, open at the same time.
 *
 * The question is not whether a session works - the single-session modes
 * answer that - but whether two of them stay apart.  Each session sends a
 * line that names itself and nothing else does; the far end echoes it back;
 * a session that reads a line belonging to another session is the fault
 * being looked for.  It needs no cooperation from the other end beyond an
 * echo, which is what makes it usable against a real node as well.
 *
 * They are all opened before any of them is read, because a fault that only
 * shows up while two sessions overlap will not show up if they are run one
 * after the other.
 */

#define MULTI_MAX	8

struct session {
	int	fd;
	char	src[16];
	char	dst[16];
	char	port[32];	/* the axports entry, when it differs */
	char	tag[64];
	char	got[256];
	int	err;		/* errno from connect, 0 when it came up */
};

static int multi_open(struct session *ses, const char *portcall)
{
	/* ses->port names an axports entry of its own when the spec carried
	 * one - which is how one program comes to hold a WAMPES session and
	 * an AGWPE session at the same time, the case the two backends have
	 * to keep apart. */
	struct full_sockaddr_ax25 sa;

	if ((ses->fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		ses->err = errno;
		return -1;
	}
	if (bind_port(ses->fd, portcall,
		      strcmp(ses->src, "-") == 0 ? portcall : ses->src) < 0) {
		ses->err = errno;
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (aton_entry(ses->dst, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid destination '%s'\n", ses->dst);
		return -1;
	}
	sa.fsa_ax25.sax25_ndigis = 0;
	if (p_connect(ses->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		ses->err = errno;
		return 0;		/* refused is an answer, not a failure */
	}
	ses->err = 0;
	return 0;
}

static const char *axports_file;

/*
 * What the far end is.  An echo sends back what it was given, so a session
 * expects its own line; a bridge - the "loop" port of ax25netd(8), or a real
 * link with the other side of this same program on it - hands each session
 * the line of its partner.  Both are worth testing and they are not the same
 * check, so it is said rather than guessed.
 *
 * Both lines are built the same way, from the pair of callsigns and nothing
 * else, so each side can name what it expects without being told.
 */
static int peer_mode;

/*
 * Which session to close first, when several are up.  Closing one must not
 * disturb the others, and the two ends of that are worth trying separately:
 * the first one opened and the last one, because the table is a list and a
 * fault in the unlinking shows on one end or the other, not both.  -1 closes
 * none, which is the plain run.
 */
static int close_which = -1;

static int gather(int fd, char *buf, size_t buflen, const char *want,
		  int msec);

/* DL9SAU and DL9SAU-0 are one callsign written two ways.  The library knows
 * that; a string compare does not, so the mark is built from the written-out
 * form and a session is not accused of crossing over a hyphen. */
static const char *nossid0(const char *call, char *buf, size_t len)
{
	size_t n = strlen(call);

	if (n > 2 && strcmp(call + n - 2, "-0") == 0 && n - 2 < len) {
		memcpy(buf, call, n - 2);
		buf[n - 2] = '\0';
		return buf;
	}
	return call;
}

static void pair_tag(char *out, size_t len, const char *from, const char *to)
{
	char fb[16], tb[16];

	snprintf(out, len, "pair-%s-%s", nossid0(from, fb, sizeof(fb)),
		 nossid0(to, tb, sizeof(tb)));
}


static int multi(int argc, char **argv, int optind_, const char *portcall)
{
	struct session ses[MULTI_MAX];
	int n = 0, i, bad = 0;

	memset(ses, 0, sizeof(ses));
	while (optind_ < argc && n < MULTI_MAX) {
		const char *spec = argv[optind_++];
		const char *at = strchr(spec, '@');
		const char *colon;
		char bare[64];

		if (at != NULL) {
			if ((size_t)(at - spec) >= sizeof(bare) ||
			    strlen(at + 1) >= sizeof(ses[0].port)) {
				fprintf(stderr, "axprobe: '%s' too long\n", spec);
				return 1;
			}
			memcpy(bare, spec, (size_t)(at - spec));
			bare[at - spec] = '\0';
			strcpy(ses[n].port, at + 1);
			spec = bare;
		}
		colon = strchr(spec, ':');

		if (colon == NULL || colon == spec || colon[1] == '\0') {
			fprintf(stderr, "axprobe: '%s' is not <src>:<dest>\n",
				spec);
			return 1;
		}
		if ((size_t)(colon - spec) >= sizeof(ses[0].src) ||
		    strlen(colon + 1) >= sizeof(ses[0].dst)) {
			fprintf(stderr, "axprobe: callsign too long in '%s'\n",
				spec);
			return 1;
		}
		memcpy(ses[n].src, spec, (size_t)(colon - spec));
		strcpy(ses[n].dst, colon + 1);
		ses[n].fd = -1;
		n++;
	}
	if (n == 0)
		usage();

	/* All of them up first, then all of them spoken to. */
	for (i = 0; i < n; i++) {
		pair_tag(ses[i].tag, sizeof(ses[i].tag), ses[i].src,
			 ses[i].dst);
		if (ses[i].port[0] != '\0') {
			char own[16];

			if (port_callsign(axports_file, ses[i].port, own,
					  sizeof(own)) < 0)
				return 1;
			if (multi_open(&ses[i], own) < 0)
				return 1;
		} else if (multi_open(&ses[i], portcall) < 0) {
			return 1;
		}
		if (ses[i].err != 0)
			fprintf(stderr, "axprobe: %d %s>%s refused: %s\n",
				i, ses[i].src, ses[i].dst,
				strerror(ses[i].err));
		else if (verbose)
			fprintf(stderr, "axprobe: %d %s>%s up on fd %d\n",
				i, ses[i].src, ses[i].dst, ses[i].fd);
	}

	for (i = 0; i < n; i++) {
		char line[80];

		if (ses[i].err != 0)
			continue;
		snprintf(line, sizeof(line), "%s\n", ses[i].tag);
		if (write(ses[i].fd, line, strlen(line)) < 0)
			fprintf(stderr, "axprobe: %d write: %s\n", i,
				strerror(errno));
	}

	for (i = 0; i < n; i++) {
		struct pollfd pfd;
		ssize_t r;

		if (ses[i].err != 0)
			continue;
		pfd.fd = ses[i].fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 3000) <= 0) {
			snprintf(ses[i].got, sizeof(ses[i].got), "(nothing)");
			continue;
		}
		if ((r = read(ses[i].fd, ses[i].got,
			      sizeof(ses[i].got) - 1)) <= 0) {
			snprintf(ses[i].got, sizeof(ses[i].got), "(eof)");
			continue;
		}
		ses[i].got[r] = '\0';
		while (r > 0 && (ses[i].got[r - 1] == '\n' ||
				 ses[i].got[r - 1] == '\r'))
			ses[i].got[--r] = '\0';
	}

	for (i = 0; i < n; i++) {
		const char *verdict;

		if (ses[i].err != 0) {
			printf("%d %s %s>%s refused %s\n", i,
			       ses[i].port[0] ? ses[i].port : "-",
			       ses[i].src, ses[i].dst, strerror(ses[i].err));
			continue;
		}
		{
			char want[64];

			if (peer_mode)
				pair_tag(want, sizeof(want), ses[i].dst,
					 ses[i].src);
			else
				strcpy(want, ses[i].tag);
			if (strstr(ses[i].got, want) != NULL) {
				verdict = "ok";
			} else {
				int j;

				verdict = "lost";
				for (j = 0; j < n; j++) {
					char other[64];

					if (j == i || ses[j].err != 0)
						continue;
					if (peer_mode)
						pair_tag(other, sizeof(other),
							 ses[j].dst,
							 ses[j].src);
					else
						strcpy(other, ses[j].tag);
					if (strstr(ses[i].got, other) != NULL) {
						verdict = "CROSSED";
						break;
					}
				}
				bad = 1;
			}
		}
		printf("%d %s %s>%s %s: %s\n", i,
		       ses[i].port[0] ? ses[i].port : "-",
		       ses[i].src, ses[i].dst, verdict, ses[i].got);
	}

	/*
	 * Close one and speak on the rest.  A session that stopped answering
	 * because a different one was closed is the fault being looked for,
	 * and it needs the second exchange to show - the first proves only
	 * that they came up.
	 */
	if (close_which >= 0 && close_which < n && n > 1) {
		int shut = close_which;

		if (ses[shut].fd >= 0) {
			close(ses[shut].fd);
			ses[shut].fd = -1;
		}
		printf("closed %d (%s>%s), asking the rest again\n", shut,
		       ses[shut].src, ses[shut].dst);

		for (i = 0; i < n; i++) {
			char line[80];

			if (i == shut || ses[i].fd < 0 || ses[i].err != 0)
				continue;
			snprintf(line, sizeof(line), "%s\n", ses[i].tag);
			if (write(ses[i].fd, line, strlen(line)) < 0) {
				printf("%d %s>%s AFTER: write: %s\n", i,
				       ses[i].src, ses[i].dst,
				       strerror(errno));
				bad = 1;
			}
		}
		for (i = 0; i < n; i++) {
			char want[64], got[256];

			if (i == shut || ses[i].fd < 0 || ses[i].err != 0)
				continue;
			if (peer_mode)
				pair_tag(want, sizeof(want), ses[i].dst,
					 ses[i].src);
			else
				strcpy(want, ses[i].tag);
			if (gather(ses[i].fd, got, sizeof(got), want, 3000)) {
				printf("%d %s>%s AFTER: still there\n", i,
				       ses[i].src, ses[i].dst);
			} else {
				char *nl;

				while ((nl = strchr(got, '\n')) != NULL)
					*nl = '|';
				printf("%d %s>%s AFTER: GONE: %s\n", i,
				       ses[i].src, ses[i].dst,
				       got[0] ? got : "(nothing)");
				bad = 1;
			}
		}
	}

	for (i = 0; i < n; i++)
		if (ses[i].fd >= 0)
			close(ses[i].fd);
	return bad;
}

/*
 * The same question on the listening side, and the one with the history:
 * two callsigns listened for in one program, two calls arriving, and the
 * data of one must not surface on the other.  This is where a fault showed
 * up twice before - first only one session was possible at all, then the
 * sessions were possible but the traffic of one landed on the other.
 *
 * As above, every session says its own name and the far end echoes it, so
 * nothing has to be trusted about the other end except that it echoes.  The
 * caller's callsign is checked as well, because it is the second half of the
 * same question: the right data reaching the wrong session and the right
 * session reporting the wrong peer are one fault seen from two sides.
 */

static int gather(int fd, char *buf, size_t buflen, const char *want,
		  int msec)
{
	size_t used = 0;

	buf[0] = '\0';
	while (used + 1 < buflen) {
		struct pollfd pfd;
		ssize_t r;

		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, msec) <= 0)
			return 0;
		if ((r = read(fd, buf + used, buflen - used - 1)) <= 0)
			return 0;
		used += (size_t) r;
		buf[used] = '\0';
		if (strstr(buf, want) != NULL)
			return 1;
	}
	return 0;
}

static int multilisten(int argc, char **argv, int optind_, const char *portcall)
{
	struct session ses[MULTI_MAX];
	int lfd[MULTI_MAX];
	int n = 0, i, open_ = 0, bad = 0;

	memset(ses, 0, sizeof(ses));
	while (optind_ < argc && n < MULTI_MAX) {
		const char *spec = argv[optind_++];
		const char *at = strchr(spec, '@');
		size_t len = at ? (size_t)(at - spec) : strlen(spec);

		if (len == 0 || len >= sizeof(ses[0].src) ||
		    (at != NULL && strlen(at + 1) >= sizeof(ses[0].port))) {
			fprintf(stderr, "axprobe: bad spec '%s'\n", spec);
			return 1;
		}
		memcpy(ses[n].src, spec, len);
		if (at != NULL)
			strcpy(ses[n].port, at + 1);
		ses[n].fd = -1;
		lfd[n] = -1;
		n++;
	}
	if (n == 0)
		usage();

	/* Every listener up before any call is taken. */
	for (i = 0; i < n; i++) {
		const char *pc = portcall;
		char own[16];

		if (ses[i].port[0] != '\0') {
			if (port_callsign(axports_file, ses[i].port, own,
					  sizeof(own)) < 0)
				return 1;
			pc = own;
		}
		if ((lfd[i] = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
			perror("axprobe: socket");
			return 1;
		}
		if (bind_port(lfd[i], pc, ses[i].src) < 0)
			return 1;
		if (p_listen(lfd[i], 1) < 0) {
			fprintf(stderr, "axprobe: listen %s: %s\n", ses[i].src,
				strerror(errno));
			return 1;
		}
		if (verbose)
			fprintf(stderr, "axprobe: listening for %s on fd %d\n",
				ses[i].src, lfd[i]);
		open_++;
	}

	/* Take them as they come, not in the order they were asked for. */
	while (open_ > 0) {
		struct pollfd pfd[MULTI_MAX];
		int map[MULTI_MAX], np = 0;

		for (i = 0; i < n; i++) {
			if (lfd[i] < 0 || ses[i].fd >= 0)
				continue;
			pfd[np].fd = lfd[i];
			pfd[np].events = POLLIN;
			map[np] = i;
			np++;
		}
		if (np == 0)
			break;
		if (poll(pfd, (nfds_t) np, 6000) <= 0)
			break;
		for (i = 0; i < np; i++) {
			struct full_sockaddr_ax25 sa;
			socklen_t len = sizeof(sa);
			int k = map[i];

			if (!(pfd[i].revents & POLLIN))
				continue;
			memset(&sa, 0, sizeof(sa));
			ses[k].fd = p_accept(lfd[k], (struct sockaddr *)&sa,
					     &len);
			if (ses[k].fd < 0) {
				ses[k].err = errno;
				open_--;
				continue;
			}
			snprintf(ses[k].dst, sizeof(ses[k].dst), "%s",
				 ntoa(sa.fsa_ax25.sax25_call.ax25_call));
			open_--;
		}
	}

	for (i = 0; i < n; i++) {
		char line[80];

		if (ses[i].fd < 0)
			continue;
		pair_tag(ses[i].tag, sizeof(ses[i].tag), ses[i].src,
			 ses[i].dst);
		snprintf(line, sizeof(line), "%s\n", ses[i].tag);
		if (write(ses[i].fd, line, strlen(line)) < 0)
			fprintf(stderr, "axprobe: %d write: %s\n", i,
				strerror(errno));
	}

	for (i = 0; i < n; i++) {
		char want[64];

		if (ses[i].fd < 0)
			continue;
		if (peer_mode)
			pair_tag(want, sizeof(want), ses[i].dst, ses[i].src);
		else
			strcpy(want, ses[i].tag);
		if (!gather(ses[i].fd, ses[i].got, sizeof(ses[i].got), want,
			    3000) && ses[i].got[0] == '\0')
			snprintf(ses[i].got, sizeof(ses[i].got), "(nothing)");
	}

	for (i = 0; i < n; i++) {
		const char *verdict;
		char *nl;

		if (ses[i].fd < 0) {
			printf("%d %s no call (%s)\n", i, ses[i].src,
			       ses[i].err ? strerror(ses[i].err) : "timeout");
			bad = 1;
			continue;
		}
		while ((nl = strchr(ses[i].got, '\n')) != NULL)
			*nl = '|';
		{
			char want[64];

			if (peer_mode)
				pair_tag(want, sizeof(want), ses[i].dst,
					 ses[i].src);
			else
				strcpy(want, ses[i].tag);
			if (strstr(ses[i].got, want) != NULL) {
				verdict = "ok";
			} else {
				int j;

				verdict = "lost";
				for (j = 0; j < n; j++) {
					char other[64];

					if (j == i || ses[j].fd < 0)
						continue;
					if (peer_mode)
						pair_tag(other, sizeof(other),
							 ses[j].dst,
							 ses[j].src);
					else
						strcpy(other, ses[j].tag);
					if (strstr(ses[i].got, other) != NULL) {
						verdict = "CROSSED";
						break;
					}
				}
				bad = 1;
			}
		}
		printf("%d %s <- %s %s: %s\n", i, ses[i].src, ses[i].dst,
		       verdict, ses[i].got);
	}

	for (i = 0; i < n; i++) {
		if (ses[i].fd >= 0)
			close(ses[i].fd);
		if (lfd[i] >= 0)
			close(lfd[i]);
	}
	return bad;
}

/*
 * Datagram sockets, several at once.
 *
 * A UI frame carries no session, so the only thing that keeps two of them
 * apart is the callsign each socket bound - which makes this the same
 * question as the two modes above, asked where there is no connection to
 * hide behind.  Each socket sends a line naming the pair it belongs to and
 * every socket reports what reached it, so a frame delivered to the wrong
 * socket, or to every socket, is visible either way.
 *
 * Delivering a copy to everyone is worth telling from crossing: the kernel
 * may well do that (see doc/TODO.md), and it is not the same fault.
 */

static int multiui(int argc, char **argv, int optind_, const char *portcall)
{
	struct session ses[MULTI_MAX];
	int n = 0, i, bad = 0;

	memset(ses, 0, sizeof(ses));
	while (optind_ < argc && n < MULTI_MAX) {
		const char *spec = argv[optind_++];
		const char *colon = strchr(spec, ':');
		size_t len = colon ? (size_t)(colon - spec) : strlen(spec);

		if (len == 0 || len >= sizeof(ses[0].src)) {
			fprintf(stderr, "axprobe: bad spec '%s'\n", spec);
			return 1;
		}
		memcpy(ses[n].src, spec, len);
		if (colon != NULL) {
			if (strlen(colon + 1) >= sizeof(ses[0].dst)) {
				fprintf(stderr, "axprobe: bad spec '%s'\n",
					spec);
				return 1;
			}
			strcpy(ses[n].dst, colon + 1);
		}
		ses[n].fd = -1;
		n++;
	}
	if (n == 0)
		usage();

	for (i = 0; i < n; i++) {
		if ((ses[i].fd = p_socket(AF_AX25, SOCK_DGRAM, 0)) < 0) {
			perror("axprobe: socket");
			return 1;
		}
		if (bind_port(ses[i].fd, portcall, ses[i].src) < 0)
			return 1;
		if (verbose)
			fprintf(stderr, "axprobe: %s bound on fd %d\n",
				ses[i].src, ses[i].fd);
	}

	for (i = 0; i < n; i++) {
		struct full_sockaddr_ax25 to;
		char line[80];

		if (ses[i].dst[0] == '\0')
			continue;
		pair_tag(ses[i].tag, sizeof(ses[i].tag), ses[i].src,
			 ses[i].dst);
		snprintf(line, sizeof(line), "%s\n", ses[i].tag);
		memset(&to, 0, sizeof(to));
		to.fsa_ax25.sax25_family = AF_AX25;
		if (aton_entry(ses[i].dst,
			       to.fsa_ax25.sax25_call.ax25_call) < 0) {
			fprintf(stderr, "axprobe: invalid destination '%s'\n",
				ses[i].dst);
			return 1;
		}
		to.fsa_ax25.sax25_ndigis = 0;
		if (sendto(ses[i].fd, line, strlen(line), 0,
			   (struct sockaddr *)&to, sizeof(to)) < 0)
			fprintf(stderr, "axprobe: %s sendto %s: %s\n",
				ses[i].src, ses[i].dst, strerror(errno));
	}

	for (i = 0; i < n; i++) {
		struct pollfd pfd;
		ssize_t r;

		pfd.fd = ses[i].fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 3000) <= 0) {
			snprintf(ses[i].got, sizeof(ses[i].got), "(nothing)");
			continue;
		}
		if ((r = recvfrom(ses[i].fd, ses[i].got,
				  sizeof(ses[i].got) - 1, 0, NULL, NULL)) <= 0) {
			snprintf(ses[i].got, sizeof(ses[i].got), "(eof)");
			continue;
		}
		ses[i].got[r] = '\0';
		while (r > 0 && (ses[i].got[r - 1] == '\n' ||
				 ses[i].got[r - 1] == '\r'))
			ses[i].got[--r] = '\0';
	}

	/*
	 * Who was this frame for?  The mark names the pair, so the tail of it
	 * is the callsign it was addressed to - and that is the only thing
	 * worth checking, because the sender may well be in another process.
	 * A frame is ours when the mark ends in our callsign, someone else's
	 * when it ends in the callsign of another socket here.
	 */
	for (i = 0; i < n; i++) {
		const char *verdict = "lost";
		const char *tok = strstr(ses[i].got, "pair-");
		char end[24];
		int j;

		if (tok != NULL) {
			char token[96];
			size_t k = 0;

			while (tok[k] != '\0' && !isspace((unsigned char)tok[k]) &&
			       k + 1 < sizeof(token)) {
				token[k] = tok[k];
				k++;
			}
			token[k] = '\0';

			snprintf(end, sizeof(end), "-%s", ses[i].src);
			if (k >= strlen(end) &&
			    strcasecmp(token + k - strlen(end), end) == 0) {
				verdict = "ok";
			} else {
				for (j = 0; j < n; j++) {
					if (j == i)
						continue;
					snprintf(end, sizeof(end), "-%s",
						 ses[j].src);
					if (k >= strlen(end) &&
					    strcasecmp(token + k - strlen(end),
						       end) == 0) {
						verdict = "CROSSED";
						break;
					}
				}
			}
		}
		if (strcmp(verdict, "ok") != 0)
			bad = 1;
		printf("%d %s <- %s: %s\n", i, ses[i].src, verdict,
		       ses[i].got);
	}

	for (i = 0; i < n; i++)
		if (ses[i].fd >= 0)
			close(ses[i].fd);
	return bad;
}

/*
 * A fast sender and a slow reader, which is the pair that used to lose data.
 *
 * Every line carries its own number, so the check is not "did roughly the
 * right amount arrive" but "is line 4711 the one after 4710": a hole in the
 * middle of the stream is what a dropped frame tail actually looks like, and
 * counting bytes would not see it.  The reader sleeps first so that the
 * socketpair fills while nothing is draining it - without that pause the
 * application keeps up and the interesting path is never taken.
 */

#define FLOOD_LINE	1024

static int flood(const char *portcall, const char *src, const char *dst,
		 int count, int linger_ms)
{
	struct full_sockaddr_ax25 sa;
	char line[FLOOD_LINE + 1];
	int fd, i;

	if ((fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(fd, portcall, strcmp(src, "-") ? src : portcall) < 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (aton_entry(dst, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid destination '%s'\n", dst);
		return 1;
	}
	if (p_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("axprobe: connect");
		return 1;
	}

	memset(line, '.', sizeof(line));
	for (i = 0; i < count; i++) {
		size_t off = 0;

		snprintf(line, sizeof(line), "SEQ %06d ", i);
		line[strlen(line)] = '.';	/* undo the terminator */
		line[FLOOD_LINE - 1] = '\n';
		while (off < FLOOD_LINE) {
			ssize_t n = write(fd, line + off, FLOOD_LINE - off);

			if (n < 0) {
				fprintf(stderr, "axprobe: write at line %d: %s\n",
					i, strerror(errno));
				close(fd);
				return 1;
			}
			off += (size_t) n;
		}
	}
	if (verbose)
		fprintf(stderr, "axprobe: sent %d lines of %d bytes\n", count,
			FLOOD_LINE);
	/* How long to wait before closing.  Zero is the case that matters:
	 * the disconnect arrives right behind the last frame, while the far
	 * end is still working through what it was sent.  DISC says "done,
	 * all of it delivered" - so everything sent before it has to reach
	 * the application, and closing on the spot is what proves it. */
	if (linger_ms > 0)
		poll(NULL, 0, linger_ms);
	close(fd);
	return 0;
}

static int sink(const char *portcall, const char *call, int slow_ms)
{
	struct full_sockaddr_ax25 sa;
	socklen_t alen = sizeof(sa);
	char buf[65536];
	char partial[FLOOD_LINE + 1];
	size_t plen = 0;
	long long bytes = 0;
	int fd, nfd, want = 0, holes = 0, disorder = 0;

	if ((fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(fd, portcall, call) < 0)
		return 1;
	if (p_listen(fd, 1) < 0) {
		perror("axprobe: listen");
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	if ((nfd = p_accept(fd, (struct sockaddr *)&sa, &alen)) < 0) {
		perror("axprobe: accept");
		return 1;
	}
	if (verbose)
		fprintf(stderr, "axprobe: call from %s, sleeping %d ms\n",
			ntoa(sa.fsa_ax25.sax25_call.ax25_call), slow_ms);

	/* Nothing is read while this runs: the socketpair fills up, and the
	 * library has to hold what it cannot deliver. */
	poll(NULL, 0, slow_ms);

	for (;;) {
		struct pollfd pfd;
		ssize_t n;
		size_t off = 0;

		pfd.fd = nfd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 5000) <= 0)
			break;
		if ((n = read(nfd, buf, sizeof(buf))) <= 0)
			break;
		bytes += n;

		/* Line by line, across read boundaries. */
		while (off < (size_t) n) {
			size_t k = off;

			while (k < (size_t) n && buf[k] != '\n')
				k++;
			if (k == (size_t) n) {		/* no newline yet */
				size_t rest = (size_t) n - off;

				if (plen + rest < sizeof(partial)) {
					memcpy(partial + plen, buf + off, rest);
					plen += rest;
				}
				break;
			}
			{
				char whole[FLOOD_LINE + 1];
				size_t len = k - off;
				int got;

				if (plen > 0) {
					if (plen + len >= sizeof(whole))
						len = sizeof(whole) - plen - 1;
					memcpy(whole, partial, plen);
					memcpy(whole + plen, buf + off, len);
					whole[plen + len] = '\0';
					plen = 0;
				} else {
					if (len >= sizeof(whole))
						len = sizeof(whole) - 1;
					memcpy(whole, buf + off, len);
					whole[len] = '\0';
				}
				if (sscanf(whole, "SEQ %d", &got) == 1) {
					if (got == want) {
						want++;
					} else if (got > want) {
						holes += got - want;
						want = got + 1;
					} else {
						disorder++;
					}
				}
			}
			off = k + 1;
		}
	}

	printf("sink: %d lines, %lld bytes, %d missing, %d out of order\n",
	       want, bytes, holes, disorder);
	close(nfd);
	close(fd);
	return (holes != 0 || disorder != 0) ? 1 : 0;
}

/*
 * The same descriptor number, again and again.
 *
 *	fd = socket(); bind(); connect(); ... ; close(fd); fd = socket();
 *
 * The kernel hands back the lowest free number, so the second socket is
 * usually the first one's number over again.  If anything of the old session
 * is still in the table under that number - or worse, still matching on the
 * callsigns it was using - the new session inherits it, and the symptom is
 * traffic surfacing where it does not belong.
 *
 * Each round says who it is, so an echo from the round before would be
 * recognised as such rather than counted as success.  The descriptor number
 * is printed with it: rounds that do not reuse the number never asked the
 * question.
 */

static int churn(const char *portcall, const char *src, const char *dst,
		 int rounds)
{
	int r, bad = 0, first_fd = -1, reused = 0;

	for (r = 0; r < rounds; r++) {
		struct full_sockaddr_ax25 sa;
		char tag[64], line[80], got[256];
		int fd;

		snprintf(tag, sizeof(tag), "round-%d-%s-%s", r, src, dst);

		if ((fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
			perror("axprobe: socket");
			return 1;
		}
		if (r == 0)
			first_fd = fd;
		else if (fd == first_fd)
			reused++;

		if (bind_port(fd, portcall, strcmp(src, "-") ? src : portcall)
		    < 0)
			return 1;
		memset(&sa, 0, sizeof(sa));
		sa.fsa_ax25.sax25_family = AF_AX25;
		if (aton_entry(dst, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
			fprintf(stderr, "axprobe: invalid destination\n");
			return 1;
		}
		if (p_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			printf("%d fd=%d connect: %s\n", r, fd,
			       strerror(errno));
			close(fd);
			bad = 1;
			continue;
		}

		snprintf(line, sizeof(line), "%s\n", tag);
		if (write(fd, line, strlen(line)) < 0) {
			printf("%d fd=%d write: %s\n", r, fd, strerror(errno));
			close(fd);
			bad = 1;
			continue;
		}

		if (!gather(fd, got, sizeof(got), tag, 3000)) {
			char *nl;

			while ((nl = strchr(got, '\n')) != NULL)
				*nl = '|';
			printf("%d fd=%d STALE-OR-LOST: %s\n", r, fd,
			       got[0] ? got : "(nothing)");
			bad = 1;
		} else {
			printf("%d fd=%d ok\n", r, fd);
		}
		close(fd);
	}

	printf("churn: %d rounds, %d reused fd %d\n", rounds, reused,
	       first_fd);
	if (reused == 0)
		printf("churn: the number was never reused - "
		       "this run proves nothing\n");
	return bad;
}

/* The other half of churn: take one call after another and echo. */
static int echoserver(const char *portcall, const char *call, int rounds)
{
	int fd, r;

	if ((fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(fd, portcall, call) < 0)
		return 1;
	if (p_listen(fd, 1) < 0) {
		perror("axprobe: listen");
		return 1;
	}

	for (r = 0; r < rounds; r++) {
		struct full_sockaddr_ax25 sa;
		socklen_t alen = sizeof(sa);
		char buf[4096];
		int nfd;

		struct pollfd lp;

		/* Poll first, so this works against a build whose accept()
		 * answers EAGAIN instead of waiting. */
		lp.fd = fd;
		lp.events = POLLIN;
		if (poll(&lp, 1, 15000) <= 0)
			break;
		memset(&sa, 0, sizeof(sa));
		if ((nfd = p_accept(fd, (struct sockaddr *)&sa, &alen)) < 0) {
			perror("axprobe: accept");
			break;
		}
		for (;;) {
			struct pollfd pfd;
			ssize_t n;

			pfd.fd = nfd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 4000) <= 0)
				break;
			if ((n = read(nfd, buf, sizeof(buf))) <= 0)
				break;
			if (write(nfd, buf, (size_t) n) < 0)
				break;
		}
		close(nfd);
		if (verbose)
			fprintf(stderr, "axprobe: round %d served\n", r);
	}
	close(fd);
	return 0;
}

/*
 * Payloads that look like protocol.
 *
 * A frame is content, not text: a line ending inside it is a byte like any
 * other, and so is a NUL, and so is a line that reads exactly like an answer
 * from the node.  What decides where a frame ends is a count, and the point
 * of this is to check that the count is what decides - not a newline that
 * happened to be in the payload, and not a "*** " at the front of it.
 *
 * Every case goes out, comes back from an echoing far end, and is compared
 * byte for byte.  Length is compared too: something that swallowed a NUL or
 * stopped at a newline would still return a plausible-looking string.
 */

struct evilcase {
	const char	*what;
	const char	*data;
	size_t		len;
};

static int read_exactly(int fd, char *buf, size_t want, int msec)
{
	size_t got = 0;

	while (got < want) {
		struct pollfd pfd;
		ssize_t n;

		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, msec) <= 0)
			return -1;
		if ((n = read(fd, buf + got, want - got)) <= 0)
			return -1;
		got += (size_t) n;
	}
	return 0;
}

static int evil(const char *portcall, const char *src, const char *dst,
		int dgram)
{
	static char every[256], big[600], boundary[256];
	struct evilcase cases[] = {
		{ "plain",		"hello",		5 },
		{ "LF in the middle",	"one\ntwo",		7 },
		{ "CR in the middle",	"one\rtwo",		7 },
		{ "CRLF",		"one\r\ntwo",		8 },
		{ "NUL in the middle",	"one\0two",		7 },
		{ "leading LF",		"\nafter",		6 },
		{ "trailing LF",	"before\n",		7 },
		{ "only a LF",		"\n",			1 },
		{ "a node answer",	"*** connected to DB0XXX\n", 24 },
		{ "a link failure",	"*** link failure with X - busy\n", 31 },
		{ "a counted header",	"[5]DL1ABC>DB0AAA:12345", 22 },
		{ "brackets and colon",	"]:[99]x:y", 9 },
		{ "every byte value",	every,			sizeof(every) },
		{ "256 bytes",		boundary,		sizeof(boundary) },
		{ "600 bytes",		big,			sizeof(big) },
	};
	struct full_sockaddr_ax25 sa;
	int fd, i, bad = 0;
	size_t k;

	for (k = 0; k < sizeof(every); k++)
		every[k] = (char) k;
	memset(boundary, 'B', sizeof(boundary));
	memset(big, 'G', sizeof(big));

	if ((fd = p_socket(AF_AX25, dgram ? SOCK_DGRAM : SOCK_SEQPACKET, 0))
	    < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(fd, portcall, strcmp(src, "-") ? src : portcall) < 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (aton_entry(dst, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid destination\n");
		return 1;
	}
	if (!dgram && p_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("axprobe: connect");
		return 1;
	}

	for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		char back[700];
		size_t off = 0;

		if (dgram) {
			/* Nothing comes back: what the node made of it is on
			 * the node, and that is where the count is read. */
			if (sendto(fd, cases[i].data, cases[i].len, 0,
				   (struct sockaddr *)&sa, sizeof(sa)) < 0) {
				printf("%-20s sendto: %s\n", cases[i].what,
				       strerror(errno));
				bad = 1;
			} else {
				printf("%-20s %zu bytes sent\n",
				       cases[i].what, cases[i].len);
			}
			continue;
		}

		while (off < cases[i].len) {
			ssize_t n = write(fd, cases[i].data + off,
					  cases[i].len - off);

			if (n < 0) {
				printf("%-20s write: %s\n", cases[i].what,
				       strerror(errno));
				bad = 1;
				break;
			}
			off += (size_t) n;
		}
		if (off < cases[i].len)
			continue;

		if (read_exactly(fd, back, cases[i].len, 4000) < 0) {
			printf("%-20s %zu bytes out, did not all come back\n",
			       cases[i].what, cases[i].len);
			bad = 1;
			continue;
		}
		if (memcmp(back, cases[i].data, cases[i].len) != 0) {
			size_t j;

			for (j = 0; j < cases[i].len; j++)
				if (back[j] != cases[i].data[j])
					break;
			printf("%-20s CHANGED at byte %zu: sent 0x%02x, got 0x%02x\n",
			       cases[i].what, j,
			       (unsigned char) cases[i].data[j],
			       (unsigned char) back[j]);
			bad = 1;
			continue;
		}
		printf("%-20s %zu bytes, identical\n", cases[i].what,
		       cases[i].len);
	}

	close(fd);
	return bad;
}

/*
 * Listening and connecting at the same time, in one process.
 *
 * Two of these call each other: each one listens for its own callsign and
 * connects to the other's, so each ends up holding an outgoing session and
 * an incoming one at once - which is what a node program does all day and
 * what no single-purpose mode here was covering.  The two sessions share the
 * table, the lock and the dispatch, and the question is whether they stay
 * apart while they are both up.
 *
 * The marks name the pair, so each side can say what it expects to read on
 * either socket without being told anything by the other.
 */

static int mixed(const char *portcall, const char *listencall,
		 const char *src, const char *dst)
{
	struct full_sockaddr_ax25 sa;
	socklen_t alen = sizeof(sa);
	char otag[64], itag[64], owant[64], iwant[64], got[256], line[80];
	char peer[16];
	struct pollfd pfd;
	int lfd, ofd, ifd = -1, bad = 0;

	/* Listener up before the call goes out, or the other side may find
	 * nobody home. */
	if ((lfd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(lfd, portcall, listencall) < 0)
		return 1;
	if (p_listen(lfd, 1) < 0) {
		perror("axprobe: listen");
		return 1;
	}

	/*
	 * Let the other side get its listener up before calling it.  Not
	 * politeness: ax25netd(8) refuses a connect to a callsign nobody
	 * has registered yet - "kein Besitzer", answered with a retryout
	 * disconnect - which is right, it is what a station with nobody
	 * listening looks like.  Three seconds, because the first AX.25
	 * socket in a process opens the connection to the server and
	 * fetches the port table before anything else, so "the listener
	 * is up" comes a good deal later than "the program started".
	 */
	poll(NULL, 0, 3000);

	if ((ofd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (bind_port(ofd, portcall, src) < 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.fsa_ax25.sax25_family = AF_AX25;
	if (aton_entry(dst, sa.fsa_ax25.sax25_call.ax25_call) < 0) {
		fprintf(stderr, "axprobe: invalid destination\n");
		return 1;
	}
	if (p_connect(ofd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("out %s>%s connect: %s\n", src, dst, strerror(errno));
		bad = 1;
	} else {
		pair_tag(otag, sizeof(otag), src, dst);
		snprintf(line, sizeof(line), "%s\n", otag);
		if (write(ofd, line, strlen(line)) < 0) {
			printf("out %s>%s write: %s\n", src, dst,
			       strerror(errno));
			bad = 1;
		}
	}

	/* Now the call from the other side. */
	pfd.fd = lfd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 8000) > 0) {
		memset(&sa, 0, sizeof(sa));
		ifd = p_accept(lfd, (struct sockaddr *)&sa, &alen);
	}
	if (ifd < 0) {
		printf("in  %s: no call arrived\n", listencall);
		bad = 1;
	} else {
		snprintf(peer, sizeof(peer), "%s",
			 ntoa(sa.fsa_ax25.sax25_call.ax25_call));
		pair_tag(itag, sizeof(itag), listencall, peer);
		snprintf(line, sizeof(line), "%s\n", itag);
		if (write(ifd, line, strlen(line)) < 0) {
			printf("in  %s<%s write: %s\n", listencall, peer,
			       strerror(errno));
			bad = 1;
		}
	}

	/* What each socket should be holding: the other side's mark. */
	if (ofd >= 0) {
		if (peer_mode)
			pair_tag(owant, sizeof(owant), dst, src);
		else
			strcpy(owant, otag);
		if (gather(ofd, got, sizeof(got), owant, 5000)) {
			printf("out %s>%s ok\n", src, dst);
		} else {
			char *nl;

			while ((nl = strchr(got, '\n')) != NULL)
				*nl = '|';
			if (ifd >= 0 && strstr(got, iwant) != NULL)
				printf("out %s>%s CROSSED: %s\n", src, dst,
				       got);
			else
				printf("out %s>%s lost: %s\n", src, dst,
				       got[0] ? got : "(nothing)");
			bad = 1;
		}
	}
	if (ifd >= 0) {
		if (peer_mode)
			pair_tag(iwant, sizeof(iwant), peer, listencall);
		else
			strcpy(iwant, itag);
		if (gather(ifd, got, sizeof(got), iwant, 5000)) {
			printf("in  %s<%s ok\n", listencall, peer);
		} else {
			char *nl;

			while ((nl = strchr(got, '\n')) != NULL)
				*nl = '|';
			if (strstr(got, owant) != NULL)
				printf("in  %s<%s CROSSED: %s\n", listencall,
				       peer, got);
			else
				printf("in  %s<%s lost: %s\n", listencall,
				       peer, got[0] ? got : "(nothing)");
			bad = 1;
		}
	}

	if (ifd >= 0)
		close(ifd);
	close(ofd);
	close(lfd);
	return bad;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: axprobe [-d] [-q] [-f axports] connect <port> <src|-> <dest> [<digi> ...]\n"
		"       axprobe [-d] [-q] [-f axports] listen  <port> <call>\n"
		"       axprobe [-d] [-q] [-f axports] bind    <port> <call>\n"
		"       axprobe [-d] [-q] [-f axports] multi   <port> <src>:<dest>[@<port>] ...\n"
		"       axprobe [-d] [-q] [-f axports] mlisten <port> <call>[@<port>] ...\n"
		"       axprobe [-d] [-q] [-f axports] mui     <port> <call>[:<dest>] ...\n"
		"       axprobe [-d] [-q] [-f axports] flood   <port> <src>:<dest> <lines> [<linger-ms>]\n"
		"       axprobe [-d] [-q] [-f axports] sink    <port> <call> <sleep-ms>\n"
		"       axprobe [-d] [-q] [-f axports] churn   <port> <src>:<dest> <rounds>\n"
		"       axprobe [-d] [-q] [-f axports] echo    <port> <call> <rounds>\n"
		"       axprobe [-d] [-q] [-f axports] evil    <port> <src>:<dest> [ui]\n"
		"       axprobe [-d] [-q] [-f axports] mixed   <port> <listen> <src>:<dest>\n"
		"\n"
		"  -d  reach the socket calls through dlsym(RTLD_DEFAULT) instead of\n"
		"      calling them directly - the only way an inserted library is seen\n"
		"      on macOS, and it says where each call came from\n"
		"  -p  the far end is a partner, not an echo: each session expects\n"
		"      the line of the station it is talking to (ax25netd loop port,\n"
		"      or the other half of this same test)\n"
		"  -z  multi: close this session after the first exchange and ask the\n"
		"      others again - 0 is the one opened first\n"
		"  -q  no commentary on stderr\n"
		"  -f  axports to read instead of " CONF_AXPORTS_FILE "\n"
		"\n"
		"A source callsign of - means the callsign of the axports entry.\n");
	exit(1);
}

int main(int argc, char **argv)
{
	const char *axports = CONF_AXPORTS_FILE;
	const char *cmd, *port, *call;
	char portcall[16];
	int use_dlsym = 0;
	int fd, c;

	while ((c = getopt(argc, argv, "df:pqz:")) != -1) {
		switch (c) {
		case 'd':
			use_dlsym = 1;
			break;
		case 'f':
			axports = optarg;
			break;
		case 'z':
			close_which = atoi(optarg);
			break;
		case 'p':
			peer_mode = 1;
			break;
		case 'q':
			verbose = 0;
			break;
		default:
			usage();
		}
	}
	if (argc - optind < 3)
		usage();

	cmd = argv[optind++];
	port = argv[optind++];
	call = argv[optind++];

	bind_calls(use_dlsym);

	if (port_callsign(axports, port, portcall, sizeof(portcall)) < 0)
		return 1;

	if ((fd = p_socket(AF_AX25, SOCK_SEQPACKET, 0)) < 0) {
		perror("axprobe: socket");
		return 1;
	}
	if (verbose)
		fprintf(stderr, "axprobe: socket(AF_AX25) = %d\n", fd);

	if (strcmp(cmd, "connect") == 0) {
		struct full_sockaddr_ax25 sa;
		int n = 0;

		if (bind_port(fd, portcall,
			      strcmp(call, "-") == 0 ? portcall : call) < 0)
			return 1;

		if (optind >= argc) {
			fprintf(stderr, "axprobe: connect needs a destination\n");
			return 1;
		}
		memset(&sa, 0, sizeof(sa));
		sa.fsa_ax25.sax25_family = AF_AX25;
		if (aton_entry(argv[optind++],
			       sa.fsa_ax25.sax25_call.ax25_call) < 0) {
			fprintf(stderr, "axprobe: invalid destination\n");
			return 1;
		}
		while (optind < argc && n < AX25_MAX_DIGIS) {
			if (aton_entry(argv[optind++],
				       sa.fsa_digipeater[n].ax25_call) < 0) {
				fprintf(stderr, "axprobe: invalid digipeater\n");
				return 1;
			}
			n++;
		}
		sa.fsa_ax25.sax25_ndigis = n;

		if (p_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			perror("axprobe: connect");
			return 1;
		}
		if (verbose) {
			fprintf(stderr, "axprobe: connected\n");
			report(fd);
		}
		return shovel(fd);
	}

	if (strcmp(cmd, "listen") == 0) {
		struct full_sockaddr_ax25 sa;
		socklen_t len = sizeof(sa);
		int nfd;

		if (bind_port(fd, portcall, call) < 0)
			return 1;
		if (p_listen(fd, 1) < 0) {
			perror("axprobe: listen");
			return 1;
		}
		if (verbose)
			fprintf(stderr, "axprobe: listening for %s, waiting\n",
				call);
		memset(&sa, 0, sizeof(sa));
		if ((nfd = p_accept(fd, (struct sockaddr *)&sa, &len)) < 0) {
			perror("axprobe: accept");
			return 1;
		}
		fprintf(stderr, "axprobe: call from %s (fd %d)\n",
			ntoa(sa.fsa_ax25.sax25_call.ax25_call), nfd);
		if (verbose)
			report(nfd);
		return shovel(nfd);
	}

	if (strcmp(cmd, "mixed") == 0) {
		const char *spec, *colon;
		char src[16];

		close(fd);
		if (optind >= argc)
			usage();
		spec = argv[optind];
		if ((colon = strchr(spec, ':')) == NULL)
			usage();
		snprintf(src, sizeof(src), "%.*s", (int)(colon - spec), spec);
		return mixed(portcall, call, src, colon + 1);
	}

	if (strcmp(cmd, "evil") == 0) {
		const char *colon = strchr(call, ':');
		char src[16];

		close(fd);
		if (colon == NULL)
			usage();
		snprintf(src, sizeof(src), "%.*s", (int)(colon - call), call);
		return evil(portcall, src, colon + 1,
			    optind < argc && !strcmp(argv[optind], "ui"));
	}

	if (strcmp(cmd, "churn") == 0) {
		const char *colon = strchr(call, ':');
		char src[16];

		close(fd);
		if (colon == NULL || optind >= argc)
			usage();
		snprintf(src, sizeof(src), "%.*s", (int)(colon - call), call);
		return churn(portcall, src, colon + 1, atoi(argv[optind]));
	}

	if (strcmp(cmd, "echo") == 0) {
		close(fd);
		if (optind >= argc)
			usage();
		return echoserver(portcall, call, atoi(argv[optind]));
	}

	if (strcmp(cmd, "flood") == 0) {
		const char *colon = strchr(call, ':');
		char src[16];

		close(fd);
		if (colon == NULL || optind >= argc)
			usage();
		snprintf(src, sizeof(src), "%.*s", (int)(colon - call), call);
		return flood(portcall, src, colon + 1, atoi(argv[optind]),
			     optind + 1 < argc ? atoi(argv[optind + 1]) : 3000);
	}

	if (strcmp(cmd, "sink") == 0) {
		close(fd);
		if (optind >= argc)
			usage();
		return sink(portcall, call, atoi(argv[optind]));
	}

	if (strcmp(cmd, "mui") == 0) {
		close(fd);
		axports_file = axports;
		return multiui(argc, argv, optind - 1, portcall);
	}

	if (strcmp(cmd, "mlisten") == 0) {
		close(fd);
		axports_file = axports;
		return multilisten(argc, argv, optind - 1, portcall);
	}

	if (strcmp(cmd, "multi") == 0) {
		close(fd);		/* multi opens its own, one per session */
		axports_file = axports;
		return multi(argc, argv, optind - 1, portcall);
	}

	if (strcmp(cmd, "bind") == 0) {
		if (bind_port(fd, portcall, call) < 0)
			return 1;
		if (verbose)
			report(fd);
		printf("bind ok\n");
		return 0;
	}

	usage();
	return 1;
}
