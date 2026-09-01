/*
 * WAMPES backend for the AF_AX25 shim - outgoing direction.
 *
 * WAMPES runs a real AX.25 stack in user space and offers it on a socket:
 * one socket is one connection, spoken in lines until the link stands and
 * as a byte stream afterwards.  That shape is what makes this small.  With
 * AGWPE one connection carries every session, so axsock.c has to demultiplex
 * and every read and write must pass through it; here the descriptor the
 * application holds simply becomes the connection, and read(), write(),
 * poll() and close() go straight to the kernel with nothing in between.
 *
 * What that costs: the service socket is a byte stream, so frame boundaries
 * do not survive it.  Terminal traffic and text services do not care; FBB's
 * compressed forwarding does, and will need the descriptor-passing route
 * that is not built yet.
 *
 * The conversation, which is the whole protocol:
 *
 *      -> binary
 *      -> connect DB0AAA-8 via DB0BBB < DL9SAU-3
 *      <- link setup (hf1)...                  progress, ignored
 *      <- *** connected to DB0AAA-8            or *** link failure ... - why
 *
 * Every line that does not start with "***" is progress.  Exactly one does,
 * and it is the answer.  End of file before it means the link was never
 * established and WAMPES gave up.
 */

#include <config.h>

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "netax25/ax25.h"
#include "netax25/axlib.h"
#include "netax25/axconfig.h"

#include "pathnames.h"

#include "wampes.h"

#define WAMPES_MAX_SOCK 64

/* Where a node listens when nothing says otherwise: one machine, one node,
 * no configuration file worth the name. */
#define WAMPES_DEFAULT_SOCKET "/usr/local/wampes/sockets/ax25"
#define WAMPES_CALLLEN  10              /* "DL9SAU-15" and the NUL */

/* A socket the application holds.  After connect() the descriptor IS the
 * connection and nothing intercepts read(), write(), poll() or close() any
 * more; the entry stays only so that the two calls which have no meaning to
 * a plain socket - setsockopt(SOL_AX25) and its like - can still be
 * answered, and so that close() knows to forget it.
 */

struct wampes_sock {
	struct wampes_sock *next;
	int fd;
	char local[WAMPES_CALLLEN];         /* source call, from bind() */
	char port[32];                      /* axports entry, from bind() */
	int pid;                            /* protocol id, from the third
					     * argument of socket() - 0 means
					     * plain text, as it always did */
	int connected;                      /* connect() has put the real
					     * socket behind this number */
	int listening;                      /* listen() has put the control
					     * connection behind it, and
					     * accept() reads calls off it */
	int dgram;                          /* socket() asked for SOCK_DGRAM:
					     * UI frames, not a connection */
	int rxclaimed;                      /* the node hands us UI frames for
					     * this callsign, and the
					     * descriptor is that connection */
	int rxerr;                          /* why not, kept until somebody
					     * calls recvfrom() and can be
					     * told */
	int ctl;                            /* the service connection a
					     * datagram socket sends over, -1
					     * until the first sendto() */
	/* What getsockname() and getpeername() answer.  Both are known - the
	 * handover line carries the caller and the called callsign, and
	 * outgoing we have the destination and the bound source - and they
	 * are kept only because those two calls ask for them again later.
	 */
	struct full_sockaddr_ax25 me, him;
	int have_me, have_him;
};

static struct wampes_sock *Socks;
static int Nsocks;

/*---------------------------------------------------------------------------*/

static struct wampes_sock *find_sock(int fd)
{
	struct wampes_sock *s;

	for (s = Socks; s != NULL; s = s->next)
		if (s->fd == fd)
			return s;
	return NULL;
}

static void drop_sock(int fd)
{
	struct wampes_sock *s, **pp;

	for (pp = &Socks; (s = *pp) != NULL; pp = &s->next)
		if (s->fd == fd) {
			*pp = s->next;
			free(s);
			Nsocks--;
			return;
		}
}

/*---------------------------------------------------------------------------*/

/* Which node, and where it listens.  One node per line in wampes.conf, in the
 * shape agwpe.conf has:
 *
 *      <name>  <address>  [description]
 *
 * where the address is a path for a unix socket or host:port for the node's
 * TCP service.  The name is what stands before the colon in an axports entry,
 * so "wampes:hfb" and "wampes:70cm" are two interfaces of the node "wampes".
 *
 * Read once, on first use.  A missing file is not an error, but it does mean
 * no port is ever claimed here: the lookup below answers NULL for every
 * name, so a machine without the file keeps whatever backend it had.  The
 * compiled-in place is the fallback for a node already known to serve a
 * port, and for WAMPES_SOCKET - not a node assumed into existence.
 */

#define WAMPES_MAX_NODE 16

static struct {
	char name[32];
	char addr[256];
} Nodes[WAMPES_MAX_NODE];
static int Nnodes;
static int Nodes_read;

