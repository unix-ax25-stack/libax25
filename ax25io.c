#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/telnet.h>

#include "config.h"
#include "ax25io.h"

static ax25io *Iolist = NULL;

/* --------------------------------------------------------------------- */

#ifdef HAVE_ZLIB_H
#include <zlib.h>

struct compr_s {
	int z_error;		/* "(de)compression error" flag		*/
	unsigned char char_buf;	/* temporary character buffer		*/
	z_stream zin;		/* decompressor structure		*/
	z_stream zout;		/* compressor structure			*/
};
#endif

/* --------------------------------------------------------------------- */

ax25io *axio_init(int in, int out, int paclen, char *eol)
{
	ax25io *new;

	if ((new = calloc(1, sizeof(ax25io))) == NULL)
		return NULL;

	new->ifd	= in;
	new->ofd	= out;
	new->eolmode	= EOLMODE_TEXT;
	new->paclen	= paclen;

	strncpy(new->eol, eol, 3);
	new->eol[3] = 0;

	new->next = Iolist;
	Iolist = new;

	return new;
}

void axio_end(ax25io *p)
{
	axio_flush(p);

#ifdef HAVE_ZLIB_H
	if (p->zptr) {
		struct compr_s *z = (struct compr_s *) p->zptr;
		deflateEnd(&z->zout);
		inflateEnd(&z->zin);
	}
#endif
	close(p->ifd);
	close(p->ofd);
	p->ifd = -1;
	p->ofd = -1;
	return;
}

void axio_end_all(void)
{
	ax25io *p = Iolist;

	while (p != NULL) {
		if (p->ifd != -1 && p->ofd != -1)
			axio_end(p);
		p = p->next;
	}
}

/* --------------------------------------------------------------------- */

int axio_compr(ax25io *p, int flag)
{
#ifdef HAVE_ZLIB_H
	if (flag) {
		struct compr_s *z = calloc(1, sizeof(struct compr_s));

		if (!z)
			return -1;
		p->zptr = z;
		if (inflateInit(&z->zin) != Z_OK)
			return -1;
		if (deflateInit(&z->zout, Z_BEST_COMPRESSION) != Z_OK)
			return -1;
	}
	return 0;
#endif
	/*ax25_errno = AX25IO_NO_Z_SUPPORT;*/
	return -1;
}

int axio_paclen(ax25io *p, int paclen)
{
	axio_flush(p);

	if (paclen > AXBUFLEN)
		return -1;

	return p->paclen = paclen;
}

int axio_eolmode(ax25io *p, int newmode)
{
	int oldmode;

	oldmode = p->eolmode;
	p->eolmode = newmode;
	return oldmode;
}

int axio_cmpeol(ax25io *p1, ax25io *p2)
{
	return strcmp(p1->eol, p2->eol);
}

int axio_tnmode(ax25io *p, int newmode)
{
	int oldmode;

	oldmode = p->telnetmode;
	p->telnetmode = newmode;
	return oldmode;
}

static int flush_obuf(ax25io *p)
{
	int ret;
	int count=0;

	if (p->optr == 0)
		return 0;

	do {
		if ((ret = write(p->ofd, p->obuf, p->optr < p->paclen ? p->optr : p->paclen)) < 0)
			return -1;

		if (ret < p->optr)
			memmove(p->obuf, &p->obuf[ret], p->optr - ret);
		p->optr -= ret;
		count += ret;

		/* If buffer full block until there is room */
		if (p->optr>=AXBUFLEN) {
			fd_set fdset;

			FD_ZERO(&fdset);
			FD_SET(p->ofd, &fdset);
			if (select(p->ofd+1, NULL, &fdset, NULL, NULL)<0)
				return -1;
		}
	} while (p->optr>=AXBUFLEN);

	return count;
}

