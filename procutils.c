#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "pathnames.h"
#include "procutils.h"

/*
 * Version of atoi() that returns zero if s == NULL.
 */
static int safe_atoi(const char *s)
{
	return (s == NULL) ? 0 : atoi(s);
}

/*
 * Version of strncpy() that returns NULL if either src or dest is NULL
 * and also makes sure destination string is always terminated.
 */
static char *safe_strncpy(char *dest, char *src, int n)
{
	if (dest == NULL || src == NULL)
		return NULL;
	dest[n] = 0;
	return strncpy(dest, src, n);
}

struct proc_ax25 *read_proc_ax25(void)
{
	FILE *fp;
	char buffer[256], *cp;
	struct proc_ax25 *p;
	struct proc_ax25 *list = NULL;
	int i = 0;

	errno = 0;
	if ((fp = fopen(PROC_AX25_FILE, "r")) == NULL)
		return NULL;
	while (fgets(buffer, 256, fp) != NULL) {
		if (!i++) continue;
		if ((p = calloc(1, sizeof(struct proc_ax25))) == NULL)
			break;
		/* Some versions have '*', some don't */
		if (*buffer == ' ') {
			strcpy(p->dest_addr, "*");
			safe_strncpy(p->src_addr, strtok(buffer, " \t\n\r"), 9);
		} else {
			safe_strncpy(p->dest_addr, strtok(buffer, " \t\n\r"), 9);
			safe_strncpy(p->src_addr, strtok(NULL, " \t\n\r"), 9);
		}
		safe_strncpy(p->dev, strtok(NULL, " \t\n\r"), 13);
		p->st = safe_atoi(strtok(NULL, " \t\n\r"));
		p->vs = safe_atoi(strtok(NULL, " \t\n\r"));
		p->vr = safe_atoi(strtok(NULL, " \t\n\r"));
		p->va = safe_atoi(strtok(NULL, " \t\n\r"));
		cp = strtok(NULL, " \t\n\r");
		p->t1timer = safe_atoi(cp);
		if ((cp = strchr(cp, '/')) != NULL)
			p->t1 = safe_atoi(++cp);
		cp = strtok(NULL, " \t\n\r");
		p->t2timer = safe_atoi(cp);
		if ((cp = strchr(cp, '/')) != NULL)
			p->t2 = safe_atoi(++cp);
		cp = strtok(NULL, " \t\n\r");
		p->t3timer = safe_atoi(cp);
		if ((cp = strchr(cp, '/')) != NULL)
			p->t3 = safe_atoi(++cp);
		cp = strtok(NULL, " \t\n\r");
		p->idletimer = safe_atoi(cp);
		if ((cp = strchr(cp, '/')) != NULL)
			p->idle = safe_atoi(++cp);
		cp = strtok(NULL, " \t\n\r");
		p->n2count = safe_atoi(cp);
		if ((cp = strchr(cp, '/')) != NULL)
			p->n2 = safe_atoi(++cp);
		p->rtt = safe_atoi(strtok(NULL, " \t\n\r"));
		p->window = safe_atoi(strtok(NULL, " \t\n\r"));
		p->paclen = safe_atoi(strtok(NULL, " \t\n\r"));
		p->sndq = safe_atoi(strtok(NULL, " \t\n\r"));
		p->rcvq = safe_atoi(strtok(NULL, " \t\n\r"));
		p->next = list;
		list = p;
	}
	fclose(fp);
	return list;
}

void free_proc_ax25(struct proc_ax25 *ap)
{
	struct proc_ax25 *p;

	while (ap != NULL) {
		p = ap->next;
		free(ap);
		ap = p;
	}
}

struct proc_ax25_route *read_proc_ax25_route(void)
{
	FILE *fp;
	char buffer[256];
	struct proc_ax25_route *new, *tmp, *p;
	struct proc_ax25_route *list = NULL;
	int i = 0;

	errno = 0;
	if ((fp = fopen(PROC_AX25_ROUTE_FILE, "r")) == NULL)
		return NULL;
	while (fgets(buffer, 256, fp) != NULL) {
		if (!i++) continue;
		if ((new = calloc(1, sizeof(struct proc_ax25_route))) == NULL)
			break;
		safe_strncpy(new->call, strtok(buffer, " \t\n\r"), 9);
		safe_strncpy(new->dev, strtok(NULL, " \t\n\r"), 13);
		new->cnt  = safe_atoi(strtok(NULL, " \t\n\r"));
		new->t    = safe_atoi(strtok(NULL, " \t\n\r"));
		if (list == NULL || new->t > list->t) {
			tmp = list;
			list = new;
			new->next = tmp;
		} else {
			for (p = list; p->next != NULL; p = p->next)
				if (new->t > p->next->t)
					break;
			tmp = p->next;
			p->next = new;
			new->next = tmp;
		}
	}
	fclose(fp);
	return list;
}

void free_proc_ax25_route(struct proc_ax25_route *rp)
{
	struct proc_ax25_route *p;

	while (rp != NULL) {
		p = rp->next;
		free(rp);
		rp = p;
	}
}

struct proc_nr_neigh *read_proc_nr_neigh(void)
{
	FILE *fp;
	char buffer[256];
	struct proc_nr_neigh *p;
	struct proc_nr_neigh *list = NULL;
	int i = 0;

