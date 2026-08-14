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
 * AGWPE client support for libax25.
 *
 * This is a C client for the AGWPE TCP/IP protocol (see netax25/agwpe.h).
 * It can be used both by programs talking directly to an AGWPE server
 * (Direwolf, AGWPE on Windows, ...) and by libax25's own daemon which
 * multiplexes several such servers.
 *
 * The client is event driven: after connecting, the application registers
 * the interesting call signs, then selects() on the file descriptor
 * returned by agwpe_client_fd() and calls agwpe_client_recv() whenever it
 * is readable.  Incoming frames are dispatched to the callbacks given to
 * agwpe_client_new().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/tcp.h>

#include "netax25/agwpe.h"
#include "netax25/agwpe_client.h"

#define	AGWPE_BUF_INIT	4096
#define	AGWPE_BUF_MAX	(16 * 1024 * 1024)

struct agwpe_client {
	int			fd;
	struct agwpe_client_cb	cb;
	void			*opaque;
	int			err;

	/*
	 * Serializes the outgoing frame stream.  The shim (libax25
	 * AXSOCK) drops its table lock while a frame is being sent so a
	 * full TCP buffer never blocks every other socket call in the
	 * process; several threads can then send concurrently, and this
	 * mutex keeps whole frames (header + data) contiguous on the wire
	 * instead of interleaved.
	 */
	pthread_mutex_t		wlock;

	unsigned char		*rbuf;
	size_t			rlen;
	size_t			ra;
};

/*
 * Allocate a new client object.  cb may be NULL, in which case the
 * application must use agwpe_client_recv_frame() to read frames itself.
 */
agwpe_client_t *agwpe_client_new(const struct agwpe_client_cb *cb, void *opaque)
{
	agwpe_client_t *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;

	c->fd = -1;
	c->opaque = opaque;
	if (cb != NULL)
		c->cb = *cb;

	pthread_mutex_init(&c->wlock, NULL);

	c->rbuf = malloc(AGWPE_BUF_INIT);
	if (c->rbuf == NULL) {
		free(c);
		return NULL;
	}
	c->ra = AGWPE_BUF_INIT;

	return c;
}

void agwpe_client_free(agwpe_client_t *c)
{
	if (c == NULL)
		return;

	if (c->fd >= 0)
		close(c->fd);
	pthread_mutex_destroy(&c->wlock);
	free(c->rbuf);
	free(c);
}

/*
 * Non-blocking connect with a bounded wait; defined below, used by
 * both connect paths.  Returns 0 on success (fd blocking, open), -1 on
 * failure (fd closed, errno set).
 */
static int agwpe_connect_wait(int fd, const struct sockaddr *sa, socklen_t len);

/*
 * Connect to the AGWPE server.  Returns 0 on success, -1 on error.
 */
int agwpe_client_connect_host(agwpe_client_t *c, const char *host, int tcp_port)
{
	struct addrinfo hints, *res, *ai;
	char portstr[16], *h = NULL;
	int s, save_errno = 0;

	if (host == NULL) {
		c->err = EINVAL;
		return -1;
	}

	/* Accept the bracketed IPv6 notation "[::1]" from configuration
	 * files; getaddrinfo wants the bare address.  */
	if (host[0] == '[') {
		size_t n = strlen(host);

		if (n < 3 || host[n - 1] != ']') {
			c->err = EINVAL;
			return -1;
		}
		h = malloc(n - 1);
		if (h == NULL) {
			c->err = ENOMEM;
			return -1;
		}
		memcpy(h, host + 1, n - 2);
		h[n - 2] = '\0';
		host = h;
	}

	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}

	/* A new connection starts a fresh frame stream.  */
	c->rlen = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(portstr, sizeof(portstr), "%d", tcp_port);

	if (getaddrinfo(host, portstr, &hints, &res) != 0) {
		free(h);
		c->err = EADDRNOTAVAIL;
		return -1;
	}
	free(h);

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0)
			continue;
		if (agwpe_connect_wait(s, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		/* agwpe_connect_wait closed s and set errno */
		save_errno = errno;
	}
	freeaddrinfo(res);

	if (ai == NULL) {
		c->err = save_errno ? save_errno : ECONNREFUSED;
		return -1;
	}

	c->fd = s;
	c->err = 0;

	/* The AGWPE stream is a sequence of small frames sent one at a
	 * time; Nagle's algorithm would hold each of them until the
	 * previous ACK, throttling throughput to ~one frame per RTT.
	 */
	{
		int one = 1;

		(void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one,
				 sizeof(one));
	}

	return 0;
}

