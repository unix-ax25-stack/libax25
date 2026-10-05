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
 * Opening and reading a raw AX.25 monitor, whichever end it comes from.  See
 * netax25/axmon.h for the framing this has to cope with.  Every program
 * that watches raw frames - listen(1), mheardd(8), net2kiss(8) - reads
 * them the same way, so the loop lives here rather than three times over.
 */

#include <config.h>

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <net/if.h>
#include <netpacket/packet.h>
#endif

#include "netax25/axmon.h"
#include "netax25/ax25.h"
#include "netax25/axconfig.h"
#include "netax25/agwpe_config.h"
#include "agwpe_sock.h"
#include "axsock_real.h"

/*
 * The three numbers a packet socket is asked for by name, where the platform
 * has them.  The monitor descriptor is an AF_UNIX stream on a platform with no
 * packet socket, and its bind() reads the family only to tell the call apart
 * from an AX.25 bind - so the values have nothing to be right about there.
 * listen(1) has carried the same two defines since it was ported, and
 * agwpe_sock.c the third.
 */
#ifndef PF_PACKET
#define	PF_PACKET	17
#endif
#ifndef AF_PACKET
#define	AF_PACKET	PF_PACKET
#endif
#ifndef SOCK_PACKET
#define	SOCK_PACKET	10
#endif

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
	ssize_t n;

	/* Both kinds of descriptor hand over one frame per call: the kernel
	 * one because a packet socket is message oriented, the shim one
	 * because it takes its header off in recvfrom() before the
	 * payload reaches us (see netax25/axmon.h).  That is what the
	 * framing is for - a stream would otherwise run two frames that
	 * arrive back to back into one read - and it is why the header
	 * moved into the shim, where the writer of it already was.  */
	(void) framed;

	n = mon_read(fd, buf, buflen, sa, asize);

	/* End of file is 0, and it is reported as 0 rather than as a frame
	 * of no bytes.  There is no such frame: a KISS framed AX.25 packet
	 * carries at least a channel byte, so a caller that decodes what it
	 * got cannot tell the two apart and decoded an empty one.  Every
	 * program that watches raw frames had that loop, and every one of
	 * them spun on it printing nothing after the link to the server
	 * went away - the monitor had no error to report, because nothing
	 * had failed.  It had ended.  */
	if (n == 0)
		errno = 0;

	return n;
}

/*
 * Is this name in axports at all?
 *
 * Separate from ax25_config_get_dev(), which answers the question a kernel
 * filter asks - "is this name an interface" - and says no for every port that
 * belongs to ax25netd, including the one "listen -p loop" wanted.  That is the
 * whole of the reason there was no such command: the check that was meant to
 * catch a typo was asking whether the port was a kernel interface, and a
 * userspace port failing that test was reported as a name that does not exist.
 */
static int axmon_port_is_known(const char *name)
{
	char *entry;

	for (entry = ax25_config_get_next(NULL); entry != NULL;
	     entry = ax25_config_get_next(entry))
		if (strcasecmp(entry, name) == 0)
			return 1;

	/* The loopback port is the one name that needs no axports line, the
	 * same way axsock_port_of_entry() needs none to give it a number. */
	return strcasecmp(name, AGWPE_LOOP_NAME) == 0;
}