static void wampes_config_load(void)
{
	FILE *fp;
	char line[512];
	int lineno = 0;

	Nodes_read = 1;
	if ((fp = fopen(CONF_WAMPES_FILE, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *name;
		char *addr;
		char *p;

		lineno++;
		if ((p = strchr(line, '#')) != NULL) *p = '\0';
		if ((name = strtok(line, " \t\r\n")) == NULL)
			continue;
		if ((addr = strtok(NULL, " \t\r\n")) == NULL) {
			fprintf(stderr, "wampes_config: %s line %d: "
				"no address for \"%s\"\n",
				CONF_WAMPES_FILE, lineno, name);
			continue;
		}
		if (Nnodes >= WAMPES_MAX_NODE) {
			fprintf(stderr, "wampes_config: %s line %d: "
				"more than %d nodes\n",
				CONF_WAMPES_FILE, lineno, WAMPES_MAX_NODE);
			break;
		}
		if (strlen(name) >= sizeof(Nodes[0].name) ||
		    strlen(addr) >= sizeof(Nodes[0].addr)) {
			fprintf(stderr, "wampes_config: %s line %d: "
				"name or address too long\n",
				CONF_WAMPES_FILE, lineno);
			continue;
		}
		strcpy(Nodes[Nnodes].name, name);
		strcpy(Nodes[Nnodes].addr, addr);
		Nnodes++;
	}
	fclose(fp);
}

/* Is this axports entry served by a WAMPES node, and which?  The node is the
 * part before the colon; an entry with no colon is a node name on its own.
 */

static const char *wampes_node_addr(const char *port)
{
	char name[32];
	const char *colon;
	int i;
	size_t n;

	if (!Nodes_read)
		wampes_config_load();
	colon = strchr(port, ':');
	n = colon ? (size_t) (colon - port) : strlen(port);
	if (n == 0 || n >= sizeof(name))
		return NULL;
	memcpy(name, port, n);
	name[n] = '\0';
	for (i = 0; i < Nnodes; i++)
		if (!strcasecmp(Nodes[i].name, name))
			return Nodes[i].addr;
	return NULL;
}

/* A "wampes:70cm" with no axports entry of its own is accepted and the entry
 * for the node is used.  What it cannot do is pin the port, and the reason is
 * worth knowing: bind() hands us a callsign, never a port name, so the name
 * comes back from a reverse lookup - and entries that share a callsign cannot
 * be told apart by one, while axports refuses duplicate callsigns anyway.  So
 * the suffix is accepted and the node routes.  Pinning a port keeps needing
 * an entry, and that entry keeps needing a callsign of its own.
 *
 * Said once per name rather than silently, because somebody who typed the
 * suffix meant something by it.
 */

static int wampes_lazy_base(const char *name, const char *base)
{
	static char said[8][64];
	static int nsaid;
	int i;

	if (wampes_node_addr(base) == NULL)
		return 0;                   /* not one of ours */
	for (i = 0; i < nsaid; i++)
		if (!strcasecmp(said[i], name))
			return 1;
	if (nsaid < (int) (sizeof(said) / sizeof(said[0])) &&
	    strlen(name) < sizeof(said[0])) {
		strcpy(said[nsaid++], name);
		fprintf(stderr, "wampes: no axports entry for \"%s\" - using %s "
			"and letting the node route\n", name, base);
	}
	return 1;
}

/* A descriptor inherited across exec.
 *
 * ax25d hands its child the accepted connection on descriptor 0, and the
 * child asks getpeername(0) who is calling - axspawn does exactly that before
 * it picks a unix account.  Over a socketpair there is nothing to ask: the
 * kernel answers AF_UNIX and the child refuses the call.  So the parent says
 * so in the environment and this picks it up, which means no child has to be
 * changed for it.
 *
 *      AXSOCK_INHERIT=<fd> <local> <peer>
 *
 * Removed from the environment once read: what is true of this process is not
 * true of anything it starts in turn.
 */

static void wampes_inherit(void)
{
	char him[20], me[20];
	const char *v;
	int fd;
	struct wampes_sock *s;

	if ((v = getenv("AXSOCK_INHERIT")) == NULL)
		return;
	unsetenv("AXSOCK_INHERIT");
	if (sscanf(v, "%d %19s %19s", &fd, me, him) != 3 || fd < 0)
		return;
	if ((s = calloc(1, sizeof(*s))) == NULL)
		return;
	s->fd = fd;
	s->ctl = -1;
	s->connected = 1;
	s->me.fsa_ax25.sax25_family = AF_AX25;
	s->him.fsa_ax25.sax25_family = AF_AX25;
	if (ax25_aton_entry(me, s->me.fsa_ax25.sax25_call.ax25_call) < 0 ||
	    ax25_aton_entry(him, s->him.fsa_ax25.sax25_call.ax25_call) < 0) {
		free(s);
		return;
	}
	s->have_me = 1;
	s->have_him = 1;
	s->next = Socks;
	Socks = s;
	Nsocks++;
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: inherited fd=%d, %s called %s\n",
			fd, him, me);
}

__attribute__((constructor))
static void wampes_init(void)
{
	ax25_config_lazy_hook = wampes_lazy_base;
	wampes_inherit();
}

/* An axports entry names one WAMPES interface: "wampes:hf1".  The part before
 * the colon says which node - that is resolved here, on this side, and never
 * reaches WAMPES.  The part after it says which of the node's ports to leave
 * by, and travels as the prefix WAMPES already understands ("connect
 * hf1:DB0AAA-8"), the same one an operator types at an RMNC or XNET.
 *
 * One entry per interface rather than one per node, because axports refuses
 * duplicate callsigns and every WAMPES interface has a callsign of its own
 * anyway.
 */

static const char *wampes_iface(const char *port)
{
	const char *colon = strchr(port, ':');

	return (colon != NULL && colon[1] != '\0') ? colon + 1 : NULL;
}

/* Where the node listens.  wampes.conf decides, because it is the only place
 * that can tell two nodes apart.  WAMPES_SOCKET still wins over it, so a test
 * can point a program at another node without editing a file, and the
 * compiled-in place is what a machine with one node and no file gets.
 */

static const char *wampes_address(const char *port)
{
	const char *s;

	if ((s = getenv("WAMPES_SOCKET")) != NULL && *s != '\0')
		return s;
	if (port != NULL && (s = wampes_node_addr(port)) != NULL)
		return s;
	return WAMPES_DEFAULT_SOCKET;
}

/* A path is a unix socket, anything else is host:port.  Both are ordinary
 * sockets to the kernel, which is the point.
 */

static int wampes_dial(const char *addr)
{
	char host[256];
	char *colon;
	int fd;
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *ai;

	if (*addr == '/') {
		struct sockaddr_un su;

		if (strlen(addr) >= sizeof(su.sun_path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memset(&su, 0, sizeof(su));
		su.sun_family = AF_UNIX;
		strcpy(su.sun_path, addr);
		if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
			return -1;
		if (connect(fd, (struct sockaddr *) &su, sizeof(su)) < 0) {
			int save = errno;

			close(fd);
			errno = save;
			return -1;
		}
		return fd;
	}

	if (strlen(addr) >= sizeof(host)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(host, addr);
	/* "[fd00::5]:8010" as well as "host:8010".  An IPv6 literal is full of
	 * colons, so the brackets are what says where the address ends -
	 * getaddrinfo() wants them gone again.
	 */
	if (host[0] == '[') {
		char *close = strchr(host, ']');

		if (close == NULL || close[1] != ':') {
			errno = EINVAL;
			return -1;
		}
		*close = '\0';
		colon = close + 1;
		memmove(host, host + 1, strlen(host));
	} else if ((colon = strrchr(host, ':')) == NULL) {
		errno = EINVAL;
		return -1;
	}
	*colon++ = '\0';

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, colon, &hints, &res) != 0 || res == NULL) {
		errno = EHOSTUNREACH;
		return -1;
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		if ((fd = socket(ai->ai_family, ai->ai_socktype,
				 ai->ai_protocol)) < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			freeaddrinfo(res);
			return fd;
		}
		close(fd);
	}
	freeaddrinfo(res);
	errno = ECONNREFUSED;
	return -1;
}

/*---------------------------------------------------------------------------*/

/* One line, however long, of which we keep the first buflen-1 bytes.  The
 * lines that matter are short; a long one is progress and gets ignored
 * anyway.
 *
 * Returns the length, -1 on error with errno set, WAMPES_EOF when the node
 * closed before finishing a line, or WAMPES_INCOMPLETE when a line began and
 * did not arrive within msec.  All three are worth telling apart: every line
 * here ends in a newline, so a line that stops early is not a short answer
 * but no answer at all.
 *
 * msec bounds the wait for the REST of a line, never the wait for its first
 * byte - a caller waiting for a link to come up may wait minutes, and that
 * is not this timeout's business.  It exists because a node can hand over a
 * descriptor and then fail to finish the line describing it: sendmsg() with
 * MSG_DONTWAIT accepts part of a message when the buffer is nearly full and
 * reports how much, and a node that mistakes that for success never sends
 * the remainder.  Without a bound, the caller would wait for a newline that
 * is not coming - with a blocking listener, that is a daemon that stops
 * serving.  0 means wait as long as it takes.
 */

#define WAMPES_EOF		(-2)
#define WAMPES_INCOMPLETE	(-3)

static int msec_left(const struct timespec *deadline)
{
	struct timespec now;
	long ms;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	ms = (deadline->tv_sec - now.tv_sec) * 1000 +
	     (deadline->tv_nsec - now.tv_nsec) / 1000000;
	return ms > 0 ? (int) ms : 0;
}

static int read_line(int fd, char *buf, size_t buflen, int *fdp, int msec)
{
	struct timespec deadline;
	size_t n = 0;
	int started = 0;

	for (;;) {
		if (msec > 0 && started) {
			struct pollfd pfd;
			int r;

			pfd.fd = fd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			r = poll(&pfd, 1, msec_left(&deadline));
			if (r == 0)
				return WAMPES_INCOMPLETE;
			if (r < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
		}
		char c;
		ssize_t got;
		struct cmsghdr *cm;
		struct iovec iov;
		struct msghdr msg;
		union {
			char buf[CMSG_SPACE(sizeof(int))];
			struct cmsghdr align;
		} control;

		/* recvmsg rather than read, because the answer to a handover
		 * carries a descriptor alongside the line.  On a stream it
		 * arrives with the first byte of the message that brought it,
		 * so reading a byte at a time still catches it.
		 */
		memset(&msg, 0, sizeof(msg));
		memset(&control, 0, sizeof(control));
		iov.iov_base = &c;
		iov.iov_len = 1;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.buf;
		msg.msg_controllen = sizeof(control.buf);
		got = recvmsg(fd, &msg, 0);
		if (got > 0 && fdp != NULL && *fdp < 0)
			for (cm = CMSG_FIRSTHDR(&msg); cm != NULL;
			     cm = CMSG_NXTHDR(&msg, cm))
				if (cm->cmsg_level == SOL_SOCKET &&
				    cm->cmsg_type == SCM_RIGHTS)
					memcpy(fdp, CMSG_DATA(cm), sizeof(int));
		if (got == 0)
			return WAMPES_EOF;
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!started) {
			started = 1;
			if (msec > 0 &&
			    clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) {
				deadline.tv_sec += msec / 1000;
				deadline.tv_nsec += (msec % 1000) * 1000000L;
				if (deadline.tv_nsec >= 1000000000L) {
					deadline.tv_sec++;
					deadline.tv_nsec -= 1000000000L;
				}
			}
		}
		if (c == '\n')
			break;
		if (c == '\r')
			continue;                   /* answered in our own EOL */
		if (n + 1 < buflen)
			buf[n++] = c;
	}
	buf[n] = '\0';
	return (int) n;
}

/* WAMPES names the refusal; the application wants an errno.  Everything
 * unrecognised is ECONNREFUSED, which is what a caller can act on.
 */

static int reason_to_errno(const char *line)
{
	const char *why = strstr(line, " - ");

	/* The node knows its own ports, so it is the one that says a name is
	 * not among them.  A wrong interface is a wrong device and nothing
	 * else - the caller gets ENODEV and its own perror() shows it.
	 */
	if (strstr(line, "no interface ") != NULL)
		return ENODEV;
	if (strstr(line, "does not carry AX.25") != NULL)
		return ENODEV;
	if (why == NULL)
		return EINVAL;                  /* a parse error, not a refusal */
	why += 3;
	if (!strcmp(why, "no route"))  return EHOSTUNREACH;
	if (!strcmp(why, "busy"))      return EADDRINUSE;
	if (!strcmp(why, "nomem"))     return ENOBUFS;
	if (!strcmp(why, "invalid"))   return EINVAL;
	if (!strcmp(why, "noproto"))   return EPROTONOSUPPORT;
	return ECONNREFUSED;
}

/* The refusals listen() can get.  They are the same three a TCP server
 * knows, which is not a coincidence: claiming a callsign is claiming an
 * address, and a second claimant has to hear so rather than quietly share.
 */

static int listen_errno(const char *line)
{
	if (strstr(line, "already taken"))         return EADDRINUSE;
	if (strstr(line, "not open for clients"))  return EACCES;
	if (strstr(line, "belongs to a port"))     return EADDRNOTAVAIL;
	return ECONNREFUSED;
}

/*---------------------------------------------------------------------------*/

/* The address family lives in the struct's own field, not in sa_family:
 * sockaddr_ax25 carries the Linux layout, and on BSD and macOS the generic
 * sockaddr begins with sa_len, so the family byte is somewhere else.
 */

static int is_ax25(const struct sockaddr *addr, socklen_t len)
{
	const struct sockaddr_ax25 *sa = (const struct sockaddr_ax25 *) addr;

	return addr != NULL && len >= (socklen_t) sizeof(*sa) &&
		sa->sax25_family == AF_AX25;
}

/*---------------------------------------------------------------------------*/

/* Is any node configured at all - the same register bind() consults, asked
 * without a port in hand.  A caller that has one should ask about that port
 * instead; this is for the questions that come before a port is known.
 */
int wampes_configured(void)
{
	if (!Nodes_read)
		wampes_config_load();
	return Nnodes > 0;
}

int wampes_enabled(void)
{
	const char *b = getenv("AXSOCK_BACKEND");

	return b != NULL && !strcmp(b, "wampes");
}

/*---------------------------------------------------------------------------*/

/* A placeholder.  It has to be a real descriptor because the application
 * gets the number now and connect() only later puts the connection behind
 * it; a socketpair would do as well, but an unbound unix socket is cheaper
 * and behaves the same for the one thing that can happen to it before
 * connect(), which is close().
 */

/* The third argument of socket() is the protocol id.
 *
 * Kernel AX.25 ignored it and every program passes 0, so it has been free all
 * along - and it is the one place the API has for saying "this connection
 * carries something other than text".  A node listens per protocol id and a
 * connect can name one, so a program can finally ask for either.  Noted here
 * for every AF_AX25 socket, because bind() may hand one over to us that
 * somebody else made.
 */

static struct { int fd; int pid; } Pids[WAMPES_MAX_SOCK];
static int Npids;

void wampes_note_protocol(int fd, int protocol)
{
	int i;

	for (i = 0; i < Npids; i++)
		if (Pids[i].fd == fd) {
			Pids[i].pid = protocol;
			return;
		}
	if (Npids < (int) (sizeof(Pids) / sizeof(Pids[0]))) {
		Pids[Npids].fd = fd;
		Pids[Npids].pid = protocol;
		Npids++;
	}
}

static int protocol_of(int fd)
{
	int i;

	for (i = 0; i < Npids; i++)
		if (Pids[i].fd == fd) {
			int pid = Pids[i].pid;

			Pids[i] = Pids[--Npids];
			return pid;
		}
	return 0;
}

int wampes_socket(int type)
{
	struct wampes_sock *s;
	int fd;

	if (type != SOCK_SEQPACKET && type != SOCK_DGRAM) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	if (Nsocks >= WAMPES_MAX_SOCK) {
		errno = EMFILE;
		return -1;
	}
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if ((s = calloc(1, sizeof(*s))) == NULL) {
		close(fd);
		errno = ENOMEM;
		return -1;
	}
	s->fd = fd;
	s->ctl = -1;
	s->dgram = (type == SOCK_DGRAM);
	s->next = Socks;
	Socks = s;
	Nsocks++;
	return fd;
}

/*---------------------------------------------------------------------------*/

/* bind() carries two different things in one address.  sax25_call is the
 * source callsign - what "call -s" sets, and what WAMPES is told with "<".
 * The first digipeater slot is not a digipeater at all: libax25 puts the
 * callsign of the axports entry there, which is how the port is named.
 */

/* Which axports entry does this bind name?  libax25 puts the entry's callsign
 * in the first digipeater slot, so the name comes back from the reverse
 * lookup.  An empty answer is not an error here: a program may bind a source
 * call without naming a port at all.
 */

static void port_of_bind(const struct sockaddr *addr, socklen_t len,
			 char *port, size_t portlen)
{
	const struct full_sockaddr_ax25 *fsa =
		(const struct full_sockaddr_ax25 *) addr;
	char *name;

	*port = '\0';
	if (len < (socklen_t) sizeof(*fsa))
		return;
	/* The port is named by the callsign in the first digipeater slot - but
	 * only by programs that put it there.  call(1) always does; beacon(8)
	 * does it only when its -c differs from the port's own callsign, and
	 * binds the bare callsign otherwise.  A socket bound that way could
	 * not be recognised as a WAMPES one and stayed with AGWPE without a
	 * word, which is a quiet way to send a beacon nowhere.  So when there
	 * is no digipeater, ask the source callsign instead: it resolves only
	 * if it is a port's callsign, and a user's own callsign has no entry
	 * and answers nothing, which is the right outcome for it.
	 *
	 * Only reads it, but says otherwise in the header.
	 */
	if (fsa->fsa_ax25.sax25_ndigis > 0)
		name = ax25_config_get_port(
			(ax25_address *) &fsa->fsa_digipeater[0]);
	else
		name = ax25_config_get_port(
			(ax25_address *) &fsa->fsa_ax25.sax25_call);
	if (name == NULL && ax25_config_get_next(NULL) == NULL) {
		/* The port table belongs to the application: every program in
		 * the suite calls ax25_config_load_ports() at startup, and a
		 * program that only had the library preloaded calls nothing at
		 * all.  With an empty table the port cannot be named, so the
		 * socket went to AGWPE without a word - and then righted itself
		 * on the next bind, because the AGWPE path loads the table as a
		 * side effect.  Load it here, once, rather than leave the
		 * backend to depend on the order of the binds.
		 */
		ax25_config_load_ports();
		name = ax25_config_get_port(fsa->fsa_ax25.sax25_ndigis > 0
			? (ax25_address *) &fsa->fsa_digipeater[0]
			: (ax25_address *) &fsa->fsa_ax25.sax25_call);
	}
	if (name == NULL)
		return;
	strncpy(port, name, portlen - 1);
	port[portlen - 1] = '\0';
}

/* Put an unbound unix socket behind a descriptor the application already
 * holds.  Used when a port turns out to be a WAMPES one after the socket was
 * made by somebody else - a kernel AF_AX25 socket, say.  The number survives,
 * which is all the application knows about it.
 */

static int replace_with_placeholder(int fd)
{
	int ph;

	if ((ph = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if (dup2(ph, fd) < 0) {
		int save = errno;

		close(ph);
		errno = save;
		return -1;
	}
	close(ph);
	return 0;
}

/* Defined with the rest of the receiving side, below. */
static void claim_ui(struct wampes_sock *s);

int wampes_bind(int fd, const struct sockaddr *addr, socklen_t len, int *ret)
{
	char port[32];
	const struct sockaddr_ax25 *sa;
	struct wampes_sock *s;

	s = find_sock(fd);
	if (s == NULL && !is_ax25(addr, len))
		return 0;                       /* not ours, and not AX.25 */
	if (!is_ax25(addr, len)) {
		*ret = -1;
		errno = EAFNOSUPPORT;
		return 1;
	}
	port_of_bind(addr, len, port, sizeof(port));

	if (s == NULL) {
		/* Not ours yet.  bind() is the first moment the port is known,
		 * so it is the first moment the backend can be chosen per port
		 * instead of per process.  A port belongs to us when its node
		 * is named in wampes.conf - the file is the register, so no
		 * name is magic and nothing has to be guessed from a prefix.
		 */
		if (!port[0] || wampes_node_addr(port) == NULL)
			return 0;
		/* Whoever made the descriptor lets go of it, or we put an
		 * empty socket behind the number ourselves.
		 */
		/* Ask before taking it: socket() decided whether this is a
		 * datagram socket or a connection, and after the handover
		 * nobody remembers.  Getting this wrong is quiet - sendto()
		 * would fall through to the descriptor itself and answer
		 * EISCONN, which is true of a socketpair end and says nothing
		 * about AX.25.
		 */
		int type = axsock_socktype(fd);

		if (axsock_forget(fd) != 0 && replace_with_placeholder(fd)) {
			*ret = -1;
			return 1;
		}
		if (Nsocks >= WAMPES_MAX_SOCK) {
			*ret = -1;
			errno = EMFILE;
			return 1;
		}
		if ((s = calloc(1, sizeof(*s))) == NULL) {
			*ret = -1;
			errno = ENOMEM;
			return 1;
		}
		s->fd = fd;
		s->ctl = -1;
		s->dgram = (type == SOCK_DGRAM);
		s->pid = protocol_of(fd);
		s->next = Socks;
		Socks = s;
		Nsocks++;
		if (getenv("AXSOCK_DEBUG"))
			fprintf(stderr, "wampes: fd=%d taken over for port '%s'\n",
				fd, port);
	}

	*ret = -1;
	sa = (const struct sockaddr_ax25 *) addr;
	strncpy(s->local, ax25_ntoa(&sa->sax25_call), sizeof(s->local) - 1);
	s->local[sizeof(s->local) - 1] = '\0';
	strcpy(s->port, port);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: bind fd=%d local='%s' port='%s'\n",
			fd, s->local, s->port);
	if (s->dgram && !s->rxclaimed)
		claim_ui(s);
	*ret = 0;
	return 1;
}

/*---------------------------------------------------------------------------*/

int wampes_connect(int fd, const struct sockaddr *addr, socklen_t len,
		   int *ret)
{
	char line[512];
	char cmd[512];
	const struct full_sockaddr_ax25 *fsa;
	const struct sockaddr_ax25 *sa;
	int i;
	int handed = -1;
	int ndigis = 0;
	int sock;
	struct wampes_sock *s;

	if ((s = find_sock(fd)) == NULL)
		return 0;                       /* not ours */
	*ret = -1;
	if (!is_ax25(addr, len)) {
		errno = EAFNOSUPPORT;
		return 1;
	}
	sa = (const struct sockaddr_ax25 *) addr;

	if ((sock = wampes_dial(wampes_address(s->port))) < 0)
		return 1;

	/* No end-of-line conversion: an AX.25 socket is what the kernel gave,
	 * and the kernel converted nothing.
	 */
	if (write(sock, "binary\n", 7) != 7) {
		close(sock);
		errno = ECONNRESET;
		return 1;
	}
	/* Ask for a descriptor rather than for this connection to become the
	 * pipe.  The node makes the pair itself and can therefore give it
	 * frame boundaries, which a connection the application made cannot
	 * have.  A node that does not know the word answers nothing and the
	 * old way still works - see below.
	 */
	(void) write(sock, "handover\n", 9);

	strcpy(cmd, "connect ");
	{
		const char *iface = wampes_iface(s->port);

		if (iface != NULL) {
			strncat(cmd, iface, sizeof(cmd) - strlen(cmd) - 2);
			strncat(cmd, ":", sizeof(cmd) - strlen(cmd) - 2);
		}
	}
	strncat(cmd, ax25_ntoa(&sa->sax25_call),
		sizeof(cmd) - strlen(cmd) - 2);

	fsa = (const struct full_sockaddr_ax25 *) addr;
	if (len >= (socklen_t) sizeof(*fsa))
		ndigis = fsa->fsa_ax25.sax25_ndigis;
	if (ndigis > AX25_MAX_DIGIS)
		ndigis = AX25_MAX_DIGIS;
	for (i = 0; i < ndigis; i++) {
		strncat(cmd, i ? "," : " via ", sizeof(cmd) - strlen(cmd) - 2);
		strncat(cmd, ax25_ntoa(&fsa->fsa_digipeater[i]),
			sizeof(cmd) - strlen(cmd) - 2);
	}
	if (s->local[0] != '\0') {
		strncat(cmd, " < ", sizeof(cmd) - strlen(cmd) - 2);
		strncat(cmd, s->local, sizeof(cmd) - strlen(cmd) - 2);
	}
	if (s->pid) {
		char opt[24];

		sprintf(opt, " --pid 0x%02x", s->pid);
		strncat(cmd, opt, sizeof(cmd) - strlen(cmd) - 2);
	}
	strcat(cmd, "\n");

	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s", cmd);
	if (write(sock, cmd, strlen(cmd)) != (ssize_t) strlen(cmd)) {
		close(sock);
		errno = ECONNRESET;
		return 1;
	}

	for (;;) {
		if (read_line(sock, line, sizeof(line), &handed, 0) < 0) {
			/* The node closed without a verdict.  It does that
			 * when the link never came up: WAMPES retried until
			 * it gave up and dropped the control block.
			 */
			close(sock);
			errno = ETIMEDOUT;
			return 1;
		}
		if (getenv("AXSOCK_DEBUG"))
			fprintf(stderr, "wampes: <- %s\n", line);
		if (strncmp(line, "*** ", 4) != 0)
			continue;                   /* progress */
		if (strncmp(line, "*** connected", 13) == 0)
			break;
		close(sock);
		errno = reason_to_errno(line);
		return 1;
	}

	/* The connection takes the descriptor number the application already
	 * holds, and the placeholder goes with it.  From here nothing of ours
	 * is in the way: the entry is dropped, so read(), write(), poll() and
	 * close() find nothing to intercept and go to the kernel.
	 */
	/* The descriptor the node handed over is the session; the connection
	 * we spoke over was only the way to ask for it.  Without one - an
	 * older node - that connection is the session, as it always was.
	 */
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, handed >= 0
			? "wampes: session on a handed-over descriptor\n"
			: "wampes: no handover - the control connection is the session\n");
	if (handed >= 0) {
		close(sock);
		sock = handed;
	}
	if (dup2(sock, fd) < 0) {
		int save = errno;

		close(sock);
		errno = save;
		return 1;
	}
	close(sock);
	s->connected = 1;
	/* ax25d and axspawn ask afterwards who is at each end; answer from
	 * what we already had rather than from the unix socket underneath,
	 * which would say AF_UNIX with an empty path and be believed.
	 */
	memset(&s->him, 0, sizeof(s->him));
	memcpy(&s->him, addr,
	       len < (socklen_t) sizeof(s->him) ? (size_t) len : sizeof(s->him));
	s->him.fsa_ax25.sax25_family = AF_AX25;
	s->have_him = 1;
	memset(&s->me, 0, sizeof(s->me));
	s->me.fsa_ax25.sax25_family = AF_AX25;
	if (s->local[0] != '\0')
		ax25_aton_entry(s->local, s->me.fsa_ax25.sax25_call.ax25_call);
	s->have_me = 1;
	*ret = 0;
	return 1;
}

/*---------------------------------------------------------------------------*/

/* listen() claims the callsign the socket was bound to.
 *
 * The claim goes over an ordinary service connection, and that connection
 * then becomes the listening descriptor - which is why poll() and select()
 * on it work with nothing of ours involved: it is readable exactly when a
 * call is waiting.
 *
 * The refusal surfaces here rather than in bind(), where a TCP server would
 * meet it.  bind() cannot ask: the same call names the source of an outgoing
 * connection, and claiming a callsign for every socket that names one would
 * be wrong.  Only listen() says what the socket is for.
 */

int wampes_listen(int fd, int *ret)
{
	char line[512];
	char cmd[128];
	int sock;
	struct wampes_sock *s;

	if ((s = find_sock(fd)) == NULL)
		return 0;                   /* not ours */
	*ret = -1;
	if (s->local[0] == '\0') {
		errno = EDESTADDRREQ;       /* nothing was bound */
		return 1;
	}
	if ((sock = wampes_dial(wampes_address(s->port))) < 0)
		return 1;

	if (s->pid)
		sprintf(cmd, "listen %s pid=0x%02x\n", s->local, s->pid);
	else
		sprintf(cmd, "listen %s\n", s->local);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s", cmd);
	if (write(sock, cmd, strlen(cmd)) != (ssize_t) strlen(cmd)) {
		close(sock);
		errno = ECONNRESET;
		return 1;
	}
	for (;;) {
		if (read_line(sock, line, sizeof(line), NULL, 0) < 0) {
			close(sock);
			errno = ECONNRESET;
			return 1;
		}
		if (getenv("AXSOCK_DEBUG"))
			fprintf(stderr, "wampes: <- %s\n", line);
		if (strncmp(line, "*** ", 4) != 0)
			continue;
		if (strncmp(line, "*** listening", 13) == 0)
			break;
		close(sock);
		errno = listen_errno(line);
		return 1;
	}
	if (dup2(sock, fd) < 0) {
		int save = errno;

		close(sock);
		errno = save;
		return 1;
	}
	close(sock);
	s->listening = 1;
	*ret = 0;
	return 1;
}

/*---------------------------------------------------------------------------*/

/* How long accept() waits for the rest of a trace line whose first byte has
 * arrived.
 *
 * The two ways of being wrong are not the same size.  Too short and a call
 * that merely needed two writes is thrown away - which is the fault this
 * bound was added to prevent.  Too long and a daemon stands still for that
 * much, once, on an event that should not happen at all.  So it is generous:
 * what has to fit is the rest of a line on a local socket, and the only
 * variable is when the node's cooperative scheduler comes round to writing
 * it.  Two seconds is a long time for that and no time at all for a station
 * waiting to log in.
 */
#define WAMPES_HANDOVER_MSEC	2000

/* accept() takes the trace line and the descriptor the node sends together,
 * so there is nothing to match up.
 *
 *      hfa DL1TST-1,DB0BBB > DL9SAU-13
 *
 * The port, then who called and by what path, then the callsign they called.
 * What goes into the address the caller gets is the calling station, which is
 * what a peer address means.
 */

static void parse_call(const char *text, struct full_sockaddr_ax25 *fsa,
		       socklen_t len)
{
	char buf[128];
	char *p;
	int n = 0;

	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (p = strtok(buf, ","); p != NULL; p = strtok(NULL, ",")) {
		if (n == 0) {
			ax25_aton_entry(p, fsa->fsa_ax25.sax25_call.ax25_call);
		} else {
			if (len < (socklen_t) sizeof(*fsa) ||
			    n - 1 >= AX25_MAX_DIGIS)
				break;
			ax25_aton_entry(p, fsa->fsa_digipeater[n - 1].ax25_call);
			fsa->fsa_ax25.sax25_ndigis = n;
		}
		n++;
	}
	fsa->fsa_ax25.sax25_family = AF_AX25;
}

int wampes_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int *ret)
{
	char line[256];
	char *sp;
	int newfd = -1;
	struct wampes_sock *s;
	int n;

	if ((s = find_sock(fd)) == NULL || !s->listening)
		return 0;                   /* not ours, or not listening */
	*ret = -1;

	/* Read to the end of the line rather than trusting one recvmsg() to
	 * hold it.  This is a stream: the node sends the trace line and the
	 * descriptor in one sendmsg(), but it sends with MSG_DONTWAIT, and a
	 * stream socket whose buffer is nearly full accepts part of a message
	 * and reports that.  The rest then arrives later - the descriptor
	 * having come with the first byte - and a single recvmsg() here would
	 * hand up a half-parsed call and leave the remainder for the next
	 * accept(), which would find a line with no descriptor and answer
	 * EPROTO.  A session that vanishes without a word, sometimes, when the
	 * channel is busy: which is exactly when a second station is being
	 * handed over at the same time.
	 *
	 * read_line() collects the descriptor from whichever byte carried it
	 * and stops at the newline, which is what the connect path has always
	 * done.
	 */
	n = read_line(fd, line, sizeof(line), &newfd, WAMPES_HANDOVER_MSEC);
	if (n < 0) {
		/* The session was handed over before the line describing it was
		 * finished; without the line there is nothing to hand up, and
		 * holding the descriptor would leak it. */
		if (newfd >= 0)
			close(newfd);
		if (n == WAMPES_EOF)
			errno = ECONNABORTED;   /* the node went away */
		else if (n == WAMPES_INCOMPLETE)
			errno = EPROTO;         /* half a line, and no more */
		return 1;                   /* otherwise errno is from recvmsg */
	}
	if (newfd < 0) {
		errno = EPROTO;             /* a line without a descriptor */
		return 1;
	}
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: accept %s", line);

	/* "<port> <src>[,<digi>...] > <dst>": the caller with the path it came
	 * by, and the callsign of ours that it reached.  Both are wanted -
	 * the first is the peer address, the second is what ax25d asks for to
	 * choose a configuration stanza.
	 */
	{
		struct full_sockaddr_ax25 him, me;
		struct wampes_sock *ns;
		char *arrow;

		memset(&him, 0, sizeof(him));
		memset(&me, 0, sizeof(me));
		me.fsa_ax25.sax25_family = AF_AX25;
		if ((sp = strchr(line, ' ')) != NULL) {
			char *end = strchr(++sp, ' ');

			if (end != NULL) *end = '\0';
			parse_call(sp, &him, (socklen_t) sizeof(him));
			if (end != NULL &&
			    (arrow = strstr(end + 1, "> ")) != NULL) {
				char *dst = arrow + 2;
				char *nl = strpbrk(dst, " \r\n");

				if (nl != NULL) *nl = '\0';
				ax25_aton_entry(dst,
					me.fsa_ax25.sax25_call.ax25_call);
			}
		}
		if (addr != NULL && addrlen != NULL &&
		    *addrlen >= (socklen_t) sizeof(struct sockaddr_ax25)) {
			if (*addrlen > (socklen_t) sizeof(him))
				*addrlen = sizeof(him);
			memcpy(addr, &him, *addrlen);
		}
		/* The session descriptor is tracked from here on, so that
		 * getsockname() and getpeername() can be answered for it.
		 * Nothing else about it is intercepted.
		 */
		if (Nsocks < WAMPES_MAX_SOCK &&
		    (ns = calloc(1, sizeof(*ns))) != NULL) {
			ns->fd = newfd;
			ns->ctl = -1;
			ns->connected = 1;
			ns->me = me;
			ns->him = him;
			ns->have_me = 1;
			ns->have_him = 1;
			strncpy(ns->port, s->port, sizeof(ns->port) - 1);
			ns->next = Socks;
			Socks = ns;
			Nsocks++;
		}
	}
	*ret = newfd;
	return 1;
}

/*---------------------------------------------------------------------------*/

/* Everything or nothing.  A short write on the service socket is not an
 * error - the node's own handover taught us that the hard way - and half a
 * counted frame would leave the node reading payload it will never get.
 */

static int write_all(int fd, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0) {
		ssize_t n;

#ifdef MSG_NOSIGNAL
		n = send(fd, p, len, MSG_NOSIGNAL);
#else
		n = write(fd, p, len);
#endif
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EPIPE;
			return -1;
		}
		p += n;
		len -= (size_t) n;
	}
	return 0;
}

