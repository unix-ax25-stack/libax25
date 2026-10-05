#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <netax25/ax25.h>
#include <netax25/axconfig.h>
#include <netax25/axlib.h>
#include <netrose/rose.h>

#include "pathnames.h"
#include "util.h"
#include "axsock_real.h"

typedef struct _axport
{
	struct _axport *Next;
	char *Name;
	char *Call;
	char *Device;
	int  Baud;
	int  Window;
	int  Paclen;
	char *Description;
	int  Kernel;		/* the callsign is an interface that is up */
} AX_Port;

/* A list that a reload replaced, kept until the cap below frees it.
 *
 * The getters hand out pointers into the list (a name, a callsign) and the
 * shim walks it from its reader thread, so a reload that freed the old list
 * left a walk reading freed memory - and left ax25_config_get_next() reading
 * a list that no longer had the entries it started with.  Retiring instead of
 * freeing costs one list per reload and makes both impossible: whoever holds
 * the old head walks a list that is still whole, whoever reads the head after
 * the swap gets the new one.  No lock is needed for that, and taking one in
 * the getters would not have helped: a walk is a sequence of calls, not one.
 */
struct ax25_ports_gen {
	struct ax25_ports_gen *Next;
	AX_Port *head;
};

/* Enough that a program which reloads in a loop cannot grow without bound.
 * Reaching it means 32 reloads inside one traversal, and a traversal is a
 * handful of pointer hops - so the backstop costs nothing and does not
 * narrow the guarantee in practice. */
#define AX25_PORTS_GENERATIONS 32

static AX_Port *ax25_ports;
static AX_Port *ax25_port_tail;
static struct ax25_ports_gen *ax25_ports_retired;
static int ax25_ports_generations;
static int ax25_ports_count;
static time_t ax25_ports_mtime;
static off_t ax25_ports_size;

/* No lock for the readers, and taking one in the getters would not have
 * helped: a walk is a sequence of calls, not one.  This one only keeps two
 * reloads from interleaving their builds, which the retire list already
 * survives anyway. */
static pthread_mutex_t ax25_ports_lock = PTHREAD_MUTEX_INITIALIZER;
#define AX25_PORTS_LOCK()	pthread_mutex_lock(&ax25_ports_lock)
#define AX25_PORTS_UNLOCK()	pthread_mutex_unlock(&ax25_ports_lock)

static void ax25_ports_free(AX_Port *p)
{
	AX_Port *n;

	while (p != NULL) {
		n = p->Next;
		free(p->Name);
		free(p->Call);
		free(p->Device);
		free(p->Description);
		free(p);
		p = n;
	}
}

static void ax25_ports_retire(AX_Port *head)
{
	struct ax25_ports_gen *g, **last;

	if (head == NULL)
		return;

	while (ax25_ports_generations >= AX25_PORTS_GENERATIONS) {
		g = ax25_ports_retired;
		if (g == NULL)
			break;
		ax25_ports_retired = g->Next;
		ax25_ports_free(g->head);
		free(g);
		ax25_ports_generations--;
	}

	if ((g = calloc(1, sizeof(*g))) == NULL) {
		/* Nowhere to park it.  A program that reloads often enough
		 * to get here has a list of a few hundred bytes per
		 * reload, and the alternative to leaking is the bug this
		 * arrangement exists to remove. */
		return;
	}
	g->head = head;

	last = &ax25_ports_retired;
	while (*last != NULL)
		last = &(*last)->Next;
	*last = g;
	ax25_ports_generations++;
}

static int is_same_call(char *call1, char *call2)
{
	if (!call1 || !call2)
		return 0;
	for (; *call1 && *call2; call1++, call2++) {
		if (*call1 == '-' || *call2 == '-')
			break;
		if (tolower(*call1 & 0xff) != tolower(*call2 & 0xff))
			return 0;
	}
	if (!*call1 && !*call2)
		return 1;
	if (!*call1 && !strcmp(call2, "-0"))
		return 1;
	if (!*call2 && !strcmp(call1, "-0"))
		return 1;
	return !strcmp(call1, call2) ? 1 : 0;
}

