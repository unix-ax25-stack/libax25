
lib_LTLIBRARIES = libax25.la libax25io.la

libax25includedir = $(includedir)/netax25
libax25ioincludedir = $(includedir)/netax25

libax25include_HEADERS = 	\
	axlib.h		\
	axconfig.h	\
	nrconfig.h	\
	rsconfig.h	\
	daemon.h	\
	ttyutils.h	\
	procutils.h	\
	kernel_ax25.h	\
	kernel_rose.h	\
	mheard.h

libax25ioinclude_HEADERS = 	\
	ax25io.h

libax25_la_SOURCES = \
	axconfig.c \
	axconfig.h \
	axlib.h \
	axutils.c \
	config.h \
	daemon.c \
	daemon.h \
	mheard.h \
	nrconfig.c \
	nrconfig.h \
	pathnames.h \
	procutils.c \
	procutils.h \
	rsconfig.c \
	rsconfig.h \
	ttyutils.c \
	ttyutils.h  \
	kernel_ax25.h \
	kernel_rose.h
	
libax25io_la_SOURCES = \
	ax25io.c \
	ax25io.h 

man_MANS = ax25.3 rose.3

EXTRA_DIST= $(man_MANS) glibc.patch

