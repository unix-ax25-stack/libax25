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
 *	[4 byte big-endian payload length][1 byte port][payload]
 *
 * The shim takes that header off again in recvfrom(), so an application
 * reads one payload per call and never sees it.  The port is in the header
 * because the payload does not say which channel it arrived on, and one
 * value per socket cannot say it either: the frame an application reads is
 * not the frame written last once two channels are busy at once.
 *
 * Applications detect the shim backend with
 *
 *	getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len)
 *
 * which the shim answers with ENOPROTOOPT for its virtual sockets; a
 * kernel packet socket answers SO_TYPE == SOCK_PACKET and is read one
 * frame per recvfrom() exactly as before.  Either way the payload is the
 * KISS framed packet (leading channel byte) listen(1) expects.
 */

#ifndef	_AXMON_H
#define	_AXMON_H

#include <sys/types.h>
#include <sys/socket.h>

/* Both ends of this are inside this library, so it is not a protocol
 * anything outside has to agree on.  */
#define	AXMON_HDR_LEN		5
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
 * Open every source of raw AX.25 frames this machine has, which is two on a
 * host that has both a kernel AX.25 stack and an ax25netd.
 *
 * protocol is in network byte order, as socket() wants it.
 *
 * AXMON_MAX_FD is the upper bound, not a promise: a host has one kernel and
 * one ax25netd, so nfd is 1 or 2.  kind[] says which is which, because the
 * two differ in a way a caller has to act on - a kernel descriptor is a
 * packet socket that the kernel filters, and ETH_P_ALL on it still has to be
 * told apart from plain IP by asking the interface, while every frame on a
 * monitor descriptor is AX.25 already.
 *
 * port restricts the monitor to one axports entry, by name.  It need not be a
 * kernel port: every port that belongs to ax25netd has no interface behind
 * it, and "listen -p loop" is the obvious case that used to be refused as an
 * invalid port name for that reason.  NULL or "" is no restriction.
 *
 * Returns 0 with nfd set, or -1 with errno set (EINVAL for a name in no
 * axports entry, ENXIO when the machine has no source of raw frames at all).
 */
#define	AXMON_MAX_FD		2
#define	AXMON_KERNEL		0	/* a kernel packet socket */
#define	AXMON_MONITOR		1	/* the ax25netd monitor stream */

struct axmon {
	int		fd[AXMON_MAX_FD];
	int		framed[AXMON_MAX_FD];
	int		kind[AXMON_MAX_FD];
	unsigned char	mask;		/* one bit per open descriptor */
	int		nfd;
};

extern int axmon_open(int protocol, const char *port, struct axmon *mon);

/*
 * Wait for frames.  Returns how many sources have something, with *ready
 * naming them as one bit per fd[] index, or -1 with errno set; 0 means the
 * timeout ran out.
 *
 * A source that has ended is reported as ready rather than as an error, because
 * a packet socket at end of file stays readable and axmon_read() is what says
 * so.  Reporting it here instead would name the wrong failure and leave the
 * other source unread for as long as the caller believed it.
 *
 * A signal comes back as -1/EINTR rather than being waited out.  With no
 * timeout a caller that blocks here would otherwise never see the flag its
 * handler set, and would have to be killed with SIGKILL.
 */
extern int axmon_poll(struct axmon *mon, int timeout, unsigned *ready);

/*
 * How many of the descriptors are still open.  A caller that retires one puts
 * -1 in its place - see axmon_poll() for why the array is not renumbered - and
 * asks this to know whether anything is left to read.
 */
extern int axmon_alive(const struct axmon *mon);

extern void axmon_close(struct axmon *mon);

/*
 * Read one frame, either way.  Returns the payload length, 0 at end of
 * file, or -1 with errno set - E2BIG if the frame does not fit in buflen,
 * ECONNRESET if the monitor closed mid frame.  sa/asize are filled in as
 * recvfrom() would, and may be NULL.
 *
 * 0 is end of file and cannot be a frame: a KISS framed AX.25 packet
 * carries at least a channel byte.  A caller that only tests for -1 decodes
 * the end of the stream as a frame of no bytes and loops on it forever,
 * which is what every monitor in the suite did when the link to ax25netd
 * went away - it had no error to report, because nothing had failed, it had
 * ended.  errno is set to 0 when 0 is returned, so an EOF can be told from
 * an error by either test.
 *
 * The shim strips its own header before the payload arrives, so both kinds
 * of descriptor hand over exactly one frame per call and the answer from
 * axmon_framed() no longer changes what this does.  It is still worth
 * asking: the two are not told apart anywhere else.
 */
extern ssize_t axmon_read(int fd, int framed, void *buf, size_t buflen,
			  struct sockaddr *sa, socklen_t *asize);

#ifdef __cplusplus
}
#endif

#endif
