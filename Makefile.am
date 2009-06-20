lib_LTLIBRARIES = libax25.la libax25io.la

libax25includedir = $(includedir)
libax25ioincludedir = $(includedir)

INCLUDES = -DAX25_SYSCONFDIR=\""$(sysconfdir)/ax25/"\" \
	   -DAX25_LOCALSTATEDIR=\""$(localstatedir)/ax25/"\" 

nobase_libax25include_HEADERS = 	\
	netax25/axlib.h			\
	netax25/axconfig.h		\
	netax25/nrconfig.h		\
	netax25/rsconfig.h		\
	netax25/daemon.h		\
	netax25/ttyutils.h		\
	netax25/procutils.h		\
	netax25/kernel_ax25.h		\
	netax25/kernel_rose.h		\
	netax25/kernel_netrom.h		\
	netax25/mheard.h	

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
	netax25/kernel_ax25.h \
	netax25/kernel_rose.h
	
libax25io_la_SOURCES = \
	ax25io.c \
	netax25/ax25io.h 

man_MANS = ax25.3 rose.3 axports.5 rsports.5 nrports.5


EXTRA_DIST= $(man_MANS) glibc.patch $(etcfiles)

AX25_SYSCONFDIR=@sysconfdir@/ax25/
AX25_LOCALSTATEDIR=@localstatedir@/ax25/

# Needed so that the install is optional
etcdir = $(AX25_SYSCONFDIR)
etcfiles =  axports rsports nrports
installconf:
	$(mkinstalldirs) $(DESTDIR)$(etcdir)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p; \
	done