void agwpe_client_close(agwpe_client_t *c)
{
	if (c != NULL && c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

/* Bound for a single connect() attempt.  A unix socket completes
 * immediately, but an unreachable TCP peer must not stall the calling
 * daemon; the attempt is aborted after this many milliseconds.  */
#define	AGWPE_CONNECT_TIMEOUT	5000

/*
 * Non-blocking connect with a bounded wait.  The socket is switched to
 * non-blocking for the attempt and back to blocking for the regular
 * frame I/O afterwards.  Returns 0 on success (fd still open, blocking
 * mode), -1 on failure (fd closed, errno set).
 */
static int agwpe_connect_wait(int fd, const struct sockaddr *sa, socklen_t len)
{
	struct pollfd pfd;
	int flags, soerr, rc;
	socklen_t sl;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		errno = EIO;
		goto fail;
	}

	rc = connect(fd, sa, len);
	if (rc < 0 && errno != EINPROGRESS)
		goto fail;

	if (rc < 0) {	/* EINPROGRESS: wait for completion */
		pfd.fd = fd;
		pfd.events = POLLOUT;
		do {
			rc = poll(&pfd, 1, AGWPE_CONNECT_TIMEOUT);
		} while (rc < 0 && errno == EINTR);
		if (rc <= 0) {
			if (rc == 0)
				errno = ETIMEDOUT;
			goto fail;
		}
		sl = sizeof(soerr);
		soerr = 0;
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 ||
		    soerr != 0) {
			if (soerr != 0)
				errno = soerr;
			goto fail;
		}
	}

	/* Back to blocking mode for the frame stream.  */
	(void)fcntl(fd, F_SETFL, flags);
	return 0;

fail:
	(void)fcntl(fd, F_SETFL, flags);
	close(fd);
	return -1;
}

/*
 * Connect to a unix domain socket instead of a TCP port.  The AGWPE
 * frame stream is unchanged, so an ax25netd listening on a unix socket
 * is interchangeable with one listening on TCP; the file permissions of
 * the socket gate who may connect at all.
 */
int agwpe_client_connect_unix(agwpe_client_t *c, const char *path){
	struct sockaddr_un sa;
	int s;

	if (c == NULL)
		return -1;

	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}

	/* A new connection starts a fresh frame stream.  */
	c->rlen = 0;

	if (path == NULL || path[0] == '\0' ||
	    strlen(path) >= sizeof(sa.sun_path)) {
		c->err = EINVAL;
		return -1;
	}

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		c->err = errno;
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

	if (agwpe_connect_wait(s, (struct sockaddr *)&sa, SUN_LEN(&sa)) < 0) {
		c->err = errno;
		return -1;
	}

	c->fd = s;
	c->err = 0;
	return 0;
}

int agwpe_client_fd(const agwpe_client_t *c)
{
	return c->fd;
}

int agwpe_client_connected(const agwpe_client_t *c)
{
	return c->fd >= 0;
}

int agwpe_client_err(const agwpe_client_t *c)
{
	return c->err;
}

/*
 * Send a single frame (header + data area) to the server.
 * data may be NULL when hdr->data_len is zero.
 */