	errno = 0;
	if ((fp = fopen(PROC_NR_NEIGH_FILE, "r")) == NULL)
		return NULL;
	while (fgets(buffer, 256, fp) != NULL) {
		if (!i++) continue;
		if ((p = calloc(1, sizeof(struct proc_nr_neigh))) == NULL)
			break;
		p->addr = safe_atoi(strtok(buffer, " \t\n\r"));
		safe_strncpy(p->call, strtok(NULL, " \t\n\r"), 9);
		safe_strncpy(p->dev,  strtok(NULL, " \t\n\r"), 13);
		p->qual = safe_atoi(strtok(NULL, " \t\n\r"));
		p->lock = safe_atoi(strtok(NULL, " \t\n\r"));
		p->cnt  = safe_atoi(strtok(NULL, " \t\n\r"));
		p->next = list;
		list = p;
	}
	fclose(fp);
	return list;
}

void free_proc_nr_neigh(struct proc_nr_neigh *np)
{
	struct proc_nr_neigh *p;

	while (np != NULL) {
		p = np->next;
		free(np);
		np = p;
	}
}

struct proc_nr_nodes *read_proc_nr_nodes(void)
{
	FILE *fp;
	char buffer[256];
	struct proc_nr_nodes *new, *tmp, *p;
	struct proc_nr_nodes *list = NULL;
	char *cp;
	int i = 0;

	errno = 0;
	if ((fp = fopen(PROC_NR_NODES_FILE, "r")) == NULL)
		return NULL;
	while (fgets(buffer, 256, fp) != NULL) {
		if (!i++) continue;
		if ((new = calloc(1, sizeof(struct proc_nr_nodes))) == NULL)
			break;
		safe_strncpy(new->call, strtok(buffer, " \t\n\r"), 9);
		if ((cp = strchr(new->call, '-')) != NULL && *(cp + 1) == '0')
			*cp = 0;
		safe_strncpy(new->alias, strtok(NULL, " \t\n\r"), 6);
		new->w     = safe_atoi(strtok(NULL, " \t\n\r"));
		new->n     = safe_atoi(strtok(NULL, " \t\n\r"));
		new->qual1 = safe_atoi(strtok(NULL, " \t\n\r"));
		new->obs1  = safe_atoi(strtok(NULL, " \t\n\r"));
		new->addr1 = safe_atoi(strtok(NULL, " \t\n\r"));
		if (new->n > 1) {
			new->qual2 = safe_atoi(strtok(NULL, " \t\n\r"));
			new->obs2  = safe_atoi(strtok(NULL, " \t\n\r"));
			new->addr2 = safe_atoi(strtok(NULL, " \t\n\r"));
		}
		if (new->n > 2) {
			new->qual3 = safe_atoi(strtok(NULL, " \t\n\r"));
			new->obs3  = safe_atoi(strtok(NULL, " \t\n\r"));
			new->addr3 = safe_atoi(strtok(NULL, " \t\n\r"));
		}
		if (list == NULL || strcmp(new->alias, list->alias) < 0) {
			tmp = list;
			list = new;
			new->next = tmp;
		} else {
			for (p = list; p->next != NULL; p = p->next)
				if (strcmp(new->alias, p->next->alias) < 0)
					break;
			tmp = p->next;
			p->next = new;
			new->next = tmp;
		}
	}
	fclose(fp);
	return list;
}

void free_proc_nr_nodes(struct proc_nr_nodes *np)
{
	struct proc_nr_nodes *p;

	while (np != NULL) {
		p = np->next;
		free(np);
		np = p;
	}
}

char *get_call(int uid)
{
	FILE *fp;
	char buf[256];
	static char call[10];
	int i = 0;

	errno = 0;
	if ((fp = fopen(PROC_AX25_CALLS_FILE, "r")) == NULL)
		return NULL;
	while (fgets(buf, 256, fp) != NULL) {
		if (!i++) continue;
		if (safe_atoi(strtok(buf, " \t\r\n")) == uid) {
			fclose(fp);
			safe_strncpy(call, strtok(NULL, " \t\r\n"), 9);
			call[9] = 0;
			return call;
		}
	}
	fclose(fp);
	return NULL;
}

struct proc_ax25 *find_link(const char *src, const char *dest, const char *dev)
{
	static struct proc_ax25 a;
	struct proc_ax25 *p, *list;

	list = read_proc_ax25();
	for (p = list; p != NULL; p = p->next) {
		if (!strcmp(src, p->src_addr) &&
		    !strcmp(dest, p->dest_addr) &&
		    !strcmp(dev, p->dev)) {
			a = *p;
			a.next = NULL;
			free_proc_ax25(list);
			return &a;
		}
	}
	free_proc_ax25(list);
	return NULL;
}

struct proc_nr_neigh *find_neigh(int addr, struct proc_nr_neigh *neighs)
{
	static struct proc_nr_neigh n;
	struct proc_nr_neigh *p, *list;

	list = neighs ? neighs : read_proc_nr_neigh();
	for (p = list; p != NULL; p = p->next) {
		if (addr == p->addr) {
			n = *p;
			n.next = NULL;
			p = &n;
			break;
		}
	}
	if (neighs == NULL)
		free_proc_nr_neigh(list);
	return p;
}

struct proc_nr_nodes *find_node(char *addr, struct proc_nr_nodes *nodes)
{
	static struct proc_nr_nodes n;
	struct proc_nr_nodes *p, *list;
        char *cp;

        if ((cp = strchr(addr, '-')) != NULL && *(cp + 1) == '0')
                *cp = 0;
	list = nodes ? nodes : read_proc_nr_nodes();
	for (p = list; p != NULL; p = p->next) {
		if (!strcasecmp(addr, p->call) || !strcasecmp(addr, p->alias)) {
			n = *p;
			n.next = NULL;
			p = &n;
			break;
		}
	}
	if (nodes == NULL)
		free_proc_nr_nodes(list);
        return p;
}
