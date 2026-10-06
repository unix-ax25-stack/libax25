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
 * This file contains the definitions of the entry points into the AX.25
 * configuration functions.
 */

#ifndef	_AXCONFIG_H
#define	_AXCONFIG_H

#include <stddef.h>

#ifndef	TRUE
#define	TRUE	1
#endif

#ifndef	FALSE
#define	FALSE	0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This function must be called before using any of the other functions in
 * this part of the library. It returns the number of active ports, or 0
 * on failure.
 */
/* Set by a backend that resolves a "base:suffix" port name itself, so that
 * such a name may be used without an axports entry of its own.  Left null,
 * an unknown name stays unknown - see ax25_port_ptr().
 */
extern int (*ax25_config_lazy_hook)(const char *name, const char *base);

/*
 * A "base:suffix" name that ax25_port_ptr() has just resolved through the
 * lazy hook is remembered against the base entry's callsign, so a later
 * bind() carrying that callsign can find out which suffix was meant.
 * ax25_config_lazy_take() hands the oldest remembered name back and
 * removes it; the backend uses it in place of the port name it would
 * reverse-engineer from the callsign.  Remember returns 0 on success.
 */
extern int ax25_config_lazy_remember(const char *call, const char *name);
extern int ax25_config_lazy_take(const char *call, char *name, size_t namelen);

/*
 * What a port name is, asked of the whole library rather than guessed by the
 * caller.
 *
 * A machine can have a kernel AX.25 stack, a WAMPES node and an ax25netd at
 * once, so "which port is this" has no single table to look in.  The shim
 * already answers it internally, at the two places that decide: wampes_bind()
 * asks whether a node of that name is configured, agwpe_bind() asks whether
 * an upstream owns it.  Both questions are asked again here, through one
 * pair of hooks, rather than reimplemented in each program that wants to
 * know.  That is not tidiness for its own sake: the numbering of the AGWPE
 * ports belongs to the server, i*16+channel, and a caller that guessed it
 * from the position of a name in a file sent frames a whole stride out.
 *
 * The fields are those of the axports entry plus what the owning backend
 * knows about it.  Returns 0 whenever the question could be asked, which
 * includes a name that nothing claims: that comes back as AX25_PORT_NONE with
 * the axports fields filled as far as they go, and telling those two apart is
 * the caller's business, not this function's.
 */
enum ax25_port_backend {
	AX25_PORT_NONE = 0,	/* nobody claims the name */
	AX25_PORT_KERNEL,	/* an interface the kernel stack answers for */
	AX25_PORT_WAMPES,	/* a port of a WAMPES node */
	AX25_PORT_AGWPE		/* a channel of an ax25netd upstream */
};

struct ax25_port_info {
	enum ax25_port_backend backend;

	/* From the axports entry of that name.  A name that has no entry
	 * of its own - one a backend resolves, or a name that is none at
	 * all - leaves these empty rather than inventing a value.  */
	char call[16];		/* callsign */
	char dev[64];		/* device, as written in axports */
	char desc[80];		/* description */
	int  baud, window, paclen;

	/* AGWPE.  port is the flat port byte to put in a frame header,
	 * which is what the server itself numbers, and is -1 for every
	 * other backend.  channel is the channel of that port, 0 to 15, and
	 * is -1 when the name is not an AGWPE port at all: "hf" and "hf:0"
	 * are the same frequency and both say 0 here.  */
	int  port;
	int  channel;
	char upstream[24];	/* the upstream name from the server's list */

	/* WAMPES.  node is the node's name from wampes.conf.  */
	char node[32];
};

/* Set by wampes.c: nonzero when the name is a port of a configured node,
 * writing the node's name into node[].
 */
extern int (*ax25_config_port_wampes_hook)(const char *name, char *node,
					    size_t nodelen);

/* Set by agwpe_sock.c: nonzero when the name is a channel of one of the
 * server's upstreams, writing the flat port, the channel and the upstream
 * name of the server's own list.
 */
extern int (*ax25_config_port_agwpe_hook)(const char *name, int *port,
					   int *channel, char *upstream,
					   size_t uplen);

extern int ax25_port_info(const char *name, struct ax25_port_info *info);

