
lib_LTLIBRARIES = libax25.la

libax25includedir = $(includedir)/netax25

libax25include_HEADERS = 	\
	axlib.h		\
	axconfig.h	\
	nrconfig.h	\
	rsconfig.h	\
	daemon.h	\
	ttyutils.h	\
	procutils.h	\
	mheard.h	\
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
	ax25io.c \
	ax25io.h \
	kernel_ax25.h \
	kernel_rose.h
	
man_MANS = ax25.3 rose.3

EXTRA_DIST= $(man_MANS)

