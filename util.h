/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#ifndef __LIBAX25_UTIL_H
#define __LIBAX25_UTIL_H

/*
 * C23 put unreachable() in <stddef.h>, so a toolchain compiling in that
 * mode has already defined it (as __builtin_unreachable()).  Define ours
 * only when there is none: redefining it is a warning here and undefined
 * behaviour by the letter of the standard.
 */
#ifndef unreachable
#define unreachable() \
	do { __builtin_unreachable(); } while (0)
#endif

#endif /* __LIBAX25_UTIL_H  */