/* Asked when a "base:suffix" name has no entry of its own.  A backend whose
 * ports carry a suffix it resolves itself may answer yes for its own base
 * names, and the entry for the base is then used.  Nothing here knows which
 * backend that is: the backend installs the hook.
 *
 * It is deliberately not the default.  Where a suffix selects something -
 * the AGWPE channel, say - falling back to the base would quietly use the
 * wrong one, which is worse than refusing the name.
 */

int (*ax25_config_lazy_hook)(const char *name, const char *base);

/* A "base:suffix" name (e.g. "wampes:xnet") has no axports entry of its
 * own: ax25_port_ptr() resolves it to the base entry and the bind() that
 * follows carries only that base entry's callsign, so the suffix would be
 * lost.  It is remembered here, against the callsign it resolved to, and a
 * backend consumes it when the bind actually happens - see
 * ax25_config_lazy_take().
 *
 * A FIFO and not a map: every "base:suffix" of the same base resolves to
 * the SAME callsign, so the callsign alone cannot tell two waiting names
 * apart.  Keeping the order of resolution lets a program that resolves
 * two interfaces up front and then opens them the same way round get each
 * one back.  Bounded so that a name merely resolved and never bound
 * cannot pin a later, unrelated bind forever; when it is full the newest
 * is simply not remembered, and that connect takes the default path (the
 * node routes) rather than pinning the wrong interface.
 *
 * Depth 8 = the largest holder in the suite, axprobe(1) with MULTI_MAX
 * sessions, can legitimately keep unresolved before its binds; every other
 * program holds at most one.  Deeper would only widen the stray-pin window,
 * so 8 is the largest that remains safe.
 */
#define AX25_LAZY_MEMORY	8

static struct {
	char call[16];
	char name[39];
} Lazy_mem[AX25_LAZY_MEMORY];
static int Lazy_n;

int ax25_config_lazy_remember(const char *call, const char *name)
{
	if (call == NULL || name == NULL || *call == '\0')
		return -1;
	if (Lazy_n >= (int) (sizeof(Lazy_mem) / sizeof(Lazy_mem[0])))
		return -1;
	strncpy(Lazy_mem[Lazy_n].call, call, sizeof(Lazy_mem[0].call) - 1);
	Lazy_mem[Lazy_n].call[sizeof(Lazy_mem[0].call) - 1] = '\0';
	strncpy(Lazy_mem[Lazy_n].name, name, sizeof(Lazy_mem[0].name) - 1);
	Lazy_mem[Lazy_n].name[sizeof(Lazy_mem[0].name) - 1] = '\0';
	Lazy_n++;
	return 0;
}

/* Take the oldest remembered name for this callsign - the resolution that
 * predates the other ones, which is the one the bind with this callsign
 * belongs to in the pattern real tools follow (resolve, then bind at
 * once).  Consumed either way: an entry that is never re-found cannot
 * pin a later, unrelated bind.
 */
int ax25_config_lazy_take(const char *call, char *name, size_t namelen)
{
	int i;

	if (call == NULL)
		return -1;
	for (i = 0; i < Lazy_n; i++)
		if (Lazy_mem[i].call[0] != '\0' &&
		    strcasecmp(Lazy_mem[i].call, call) == 0) {
			if (name != NULL && namelen > 0) {
				strncpy(name, Lazy_mem[i].name, namelen - 1);
				name[namelen - 1] = '\0';
			}
			for (; i < Lazy_n - 1; i++)
				Lazy_mem[i] = Lazy_mem[i + 1];
			Lazy_n--;
			return 0;
		}
	return -1;
}

