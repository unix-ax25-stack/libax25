ACLOCAL_AMFLAGS = -I m4
lib_LTLIBRARIES = libax25.la libax25io.la

libax25includedir = $(includedir)
libax25ioincludedir = $(includedir)

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(sysconfdir)/ax25/"\" \
	      -DAX25_LOCALSTATEDIR=\""$(localstatedir)/ax25/"\"

if !AX25_H
USE_LIBAX25_AX25_H = netax25/ax25.h
endif
# Serving AX.25 sockets from userspace.  axsock.c is the interception - the
# entry points that stand in front of the libc calls - and the backends it
# hands a socket to: AGWPE in agwpe_sock.c over the client in agwpe_client.c,
# which is always built, and a WAMPES node in wampes.c.  The two headers are
# the whole surface between the interception and the AGWPE backend.
if USERSPACE_AX25
USE_LIBAX25_AXSOCK = axsock.c agwpe_sock.c wampes.c \
		     axsock_real.h agwpe_sock.h wampes.h
endif
if !NETROM_H
USE_LIBAX25_NETROM_H = netrom/netrom.h
endif
if !ROSE_H
USE_LIBAX25_ROSE_H = netrose/rose.h
endif

nobase_libax25include_HEADERS = 	\
	netax25/axlib.h			\
	netax25/axconfig.h		\
	netax25/nrconfig.h		\
	netax25/rsconfig.h		\
	netax25/daemon.h		\
	netax25/ttyutils.h		\
	netax25/procutils.h		\
	netax25/mheard.h		\
	netax25/agwpe.h			\
	netax25/agwpe_client.h		\
	netax25/agwpe_config.h		\
	netax25/axcommon.h		\
	netax25/axmon.h			\
	$(USE_LIBAX25_AX25_H)		\
	$(USE_LIBAX25_NETROM_H)		\
	$(USE_LIBAX25_ROSE_H)

nobase_libax25ioinclude_HEADERS = 	\
	netax25/ax25io.h

libax25_la_SOURCES = \
	axconfig.c \
	netax25/axconfig.h \
	netax25/axlib.h \
	axutils.c \
	config.h \
	daemon.c \
	netax25/daemon.h \
	netax25/mheard.h \
	nrconfig.c \
	netax25/nrconfig.h \
	pathnames.h \
	procutils.c \
	netax25/procutils.h \
	rsconfig.c \
	netax25/rsconfig.h \
	ttyutils.c \
	netax25/ttyutils.h  \
	agwpe.c \
	netax25/agwpe.h \
	agwpe_client.c \
	netax25/agwpe_client.h \
	agwpe_config.c \
	netax25/agwpe_config.h \
	axcommon.c \
	netax25/axcommon.h \
	axmon.c \
	netax25/axmon.h \
	$(USE_LIBAX25_AX25_H) \
	$(USE_LIBAX25_AXSOCK) \
	$(USE_LIBAX25_ROSE_H)

libax25io_la_SOURCES = \
	ax25io.c \
	netax25/ax25io.h
libax25io_la_LIBADD = @Z_LIB@

dist_man_MANS = ax25.3 rose.3 axports.5 rsports.5 nrports.5 wampes.conf.5 \
		axsock.7

man_sed = sed -e 's|@@@AX25_SYSCONFDIR@@@|$(MAN_SC)|g' \
	      -e 's|@@@AX25_LOCALSTATEDIR@@@|$(MAN_SD)|g'

MAN_SC = $(sysconfdir)/ax25
MAN_SD = $(localstatedir)/ax25

axports.5: $(srcdir)/axports.man
	$(man_sed) < $(srcdir)/axports.man > $@
rsports.5: $(srcdir)/rsports.man
	$(man_sed) < $(srcdir)/rsports.man > $@
nrports.5: $(srcdir)/nrports.man
	$(man_sed) < $(srcdir)/nrports.man > $@
wampes.conf.5: $(srcdir)/wampes.conf.man
	$(man_sed) < $(srcdir)/wampes.conf.man > $@
axsock.7: $(srcdir)/axsock.man
	$(man_sed) < $(srcdir)/axsock.man > $@

axports: $(srcdir)/axports.in
	$(man_sed) < $(srcdir)/axports.in > $@
rsports: $(srcdir)/rsports.in
	$(man_sed) < $(srcdir)/rsports.in > $@
nrports: $(srcdir)/nrports.in
	$(man_sed) < $(srcdir)/nrports.in > $@
wampes.conf: $(srcdir)/wampes.conf.in
	$(man_sed) < $(srcdir)/wampes.conf.in > $@

CLEANFILES = axports rsports nrports wampes.conf \
	   axports.5 rsports.5 nrports.5 wampes.conf.5 axsock.7

# Built, never installed, and deliberately not linked against the library:
# it is the one program here that can show whether preloading reaches a
# program which does not use libax25.  See axsock(7).
#
# Only where there is an interception to test.  Without --enable-userspace-ax25
# the library serves nothing itself, so preloading it does nothing and this
# would only be one more thing to compile.
if USERSPACE_AX25
noinst_PROGRAMS = axprobe
axprobe_SOURCES = axprobe.c
axprobe_LDADD =
endif


EXTRA_DIST= glibc.patch $(etcsrcfiles) axports.man rsports.man nrports.man \
	    wampes.conf.man axsock.man libax25.spec axsock.c agwpe_sock.c \
	    axsock_real.h agwpe_sock.h wampes.c wampes.h

dist_doc_DATA = README.compile README-hints-for-non-kernel-AX25-hosts.txt \
		README-linux-systemd.txt \
		doc/AX25-WITHOUT-KERNEL-SYSOP.md \
		doc/AX25-WITHOUT-KERNEL-DEVELOPER.md \
		doc/TODO.md \
		doc/AX25-WITHOUT-KERNEL-SYSOP.de.md \
		doc/AX25-WITHOUT-KERNEL-DEVELOPER.de.md

AX25_SYSCONFDIR=@sysconfdir@/ax25/
AX25_LOCALSTATEDIR=@localstatedir@/ax25/

# Needed so that the install is optional
etcdir = $(AX25_SYSCONFDIR)
etcfiles =  axports rsports nrports wampes.conf
etcsrcfiles = axports.in rsports.in nrports.in wampes.conf.in
installconf:
	$(mkinstalldirs) $(DESTDIR)$(etcdir)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p; \
	done