/* Sending a UI frame.
 *
 *      -> datagram hfb: --pid 0xf0        once, at the first sendto()
 *      -> [4]DL9SAU-2>DL1ABC,DB0AAA:abcd  one frame, and no line ending
 *
 * The count is what makes this safe on a stream, and it is the node's own
 * form: it reads exactly n bytes after the colon, so CR and NL in the payload
 * are content and nothing closes a frame early.  The node answers only when
 * it refuses something, and the "datagram" command switches itself to binary.
 *
 * The path travels as it stands - this is a bridge, not a router: the frame
 * leaves the port as if the node had sent it there itself.  Receiving UI
 * frames is not built; the node can deliver them, nothing here asks for them.
 */

ssize_t wampes_sendto(int fd, const void *buf, size_t len, int flags,
		      const struct sockaddr *addr, socklen_t alen,
		      ssize_t *ret)
{
	const struct full_sockaddr_ax25 *fsa;
	const struct sockaddr_ax25 *sa;
	struct wampes_sock *s;
	char frame[128];
	char line[256];
	int ndigis = 0;
	int i;

	(void) flags;
	if ((s = find_sock(fd)) == NULL || !s->dgram)
		return 0;                   /* not ours, or not a datagram */
	*ret = -1;

	if (addr == NULL) {
		errno = EDESTADDRREQ;       /* a UI frame needs somewhere to go */
		return 1;
	}
	if (!is_ax25(addr, alen)) {
		errno = EAFNOSUPPORT;
		return 1;
	}
	if (len > 256) {                    /* an AX.25 frame is not a stream */
		errno = EMSGSIZE;
		return 1;
	}

	if (s->ctl < 0) {
		const char *iface = wampes_iface(s->port);
		int c;

		if ((c = wampes_dial(wampes_address(s->port))) < 0)
			return 1;
		/* No port named means every AX.25 port of the node, which is
		 * what a beacon on a node-wide entry asks for. */
		snprintf(line, sizeof(line), "datagram %s%s",
			 iface != NULL ? iface : "", iface != NULL ? ":" : "");
		if (s->pid) {
			char opt[24];

			snprintf(opt, sizeof(opt), " --pid 0x%02x", s->pid);
			strncat(line, opt, sizeof(line) - strlen(line) - 2);
		}
		strncat(line, "\n", sizeof(line) - strlen(line) - 1);
		if (getenv("AXSOCK_DEBUG"))
			fprintf(stderr, "wampes: -> %s", line);
		if (write_all(c, line, strlen(line)) != 0) {
			int save = errno;

			close(c);
			errno = save;
			return 1;
		}
		s->ctl = c;
	}

	/* "[n]SRC>DEST[,DIGI...]:" - the source is the bound callsign, or the
	 * one the port carries when the caller bound none.
	 */
	sa = (const struct sockaddr_ax25 *) addr;
	fsa = (const struct full_sockaddr_ax25 *) addr;
	if (alen >= (socklen_t) sizeof(*fsa))
		ndigis = fsa->fsa_ax25.sax25_ndigis;
	if (ndigis > AX25_MAX_DIGIS)
		ndigis = AX25_MAX_DIGIS;

	{
		const char *src = s->local;
		char *portcall;

		if (*src == '\0' && s->port[0] != '\0' &&
		    (portcall = ax25_config_get_addr(s->port)) != NULL)
			src = portcall;
		if (*src == '\0') {
			errno = EDESTADDRREQ;   /* nothing to send it from */
			return 1;
		}
		snprintf(frame, sizeof(frame), "%s>%s", src,
			 ax25_ntoa(&sa->sax25_call));
	}
	for (i = 0; i < ndigis; i++) {
		strncat(frame, ",", sizeof(frame) - strlen(frame) - 2);
		strncat(frame, ax25_ntoa(&fsa->fsa_digipeater[i]),
			sizeof(frame) - strlen(frame) - 2);
	}

	snprintf(line, sizeof(line), "[%zu]%s:", len, frame);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s<%zu bytes>\n", line, len);
	if (write_all(s->ctl, line, strlen(line)) != 0 ||
	    write_all(s->ctl, buf, len) != 0) {
		int save = errno;

		/* The node is gone or refused the command it never answered.
		 * Let the next frame open a fresh connection rather than
		 * writing into a dead one for ever. */
		close(s->ctl);
		s->ctl = -1;
		errno = save;
		return 1;
	}
	*ret = (ssize_t) len;
	return 1;
}

