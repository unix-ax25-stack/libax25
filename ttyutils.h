/*
 * Generic serial port handling functions.
 */
 
#ifndef _TTYUTILS_H
#define	_TTYUTILS_H

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
 * Put a given file descriptor into raw mode, if the hwflag is set to TRUE
 * then hardware handshaking is enabled. Returns TRUE if successful.
 */
extern int tty_raw(int fd, int hwflag);

/*
 * Set the speed of the given file descriptor. Returns TRUE is it was
 * successful.
 */
extern int tty_speed(int fd, int speed);

/*
 * Determines whether a given tty is already open by another process. Returns
 * TRUE if is already locked, or FALSE if it is free.
 */
extern int tty_is_locked(char *tty);

/*
 * Creates a lock file for the given tty. It writes the process ID to the
 * file so take care if doing a fork. Returns TRUE if everything was OK.
 */
extern int tty_lock(char *tty);

/*
 * Removes the lock file for a given tty. Returns TRUE if successful.
 */
extern int tty_unlock(char *tty);

#ifdef __cplusplus
}
#endif

#endif