int agwpe_client_send_frame(agwpe_client_t *c, const struct agwpe_s *hdr,
			    const unsigned char *data)
{
	uint32_t dlen;
	ssize_t n;
	int rv = -1;

	if (c == NULL || c->fd < 0) {
		c->err = ENOTCONN;
		return -1;
	}

	/*
	 * Send the whole frame under the write lock so concurrent
	 * threads (the shim releases its table lock around sends) never
	 * interleave header and data bytes of different frames.  The
	 * lock is dropped only on return, so c->fd stays valid for the
	 * duration even if another thread tears the client down.
	 */
	pthread_mutex_lock(&c->wlock);

	if (c->fd < 0) {
		c->err = ENOTCONN;
		goto out;
	}

	dlen = agwpe_netle2host(hdr->data_len);

	n = send(c->fd, hdr, AGWPE_HEADER_LEN, MSG_NOSIGNAL);
	if (n != AGWPE_HEADER_LEN) {
		c->err = errno;
		goto out;
	}

	if (dlen > 0 && data != NULL) {
		size_t sent = 0;
		while (sent < dlen) {
			n = send(c->fd, data + sent, dlen - sent, MSG_NOSIGNAL);
			if (n <= 0) {
				c->err = errno;
				goto out;
			}
			sent += n;
		}
	}

	c->err = 0;
	rv = 0;
out:
	pthread_mutex_unlock(&c->wlock);
	return rv;
}

/*
 * Low level: fill in a header and send it.  Used by the command helpers.
 */
static int agwpe_send_cmd(agwpe_client_t *c, unsigned char port,
			  unsigned char datakind, unsigned char pid,
			  const char *from, const char *to,
			  const unsigned char *data, uint32_t dlen)
{
	struct agwpe_s hdr;

	agwpe_header_init(&hdr, port, datakind, pid, from, to, dlen);
	return agwpe_client_send_frame(c, &hdr, data);
}

int agwpe_client_login(agwpe_client_t *c, const char *user, const char *pass)
{
	unsigned char data[510];
	struct agwpe_s hdr;

	memset(data, 0, sizeof(data));
	strncpy((char *)data, user ? user : "", 254);
	data[254] = '\0';
	strncpy((char *)data + 255, pass ? pass : "", 254);
	data[509] = '\0';

	agwpe_header_init(&hdr, 0, AGWPE_CMD_LOGIN, 0, NULL, NULL, sizeof(data));
	return agwpe_client_send_frame(c, &hdr, data);
}

int agwpe_client_register(agwpe_client_t *c, unsigned char port, const char *call)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_REGISTER, 0, call, NULL, NULL, 0);
}

int agwpe_client_unregister(agwpe_client_t *c, unsigned char port, const char *call)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_UNREGISTER, 0, call, NULL, NULL, 0);
}

/* ax25netd extension: register a listening socket on the virtual loop
 * port (AGWPE_PORT_LOOP) so that inbound connects are routed to it.  */
int agwpe_client_listen(agwpe_client_t *c, unsigned char port, const char *call)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_LISTEN, 0, call, NULL, NULL, 0);
}

