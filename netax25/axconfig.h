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
