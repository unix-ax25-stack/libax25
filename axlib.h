/*
 * Callsign conversion functions, converts callsigns into network bit
 * shifted format and vica versa.
 */
 
#ifndef _AXUTILS_H
#define	_AXUTILS_H

#ifndef	TRUE
#define	TRUE	1
#endif

#ifndef	FALSE
#define	FALSE	0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The special "null" address, used as the default callsign in routing and
 * in other places.
 */
extern ax25_address null_ax25_address;

/*
 * This function converts an ASCII representation of a callsign into network
 * format. It returns -1 on error, 0 otherwise.
 */
extern int convert_call_entry(const char *, char *);

/*
 * This converts a string with optional digipeaters into a structure understood
 * by the kernel code.
 *
 * The string should be in the format:
 *
 * callsign [[V | VIA] callsign ...]
 *
 * On error a -1 is returned, otherwise the length of the structure is returned.
 */
extern int convert_call(char *, struct full_sockaddr_ax25 *);

/*
 * Similar to convert_call above except the callsign(s) are not held in a
 * string but in a NULL terminated array of pointers to the strings.
 * On error a -1 is returned, otherwise the length of the structure is returned.
 */
extern int convert_call_arglist(char ** , struct full_sockaddr_ax25 *);

/*
 * This function converts an ASCII representation of a Rose address into
 * network format. It returns -1 on error, 0 otherwise. The address must be
 * ten numbers long.
 */
extern int convert_rose_address(const char *, char *);

/*
 * This function returns the textual representation of a callsign in
 * network format. The data returned is in a statically allocated area, and
 * subsequent calls will destroy previous callsigns returned.
 */
extern char *ax2asc(ax25_address *);

/*
 * This function returns the textual representation of a Rose address in
 * network format. The data returned is in a statically allocated area, and
 * subsequent calls will destroy previous callsigns returned.
 */
extern char *rose2asc(rose_address *);

/*
 * Compares two AX.25 callsigns in network format. Returns a 0 if they are
 * identical, 1 if they differ, or 2 if only the SSIDs differ.
 */
extern int ax25cmp(ax25_address *, ax25_address *);

/*
 * Compares two Rose addresses in network format. Returns a 0 if they are
 * identical, 1 if they differ.
 */
extern int rosecmp(rose_address *, rose_address *);

/*
 * Validates an AX.25 callsign, returns TRUE if it is valid, or FALSE if it
 * is not. The callsign should be AX.25 shifted format.
 */
extern int ax25validate(char *);

/*
 * Converts the giver string to upper case. It returns a pointer to the
 * original string.
 */
extern char *strupr(char *);

/*
 * Converts the giver string to lower case. It returns a pointer to the
 * original string.
 */
extern char *strlwr(char *);

#ifdef __cplusplus
}
#endif

#endif
