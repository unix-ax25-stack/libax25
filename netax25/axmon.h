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
 * Raw monitor framing between the userspace AGWPE shim (axsock.c) and
 * the monitor application (ax25-apps/listen).
 *
 * listen(1) opens a SOCK_PACKET socket to capture raw AX.25 frames.
 * There is no packet socket on macOS/BSD, so libax25 intercepts that
 * call and backs it with an AF_UNIX stream socketpair fed from the
 * AGWPE 'K' raw frames.  A stream does not preserve write boundaries:
 * two frames dispatched back to back merge into a single read and the
 * KISS decoder would run past the end of the first frame.  (AF_UNIX
 * SOCK_SEQPACKET socketpairs would preserve them, but macOS does not
 * support that socket type.)
 *
 * The shim therefore delivers every raw frame as one unit:
 *
 *	[4 byte big-endian payload length][payload]
 *
 * Applications detect the shim backend with
 *
 *	getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len)
 *
 * which the shim answers with ENOPROTOOPT for its virtual sockets; a
 * kernel packet socket answers SO_TYPE == SOCK_PACKET and is read one
 * frame per recvfrom() exactly as before.  Either way the payload is
 * the KISS framed packet (leading channel byte) listen(1) expects.
 */

#ifndef	_AXMON_H
#define	_AXMON_H

#define	AXMON_PREFIX_LEN	4
#define	AXMON_FRAME_MAX		1500

#endif