int axio_flush(ax25io *p)
{
	int flushed=0;
	fd_set fdset;

#ifdef HAVE_ZLIB_H
	if (p->zptr) {
		struct compr_s *z = (struct compr_s *) p->zptr;
		int ret;

		/*
		 * Fail immediately if there has been an error in
		 * compression or decompression.
		 */
		if (z->z_error) {
			errno = z->z_error;
			return -1;
		}
		/*
		 * No new input, just flush the compression block.
		 */
		z->zout.next_in = NULL;
		z->zout.avail_in = 0;
		do {
			/* Adjust the output pointers */
			z->zout.next_out = p->obuf + p->optr;
			z->zout.avail_out = p->paclen - p->optr;

			ret = deflate(&z->zout, Z_PARTIAL_FLUSH);
			p->optr = p->paclen - z->zout.avail_out;

			switch (ret) {
			case Z_OK:
				/* All OK */
				break;
			case Z_BUF_ERROR:
				/*
				 * Progress not possible, it's been
				 * flushed already (I hope).
				 */
				break;
			case Z_STREAM_END:
				/* What ??? */
				errno = z->z_error = ZERR_STREAM_END;
				return -1;
			case Z_STREAM_ERROR:
				/* Something strange */
				errno = z->z_error = ZERR_STREAM_ERROR;
				return -1;
			default:
				errno = z->z_error = ZERR_UNKNOWN;
				return -1;
			}
			/*
			 * If output buffer is full, flush it to make room
			 * for more compressed data.
			 */
			if (z->zout.avail_out == 0 && flush_obuf(p) < 0)
				return -1;

		} while (z->zout.avail_out == 0);
	}
#endif

	while (p->optr) {
		FD_ZERO(&fdset);
		FD_SET(p->ofd, &fdset);
		if (select(p->ofd+1, NULL, &fdset, NULL, NULL)<0)
			return -1;
		flushed+=flush_obuf(p);
	}

	return flushed;
}

static int rsendchar(unsigned char c, ax25io *p)
{
#ifdef HAVE_ZLIB_H
	if (p->zptr) {
		struct compr_s *z = (struct compr_s *) p->zptr;
		int ret;

		/*
		 * Fail immediately if there has been an error in
		 * compression or decompression.
		 */
		if (z->z_error) {
			errno = z->z_error;
			return -1;
		}
		/*
		 * One new character to input.
		 */
		z->char_buf = c;
		z->zout.next_in = &z->char_buf;
		z->zout.avail_in = 1;
		/*
		 * Now loop until deflate returns with avail_out != 0
		 */
		do {
			/* Adjust the output pointers */
			z->zout.next_out = p->obuf + p->optr;
			z->zout.avail_out = p->paclen - p->optr;

			ret = deflate(&z->zout, Z_NO_FLUSH);
			p->optr = p->paclen - z->zout.avail_out;

			switch (ret) {
			case Z_OK:
				/* All OK */
				break;
			case Z_STREAM_END:
				/* What ??? */
				errno = z->z_error = ZERR_STREAM_END;
				return -1;
			case Z_STREAM_ERROR:
				/* Something strange */
				errno = z->z_error = ZERR_STREAM_ERROR;
				return -1;
			case Z_BUF_ERROR:
				/* Progress not possible (should be) */
				errno = z->z_error = ZERR_BUF_ERROR;
				return -1;
			default:
				errno = z->z_error = ZERR_UNKNOWN;
				return -1;
			}
			/*
			 * If the output buffer is full, flush it to make
			 * room for more compressed data.
			 */
			if (z->zout.avail_out == 0 && flush_obuf(p) < 0)
				return -1;

		} while (z->zout.avail_out == 0);

		return c;
	}
#endif

	p->obuf[p->optr++] = c;

	if (p->optr >= p->paclen && flush_obuf(p) < 0)
		return -1;

	return c;
}

/* --------------------------------------------------------------------- */

static int recv_ibuf(ax25io *p)
{
	int ret;

	p->iptr = 0;
	p->size = 0;

	if ((ret = read(p->ifd, p->ibuf, AXBUFLEN)) < 0)
		return -1;

	if (ret == 0) {
		errno = 0;
		return -1;
	}

	return p->size = ret;
}