int agwpe_client_get_version(agwpe_client_t *c)
{
	return agwpe_send_cmd(c, 0, AGWPE_CMD_VERSION, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_get_ports(agwpe_client_t *c)
{
	return agwpe_send_cmd(c, 0, AGWPE_CMD_PORT_INFO, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_get_capab(agwpe_client_t *c, unsigned char port)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_PORT_CAPAB, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_get_heard(agwpe_client_t *c, unsigned char port)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_HEARD, 0, NULL, NULL, NULL, 0);
}

/*
 * The monitor command is a toggle on both AGWPE and Direwolf, so there is
 * no enable argument.
 */
int agwpe_client_monitor(agwpe_client_t *c)
{
	return agwpe_send_cmd(c, 0, AGWPE_CMD_MONITOR, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_raw_toggle(agwpe_client_t *c)
{
	return agwpe_send_cmd(c, 0, AGWPE_CMD_RAW_MONITOR, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_send_unproto(agwpe_client_t *c, unsigned char port,
			      unsigned char pid, const char *from,
			      const char *to, const unsigned char *data, int len)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_UNPROTO, pid, from, to, data, len);
}

/*
 * Send an UNPROTO (UI) frame through a digipeater path.  digis is an
 * array of "CALLSIGN-SSID" strings, ndigis in the range 1..7.
 */
int agwpe_client_send_unproto_via(agwpe_client_t *c, unsigned char port,
				  unsigned char pid, const char *from,
				  const char *to, const char *const digis[],
				  int ndigis, const unsigned char *data, int len)
{
	unsigned char *buf;
	unsigned char *p;
	uint32_t dlen;
	int i, ret;

	if (ndigis < 1 || ndigis > AGWPE_MAX_DIGIS - 1) {
		c->err = EINVAL;
		return -1;
	}

	dlen = 1 + ndigis * AGWPE_MAX_CALL + len;
	buf = malloc(dlen);
	if (buf == NULL) {
		c->err = ENOMEM;
		return -1;
	}

	buf[0] = ndigis;
	p = buf + 1;
	for (i = 0; i < ndigis; i++) {
		agwpe_call_pack((char *)p, digis[i]);
		p += AGWPE_MAX_CALL;
	}
	if (len > 0)
		memcpy(p, data, len);

	ret = agwpe_send_cmd(c, port, AGWPE_CMD_UNPROTO_VIA, pid, from, to, buf, dlen);
	free(buf);
	return ret;
}

int agwpe_client_connect(agwpe_client_t *c, unsigned char port,
			 unsigned char pid, const char *from, const char *to)
{
	return agwpe_send_cmd(c, port,
		(pid == 0 || pid == AGWPE_PID_AX25) ? AGWPE_CMD_CONNECT : AGWPE_CMD_CONNECT_PID,
		pid, from, to, NULL, 0);
}

int agwpe_client_connect_via(agwpe_client_t *c, unsigned char port,
			     unsigned char pid, const char *from,
			     const char *to, const char *const digis[], int ndigis)
{
	unsigned char *buf;
	unsigned char *p;
	uint32_t dlen;
	int i, ret;

	if (ndigis < 1 || ndigis > AGWPE_MAX_DIGIS - 1) {
		c->err = EINVAL;
		return -1;
	}

	dlen = 1 + ndigis * AGWPE_MAX_CALL;
	buf = malloc(dlen);
	if (buf == NULL) {
		c->err = ENOMEM;
		return -1;
	}

	buf[0] = ndigis;
	p = buf + 1;
	for (i = 0; i < ndigis; i++) {
		agwpe_call_pack((char *)p, digis[i]);
		p += AGWPE_MAX_CALL;
	}

	ret = agwpe_send_cmd(c, port, AGWPE_CMD_CONNECT_VIA, pid, from, to, buf, dlen);
	free(buf);
	return ret;
}

int agwpe_client_send_data(agwpe_client_t *c, unsigned char port,
			   unsigned char pid, const char *from, const char *to,
			   const unsigned char *data, int len)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_DATA, pid, from, to, data, len);
}

int agwpe_client_disconnect(agwpe_client_t *c, unsigned char port,
			    const char *from, const char *to)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_DISCONNECT, 0, from, to, NULL, 0);
}

int agwpe_client_outstanding_port(agwpe_client_t *c, unsigned char port)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_OUTSTANDING_PORT, 0, NULL, NULL, NULL, 0);
}

int agwpe_client_outstanding_conn(agwpe_client_t *c, unsigned char port,
				  const char *from, const char *to)
{
	return agwpe_send_cmd(c, port, AGWPE_CMD_OUTSTANDING_CONN, 0, from, to, NULL, 0);
}

/*
 * Parse the "G" reply: a sequence of ';' separated ASCII tokens, the
 * first one being the number of ports.
 */
static void parse_ports(agwpe_client_t *c, const unsigned char *data, size_t len)
{
	struct agwpe_port_list *list;
	char *s, *p, *tok;

	if (c->cb.ports == NULL)
		return;

	list = calloc(1, sizeof(*list));
	if (list == NULL)
		return;

	s = malloc(len + 1);
	if (s == NULL) {
		free(list);
		return;
	}
	memcpy(s, data, len);
	s[len] = '\0';

	p = s;
	while (p != NULL && list->count < AGWPE_PORT_MAX) {
		tok = p;
		p = strchr(p, ';');
		if (p != NULL)
			*p++ = '\0';

		/* First token is the total number of ports.  */
		if (tok == s && list->count == 0) {
			if (tok[0] == '\0')
				continue;
			list->count = 0;
			continue;
		}

		/* Port token: "PortN description".  */
		if (strncmp(tok, "Port", 4) == 0) {
			char *sp = strchr(tok, ' ');
			int i = list->count;

			if (sp != NULL) {
				size_t nlen = sp - tok;

				if (nlen > sizeof(list->names[i]) - 1)
					nlen = sizeof(list->names[i]) - 1;
				memcpy(list->names[i], tok, nlen);
				list->names[i][nlen] = '\0';
				while (*sp == ' ')
					sp++;
				strncpy(list->descs[i], sp, sizeof(list->descs[i]) - 1);
			} else {
				strncpy(list->names[i], tok, sizeof(list->names[i]) - 1);
			}
			list->count++;
		}
	}

	free(s);

	c->cb.ports(c, list);
	free(list);
}

