/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 2026 Thomas Osterried <dl9sau@darc.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */
/*
 * The parts of the AX.25 configuration that are between the library's own
 * files and not a program's business - and they are here for a reason worth
 * writing down, because the reason cost a working program.
 *
 * netax25/axconfig.h used to carry ax25_config_bind_port() below.  It takes a
 * struct sockaddr and a socklen_t, and a header that names a type has to name
 * the header that defines it - so that one line put <sys/socket.h> in front of
 * every program including the port table, and broke the ones that include this
 * family's headers in the other order (Thomas: "unknown type name 'socklen_t';
 * did you mean 'locale_t'?").  The public header has named an ax25_address
 * since 1997 and nothing else, and that is the whole contract: a type from
 * <netax25/ax25.h>, which a program includes first because it is a program
 * about AX.25.
 *
 * So the declaration is here, the library includes this file where it uses the
 * function, and the public header keeps the shape it had before any of this
 * existed.
 */

#ifndef	_AXCONFIG_INT_H
#define	_AXCONFIG_INT_H

#include <sys/socket.h>		/* socklen_t, struct sockaddr */
#include <stddef.h>

/*
 * Ensure the port table is current (reload if axports changed).
 * Internal to libax25.
 */
extern int ax25_config_ports_ensure(void);

/*
 * Set by a backend that resolves a "base:suffix" port name itself, so that
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
 * Whether an AX.25 interface carries this callsign and is down, 1 and the
 * interface name in dev when it is, 0 when none does.  The port list keeps
 * only interfaces that are up, so a refused bind needs to be able to say
 * that the callsign is there and the interface is not, rather than that no
 * interface carries it.
 */
extern int ax25_config_call_if_down(const char *call, char *dev, size_t devlen);

#endif	/* _AXCONFIG_INT_H */
