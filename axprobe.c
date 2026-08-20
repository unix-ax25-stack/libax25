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

static void usage(void)
{
	fprintf(stderr,
		"usage: axprobe [-d] [-q] [-f axports] connect <port> <src|-> <dest> [<digi> ...]\n"
		"       axprobe [-d] [-q] [-f axports] listen  <port> <call>\n"
		"       axprobe [-d] [-q] [-f axports] bind    <port> <call>\n"
		"\n"
		"  -d  reach the socket calls through dlsym(RTLD_DEFAULT) instead of\n"
		"      calling them directly - the only way an inserted library is seen\n"
		"      on macOS, and it says where each call came from\n"
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

	while ((c = getopt(argc, argv, "df:q")) != -1) {
		switch (c) {
		case 'd':
			use_dlsym = 1;
			break;
		case 'f':
			axports = optarg;
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
