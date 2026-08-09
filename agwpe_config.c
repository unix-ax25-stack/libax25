/* LIBAX25 - Library for AX.25 programs
 * Copyright (C) 1997-1999 Jonathan Naylor, Tomi Manninen, Jean-Paul Roubelat
 * and Alan Cox.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */
/*
 * Parser for agwpe.conf.  One upstream per line:
 *
 *	<name>	<host>	<tcp-port>	[description]
 *
 * The reserved name "loop" (AGWPE_LOOP_NAME) enables the virtual local
 * loopback port; it needs neither host nor tcp-port.  A line
 *
 *	auth	no|extern|yes
 *
 * selects how much authentication the loop port clients need.  The
 * default is "extern": clients connecting over loopback (127.0.0.1 and
 * ::1) are trusted, any other client must log in first.  "yes" asks
 * every client to log in, "no" trusts everyone.  "required" and
 * "always" are synonyms for "yes", "loop" for "extern".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <netax25/agwpe_config.h>

/* Credentials go into agwpe_shadow.conf, never into agwpe.conf, so that
 * agwpe.conf stays readable for orientation while the passwords live in
 * a mode 0600 root owned file.  */
#define	AGWPE_SHADOW_SUFFIX	"-shadow.conf"

static int auth_value(const char *v)
{
	if (v == NULL)
		return AGWPE_AUTH_EXTERN;
	if (strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
	    strcmp(v, "0") == 0)
		return AGWPE_AUTH_OFF;
	if (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 ||
	    strcmp(v, "1") == 0 ||
	    strcasecmp(v, "required") == 0 ||
	    strcasecmp(v, "always") == 0)
		return AGWPE_AUTH_ALWAYS;
	if (strcasecmp(v, "extern") == 0 || strcasecmp(v, "external") == 0 ||
	    strcasecmp(v, "loop") == 0 || strcasecmp(v, "loopback") == 0)
		return AGWPE_AUTH_EXTERN;
	return AGWPE_AUTH_EXTERN;	/* unknown: the safe default */
}

