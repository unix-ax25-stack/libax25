AC_PREREQ([2.59])
AC_INIT([libax25],[0.0.12-rc5],[linux-hams@vger.kernel.org])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_SRCDIR(axconfig.c)
AM_INIT_AUTOMAKE([foreign no-define 1.7])

AC_CONFIG_HEADERS(config.h)

dnl Checks for programs.
AC_PROG_CC
AC_PROG_GCC_TRADITIONAL
LT_INIT

dnl Checks for libraries.

AC_CHECK_FUNC(setsid,, AC_MSG_ERROR([setsid not found]))
AC_CHECK_FUNCS(socket strdup strerror strspn)

dnl Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS(fcntl.h sys/file.h sys/ioctl.h unistd.h)
AC_CHECK_HEADER([zlib.h], AC_CHECK_LIB(z, zlibVersion,Z_LIB="-lz",Z_LIB=))
AC_SUBST(Z_LIB, [$Z_LIB])

dnl Checks for working glibc 2.1 headers
dnl
dnl The bundled netax25/netrom/netrose headers must only be suppressed
dnl on Linux, where glibc provides equivalent system headers.  On
dnl platforms without native kernel AX.25 support (BSD, SysV, macOS,
dnl ...) they are the only copies available and must be installed.
AC_MSG_CHECKING(whether native kernel AX.25 headers are available)
case `uname -s` in
	Linux) ac_have_kernel_ax25=yes ;;
	*)     ac_have_kernel_ax25=no ;;
esac
AC_MSG_RESULT($ac_have_kernel_ax25)

AC_MSG_CHECKING(for working netax25/ax25.h header file)
AC_EGREP_HEADER(ax25_fwd_struct, netax25/ax25.h, has_ax25=yes, has_ax25=no)
if test "$has_ax25" = "yes" ; then
  AC_DEFINE(HAVE_NETAX25_AX25_H, 1, [Define if you have a proper netax25/ax25.h header file])
  AC_MSG_RESULT(yes)
else
  AC_MSG_RESULT(no)
fi
AM_CONDITIONAL([AX25_H], [test "$has_ax25" = "yes" -a "$ac_have_kernel_ax25" = "yes"])

if test "$ac_have_kernel_ax25" = "yes" ; then
  AC_DEFINE(HAVE_KERNEL_AX25, 1, [Define if the platform has a native kernel AX.25 stack])
fi

dnl The userspace AGWPE shim (axsock.c) backs AF_AX25 sockets with the
dnl AGWPE protocol.  It is required on platforms without a native kernel
dnl AX.25 stack and can additionally be enabled on Linux so that a single
dnl libax25 serves both kernel and AGWPE ports (selected per process with
dnl the AXSOCK_BACKEND environment variable).
AC_ARG_ENABLE([agwpe],
	AS_HELP_STRING([--enable-agwpe], [build the userspace AGWPE socket shim (axsock.c), even on platforms with a native kernel AX.25 stack]),
	[ac_enable_agwpe=$enableval],
	[ac_enable_agwpe=auto])
AC_MSG_CHECKING(whether to build the userspace AGWPE socket shim)
case $ac_enable_agwpe in
	yes) ac_build_agwpe=yes ;;
	no)  ac_build_agwpe=no ;;
	*)   if test "$ac_have_kernel_ax25" = "yes" ; then
		ac_build_agwpe=no
	     else
		ac_build_agwpe=yes
	     fi ;;
esac
AC_MSG_RESULT($ac_build_agwpe)
AM_CONDITIONAL([AGWPE], [test "$ac_build_agwpe" = "yes"])

AC_MSG_CHECKING(for working netrom/netrom.h header file)
AC_EGREP_HEADER(unsigned int ndigis, netrom/netrom.h, has_netrom=yes, has_netrom=no)
if test "$has_netrom" = "yes" ; then
  AC_DEFINE(HAVE_NETROM_NETROM_H, 1, [Define if you have a proper netrom/netrom.h header file.])
  AC_MSG_RESULT(yes)
else
  AC_MSG_RESULT(no)
fi
AM_CONDITIONAL([NETROM_H], [test "$has_netrom" = "yes" -a "$ac_have_kernel_ax25" = "yes"])

AC_MSG_CHECKING(for working netrose/rose.h header file)
AC_EGREP_HEADER(rose_facilities_struct, netrose/rose.h, has_rose=yes, has_rose=no)
if test "$has_rose" = "yes" ; then
  AC_DEFINE(HAVE_NETROSE_ROSE_H, 1, [Define if you have a proper netrose/rose.h header file.])
  AC_MSG_RESULT(yes)
else
  AC_MSG_RESULT(no)
fi
AM_CONDITIONAL([ROSE_H], [test "$has_rose" = "yes" -a "$ac_have_kernel_ax25" = "yes"])

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

dnl Only use -Wall if we have gcc
if test "x$GCC" = "xyes"; then
  if test -z "`echo "$CFLAGS" | grep "\-Wall" 2> /dev/null`" ; then
    CFLAGS="$CFLAGS -Wall"
  fi
fi

dnl Checks for library functions.
basever=$(echo $PACKAGE_VERSION | sed -e 's@-.*$@@')
AC_SUBST(BASEVERSION, [$basever])
extraver=$(echo $PACKAGE_VERSION | sed -e 's@@<:@^-@:>@*-@@' -e 's@-@_@')
AC_SUBST(EXTRAVERSION, [$extraver])

AM_CONDITIONAL([BUILD_STATIC], [test "$enable_static" = yes])
AC_CONFIG_FILES([Makefile libax25.spec])
AC_OUTPUT