static AX_Port *ax25_port_ptr(char *name)
{
	AX_Port *p = ax25_ports;
	char base[64];
	char *colon;

	if (name == NULL)
		return p;

	while (p != NULL) {
		if (p->Name != NULL && strcasecmp(p->Name, name) == 0)
			return p;
		if (p->Call != NULL && is_same_call(p->Call, name))
			return p;
		p = p->Next;
	}

	if (ax25_config_lazy_hook == NULL)
		return NULL;
	if ((colon = strchr(name, ':')) == NULL || colon == name)
		return NULL;
	if ((size_t) (colon - name) >= sizeof(base))
		return NULL;
	memcpy(base, name, colon - name);
	base[colon - name] = '\0';
	if (!(*ax25_config_lazy_hook)(name, base))
		return NULL;
	for (p = ax25_ports; p != NULL; p = p->Next)
		if (p->Name != NULL && strcasecmp(p->Name, base) == 0) {
			/* The full name is what the bind() that follows would
			 * otherwise lose: the calling function only hands the
			 * base entry's callsign onwards.  Remember it before
			 * answering - see ax25_config_lazy_remember(). */
			ax25_config_lazy_remember(p->Call, name);
			return p;
		}
	return NULL;
}

char *ax25_config_get_next(char *name)
{
	AX_Port *p;

	if (ax25_ports == NULL)
		return NULL;

	if (name == NULL)
		return ax25_ports->Name;

	if ((p = ax25_port_ptr(name)) == NULL)
		return NULL;

	p = p->Next;

	if (p == NULL)
		return NULL;

	return p->Name;
}

char *ax25_config_get_name(char *device)
{
	AX_Port *p = ax25_ports;

	while (p != NULL) {
		if (p->Device != NULL) {
			if (strcmp(p->Device, device) == 0)
				return p->Name;
		}
		p = p->Next;
	}

	return NULL;
}

char *ax25_config_get_addr(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return NULL;

	return p->Call;
}

char *ax25_config_get_dev(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return NULL;

	return p->Device;
}

char *ax25_config_get_port(ax25_address *callsign)
{
	AX_Port *p = ax25_ports;
	ax25_address addr;

	if (ax25_cmp(callsign, &null_ax25_address) == 0)
		return "*";

	while (p != NULL) {
		if (p->Call != NULL) {
			ax25_aton_entry(p->Call, (char *)&addr);

			if (ax25_cmp(callsign, &addr) == 0)
				return p->Name;

		}
		p = p->Next;
	}

	return NULL;
}

int ax25_config_get_window(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return 0;

	return p->Window;
}

int ax25_config_get_paclen(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return 0;

	return p->Paclen;
}

int ax25_config_get_baud(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return 0;

	return p->Baud;
}

/*
 * Is this the name of a port the kernel answers for?
 *
 * The load is where the verdict is reached, because that is where the AX.25
 * address of every interface that is up is in hand and can be compared with
 * the callsign in the file; afterwards nothing here can tell the two kinds
 * apart again.  Not from the device name, either, which is the trap: the
 * commonest axports line names a port exactly like the interface behind it
 * ("bpq0  TE1ST-1 ..." with an interface bpq0 that is up), so the device of
 * a kernel port and the name of a userspace one are the same string in the
 * one case that is easiest to get wrong.  The answer is remembered instead of
 * rederived.
 *
 * The backends ask this to keep their hands off the kernel's ports.  A bind
 * that names a kernel port has already been answered by socket() and belongs
 * to the kernel, and taking it over would put frames on a radio that the
 * process never asked for.
 */
int ax25_config_port_is_kernel(const char *name)
{
	AX_Port *p;

	if (name == NULL || ax25_ports == NULL)
		return FALSE;

	for (p = ax25_ports; p != NULL; p = p->Next)
		if (p->Name != NULL && strcasecmp(name, p->Name) == 0)
			return p->Kernel;

	return FALSE;
}

/*
 * How many ports are the kernel's, that is: how many axports entries name a
 * callsign that is an AX.25 interface and that interface is up.
 *
 * Whether a raw monitor has a kernel side at all, and nothing narrower than
 * that.  A packet socket can be opened whether or not there is anything to
 * hear on it, so on a host whose AX.25 ports all belong to ax25netd - the
 * usual shape of a userspace machine - the answer is zero and there is no
 * second source of frames to read from.  A host with no axports entry for its
 * kernel port would answer zero too, which costs the monitor that port: the
 * kernel still has the frames, but nothing here says so.
 */