/*---------------------------------------------------------------------------*/

/* Receiving UI frames.
 *
 * The node hands them to whoever claimed the callsign, in the same counted
 * form the sending direction uses and on the connection the claim was made
 * on:
 *
 *      <- [5]DL1ABC>DB0FHN-13,DB0AAA*:hallo
 *
 * So the claim happens at bind(), not at the first recvfrom(): the connection
 * then takes the descriptor's place, and select() and poll() on it are
 * readable exactly when a frame has arrived - which is what a program that
 * waits for one does.  A refusal is not an error at bind(): a program that
 * only sends beacons binds too, and the callsign it binds is usually a port's
 * own, which no client may claim.  The reason is kept and handed to whoever
 * calls recvfrom().
 */

#define AX25_REPEATED	0x80		/* in the SSID byte, as on the air */

static void claim_ui(struct wampes_sock *s)
{
	char cmd[128];
	char line[256];
	int c;

	s->rxerr = ENOTCONN;
	if (s->local[0] == '\0')
		return;
	if ((c = wampes_dial(wampes_address(s->port))) < 0) {
		s->rxerr = errno;
		return;
	}
	if (s->pid)
		snprintf(cmd, sizeof(cmd), "listen ui %s pid=0x%02x\n",
			 s->local, s->pid);
	else
		snprintf(cmd, sizeof(cmd), "listen ui %s\n", s->local);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s", cmd);
	if (write_all(c, cmd, strlen(cmd)) != 0) {
		s->rxerr = errno;
		close(c);
		return;
	}
	for (;;) {
		if (read_line(c, line, sizeof(line), NULL, 0) < 0) {
			s->rxerr = ECONNRESET;
			close(c);
			return;
		}
		if (getenv("AXSOCK_DEBUG"))
			fprintf(stderr, "wampes: <- %s\n", line);
		if (strncmp(line, "*** ", 4) != 0)
			continue;
		if (strncmp(line, "*** listening", 13) == 0)
			break;
		s->rxerr = listen_errno(line);
		close(c);
		return;
	}
	/* The connection becomes the descriptor, as it does for a listening
	 * socket: from here poll() and select() answer for it and nothing of
	 * ours is asked. */
	if (dup2(c, s->fd) < 0) {
		s->rxerr = errno;
		close(c);
		return;
	}
	close(c);
	s->rxclaimed = 1;
	s->rxerr = 0;
}

