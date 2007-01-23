#define _LINUX_STRING_H_

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
#ifdef HAVE_NETAX25_AX25_H
#include <netax25/ax25.h>
#else
#include "kernel_ax25.h"
#endif
#ifdef HAVE_NETROSE_ROSE_H
#include <netrose/rose.h>
#else
#include "kernel_rose.h"
#endif
#include "axlib.h"
#include "pathnames.h"
#include "axconfig.h"

#define _PATH_PROCNET_DEV           "/proc/net/dev"

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

static AX_Port *ax25_ports     = NULL;
static AX_Port *ax25_port_tail = NULL;

typedef struct _axiface
{
	struct _axiface *Next;
	char *Name;
	char *Call;
	char *Device;
} AX_Iface;

static AX_Iface *ax25_ifaces     = NULL;

static int ax25_hw_cmp(char *callsign, char *hw_addr)
{
	ax25_address call;

	ax25_aton_entry(callsign, call.ax25_call);
	
	return ax25_cmp(&call, (ax25_address *)hw_addr) == 0;
}

static AX_Port *ax25_port_ptr(char *name)
{
	AX_Port *p = ax25_ports;

	if (name == NULL)
		return p;

	while (p != NULL) {
		if (strcasecmp(p->Name, name) == 0)
			return p;

		p = p->Next;
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
		if (strcmp(p->Device, device) == 0)
			return p->Name;

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
		ax25_aton_entry(p->Call, (char *)&addr);

		if (ax25_cmp(callsign, &addr) == 0)
			return p->Name;

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

static void free_ax25_ports() {
	AX_Port *axp;
	for (axp = ax25_ports; axp; ) {
		AX_Port *q = axp->Next;
		if (axp->Name) free(axp->Name);
		if (axp->Call) free(axp->Call);
		if (axp->Device) free(axp->Device);
		if (axp->Description) free(axp->Description);
		free(axp);
		axp = q;
	}
	ax25_ports = ax25_port_tail = NULL;
}

static void free_ax25_ifaces() {
	AX_Iface *axif;
	for (axif = ax25_ifaces; axif; ) {
		AX_Iface *q = axif->Next;
		if (axif->Name) free(axif->Name);
		if (axif->Device) free(axif->Device);
		if (axif->Call) free(axif->Call);
		free(axif);
		axif = q;
	}
	ax25_ifaces = NULL;
}

static int test_and_add_ax25_iface(int fd, char *name, struct ifreq *ifr) {
	AX_Iface *axif;

	if (fd == -1)
		return 0;
	if (!name)
		return 0;
	if (!strcmp(name, "lo")) return 0;
	if (!ifr)
		return 0;

	strncpy(ifr->ifr_name, name, IFNAMSIZ-1);
	ifr->ifr_name[IFNAMSIZ-1] = 0;

	if (ioctl(fd, SIOCGIFFLAGS, ifr) < 0) {
		fprintf(stderr, "axconfig: SIOCGIFFLAGS: %s\n", strerror(errno));
		return -1;
	}

	if (!(ifr->ifr_flags & IFF_UP)) return 0;

	if (ioctl(fd, SIOCGIFHWADDR, ifr) < 0) {
		fprintf(stderr, "axconfig: SIOCGIFHWADDR: %s\n", strerror(errno));
		return -1;
	}

	if (ifr->ifr_hwaddr.sa_family != ARPHRD_AX25) return 0;

	if ((axif = (AX_Iface *)malloc(sizeof(AX_Iface))) == NULL) {
		fprintf(stderr, "axconfig: out of memory!\n");
		return -1;
	}
	if (!(axif->Name = strdup(name))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(axif);
		return -1;
	}
	if (!(axif->Device = strdup(ifr->ifr_name))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(axif->Name);
		free(axif);
		return -1;
	}
	if (!(axif->Call = strdup(ifr->ifr_hwaddr.sa_data))) {
		free(axif->Name);
		free(axif->Device);
		free(axif);
		fprintf(stderr, "axconfig: out of memory!\n");
		return -1;
	}
	axif->Next = ax25_ifaces;
	ax25_ifaces = axif;

	return 1;
}

#ifdef	FIND_ALL_INTERFACES
static char *proc_get_iface_name(char *line) {
	char *p;

	if (!(p = strchr(line, ':')))
		return 0;
	*p = 0;
	while (*line && isspace(*line & 0xff))
		line++;
	if (!*line)
		return 0;
	return line;
}
#endif

static int get_ax25_ifaces(void) {
#ifdef	FIND_ALL_INTERFACES
	FILE *fp;
#endif
	struct ifreq ifr;
	int fd = -1;
	int ret = -1;

	free_ax25_ifaces();
	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "axconfig: unable to open socket (%s)\n", strerror(errno));
		goto out;
	}

#ifdef	FIND_ALL_INTERFACES /* will be default soon, after tracing a kernel error */
	if ((fp = fopen(_PATH_PROCNET_DEV, "r"))) {
		/* Because ifc.ifc_req does not show interfaces without
		 * IP-Address assigned, we use the device list via /proc.
		 * This concept was inspired by net-tools / ifconfig
		 */
		char buf[512];
		int i = 0;
		ret = 0;
		while (fgets(buf, sizeof(buf), fp)) {
			/* skip proc header */
			if (i < 2) {
				i++;
				continue;
			}
			if (test_and_add_ax25_iface(fd, proc_get_iface_name(buf), &ifr) > 0)
				ret++;
		}
		fclose(fp);
	} else {
#else
	{
#endif
		struct ifconf ifc;
		struct ifreq *ifrp;
		char buffer[1024];
		int n = 0;
		ifc.ifc_len = sizeof(buffer);
		ifc.ifc_buf = buffer;
	
		if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) {
			fprintf(stderr, "axconfig: SIOCGIFCONF: %s\n", strerror(errno));
			goto out;
		}
		ret = 0;

		for (ifrp = ifc.ifc_req, n = ifc.ifc_len / sizeof(struct ifreq); --n >= 0; ifrp++)
			if (test_and_add_ax25_iface(fd, ifrp->ifr_name, &ifr) > 0)
				ret++;
	}

out:
	if (fd != -1)
		close(fd);
	return ret;
}