int ax25_config_kernel_ports(void)
{
	AX_Port *p;
	int n = 0;

	if (ax25_ports == NULL)
		return 0;

	for (p = ax25_ports; p != NULL; p = p->Next)
		if (p->Kernel)
			n++;

	return n;
}

/*
 * Which port a bind address names, as the name in axports.
 *
 * This used to be wampes.c's port_of_bind(), and having a second copy of it
 * is what made an AGWPE port unusable on a dualstack host: the copy here
 * resolved the callsign to a name, the copy there compared the callsign
 * itself against the server's port table, never matched, and answered no.
 * The two disagreed about the same bind and the port was left to the kernel,
 * which has no such port.  One lookup, here, where the port list and the lazy
 * map it has to consult both live.
 *
 * The port is named by the callsign in the first digipeater slot - but only
 * by programs that put it there.  call(1) always does; beacon(8) does it only
 * when its -c differs from the port's own callsign, and binds the bare
 * callsign otherwise.  A socket bound that way could not be recognised as a
 * WAMPES one and stayed with AGWPE without a word, which is a quiet way to
 * send a beacon nowhere.  So when there is no digipeater, ask the source
 * callsign instead: it resolves only if it is a port's callsign, and a
 * user's own callsign has no entry and answers nothing, which is the right
 * outcome for it.
 *
 * Only reads it, but says otherwise in the header.
 *
 * Returns 0 and the name, or 1 with an empty name.
 */
int ax25_config_bind_port(const struct sockaddr *addr, socklen_t len,
			  char *port, size_t portlen)
{
	const struct full_sockaddr_ax25 *fsa =
		(const struct full_sockaddr_ax25 *) addr;
	ax25_address *which;
	char *name;

	if (port == NULL || portlen == 0)
		return 1;
	*port = '\0';
	if (len < (socklen_t) sizeof(*fsa))
		return 1;

	which = fsa->fsa_ax25.sax25_ndigis > 0
		? (ax25_address *) &fsa->fsa_digipeater[0]
		: (ax25_address *) &fsa->fsa_ax25.sax25_call;

	/* Before the reverse lookup: a "wampes:xnet" resolved through the lazy
	 * hook has told axconfig.c the intended name against this callsign,
	 * and what the lookup could answer with - the base entry "wampes" -
	 * would throw the suffix away and let the node route.  Consumed here,
	 * so the node gets the interface prefix; a bind that resolved no name
	 * finds nothing and goes on below.
	 */
	if (ax25_config_lazy_take(ax25_ntoa(which), port, portlen) == 0)
		return 0;

	name = ax25_config_get_port(which);
	if (name == NULL && ax25_config_get_next(NULL) == NULL) {
		/* The port table belongs to the application: every program in
		 * the suite calls ax25_config_load_ports() at startup, and a
		 * program that only had the library preloaded calls nothing at
		 * all.  With an empty table the port cannot be named, so the
		 * socket went to AGWPE without a word - and then righted itself
		 * on the next bind, because the AGWPE path loads the table as a
		 * side effect.  Load it here, once, rather leave the backend to
		 * depend on the order of the binds.  Ensure() rather than the
		 * unconditional reload: this is the first half of the bind's
		 * port lookup, and the second half asks the same question and
		 * has to get the same answer.
		 */
		ax25_config_ports_ensure();
		name = ax25_config_get_port(which);
	}
	if (name == NULL)
		return 1;

	/* A name that does not fit is no answer rather than a wrong one.  The
	 * backends compare what comes back against whole names, and a name cut
	 * short matches none of them - which for the AGWPE backend would read
	 * as "not the kernel's" and claim a port that is. */
	if (strlen(name) >= portlen)
		return 1;

	strncpy(port, name, portlen - 1);
	port[portlen - 1] = '\0';
	return 0;
}

