#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <config.h>

#include <sys/types.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <netax25/ax25.h>
#include <netax25/axconfig.h>
#include <netax25/axlib.h>
#include <netrose/rose.h>

#include "pathnames.h"
#include "util.h"

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
} AX_Port;

static AX_Port *ax25_ports;
static AX_Port *ax25_port_tail;

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
	int found;

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
	found = 0;
	char *cp;
	if ((cp = strstr(call, "-0")) != NULL)
		*cp = '\0';

	if (ifcalls == NULL) {
		/*
		 * No kernel interface list is available because there is
		 * no kernel AX.25 support (BSD, SysV, macOS, ...).  Accept
		 * all configured ports and use the port name as device.
		 */
		found = 1;
		dev = name;
	} else {
		for (;ifcalls && *ifcalls; ++ifcalls, ++ifdevs) {
			if (strcmp(call, *ifcalls) == 0) {
				found = 1;
				dev = *ifdevs;
				break;
			}
		}
	}

	if (!found) {
#if 0 /* None of your business to complain about some port being down... */
		fprintf(stderr, "axconfig: port with call '%s' is not active\n", call);
#endif
		return FALSE;
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

	if (ax25_ports == NULL)
		ax25_ports = p;
	else
		ax25_port_tail->Next = p;

	ax25_port_tail = p;

	p->Next = NULL;

	return TRUE;
}

int ax25_config_load_ports(void)
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
	 */
	while (ax25_ports != NULL) {
		AX_Port *p = ax25_ports->Next;

		free(ax25_ports->Name);
		free(ax25_ports->Call);
		free(ax25_ports->Device);
		free(ax25_ports->Description);
		free(ax25_ports);
		ax25_ports = p;
	}
	ax25_port_tail = NULL;

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
				unreachable();
			}
			strcpy(ifr.ifr_name, s);

			if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
				fprintf(stderr, "axconfig: SIOCGIFHWADDR: %s\n", strerror(errno));
				return FALSE;
			}

			if (ifr.ifr_hwaddr.sa_family != ARPHRD_AX25)
				continue;

			/* store found interface callsigns */
			/* ax25_ntoa() returns pointer to static buffer */
			s = ax25_ntoa((void*)ifr.ifr_hwaddr.sa_data);

			if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
				fprintf(stderr, "axconfig: SIOCGIFFLAGS: %s\n", strerror(errno));
				return FALSE;
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

	if (ax25_ports == NULL)
		return 0;

	return n;
}