/* Exactly n bytes, however they arrive. */

static int read_all(int fd, void *data, size_t len)
{
	char *p = data;

	while (len > 0) {
		ssize_t n = read(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = ECONNRESET;
			return -1;
		}
		p += n;
		len -= (size_t) n;
	}
	return 0;
}

/* One byte at a time up to a delimiter.  The header is short and arrives
 * once per frame; a buffer of our own would have to be drained again by the
 * next call, and there is no second reader to drain it for. */

static int read_until(int fd, char stop, char *buf, size_t buflen)
{
	size_t n = 0;

	for (;;) {
		char c;

		if (read_all(fd, &c, 1) != 0)
			return -1;
		if (c == stop)
			break;
		if (n + 1 < buflen)
			buf[n++] = c;
	}
	buf[n] = '\0';
	return (int) n;
}

/* "SRC>DEST[,DIGI[*]...]" into the address a caller of recvfrom() gets: the
 * sending station, and the path it came by.  A "*" marks an element that has
 * already repeated the frame, which is the has-been-repeated bit on the air
 * and belongs in the SSID byte here.
 */

static void parse_ui_header(char *hdr, struct full_sockaddr_ax25 *fsa)
{
	char *arrow;
	char *p;
	int n = 0;

	memset(fsa, 0, sizeof(*fsa));
	fsa->fsa_ax25.sax25_family = AF_AX25;
	if ((arrow = strchr(hdr, '>')) != NULL)
		*arrow = '\0';
	ax25_aton_entry(hdr, fsa->fsa_ax25.sax25_call.ax25_call);
	if (arrow == NULL)
		return;
	for (p = strtok(arrow + 1, ","); p != NULL; p = strtok(NULL, ",")) {
		size_t l = strlen(p);
		int repeated = 0;

		if (n == 0) {                   /* the destination, not a digi */
			n++;
			continue;
		}
		if (l > 0 && p[l - 1] == '*') {
			repeated = 1;
			p[l - 1] = '\0';
		}
		if (n - 1 >= AX25_MAX_DIGIS)
			break;
		ax25_aton_entry(p, fsa->fsa_digipeater[n - 1].ax25_call);
		if (repeated)
			fsa->fsa_digipeater[n - 1].ax25_call[6] |= AX25_REPEATED;
		fsa->fsa_ax25.sax25_ndigis = n;
		n++;
	}
}