char *ax25_config_get_desc(char *name)
{
	AX_Port *p = ax25_port_ptr(name);

	if (p == NULL)
		return NULL;

	return p->Description;
}

static int ax25_config_init_port(int fd, int lineno, char *line, const char **ifcalls, const char **ifdevs)
{
	AX_Port *p;
	char *name, *call, *baud, *paclen, *window, *desc;
	const char *dev = NULL;
	int kernel = FALSE;

	name   = strtok(line, " \t");
	call   = strtok(NULL, " \t");
	baud   = strtok(NULL, " \t");
	paclen = strtok(NULL, " \t");
	window = strtok(NULL, " \t");
	desc   = strtok(NULL, "");

	if (name == NULL   || call == NULL   || baud == NULL ||
	    paclen == NULL || window == NULL || desc == NULL) {
		fprintf(stderr, "axconfig: unable to parse line %d of axports file\n", lineno);
		return FALSE;
	}

	for (p = ax25_ports; p != NULL; p = p->Next) {
		if (p->Name != NULL && strcasecmp(name, p->Name) == 0) {
			fprintf(stderr, "axconfig: duplicate port name %s in line %d of axports file\n", name, lineno);
			return FALSE;
		}
		if (p->Call != NULL && is_same_call(call, p->Call)) {
			fprintf(stderr, "axconfig: duplicate callsign %s in line %d of axports file\n", call, lineno);
			return FALSE;
		}
	}

	if (atoi(baud) < 0) {
		fprintf(stderr, "axconfig: invalid baud rate setting %s in line %d of axports file\n", baud, lineno);
		return FALSE;
	}

	if (atoi(paclen) <= 0) {
		fprintf(stderr, "axconfig: invalid packet size setting %s in line %d of axports file\n", paclen, lineno);
		return FALSE;
	}

	if (atoi(window) <= 0) {
		fprintf(stderr, "axconfig: invalid window size setting %s in line %d of axports file\n", window, lineno);
		return FALSE;
	}

	strupr(call);
	char *cp;
	if ((cp = strstr(call, "-0")) != NULL)
		*cp = '\0';

	/*
	 * What a port is, and the only question here worth answering.
	 *
	 * A kernel port is one whose callsign is the AX.25 address of an
	 * interface that is up.  The device name recorded for it is what the
	 * ioctls behind axparms and ifconfig need, and it is the only kind
	 * of port that names a device.
	 *
	 * A port of a node or of an AGWPE server is served without any
	 * interface existing, so nothing here can recognise it: the node's
	 * answer is in wampes.conf, but a server's is in its port table,
	 * which is reached over TCP and may be on the other side of a LAN -
	 * or of the world.  Asking for it would put a network round trip,
	 * and its three second timeout on netd's side, into the startup of
	 * every AX.25 program on the machine, axparms and ifconfig
	 * included, to learn one bit per entry that the bind is about to ask
	 * anyway - and to learn it less well, since a server that is merely
	 * slow or unreachable at that moment would have the port dropped.
	 *
	 * So the entry is kept, its callsign - which is right here in the
	 * file - and the verdict is left to the bind, where the backend that
	 * can answer is asked and a port that nothing serves is refused by
	 * whoever is left: the kernel, with EADDRNOTAVAIL, which names the
	 * callsign that has no interface.  Which is the truth about a
	 * mistyped port, where "invalid port setting" was a guess made
	 * before anybody had looked.
	 */
	for (;ifcalls && *ifcalls; ++ifcalls, ++ifdevs) {
		if (strcmp(call, *ifcalls) == 0) {
			dev = *ifdevs;
			kernel = TRUE;
			break;
		}
	}
	if (dev == NULL) {
		/* Not a kernel port.  The name is the best device there is,
		 * and it is what the backends and the monitor match on. */
		dev = name;
		if (axsock_debug && ifcalls != NULL)
			fprintf(stderr, "axconfig: port '%s' (call '%s') is not "
				"an AX.25 interface that is up, so it is served "
				"by a userspace backend if any\n", name, call);
	}

	if ((p = (AX_Port *)malloc(sizeof(AX_Port))) == NULL) {
		fprintf(stderr, "axconfig: out of memory!\n");
		return FALSE;
	}

	p->Name        = strdup(name);
	p->Call        = strdup(call);
	p->Device      = strdup(dev);
	p->Baud        = atoi(baud);
	p->Window      = atoi(window);
	p->Paclen      = atoi(paclen);
	p->Description = strdup(desc);
	p->Kernel      = kernel;

	if (ax25_ports == NULL)
		ax25_ports = p;
	else
		ax25_port_tail->Next = p;

	ax25_port_tail = p;

	p->Next = NULL;

	return TRUE;
}