static void parse_capab(agwpe_client_t *c, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct agwpe_port_capab cap;

	if (c->cb.capab == NULL)
		return;

	memset(&cap, 0, sizeof(cap));
	if (len >= 8) {
		cap.onair_baud = data[0];
		cap.traffic_level = data[1];
		cap.tx_delay = data[2];
		cap.tx_tail = data[3];
		cap.persist = data[4];
		cap.slot_time = data[5];
		cap.max_frame = data[6];
		cap.ax25_channels = data[7];
	}
	if (len >= 12)
		cap.how_many_bytes = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
				     ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

	c->cb.capab(c, hdr, &cap);
}

/*
 * Parse one "H" reply frame.  AGWPE sends 20 of these for each request.
 */
static void parse_heard(agwpe_client_t *c, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct agwpe_heard h;

	if (c->cb.heard == NULL)
		return;

	memset(&h, 0, sizeof(h));
	if (len > 0) {
		const unsigned char *p = data;
		const unsigned char *end = data + len;
		size_t n;
		int field = 0;

		while (p < end && field < 3) {
			const unsigned char *sp = memchr(p, ' ', end - p);
			if (sp == NULL)
				sp = end;

			n = sp - p;
			switch (field) {
			case 0:
				if (n > sizeof(h.call) - 1)
					n = sizeof(h.call) - 1;
				memcpy(h.call, p, n);
				h.call[n] = '\0';
				break;
			case 1:
				if (n > sizeof(h.first) - 1)
					n = sizeof(h.first) - 1;
				memcpy(h.first, p, n);
				h.first[n] = '\0';
				break;
			case 2:
				if (n > sizeof(h.last) - 1)
					n = sizeof(h.last) - 1;
				memcpy(h.last, p, n);
				h.last[n] = '\0';
				break;
			}
			field++;
			p = sp + 1;
		}
	}

	c->cb.heard(c, hdr, &h);
}