ssize_t wampes_recvfrom(int fd, void *buf, size_t len, int flags,
			struct sockaddr *addr, socklen_t *alen, ssize_t *ret)
{
	struct full_sockaddr_ax25 him;
	struct wampes_sock *s;
	char count[16];
	char hdr[160];
	char *end;
	long n;

	(void) flags;
	if ((s = find_sock(fd)) == NULL || !s->dgram)
		return 0;                   /* not ours, or not a datagram */
	*ret = -1;
	if (!s->rxclaimed) {
		errno = s->rxerr ? s->rxerr : ENOTCONN;
		return 1;
	}

	/* "[n]" first, so the very first byte decides and no payload can be
	 * read as a length. */
	if (read_until(fd, '[', hdr, sizeof(hdr)) < 0 ||
	    read_until(fd, ']', count, sizeof(count)) < 0)
		return 1;
	n = strtol(count, &end, 10);
	if (*end != '\0' || n < 0) {
		errno = EPROTO;
		return 1;
	}
	if (read_until(fd, ':', hdr, sizeof(hdr)) < 0)
		return 1;

	if ((size_t) n <= len) {
		if (read_all(fd, buf, (size_t) n) != 0)
			return 1;
		*ret = n;
	} else {
		/* A datagram is what it is: keep what fits and drop the rest,
		 * rather than leave half a frame in the stream for the next
		 * call to read as a header. */
		char waste[256];
		size_t left = (size_t) n - len;

		if (read_all(fd, buf, len) != 0)
			return 1;
		while (left > 0) {
			size_t k = left > sizeof(waste) ? sizeof(waste) : left;

			if (read_all(fd, waste, k) != 0)
				return 1;
			left -= k;
		}
		*ret = (ssize_t) len;
	}

	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: <- [%ld]%s: %zd bytes\n", n, hdr, *ret);