const char *ax25_config_ports_file(void)
{
	return CONF_AXPORTS_FILE;
}

/* Read axports and build the table.  The caller holds ax25_ports_lock. */
static int ax25_config_reload(void)
{
	FILE *fp = NULL;
	char buffer[256], *s;
	int fd = -1, lineno = 1, n = 0, i;
	const char **calllist = NULL;
	const char **devlist  = NULL;
#ifdef __linux__
	const char **pp;
	int callcount = 0;
#endif

	/*
	 * Reloads must start from an empty list, otherwise a second
	 * ax25_config_load_ports() would report every port as a
	 * duplicate.  Several programs (and the userspace AF_AX25
	 * shim) legitimately call this more than once.
	 *
	 * Empty head, not freed list.  The getters hand out pointers into
	 * the table and the shim walks it from its reader thread while the
	 * application binds, so freeing it here left that walk on freed
	 * memory - and, when the walk happened to land in the middle of a
	 * reload, a bind that resolved a port a moment ago refusing it
	 * with the message that no backend serves it.  See ax25_ports_retire().
	 */
	ax25_ports_retire(ax25_ports);
	ax25_ports = NULL;
	ax25_port_tail = NULL;
	ax25_ports_count = 0;
	ax25_ports_mtime = 0;
	ax25_ports_size = 0;

#ifdef __linux__
	/* Reliable listing of all network ports on Linux
	   is only available via reading  /proc/net/dev ...  */

	struct ifreq ifr;

	if ((fd = socket(PF_FILE, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "axconfig: unable to open socket (%s)\n", strerror(errno));
		goto cleanup;
	}

	if ((fp = fopen("/proc/net/dev", "r"))) {
		/* Two header lines.. */
		s = fgets(buffer, sizeof(buffer), fp);
		s = fgets(buffer, sizeof(buffer), fp);
		/* .. then network interface names */
		while (!feof(fp)) {
			if (!fgets(buffer, sizeof(buffer), fp))
				break;
			s = strchr(buffer, ':');
			if (s) *s = 0;
			s = buffer;
			while (isspace(*s & 0xff)) ++s;

			memset(&ifr, 0, sizeof(ifr));
			if (strlen(s) >= IFNAMSIZ) {
				fprintf(stderr, "axconfig: interface name too long\n");
				continue;
			}
			strcpy(ifr.ifr_name, s);

			/* One interface that cannot be asked about is that
			 * interface being gone or half up - an ax25 module
			 * being loaded or unloaded is the normal way to get
			 * here, and it is no reason to throw away axports:
			 * every backend's answer about every port comes out
			 * of this file.  Skipping the one keeps a
			 * momentary ENODEV from emptying the table under a
			 * bind that is asking right now. */
			if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
				fprintf(stderr, "axconfig: %s: SIOCGIFHWADDR: %s\n",
					s, strerror(errno));
				continue;
			}

			if (ifr.ifr_hwaddr.sa_family != ARPHRD_AX25)
				continue;

			/* store found interface callsigns */
			/* ax25_ntoa() returns pointer to static buffer */
			s = ax25_ntoa((void*)ifr.ifr_hwaddr.sa_data);

			if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
				fprintf(stderr, "axconfig: %s: SIOCGIFFLAGS: %s\n",
					s, strerror(errno));
				continue;
			}

			if (!(ifr.ifr_flags & IFF_UP))
				continue;

			if ((pp = realloc(calllist, sizeof(char *) * (callcount+2))) == 0)
				break;
			calllist = pp;
			if ((pp = realloc(devlist,  sizeof(char *) * (callcount+2))) == 0)
			break;
			devlist  = pp;
			if ((calllist[callcount] = strdup(s)) != NULL) {
				if ((devlist[callcount] = strdup(ifr.ifr_name)) != NULL) {
					++callcount;
					calllist[callcount] = NULL;
					devlist [callcount] = NULL;
				} else {
					free((void*)calllist[callcount]);
					calllist[callcount] = NULL;
				}
			}
		}
		fclose(fp);
		fp = NULL;
	}