static int ax25_config_init_port(int lineno, char *line)
{
	AX_Port *p;
	AX_Iface *axif = NULL;
	char *name, *call, *baud, *paclen, *window, *desc, *dev = NULL;
	int found = 0;

	name   = strtok(line, " \t");
	call   = strtok(NULL, " \t");
	baud   = strtok(NULL, " \t");
	paclen = strtok(NULL, " \t");
	window = strtok(NULL, " \t");
	desc   = strtok(NULL, "");

	if (name == NULL   || call == NULL   || baud == NULL ||
	    paclen == NULL || window == NULL || desc == NULL) {
		fprintf(stderr, "axconfig: unable to parse line %d of axports file\n", lineno);
		return -1;
	}

	for (p = ax25_ports; p != NULL; p = p->Next) {
		if (strcasecmp(name, p->Name) == 0) {
			fprintf(stderr, "axconfig: duplicate port name %s in line %d of axports file\n", name, lineno);
			return -1;
		}
		/* dl9sau: why? */
		if (strcasecmp(call, p->Call) == 0) {
			fprintf(stderr, "axconfig: duplicate callsign %s in line %d of axports file\n", call, lineno);
			return -1;
		}
	}

	if (atoi(baud) < 0) {
		fprintf(stderr, "axconfig: invalid baud rate setting %s in line %d of axports file\n", baud, lineno);
		return -1;
	}

	if (atoi(paclen) <= 0) {
		fprintf(stderr, "axconfig: invalid packet size setting %s in line %d of axports file\n", paclen, lineno);
		return -1;
	}

	if (atoi(window) <= 0) {
		fprintf(stderr, "axconfig: invalid window size setting %s in line %d of axports file\n", window, lineno);
		return -1;
	}

	strupr(call);

	for (axif = ax25_ifaces; axif; axif = axif->Next) {
		if (ax25_hw_cmp(call, axif->Call)) {
			/* associate list of ifaces with the name from axports we just found */
			if (!strcmp(axif->Name, name)) {
				free(axif->Name);
				if (!(axif->Name = strdup(name))) {
					fprintf(stderr, "axconfig: out of memory!\n");
					return -1;
				}
			}
			dev = axif->Device;
			found = 1;
			break;
		}
	}

	if (!found) {
		fprintf(stderr, "axconfig: port %s not active\n", name);
		return -1;
	}

	if ((p = (AX_Port *)malloc(sizeof(AX_Port))) == NULL) {
		fprintf(stderr, "axconfig: out of memory!\n");
		return -1;
	}

	if (!(p->Name        = strdup(name))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(p);
		return -1;
	}
	if (!(p->Call        = strdup(call))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(p->Name);
		free(p);
		return -1;
	}
	if (!(p->Device      = strdup(dev))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(p->Name);
		free(p->Call);
		free(p);
		return -1;
	}
	if (!(p->Description = strdup(desc))) {
		fprintf(stderr, "axconfig: out of memory!\n");
		free(p->Name);
		free(p->Call);
		free(p->Description);
		free(p);
		return -1;
	}
	p->Baud        = atoi(baud);
	p->Window      = atoi(window);
	p->Paclen      = atoi(paclen);

	if (ax25_ports == NULL)
		ax25_ports = p;
	else
		ax25_port_tail->Next = p;

	ax25_port_tail = p;

	p->Next = NULL;

	return 0;
}

int ax25_config_load_ports(void)
{
	FILE *fp;
	char buffer[256], *s;
	int lineno = 1;
	int n = 0;

	/* in case of a "reload" */
	free_ax25_ports();

	if (get_ax25_ifaces() <= 0)
		goto out;

	if ((fp = fopen(CONF_AXPORTS_FILE, "r")) == NULL) {
		fprintf(stderr, "axconfig: unable to open axports file %s (%s)\n", CONF_AXPORTS_FILE, strerror(errno));
		goto out;
	}

	while (fgets(buffer, 255, fp) != NULL) {
		if ((s = strchr(buffer, '\n')) != NULL)
			*s = '\0';

		if (strlen(buffer) > 0 && *buffer != '#')
			if (!ax25_config_init_port(lineno, buffer))
				n++;

		lineno++;
	}

	fclose(fp);

	if (ax25_ports == NULL)
		n = 0;

out:
	free_ax25_ifaces();
	return n;
}