int agwpe_config_load(const char *path, struct agwpe_config *cfg)
{
	FILE *fp;
	char line[256], *s, *name, *host, *port;
	struct agwpe_upstream *tmp;
	int lineno = 0;
	int virtual;

	if (cfg == NULL)
		return -1;
	memset(cfg, 0, sizeof(*cfg));
	cfg->auth = AGWPE_AUTH_EXTERN;	/* default: trust loopback only */

	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		lineno++;

		if ((s = strchr(line, '\n')) != NULL)
			*s = '\0';

		s = line;
		while (isspace((unsigned char)*s))
			s++;

		if (*s == '\0' || *s == '#')
			continue;

		name = strtok(s, " \t");
		host = strtok(NULL, " \t");
		port = strtok(NULL, " \t");
		/* optional description is ignored */

		if (name == NULL) {
			fprintf(stderr, "agwpe_config: unable to parse line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (strcasecmp(name, "auth") == 0) {
			cfg->auth = auth_value(host);
			continue;
		}

		virtual = (strcmp(name, AGWPE_LOOP_NAME) == 0);

		if (!virtual && (host == NULL || port == NULL)) {
			fprintf(stderr, "agwpe_config: unable to parse line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (strlen(name) >= AGWPE_UPSTREAM_NAME_MAX ||
		    strlen(host) >= AGWPE_UPSTREAM_HOST_MAX) {
			fprintf(stderr, "agwpe_config: name or host too long on line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (cfg->count > 0) {
			int i;

			for (i = 0; i < cfg->count; i++) {
				if (strcmp(cfg->upstreams[i].name, name) == 0) {
					fprintf(stderr, "agwpe_config: duplicate upstream name %s on line %d of %s\n",
						name, lineno, path);
					goto error;
				}
			}
		}

		tmp = realloc(cfg->upstreams,
			      sizeof(struct agwpe_upstream) * (cfg->count + 1));
		if (tmp == NULL) {
			fprintf(stderr, "agwpe_config: out of memory\n");
			goto error;
		}
		cfg->upstreams = tmp;

		memset(&cfg->upstreams[cfg->count], 0,
		       sizeof(struct agwpe_upstream));
		strncpy(cfg->upstreams[cfg->count].name, name,
			sizeof(cfg->upstreams[cfg->count].name) - 1);
		strncpy(cfg->upstreams[cfg->count].host, host,
			sizeof(cfg->upstreams[cfg->count].host) - 1);

		if (virtual) {
			cfg->upstreams[cfg->count].virtual = 1;
			cfg->upstreams[cfg->count].tcp_port = 0;
		} else {
			int tp = atoi(port);

			if (tp <= 0 || tp > 65535) {
				fprintf(stderr, "agwpe_config: invalid tcp port %s on line %d of %s\n",
					port, lineno, path);
				goto error;
			}
			cfg->upstreams[cfg->count].tcp_port = tp;
		}

		cfg->count++;
	}

	fclose(fp);
	return 0;

error:
	fclose(fp);
	agwpe_config_free(cfg);
	return -1;
}

/*
 * Read the credentials for an ax25netd instance from path (typically
 * "agwpe_shadow.conf" in the directory of agwpe.conf).  Lines are
 *
 *	<target>	<user>		<password>
 *
 * where target is an upstream name from agwpe.conf, or AGWPE_AUTH_TARGET
 * ("ax25netd") for the general client authentication of the daemon.  A
 * missing file is not an error; a target that matches neither is.
 * The file is expected to be mode 0600 and owned by root.
 */
int agwpe_config_load_shadow(const char *path, struct agwpe_config *cfg)
{
	FILE *fp;
	char line[256], *s, *target, *user, *pass;
	int lineno = 0;

	if (cfg == NULL)
		return -1;

	fp = fopen(path, "r");
	if (fp == NULL)
		return 0;	/* no credentials is not an error */

	while (fgets(line, sizeof(line), fp)) {
		lineno++;

		if ((s = strchr(line, '\n')) != NULL)
			*s = '\0';

		s = line;
		while (isspace((unsigned char)*s))
			s++;

		if (*s == '\0' || *s == '#')
			continue;

		target = strtok(s, " \t");
		user = strtok(NULL, " \t");
		pass = strtok(NULL, " \t");

		if (target == NULL || user == NULL || pass == NULL) {
			fprintf(stderr, "agwpe_config: unable to parse line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (strlen(user) >= AGWPE_AUTH_NAME_MAX ||
		    strlen(pass) >= AGWPE_AUTH_PASS_MAX) {
			fprintf(stderr, "agwpe_config: user or password too long on line %d of %s\n",
				lineno, path);
			goto error;
		}

		if (strcmp(target, AGWPE_AUTH_TARGET) == 0) {
			struct agwpe_auth *tmp;

			tmp = realloc(cfg->clients,
				      sizeof(struct agwpe_auth) *
				      (cfg->nclients + 1));
			if (tmp == NULL) {
				fprintf(stderr, "agwpe_config: out of memory\n");
				goto error;
			}
			cfg->clients = tmp;
			memset(&cfg->clients[cfg->nclients], 0,
			       sizeof(struct agwpe_auth));
			strncpy(cfg->clients[cfg->nclients].user, user,
				sizeof(cfg->clients[cfg->nclients].user) - 1);
			strncpy(cfg->clients[cfg->nclients].pass, pass,
				sizeof(cfg->clients[cfg->nclients].pass) - 1);
			cfg->nclients++;
		} else {
			int i, found = 0;

			for (i = 0; i < cfg->count; i++) {
				if (strcmp(cfg->upstreams[i].name, target) == 0) {
					strncpy(cfg->upstreams[i].user, user,
						sizeof(cfg->upstreams[i].user) - 1);
					strncpy(cfg->upstreams[i].pass, pass,
						sizeof(cfg->upstreams[i].pass) - 1);
					found = 1;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "agwpe_config: no upstream named %s on line %d of %s\n",
					target, lineno, path);
				goto error;
			}
		}
	}

	fclose(fp);
	return 0;

error:
	fclose(fp);
	return -1;
}

void agwpe_config_free(struct agwpe_config *cfg)
{
	if (cfg == NULL)
		return;

	free(cfg->upstreams);
	cfg->upstreams = NULL;
	cfg->count = 0;

	free(cfg->clients);
	cfg->clients = NULL;
	cfg->nclients = 0;
}
