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
 * Client side of the AGWPE TCP/IP protocol.
 *
 * An agwpe_client_t is an event driven connection to one AGWPE server
 * (Direwolf, AGWPE on Windows, ...).  After agwpe_client_connect_host()
 * the application registers the interesting call signs, then selects()
 * on agwpe_client_fd() and calls agwpe_client_recv() whenever the file
 * descriptor is readable.  Incoming frames are dispatched to the
 * callbacks given to agwpe_client_new().
 */

#ifndef	_NETAX25_AGWPE_CLIENT_H
#define	_NETAX25_AGWPE_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include <netax25/agwpe.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agwpe_client agwpe_client_t;

/*
 * Result of a "G" (port info) request.  count is the number of "PortN
 * description" tokens received; names and descs hold them without the
 * leading "Port".
 */
struct agwpe_port_list {
	int			count;
	char			names[AGWPE_PORT_MAX][24];
	char			descs[AGWPE_PORT_MAX][64];
};

/*
 * Result of a "g" (port capabilities) request.
 */
struct agwpe_port_capab {
	unsigned char		onair_baud;
	unsigned char		traffic_level;
	unsigned char		tx_delay;
	unsigned char		tx_tail;
	unsigned char		persist;
	unsigned char		slot_time;
	unsigned char		max_frame;
	unsigned char		ax25_channels;
	uint32_t		how_many_bytes;
};

/*
 * One "H" (heard list) entry: CALLSIGN-SSID and the first and last time
 * the station was heard, as raw ASCII strings.
 */
struct agwpe_heard {
	char			call[12];
	char			first[24];
	char			last[24];
};

/*
 * Callbacks.  Every callback receives the client object; the ones
 * carrying a frame also receive the (host byte order) header.  Any
 * callback may be NULL, in which case the corresponding frames are
 * silently dropped.
 *
 * data, monitor and raw all deliver the raw data area of the frame.
 * For monitor frames use agwpe_monitor_split() to separate the ASCII
 * decoded header line from the binary information part.
 */
struct agwpe_client_cb {
	/*
	 * Called first for every frame, before the type specific callbacks,
	 * with the frame exactly as received (header in host byte order).
	 * Useful for proxies and raw listeners.
	 */
	void	(*raw_frame)(agwpe_client_t *c, const struct agwpe_s *hdr,
			     const unsigned char *data, size_t len);
	void	(*version)(agwpe_client_t *c, unsigned int major,
			   unsigned int minor);
	void	(*registered)(agwpe_client_t *c, const struct agwpe_s *hdr,
			      int registered);
	void	(*ports)(agwpe_client_t *c, struct agwpe_port_list *list);
	void	(*capab)(agwpe_client_t *c, const struct agwpe_s *hdr,
			 const struct agwpe_port_capab *cap);
	void	(*heard)(agwpe_client_t *c, const struct agwpe_s *hdr,
			 const struct agwpe_heard *heard);
	void	(*outstanding)(agwpe_client_t *c, const struct agwpe_s *hdr,
			       unsigned int count);
	void	(*connection)(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg);
	void	(*disconnect)(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg);
	void	(*data)(agwpe_client_t *c, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len);
	void	(*monitor)(agwpe_client_t *c, const struct agwpe_s *hdr,
			   const unsigned char *data, size_t len);
	void	(*raw)(agwpe_client_t *c, const struct agwpe_s *hdr,
		       const unsigned char *data, size_t len);
};

/*
 * Allocate a new client object.  cb may be NULL; opaque is passed back
 * to every callback.
 */
extern agwpe_client_t *agwpe_client_new(const struct agwpe_client_cb *cb,
					void *opaque);

extern void agwpe_client_free(agwpe_client_t *c);

/*
 * Open the TCP connection to the server.  Returns 0 on success, -1 on
 * error (see agwpe_client_err()).
 */
extern int agwpe_client_connect_host(agwpe_client_t *c, const char *host,
				     int tcp_port);

/*
 * Open the connection over a unix domain socket instead of TCP.  The
 * AGWPE frame stream is unchanged; the socket file permissions decide
 * who may connect at all.  Returns 0 on success, -1 on error.
 */