static int rrecvchar(ax25io *p)
{
#ifdef HAVE_ZLIB_H
	if (p->zptr) {
		struct compr_s *z = (struct compr_s *) p->zptr;
		int ret;

		/*
		 * Fail immediately if there has been an error in
		 * compression or decompression.
		 */
		if (z->z_error) {
			errno = z->z_error;
			return -1;
		}
		for (;;) {
			/* Adjust the input pointers */
			z->zin.next_in = p->ibuf + p->iptr;
			z->zin.avail_in = p->size - p->iptr;

			/* Room for one output character */
			z->zin.next_out = &z->char_buf;
			z->zin.avail_out = 1;

			ret = inflate(&z->zin, Z_PARTIAL_FLUSH);
			p->iptr = p->size - z->zin.avail_in;

			switch (ret) {
			case Z_OK:
				/*
				 * Progress made! Return if there is
				 * something to be returned.
				 */
				if (z->zin.avail_out == 0)
					return z->char_buf;
				break;
			case Z_BUF_ERROR:
				/*
				 * Not enough new data to proceed,
				 * let's hope we'll get some more below.
				 */
				break;
			case Z_STREAM_END:
				/* What ??? */
				errno = z->z_error = ZERR_STREAM_END;
				return -1;
			case Z_STREAM_ERROR:
				/* Something weird */
				errno = z->z_error = ZERR_STREAM_ERROR;
				return -1;
			case Z_DATA_ERROR:
				/* Compression protocol error */
				errno = z->z_error = ZERR_DATA_ERROR;
				return -1;
			case Z_MEM_ERROR:
				/* Not enough memory */
				errno = z->z_error = ZERR_MEM_ERROR;
				return -1;
			default:
				errno = z->z_error = ZERR_UNKNOWN;
				return -1;
			}
			/*
			 * Our only hope is that inflate has consumed all
			 * input and we can get some more.
			 */
			if (z->zin.avail_in == 0) {
				if (recv_ibuf(p) < 0)
					return -1;
			} else {
				/* inflate didn't consume all input ??? */
				errno = z->z_error = ZERR_UNKNOWN;
				return -1;
			}
		}
		/*
		 * We should never fall through here ???
		 */
		errno = z->z_error = ZERR_UNKNOWN;
		return -1;
	}
#endif

	if (p->iptr >= p->size && recv_ibuf(p) < 0)
		return -1;

	return p->ibuf[p->iptr++];
}

/* --------------------------------------------------------------------- */

int axio_putc(int c, ax25io *p)
{
	char *cp;

	if (p->telnetmode && c == IAC) {
		if (rsendchar(IAC, p) == -1)
			return -1;
		return rsendchar(IAC, p);
	}

	if (c == INTERNAL_EOL) {
		if (p->eolmode == EOLMODE_BINARY)
			return rsendchar('\n', p);
		else
			for (cp = p->eol; *cp; cp++)
				if (rsendchar(*cp, p) == -1)
					return -1;
		return 1;
	}

	if (p->eolmode == EOLMODE_TEXT && c == '\n') {
		for (cp = p->eol; *cp; cp++)
			if (rsendchar(*cp, p) == -1)
				return -1;
		return 1;
	}

	return rsendchar(c & 0xff, p);
}