static unsigned int parse_outstanding(const unsigned char *data, size_t len)
{
	if (len < 4)
		return 0;
	return (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
	       ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
}

/*
 * Dispatch one complete frame to the appropriate callback.
 */
static void dispatch_frame(agwpe_client_t *c, const struct agwpe_s *hdr,
			   const unsigned char *data, size_t len)
{
	if (c->cb.raw_frame != NULL)
		c->cb.raw_frame(c, hdr, data, len);

	switch (hdr->datakind) {
	case AGWPE_DK_VERSION:
		if (c->cb.version != NULL) {
			unsigned int major = 0, minor = 0;

			if (len >= 2)
				major = data[0] | (data[1] << 8);
			if (len >= 6)
				minor = data[4] | (data[5] << 8);
			c->cb.version(c, major, minor);
		}
		break;

	case AGWPE_DK_REGISTERED:
		if (c->cb.registered != NULL)
			c->cb.registered(c, hdr, len >= 1 && data[0] != 0);
		break;

	case AGWPE_DK_PORTS:
		parse_ports(c, data, len);
		break;

	case AGWPE_DK_CAPAB:
		parse_capab(c, hdr, data, len);
		break;

	case AGWPE_DK_HEARD:
		parse_heard(c, hdr, data, len);
		break;

	case AGWPE_DK_OUTSTANDING_PORT:
	case AGWPE_DK_OUTSTANDING_CONN:
		if (c->cb.outstanding != NULL)
			c->cb.outstanding(c, hdr, parse_outstanding(data, len));
		break;

	case AGWPE_DK_CONNECT:
		if (c->cb.connection != NULL) {
			char msg[128];

			if (len > sizeof(msg) - 1)
				len = sizeof(msg) - 1;
			memcpy(msg, data, len);
			msg[len] = '\0';
			c->cb.connection(c, hdr, msg);
		}
		break;

	case AGWPE_DK_DISCONNECT:
		if (c->cb.disconnect != NULL) {
			char msg[128];

			if (len > sizeof(msg) - 1)
				len = sizeof(msg) - 1;
			memcpy(msg, data, len);
			msg[len] = '\0';
			c->cb.disconnect(c, hdr, msg);
		}
		break;

	case AGWPE_DK_DATA:
		if (c->cb.data != NULL)
			c->cb.data(c, hdr, data, len);
		break;

	case AGWPE_DK_MON_I:
	case AGWPE_DK_MON_S:
	case AGWPE_DK_MON_U:
	case AGWPE_DK_MON_T:
		if (c->cb.monitor != NULL)
			c->cb.monitor(c, hdr, data, len);
		break;

	case AGWPE_DK_RAW:
		if (c->cb.raw != NULL)
			c->cb.raw(c, hdr, data, len);
		break;

	default:
		break;
	}
}

/*
 * Read any pending frames from the server and dispatch them.
 * Returns the number of frames processed, or -1 on error or EOF.
 */
int agwpe_client_recv(agwpe_client_t *c)
{
	unsigned char tmp[4096];
	ssize_t n;
	int frames = 0;

	if (c->fd < 0) {
		c->err = ENOTCONN;
		return -1;
	}

	n = read(c->fd, tmp, sizeof(tmp));
	if (n <= 0) {
		c->err = (n == 0) ? 0 : errno;
		return -1;
	}

	if (c->rlen + n > c->ra) {
		size_t newsize = c->ra;

		while (newsize < c->rlen + n) {
			newsize *= 2;
			if (newsize > AGWPE_BUF_MAX)
				newsize = AGWPE_BUF_MAX;
		}
		if (newsize < c->rlen + n) {
			/* Corrupt stream; drop everything.  */
			c->rlen = 0;
			c->err = EIO;
			return -1;
		}
		{
			unsigned char *nb = realloc(c->rbuf, newsize);

			if (nb == NULL) {
				c->err = ENOMEM;
				return -1;
			}
			c->rbuf = nb;
			c->ra = newsize;
		}
	}
	memcpy(c->rbuf + c->rlen, tmp, n);
	c->rlen += n;

	while (c->rlen >= AGWPE_HEADER_LEN) {
		struct agwpe_s hdr;
		uint32_t dlen;
		size_t total;

		memcpy(&hdr, c->rbuf, AGWPE_HEADER_LEN);
		dlen = agwpe_netle2host(hdr.data_len);
		if (dlen > AGWPE_BUF_MAX - AGWPE_HEADER_LEN) {
			c->err = EIO;
			c->rlen = 0;
			return -1;
		}

		total = AGWPE_HEADER_LEN + dlen;
		if (c->rlen < total)
			break;

		dispatch_frame(c, &hdr, c->rbuf + AGWPE_HEADER_LEN, dlen);
		frames++;

		c->rlen -= total;
		memmove(c->rbuf, c->rbuf + total, c->rlen);
	}

	c->err = 0;
	return frames;
}

/*
 * Convenience: wait up to timeout_ms for data, then read and dispatch.
 * Returns 0 if nothing arrived, the number of frames otherwise, -1 on error.
 */
int agwpe_client_pump(agwpe_client_t *c, int timeout_ms)
{
	fd_set rfds;
	struct timeval tv, *tvp;
	int ret;

	if (c->fd < 0) {
		c->err = ENOTCONN;
		return -1;
	}

	FD_ZERO(&rfds);
	FD_SET(c->fd, &rfds);

	if (timeout_ms < 0) {
		tvp = NULL;
	} else {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}

	ret = select(c->fd + 1, &rfds, NULL, NULL, tvp);
	if (ret < 0) {
		c->err = errno;
		return -1;
	}
	if (ret == 0)
		return 0;

	return agwpe_client_recv(c);
}

void *agwpe_client_opaque(const agwpe_client_t *c)
{
	return c->opaque;
}
