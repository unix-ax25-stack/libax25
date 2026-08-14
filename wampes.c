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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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
	int connected;                      /* connect() has put the real
					     * socket behind this number */
	int listening;                      /* listen() has put the control
					     * connection behind it, and
					     * accept() reads calls off it */
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
 * Read once, on first use.  A missing file is not an error: without one the
 * single node at the compiled-in place is assumed, which is what a machine
 * with one WAMPES on it wants and saves it a configuration file that would
 * only ever hold one line.
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

__attribute__((constructor))
static void wampes_init(void)
{
	ax25_config_lazy_hook = wampes_lazy_base;
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
 */

static int read_line(int fd, char *buf, size_t buflen)
{
	size_t n = 0;

	for (;;) {
		char c;
		ssize_t got = read(fd, &c, 1);

		if (got == 0)
			return n ? (int) n : -1;    /* EOF */
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
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
	if (len < (socklen_t) sizeof(*fsa) || fsa->fsa_ax25.sax25_ndigis <= 0)
		return;
	/* Only reads it, but says otherwise in the header. */
	name = ax25_config_get_port((ax25_address *) &fsa->fsa_digipeater[0]);
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
	strcat(cmd, "\n");

	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s", cmd);
	if (write(sock, cmd, strlen(cmd)) != (ssize_t) strlen(cmd)) {
		close(sock);
		errno = ECONNRESET;
		return 1;
	}

	for (;;) {
		if (read_line(sock, line, sizeof(line)) < 0) {
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
	if (dup2(sock, fd) < 0) {
		int save = errno;

		close(sock);
		errno = save;
		return 1;
	}
	close(sock);
	s->connected = 1;
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

	sprintf(cmd, "listen %s\n", s->local);
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: -> %s", cmd);
	if (write(sock, cmd, strlen(cmd)) != (ssize_t) strlen(cmd)) {
		close(sock);
		errno = ECONNRESET;
		return 1;
	}
	for (;;) {
		if (read_line(sock, line, sizeof(line)) < 0) {
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

/* accept() is one recvmsg(): the node sends the trace line and the descriptor
 * for the session together, so there is nothing to match up.
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
	struct cmsghdr *cm;
	struct iovec iov;
	struct msghdr msg;
	struct wampes_sock *s;
	ssize_t n;
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} control;

	if ((s = find_sock(fd)) == NULL || !s->listening)
		return 0;                   /* not ours, or not listening */
	*ret = -1;

	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	iov.iov_base = line;
	iov.iov_len = sizeof(line) - 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buf;
	msg.msg_controllen = sizeof(control.buf);

	while ((n = recvmsg(fd, &msg, 0)) < 0 && errno == EINTR)
		;
	if (n < 0)
		return 1;                   /* errno is the caller's answer */
	if (n == 0) {
		errno = ECONNABORTED;       /* the node went away */
		return 1;
	}
	line[n] = '\0';
	for (cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
			memcpy(&newfd, CMSG_DATA(cm), sizeof(newfd));
	if (newfd < 0) {
		errno = EPROTO;             /* a line without a descriptor */
		return 1;
	}
	if (getenv("AXSOCK_DEBUG"))
		fprintf(stderr, "wampes: accept %s", line);

	/* "<port> <src>[,<digi>...] > <dst>" - take the second word. */
	if (addr != NULL && addrlen != NULL &&
	    *addrlen >= (socklen_t) sizeof(struct sockaddr_ax25)) {
		struct full_sockaddr_ax25 fsa;

		memset(&fsa, 0, sizeof(fsa));
		if ((sp = strchr(line, ' ')) != NULL) {
			char *end = strchr(++sp, ' ');

			if (end != NULL) *end = '\0';
			parse_call(sp, &fsa, *addrlen);
		}
		if (*addrlen > (socklen_t) sizeof(fsa))
			*addrlen = sizeof(fsa);
		memcpy(addr, &fsa, *addrlen);
	}
	*ret = newfd;
	return 1;
}

/*---------------------------------------------------------------------------*/

/* The channel parameters belong to the node's interface configuration, so
 * this has nothing to set - but it must not fail.  A program that checks the
 * return value would give up on being told its window size was refused, and
 * that was the agreed behaviour before any of this: answer success, change
 * nothing.  Only descriptors that are ours are answered for.
 */

int wampes_setsockopt(int fd, int level, int *ret)
{
	if (level != SOL_AX25 || find_sock(fd) == NULL)
		return 0;
	*ret = 0;
	return 1;
}

/*---------------------------------------------------------------------------*/

int wampes_close(int fd)
{
	if (find_sock(fd) == NULL)
		return 0;
	drop_sock(fd);
	return 0;
}
