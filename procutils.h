/*
 * Support routines to simplify the reading of the /proc/net/ax25* and
 * /proc/net/nr* files.
 */

#ifndef _PROCUTILS_H
#define	_PROCUTILS_H

#ifndef	TRUE
#define	TRUE	1
#endif

#ifndef	FALSE
#define	FALSE	0
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct proc_ax25_route {
	char			call[10];
	char			dev[14];
	int			cnt;
	long			t;

	struct proc_ax25_route	*next;
};

struct proc_ax25 {
	char			dest_addr[10], src_addr[10];
	char			dev[14];
	unsigned char		st;
	unsigned short		vs, vr, va;
	unsigned short		t1, t1timer, t2, t2timer, t3, t3timer;
	unsigned short		idle, idletimer;
	unsigned char		n2, n2count;
	unsigned short		rtt;
	unsigned char		window;
	unsigned short		paclen;
	unsigned long		sndq, rcvq;

	struct proc_ax25	*next;
};

struct proc_nr_neigh {
	int			addr;
	char			call[10];
	char			dev[14];
	int			qual;
	int			lock;
	int			cnt;

	struct proc_nr_neigh	*next;
};

struct proc_nr_nodes {
	char			call[10], alias[7];
	unsigned char		w, n;
	unsigned char		qual1, qual2, qual3;
	unsigned char		obs1, obs2, obs3;
	int			addr1, addr2, addr3;

	struct proc_nr_nodes	*next;
};

extern struct proc_ax25 *read_proc_ax25(void);
extern void free_proc_ax25(struct proc_ax25 *ap);

extern struct proc_ax25_route *read_proc_ax25_route(void);
extern void free_proc_ax25_route(struct proc_ax25_route *rp);

extern struct proc_nr_neigh *read_proc_nr_neigh(void);
extern void free_proc_nr_neigh(struct proc_nr_neigh *np);

extern struct proc_nr_nodes *read_proc_nr_nodes(void);
extern void free_proc_nr_nodes(struct proc_nr_nodes *np);

extern char *get_call(int uid);

extern struct proc_ax25 *find_link(const char *src, const char *dest, const char *dev);
extern struct proc_nr_neigh *find_neigh(int addr, struct proc_nr_neigh *neigh);
extern struct proc_nr_nodes *find_node(char *addr, struct proc_nr_nodes *nodes);

#ifdef _cplusplus
}
#endif

#endif