/*
 * Open every source of raw AX.25 frames this machine has.
 *
 * Two, on a host that has both: the kernel's packet socket for the ports the
 * kernel AX.25 stack owns, and ax25netd's monitor stream for everything the
 * userspace side owns - the AGWPE radios, and the WAMPES traffic, which is
 * mirrored into that same stream because there is no packet socket for a node
 * to put frames on.  One, on a host that has only one of them, which is most
 * of them: the kernel socket on a machine whose ports all belong to
 * ax25netd has nothing to hear, and the monitor socket on a machine with no
 * ax25netd has nothing to be fed by.
 *
 * Both is what a monitor is for.  listen(1) used to open one socket and get
 * whichever source the process had been given for AF_AX25, so on a dualstack
 * host it showed the kernel's ports and said nothing about the fact that the
 * radios were not in the output.  Nothing in that output was wrong; it was
 * incomplete without saying so, which is the worst kind of wrong for a tool
 * whose job is to show what is on the air.
 *
 * port names one axports entry and restricts the monitor to it.  Both sides
 * can filter and both are told to: the kernel by binding its socket to the
 * device the entry names, the monitor by the name the frames carry.  An entry
 * with no device - every userspace port, and the loop port among them - is
 * not an error here.  It was, and that is why there was no "listen -p loop":
 * the check asked ax25_config_get_dev(), which answers NULL for a port that
 * was never an interface, and NULL read as "no such port".
 *
 * protocol is in network byte order, as socket() wants it.
 *
 * Returns 0 when at least one source was opened, -1 otherwise, with errno set.
 * nfd is the number of descriptors; fd[i], framed[i] and kind[i] belong
 * together.
 */
int axmon_open(int protocol, const char *port, struct axmon *mon)
{
	mon->nfd = 0;
	mon->mask = 0;

	/* A caller that only reads has no reason to have loaded the port
	 * table, and the question below is asked of it.  */
	ax25_config_ports_ensure();

	if (port != NULL && port[0] != '\0' &&
	    ax25_config_get_dev((char *)port) == NULL &&
	    !axmon_port_is_known(port)) {
		/* A name that is in no axports entry at all.  A name that has
		 * no device but is in the file is fine - that is what a
		 * userspace port is - so the question is whether it is there,
		 * not whether it names an interface.  */
		errno = EINVAL;
		return -1;
	}

#ifdef __linux__
	/*
	 * The kernel side.  real_socket() and not socket(): this is the
	 * kernel's own packet socket, wanted whatever the interception layer
	 * would have decided for AF_AX25, and going through socket() would
	 * ask the shim for it.
	 *
	 * Only when the kernel has AX.25 ports of its own.  A packet socket
	 * can be opened on a host whose AX.25 ports all belong to ax25netd,
	 * and it will then report no AX.25 frames at all - not because
	 * nothing is on the air, but because nothing is on an interface of
	 * the kernel's.  Handing that back as a source would be a monitor
	 * with two halves, one of which is silent for a reason that never
	 * shows up in the output.
	 */
	if (ax25_config_kernel_ports() > 0) {
		int fd = real_socket(PF_PACKET, SOCK_PACKET, protocol);

		if (fd >= 0) {
			const char *dev = (port != NULL)
				? ax25_config_get_dev((char *)port) : NULL;

			if (dev != NULL) {
				struct sockaddr_ll sll;

				memset(&sll, 0, sizeof(sll));
				sll.sll_family = AF_PACKET;
				sll.sll_protocol = protocol;
				sll.sll_ifindex = (int)if_nametoindex(dev);
				if (sll.sll_ifindex == 0 ||
				    real_bind(fd, (struct sockaddr *)&sll,
					      sizeof(sll)) < 0) {
					/*
					 * The entry names a device that is
					 * not there, or the kernel will not
					 * bind to it.  Say so and leave the
					 * filter to the other side: the
					 * monitor still opens, restricted to
					 * the name rather than to the
					 * device, which is the narrower of
					 * the two questions and the one the
					 * operator meant.
					 */
					if (axsock_debug)
						fprintf(stderr, "axmon: "
							"cannot bind the kernel "
							"monitor to '%s': %s\n",
							dev, strerror(errno));
				}
			}
			mon->fd[mon->nfd] = fd;
			mon->framed[mon->nfd] = 0;
			mon->mask |= (unsigned char)(1u << mon->nfd);
			mon->kind[mon->nfd] = AXMON_KERNEL;
			mon->nfd++;
		}
	}
#endif /* __linux__ */

	/*
	 * The monitor side.  Whatever this process was given as its AF_AX25
	 * backend, because a raw monitor is not answered by that question: a
	 * host with a kernel stack has an ax25netd too often enough that the
	 * question would hide half of what is on the air.
	 */
	{
		int fd;

		if (agwpe_mon_open(protocol, &fd) == 1 && fd >= 0) {
			if (port != NULL && port[0] != '\0') {
				struct sockaddr spp;

				memset(&spp, 0, sizeof(spp));
				spp.sa_family = AF_PACKET;
				/*
				 * The name goes in sa_data, which is what
				 * axsock_raw_match() compares the port name of
				 * each frame against, and the device name of the
				 * entry as a second chance.  Not the device: a
				 * userspace port has none, and a port name is
				 * what the frames are reported under.
				 */
				strncpy(spp.sa_data, port, sizeof(spp.sa_data) - 1);
				real_bind(fd, (struct sockaddr *)&spp,
					  sizeof(spp));
			}
			mon->fd[mon->nfd] = fd;
			mon->framed[mon->nfd] = axmon_framed(fd);
			mon->mask |= (unsigned char)(1u << mon->nfd);
			mon->kind[mon->nfd] = AXMON_MONITOR;
			mon->nfd++;
		}
	}

	if (mon->nfd == 0) {
		/*
		 * Neither.  Say which, because the two reasons need different
		 * answers: no kernel AX.25 ports means load a module or leave
		 * the ports to ax25netd, and no ax25netd means start it.
		 */
		errno = ENXIO;
		return -1;
	}

	return 0;
}