	parse_ui_header(hdr, &him);
	if (addr != NULL && alen != NULL &&
	    *alen >= (socklen_t) sizeof(struct sockaddr_ax25)) {
		if (*alen > (socklen_t) sizeof(him))
			*alen = sizeof(him);
		memcpy(addr, &him, *alen);
	}
	return 1;
}

/*---------------------------------------------------------------------------*/

/* Who is at each end.  ax25d asks the first of these on every accepted socket
 * to learn which of the node's callsigns was dialled, because that decides
 * which of its stanzas applies; axspawn asks the second to learn who is
 * logging in.  Without an answer here they reach the unix socket underneath
 * and get AF_UNIX with an empty path - not an error, just rubbish, which is
 * the worst of the two.
 */

static int answer_addr(const struct full_sockaddr_ax25 *src, int have,
		       struct sockaddr *addr, socklen_t *addrlen, int *ret)
{
	socklen_t n;

	*ret = -1;
	if (!have) {
		errno = ENOTCONN;
		return 1;
	}
	if (addr == NULL || addrlen == NULL ||
	    *addrlen < (socklen_t) sizeof(struct sockaddr_ax25)) {
		errno = EINVAL;
		return 1;
	}
	n = *addrlen;
	if (n > (socklen_t) sizeof(*src)) n = sizeof(*src);
	memcpy(addr, src, n);
	*addrlen = n;
	*ret = 0;
	return 1;
}

