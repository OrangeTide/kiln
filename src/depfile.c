/* depfile.c : gcc/clang -MMD depfile parser */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "depfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char **
depfile_parse(const char *path, int *count)
{
	FILE *fp = fopen(path, "r");

	if (!fp) {
		*count = 0;
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *buf = malloc(sz + 1);
	size_t n = fread(buf, 1, sz, fp);
	buf[n] = '\0';
	fclose(fp);

	/* Skip "target:" prefix */
	char *p = strchr(buf, ':');

	if (!p) {
		free(buf);
		*count = 0;
		return NULL;
	}

	p++; /* skip ':' */

	int cap = 32, cnt = 0;
	char **deps = malloc(sizeof(char *) * cap);

	while (*p) {
		/* Skip whitespace and line continuations */
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

		if (*p == '\\' && (*(p + 1) == '\n' || *(p + 1) == '\r')) {
			p += 2;

			if (*p == '\n') p++; /* \r\n */

			continue;
		}

		if (*p == '\0') break;

		/* Read one path - may contain backslash-escaped spaces */
		char *start = p;

		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
			if (*p == '\\' && *(p + 1) == ' ') {
				p += 2; /* escaped space in path */
			} else {
				p++;
			}
		}

		if (p > start) {
			int len = p - start;
			/* Unescape backslash-space */
			char *dep = malloc(len + 1);
			char *d = dep;

			for (char *s = start; s < p; s++) {
				if (*s == '\\' && s + 1 < p && *(s + 1) == ' ') {
					*d++ = ' ';
					s++;
				} else {
					*d++ = *s;
				}
			}

			*d = '\0';

			if (cnt >= cap) {
				cap *= 2;
				deps = realloc(deps, sizeof(char *) * cap);
			}

			deps[cnt++] = dep;
		}
	}

	free(buf);
	*count = cnt;
	return deps;
}

void
depfile_free(char **deps, int count)
{
	for (int i = 0; i < count; i++) free(deps[i]);

	free(deps);
}