/*
 * Wait for frames on any of the descriptors.
 *
 * Returns the number of sources with something to read, or -1 with errno set.
 * ready names them, one bit per fd[] index, so the caller reads from exactly
 * those.  A source that has ended is in ready too: a packet socket at end of
 * file is readable for ever, and axmon_read() has to be called to be told so.
 * Treating it as an error here instead would hide which source ended and leave
 * the other one unread for as long as the caller believed the failure.
 */
int axmon_poll(struct axmon *mon, int timeout, unsigned *ready)
{
	struct pollfd pfd[AXMON_MAX_FD];
	int map[AXMON_MAX_FD];
	int i, nf = 0, n = 0;

	*ready = 0;

	for (i = 0; i < mon->nfd; i++) {
		if (mon->fd[i] < 0)
			continue;
		pfd[nf].fd = mon->fd[i];
		pfd[nf].events = POLLIN;
		pfd[nf].revents = 0;
		map[nf] = i;
		nf++;
	}

	if (nf == 0) {
		errno = EBADF;
		return -1;
	}

	for (;;) {
		int rc = poll(pfd, nf, timeout);

		if (rc >= 0)
			break;
		if (errno != EINTR)
			return -1;
		/* A signal is not an answer, and a program that asks for a
		 * timeout has said it is willing to wait for one.  Retrying
		 * starts the wait over, which is what poll() itself would
		 * do if it were not for the signal - and returning 0 here
		 * would tell a caller that asked for a second that the first
		 * second is up, which it never was. */
	}

	for (i = 0; i < nf; i++)
		if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
			*ready |= 1u << map[i];
			n++;
		}

	return n;
}

/*
 * How many of the descriptors are still open.
 *
 * A caller retires one by closing it and putting -1 in its place rather than
 * by closing it and taking the array apart.  The loop above counts in fd[]
 * indices and the caller walks the same indices, so renumbering under one that
 * is halfway through a pass is how one source's frames end up attributed to
 * another - and, with one source dead and the other alive, how a monitor ends
 * because the wrong half of it closed.
 */
int axmon_alive(const struct axmon *mon)
{
	int i, n = 0;

	for (i = 0; i < mon->nfd; i++)
		if (mon->fd[i] >= 0)
			n++;

	return n;
}

void axmon_close(struct axmon *mon)
{
	int i;

	for (i = 0; i < mon->nfd; i++)
		if (mon->fd[i] >= 0)
			real_close(mon->fd[i]);

	mon->nfd = 0;
	mon->mask = 0;
}