/* The flat AGWPE port of a name, or -1 when it is not one.  ax25_port_info()
 * for the caller who wants nothing else.  Returns -1 with errno EADDRNOTAVAIL
 * for a name that is not an AGWPE port, which is the same answer the frame
 * header path gives and the one a caller is about to hit anyway.
 */
extern int ax25_port_number(const char *name);

extern int ax25_config_load_ports(void);

/*
 * The same, but only when the file has changed since the last read.
 *
 * ax25_config_load_ports() is a forced reload: every program in the suite
 * calls it at startup and axparms(8) calls it to re-read a file the operator
 * has just edited.  Callers inside the library that go looking for a port
 * while the application is doing something else want the other thing - the
 * answer, not the read - and they are on a path where the application is
 * holding a bind.  Returns the number of entries in the table, like
 * ax25_config_load_ports() does.
 */
extern int ax25_config_ports_ensure(void);

/*
 * The axports file that ax25_config_load_ports() reads.
 *
 * A caller that wants to say something about the entries has to name the
 * file it looked at, and it may not have a second opinion about where that
 * is: the shim in a libax25-using program and a daemon that compares the
 * two configuration files can be built with different sysconfdir, and then
 * a name of its own would be about a file nobody read.
 */
extern const char *ax25_config_ports_file(void);

/*
 * This function allows the enumeration of all the active configured ports.
 * Passing NULL as the argument returns the first port name in the list,
 * subsequent calls to this function should be made with the last port name
 * returned. A NULL return indicates either an error, or the end of the list.
 */
extern char *ax25_config_get_next(char *);

/*
 * This function maps the device name onto the port name (as used in the axports
 * file. On error a NULL is returned.
 */
extern char *ax25_config_get_name(char *);

/*
 * This function maps the port name onto the callsign of the port. On error a
 * NULL is returned.
 */
extern char *ax25_config_get_addr(char *);

/*
 * This function maps the port name onto the device name of the port. On error a
 * NULL is returned.
 */
extern char *ax25_config_get_dev(char *);

/*
 * This function maps the callsign onto the port name. The callsign should be
 * in shifted format as per get{peer,sock}name(2). A null_ax25_address will
 * return a "*" meaning all ports. On error NULL is returned.
 */
extern char *ax25_config_get_port(ax25_address *);

/*
 * Nonzero if the named port is one the kernel answers for, i.e. its callsign
 * is the AX.25 address of an interface that was up when the port list was
 * loaded.  Only the backends ask this, to leave the kernel's ports alone: a
 * bind naming one of those was already answered by socket() and does not
 * belong to a userspace backend, however much the server behind it might
 * also be able to send.
 */
extern int ax25_config_port_is_kernel(const char *);

/*
 * How many ports are the kernel's, that is: axports entries whose callsign is
 * an AX.25 interface that is up.  Whether a raw monitor has a kernel side at
 * all, and there is no cheaper way to ask.
 */
extern int ax25_config_kernel_ports(void);

/*
 * The name in axports of the port a bind address names, 0 on success and 1
 * with an empty name.  The callsign in the first digipeater slot names the
 * port, or the source callsign when no slot is given; a "base:suffix" name
 * that ax25_port_ptr() resolved through the lazy hook is taken from the
 * remembered entry instead, so the suffix survives the round trip.
 *
 * Both backends resolve binds this way and must agree on the answer, so it is
 * here once rather than in either of them.
 */
extern int ax25_config_bind_port(const struct sockaddr *, socklen_t, char *, size_t);

/*
 * This function takes the port name and returns the default window size. On
 * error 0 is returned.
 */
extern int ax25_config_get_window(char *);

/*
 * This function takes the port name and returns the maximum packet length.
 * On error a 0 is returned.
 */
extern int ax25_config_get_paclen(char *);

/*
 * This function takes the port name and returns the baud rate. On error a
 * 0 is returned.
 */
extern int ax25_config_get_baud(char *);

/*
 * This function takes the port name and returns the description of the port.
 * On error a NULL is returned.
 */
extern char *ax25_config_get_desc(char *);

#ifdef __cplusplus
}
#endif

#endif