int axio_getc(ax25io *p)
{
	int c, opt;
	char *cp;

	opt = 0; /* silence warning */

	if ((c = rrecvchar(p)) == -1)
		return -1;

	if (p->telnetmode && c == IAC) {
		if ((c = rrecvchar(p)) == -1)
			return -1;

		if (c > 249 && c < 255 && (opt = rrecvchar(p)) == -1)
			return -1;

		switch(c) {
		case IP:
		case ABORT:
		case xEOF:
			return -1;
		case SUSP:
			break;
		case SB:
			/*
			 * Start of sub-negotiation. Just ignore everything
			 * until we see a IAC SE (some negotiation...).
			 */
			if ((c = rrecvchar(p)) == -1)
				return -1;
			if ((opt = rrecvchar(p)) == -1)
				return -1;
			while (!(c == IAC && opt == SE)) {
				c = opt;
				if ((opt = rrecvchar(p)) == -1)
					return -1;
			}
			break;
		case WILL:
			/*
			 * Client is willing to negotiate linemode and
			 * we want it too. Tell the client to go to
			 * local edit mode and also to translate any
			 * signals to telnet commands. Clients answer
			 * is ignored above (rfc1184 says client must obey
			 * ECHO and TRAPSIG).
			 */
			if (opt == TELOPT_LINEMODE && p->tn_linemode) {
				rsendchar(IAC,                      p);
				rsendchar(SB,                       p);
				rsendchar(TELOPT_LINEMODE,          p);
				rsendchar(LM_MODE,                  p);
				rsendchar(MODE_EDIT | MODE_TRAPSIG, p);
				rsendchar(IAC,                      p);
				rsendchar(SE,                       p);
			} else {
				rsendchar(IAC,  p);
				rsendchar(DONT, p);
				rsendchar(opt,  p);
			}
			axio_flush(p);
			break;
		case DO:
			switch (opt) {
			case TELOPT_SGA:
				rsendchar(IAC,  p);
				rsendchar(WILL, p);
				rsendchar(opt,  p);
				axio_flush(p);
				break;
			case TELOPT_ECHO:
				/*
				 * If we want echo then just silently
				 * approve, otherwise deny.
				 */
				if (p->tn_echo)
					break;
				/* Note fall-through */
			default:
				rsendchar(IAC,  p);
				rsendchar(WONT, p);
				rsendchar(opt,  p);
				axio_flush(p);
				break;
			}
			break;
		case IAC:	/* Escaped IAC */
			return IAC;
			break;
		case WONT:
		case DONT:
		default:
			break;
		}

		return axio_getc(p);
	}

	if (c == p->eol[0]) {
		switch (p->eolmode) {
		case EOLMODE_BINARY:
			break;
		case EOLMODE_TEXT:
			for (cp = p->eol + 1; *cp; cp++)
				if (rrecvchar(p) == -1)
					return -1;
			c = '\n';
			break;
		case EOLMODE_GW:
			for (cp = p->eol + 1; *cp; cp++)
				if (rrecvchar(p) == -1)
					return -1;
			c = INTERNAL_EOL;
			break;
		}
	}

	return c;
}

/* --------------------------------------------------------------------- */

char *axio_gets(char *buf, int buflen, ax25io *p)
{
	int c, len = 0;

	while (len < (buflen - 1)) {
		c = axio_getc(p);
		if (c == -1) {
			if (len > 0) {
				buf[len] = 0;
				return buf;
			} else
				return NULL;
		}
		/* NUL also interpreted as EOL */
		if (c == '\n' || c == '\r' || c == 0) {
			buf[len] = 0;
			return buf;
		}
		buf[len++] = c;
	}
	buf[buflen - 1] = 0;
	return buf;
}

char *axio_getline(ax25io *p)
{
	static char buf[AXBUFLEN];

	return axio_gets(buf, AXBUFLEN, p);
}

int axio_puts(const char *s, ax25io *p)
{
	while (*s)
		if (axio_putc(*s++, p) == -1)
			return -1;
	return 0;
}

/* --------------------------------------------------------------------- */

static int axio_vprintf(ax25io *p, const char *fmt, va_list args)
{
	static char buf[AXBUFLEN];
	int len, i;

	len = vsprintf(buf, fmt, args);
	for (i = 0; i < len; i++)
		if (axio_putc(buf[i], p) == -1)
			return -1;
	return len;
}

int axio_printf(ax25io *p, const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = axio_vprintf(p, fmt, args);
	va_end(args);
	return len;
}

/* --------------------------------------------------------------------- */

void axio_tn_do_linemode(ax25io *p)
{
	rsendchar(IAC,             p);
	rsendchar(DO,              p);
	rsendchar(TELOPT_LINEMODE, p);
	p->tn_linemode = 1;
}

void axio_tn_will_echo(ax25io *p)
{
	rsendchar(IAC,         p);
	rsendchar(WILL,        p);
	rsendchar(TELOPT_ECHO, p);
	p->tn_echo = 1;
}

void axio_tn_wont_echo(ax25io *p)
{
	rsendchar(IAC,         p);
	rsendchar(WONT,        p);
	rsendchar(TELOPT_ECHO, p);
	p->tn_echo = 0;
}

/* --------------------------------------------------------------------- */

