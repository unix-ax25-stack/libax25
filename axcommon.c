/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 1997-1999 Jonathan Naylor, Tomi Manninen, Jean-Paul Roubelat
 * and Alan Cox.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
/*
 * Parser for ax25common.conf, the shared configuration of the ax25netd
 * loop port.  One directive per line:
 *
 *	loop socket <path|no>
 *	loop tcp <port|yes|no>
 *	loop group <name|gid|all|default>
 *	loop mode <octal>
 *
 * See netax25/axcommon.h for the semantics.  A missing file is not an
 * error: the defaults are left in place.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include <netax25/axcommon.h>

#ifndef	AX25_SYSCONFDIR
#define	AX25_SYSCONFDIR	"/usr/local/etc/ax25/"
#endif

const char *ax25common_default_config(void)
{
	return AX25_SYSCONFDIR "ax25common.conf";
}

/* Octal, always: "1775" has to arrive as 01775 and not as 1775 decimal,
 * and "755" as 0755.  A value outside 0000..07777 is a typo rather than
 * something to truncate, and so is anything that is not octal at all. */
static int parse_mode(const char *s, mode_t *out)
{
	char *end;
	unsigned long v;

	if (s[0] == '\0')
		return -1;

	errno = 0;
	v = strtoul(s, &end, 8);
	if (errno != 0 || *end != '\0' || v > 07777)
		return -1;

	*out = (mode_t)v;
	return 0;
}

int ax25common_config_load(const char *path, struct ax25common *cfg)
{
	FILE *fp;
	char line[256], *s, *tok[4];
	int lineno = 0;

	if (cfg == NULL)
		return -1;
	memset(cfg, 0, sizeof(*cfg));
	strncpy(cfg->loop_socket, AX25COMMON_SOCKET_DEFAULT,
		sizeof(cfg->loop_socket) - 1);
	cfg->loop_tcp_enabled = 0;
	cfg->loop_tcp_port = AX25COMMON_TCP_DEFAULT;
	cfg->group_mode = AGWPE_GROUP_ALL;
	cfg->loop_mode = AX25COMMON_MODE_DEFAULT;

	fp = fopen(path, "r");
	if (fp == NULL)
		return 0;	/* no shared config is not an error */

	while (fgets(line, sizeof(line), fp)) {
		int ntok = 0;

		lineno++;

		if ((s = strchr(line, '\n')) != NULL)
			*s = '\0';

		s = line;
		while (isspace((unsigned char)*s))
			s++;

		if (*s == '\0' || *s == '#')
			continue;

		tok[0] = strtok(s, " \t");
		for (ntok = 1; ntok < 4 && tok[ntok - 1] != NULL; ntok++)
			tok[ntok] = strtok(NULL, " \t");
		/* ntok = number of tokens read */

		if (ntok < 3) {
			fprintf(stderr,
				"ax25common: unable to parse line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (strcasecmp(tok[0], "loop") != 0) {
			fprintf(stderr,
				"ax25common: unknown directive '%s' on line %d of %s\n",
				tok[0], lineno, path);
			goto error;
		}

		if (strcasecmp(tok[1], "socket") == 0) {
			if (strcasecmp(tok[2], "no") == 0 ||
			    strcmp(tok[2], "0") == 0)
				cfg->loop_socket[0] = '\0';
			else if (strlen(tok[2]) >=
				 sizeof(cfg->loop_socket)) {
				fprintf(stderr,
					"ax25common: loop socket path too long on line %d of %s\n",
					lineno, path);
				goto error;
			} else {
				strncpy(cfg->loop_socket, tok[2],
					sizeof(cfg->loop_socket) - 1);
			}
			continue;
		}

		if (strcasecmp(tok[1], "tcp") == 0) {
			if (strcasecmp(tok[2], "no") == 0 ||
			    strcasecmp(tok[2], "false") == 0 ||
			    strcmp(tok[2], "0") == 0) {
				cfg->loop_tcp_enabled = 0;
			} else if (strcasecmp(tok[2], "yes") == 0 ||
				   strcasecmp(tok[2], "true") == 0) {
				cfg->loop_tcp_enabled = 1;
			} else {
				int p = atoi(tok[2]);

				if (p <= 0 || p > 65535) {
					fprintf(stderr,
						"ax25common: invalid loop tcp port %s on line %d of %s\n",
						tok[2], lineno, path);
					goto error;
				}
				cfg->loop_tcp_enabled = 1;
				cfg->loop_tcp_port = p;
			}
			continue;
		}

		if (strcasecmp(tok[1], "group") == 0) {
			if (strcasecmp(tok[2], "all") == 0 ||
			    strcmp(tok[2], "*") == 0) {
				cfg->group_mode = AGWPE_GROUP_ALL;
			} else if (strcasecmp(tok[2], "default") == 0) {
				/* The daemon's run user and its primary
				 * group.  Not what a missing directive
				 * means any more - see AX25COMMON_GROUP_DEFAULT
				 * in netax25/axcommon.h - but reachable, which
				 * is what matters here.  */
				cfg->group_mode = AGWPE_GROUP_DEFAULT;
			} else {
				cfg->group_mode = AGWPE_GROUP_NAMED;
				strncpy(cfg->group_name, tok[2],
					sizeof(cfg->group_name) - 1);
			}
			continue;
		}

		if (strcasecmp(tok[1], "mode") == 0) {
			if (parse_mode(tok[2], &cfg->loop_mode) < 0) {
				fprintf(stderr,
					"ax25common: invalid loop mode %s on line %d of %s\n",
					tok[2], lineno, path);
				goto error;
			}
			continue;
		}

		fprintf(stderr,
			"ax25common: unknown loop directive '%s' on line %d of %s\n",
			tok[1], lineno, path);
		goto error;
	}

	fclose(fp);
	return 0;

error:
	fclose(fp);
	return -1;
}
