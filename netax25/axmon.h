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
 * KISS decoder would run past the end of the first frame.
 *
 * Neither message oriented socketpair is a way out.  AF_UNIX
 * SOCK_SEQPACKET keeps the boundaries, but macOS does not implement it
 * for the unix domain at all (socketpair fails with EPROTONOSUPPORT).
 * AF_UNIX SOCK_DGRAM does work there and does keep them, but it brings
 * two limits of its own, both measured on macOS 24.6:
 * net.local.dgram.maxdgram is 2048, so a full sized KISS frame is close
 * to the largest datagram the domain will carry, and
 * net.local.dgram.recvspace is 4096 - two frames of 1500 bytes fill the
 * receive buffer and the third is refused with ENOBUFS.  A monitor that
 * pauses for a moment would lose frames outright.  A stream buffers
 * more and pushes back instead of dropping, so it is the stream plus
 * the length prefix below.
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

#include <sys/types.h>
#include <sys/socket.h>

#define	AXMON_PREFIX_LEN	4
#define	AXMON_FRAME_MAX		1500

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ask a monitor descriptor which of the two it is.  Non-zero means the
 * shim backend, whose frames carry the length prefix described above;
 * zero means a kernel packet socket, one frame per recvfrom().  Ask once
 * after socket(), then pass the answer to every axmon_read().
 */
extern int axmon_framed(int fd);

/*
 * Read one frame, either way.  Returns the payload length, or -1 with
 * errno set - E2BIG if the frame does not fit in buflen, ECONNRESET if
 * the monitor closed mid frame.  sa/asize are filled in as recvfrom()
 * would, and may be NULL.
 */
extern ssize_t axmon_read(int fd, int framed, void *buf, size_t buflen,
			  struct sockaddr *sa, socklen_t *asize);

#ifdef __cplusplus
}
#endif

#endif