extern int agwpe_client_connect_unix(agwpe_client_t *c, const char *path);

extern void agwpe_client_close(agwpe_client_t *c);

extern int agwpe_client_fd(const agwpe_client_t *c);

extern int agwpe_client_connected(const agwpe_client_t *c);

extern int agwpe_client_err(const agwpe_client_t *c);

extern void *agwpe_client_opaque(const agwpe_client_t *c);

/*
 * Send a single frame (header + data area).  data may be NULL when
 * hdr->data_len is zero.
 */
extern int agwpe_client_send_frame(agwpe_client_t *c,
				   const struct agwpe_s *hdr,
				   const unsigned char *data);

/* Login with user name and password.  */
extern int agwpe_client_login(agwpe_client_t *c, const char *user,
			      const char *pass);

/* Register / unregister a call sign on a port.  */
extern int agwpe_client_register(agwpe_client_t *c, unsigned char port,
				 const char *call);
extern int agwpe_client_unregister(agwpe_client_t *c, unsigned char port,
				   const char *call);

/* ax25netd extension: register a listening socket on the virtual loop
 * port (AGWPE_PORT_LOOP) so that inbound connects are routed to it.  */
extern int agwpe_client_listen(agwpe_client_t *c, unsigned char port,
			       const char *call);

/* The queries below generate an asynchronous reply frame.  */
extern int agwpe_client_get_version(agwpe_client_t *c);
extern int agwpe_client_get_ports(agwpe_client_t *c);
extern int agwpe_client_get_capab(agwpe_client_t *c, unsigned char port);
extern int agwpe_client_get_heard(agwpe_client_t *c, unsigned char port);

/*
 * The monitor command is a toggle on both AGWPE and Direwolf, so there
 * is no enable argument.
 */
extern int agwpe_client_monitor(agwpe_client_t *c);

/* Same toggle for raw AX.25 frames.  */
extern int agwpe_client_raw_toggle(agwpe_client_t *c);

/* Send an UNPROTO (UI) frame.  */
extern int agwpe_client_send_unproto(agwpe_client_t *c, unsigned char port,
				     unsigned char pid, const char *from,
				     const char *to, const unsigned char *data,
				     int len);

/* Send an UNPROTO frame through a digipeater path.  */
extern int agwpe_client_send_unproto_via(agwpe_client_t *c,
					 unsigned char port,
					 unsigned char pid, const char *from,
					 const char *to,
					 const char *const digis[],
					 int ndigis,
					 const unsigned char *data, int len);

/*
 * Establish a connection.  pid is used only when the C-call is not
 * appropriate, e.g. AGWPE_PID_NETROM or AGWPE_PID_ROSE.
 */
extern int agwpe_client_connect(agwpe_client_t *c, unsigned char port,
				unsigned char pid, const char *from,
				const char *to);

extern int agwpe_client_connect_via(agwpe_client_t *c, unsigned char port,
				    unsigned char pid, const char *from,
				    const char *to, const char *const digis[],
				    int ndigis);

/* Send data on an established connection.  */
extern int agwpe_client_send_data(agwpe_client_t *c, unsigned char port,
				  unsigned char pid, const char *from,
				  const char *to, const unsigned char *data,
				  int len);

extern int agwpe_client_disconnect(agwpe_client_t *c, unsigned char port,
				   const char *from, const char *to);

/* How many connections are pending.  */
extern int agwpe_client_outstanding_port(agwpe_client_t *c,
					 unsigned char port);
extern int agwpe_client_outstanding_conn(agwpe_client_t *c,
					 unsigned char port, const char *from,
					 const char *to);

/*
 * Read any pending frames and dispatch them.  Returns the number of
 * frames processed, or -1 on error or EOF.
 */
extern int agwpe_client_recv(agwpe_client_t *c);

/*
 * Wait up to timeout_ms for data (-1 blocks), then read and dispatch.
 * Returns 0 if nothing arrived, the number of frames otherwise, -1 on
 * error.
 */
extern int agwpe_client_pump(agwpe_client_t *c, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