#endif /* __linux__ */


	if ((fp = fopen(CONF_AXPORTS_FILE, "r")) == NULL) {
		fprintf(stderr, "axconfig: unable to open axports file %s (%s)\n", CONF_AXPORTS_FILE, strerror(errno));
		goto cleanup;
	}

	while (fp && fgets(buffer, 255, fp)) {
		if ((s = strchr(buffer, '\n')))
			*s = '\0';

		if (strlen(buffer) > 0 && *buffer != '#')
			if (ax25_config_init_port(fd, lineno, buffer, calllist, devlist))
				n++;

		lineno++;
	}

 cleanup:;
	if (fd >= 0) close(fd);
	if (fp) fclose(fp);

	for (i = 0; calllist && calllist[i]; ++i) {
		free((void*)calllist[i]);
		if (devlist[i] != NULL)
			free((void*)devlist[i]);
	}
	if (calllist) free(calllist);
	if (devlist) free(devlist);

	if (ax25_ports == NULL) {
		ax25_ports_count = 0;
		return 0;
	}

	/* What ax25_config_ports_ensure() compares against to decide that
	 * the table is still the file.  Taken after the read, so an editor
	 * that rewrites axports between the two does not make a half-written
	 * file look current. */
	{
		struct stat st;

		if (stat(CONF_AXPORTS_FILE, &st) == 0) {
			ax25_ports_mtime = st.st_mtime;
			ax25_ports_size = st.st_size;
		}
	}

	ax25_ports_count = n;

	return n;
}

/*
 * Read axports now, whether or not it has changed.
 *
 * What every program in the suite calls at startup and what
 * axparms(8) calls to re-read a file the operator has edited - so this is a
 * forced reload, and it stays one.
 */
int ax25_config_load_ports(void)
{
	int n;

	AX25_PORTS_LOCK();
	n = ax25_config_reload();
	AX25_PORTS_UNLOCK();

	return n;
}

/*
 * Read axports unless the file is unchanged since the last read.
 *
 * For the callers inside the library that go looking for a port while the
 * application is doing something else.  They used to reload unconditionally,
 * which cost a file read, an interface scan and a rebuilt list per lookup -
 * and three lookups per bind - while the answer could only differ if the
 * operator had edited axports in the meantime.  Comparing mtime and size
 * makes the common case the cheap one and still notices an edit.
 *
 * A file that cannot be stat()ed now keeps the table that is already here:
 * an unreadable axports is no reason to forget the ports that were read from
 * it while it could be read.
 */
int ax25_config_ports_ensure(void)
{
	struct stat st;
	int n;

	AX25_PORTS_LOCK();

	if (ax25_ports != NULL) {
		if (stat(CONF_AXPORTS_FILE, &st) != 0)
			n = ax25_ports_count;
		else if (st.st_mtime == ax25_ports_mtime && st.st_size == ax25_ports_size)
			n = ax25_ports_count;
		else
			n = ax25_config_reload();
	} else {
		n = ax25_config_reload();
	}

	AX25_PORTS_UNLOCK();

	return n;
}