int wampes_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen,
		       int *ret)
{
	struct wampes_sock *s;

	if ((s = find_sock(fd)) == NULL)
		return 0;
	return answer_addr(&s->me, s->have_me, addr, addrlen, ret);
}

int wampes_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen,
		       int *ret)
{
	struct wampes_sock *s;

	if ((s = find_sock(fd)) == NULL)
		return 0;
	return answer_addr(&s->him, s->have_him, addr, addrlen, ret);
}

/*---------------------------------------------------------------------------*/

/* The channel parameters belong to the node's interface configuration, so
 * this has nothing to set - but it must not fail.  A program that checks the
 * return value would give up on being told its window size was refused, and
 * that was the agreed behaviour before any of this: answer success, change
 * nothing.  Only descriptors that are ours are answered for.
 */

/*
 * The same answer the AGWPE side gives, and for the same reasons - see
 * axsock_opt_refuse() there.  It used to accept everything under SOL_AX25 in
 * silence, which meant a sysop who had set window in axports(5) got a hint on
 * one backend and nothing on the other, and rsuplnk(8) was told its
 * AX25_PIDINCL had been honoured when it had not.
 */
int wampes_setsockopt(int fd, int level, int optname, int *ret)
{
	if (level != SOL_AX25 || find_sock(fd) == NULL)
		return 0;
	if (axsock_opt_refuse(optname)) {
		errno = ENOPROTOOPT;
		*ret = -1;
		return 1;
	}
	axsock_opt_note_ignored(optname);
	*ret = 0;
	return 1;
}

/*---------------------------------------------------------------------------*/

int wampes_close(int fd)
{
	struct wampes_sock *s;

	if ((s = find_sock(fd)) == NULL)
		return 0;
	/* The service connection a datagram socket sends over is ours, not the
	 * application's: nothing else will ever close it. */
	if (s->ctl >= 0)
		close(s->ctl);
	drop_sock(fd);
	return 0;
}
