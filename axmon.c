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
 * Reading a raw AX.25 monitor, whichever end it comes from.  See
 * netax25/axmon.h for the framing this has to cope with.  Every program
 * that watches raw frames - listen(1), mheardd(8), net2kiss(8) - reads
 * them the same way, so the loop lives here rather than three times over.
 */

#include <config.h>

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "netax25/axmon.h"
#include "axsock_real.h"

/*
 * Is this descriptor the shim's monitor rather than a kernel packet socket?
 *
 * The old test was that getsockopt() fails with ENOPROTOOPT, which the shim
 * answers for anything but SOL_AX25.  It works where the interception is
 * strong symbols, and not where it is an interpose table: this file is *in*
 * the library, and a call from there to getsockopt() is not redirected to the
 * entry point beside it - it goes straight to libc, succeeds, and the answer
 * came back "kernel socket".  listen(1) then read our four-byte length as the
 * front of the frame and decoded four bytes of rubbish followed by a callsign
 * rotated out of shape.  Nobody saw it for months because the test was that
 * listen kept running, never that what it printed was right.
 *
 * So ask something that is true rather than something that fails.  The shim
 * hands out one end of a AF_UNIX socketpair; a packet socket is SOCK_PACKET
 * or SOCK_RAW and never SOCK_STREAM.  Both platforms answer the same way now,
 * and the ENOPROTOOPT case stays for the one where the call is intercepted.
 */
int axmon_framed(int fd)
{
	int type;
	socklen_t len = sizeof(type);

	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == -1)
		return errno == ENOPROTOOPT;
	return type == SOCK_STREAM;
}

/* recvfrom(), retrying signals: a partial length prefix must not be lost
 * to EINTR or the frame stream desynchronizes.
 *
 * The call goes to the shim's entry point rather than to recvfrom().  This
 * file is part of the image that provides the interposition, and dyld does
 * not redirect the bindings of that image - see axsock_framed() below for
 * the same trap, measured - so a plain recvfrom() here went to libc, the
 * framed stream arrived undecoded, and the monitor was handed a length
 * where a callsign belonged.  */
static ssize_t mon_read(int fd, void *buf, size_t len,
			struct sockaddr *sa, socklen_t *asize)
{
	for (;;) {
		ssize_t n = AXSOCK_ENTRY(recvfrom)(fd, buf, len, 0, sa, asize);

		if (n >= 0)
			return n;
		if (errno != EINTR)
			return -1;
	}
}

ssize_t axmon_read(int fd, int framed, void *buf, size_t buflen,
		   struct sockaddr *sa, socklen_t *asize)
{
	/* Both kinds of descriptor hand over one frame per call: the kernel
	 * one because a packet socket is message oriented, the shim one
	 * because it takes its header off in recvfrom() before the
	 * payload reaches us (see netax25/axmon.h).  That is what the
	 * framing is for - a stream would otherwise run two frames that
	 * arrive back to back into one read - and it is why the header
	 * moved into the shim, where the writer of it already was.  */
	(void) framed;
	return mon_read(fd, buf, buflen, sa, asize);
}
