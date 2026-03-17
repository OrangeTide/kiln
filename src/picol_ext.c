/* picol_ext.c : Tcl extension commands (glob, file, lindex, llength) */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include <string.h>
#include <stdlib.h>

#define PICOL_NO_MAIN
#include "picol.c"
#include "platform.h"

/***************************************************************************
 * Tcl list parsing helpers (shared by lindex and llength)
 **************************************************************************/

/* Advance *p past whitespace. */
void
list_skip_space(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

/* Parse one list element starting at *p.
 * Sets *start and *len to the element content (inside braces/quotes if any).
 * Advances *p past the element.
 * Returns 1 if an element was found, 0 at end of string. */
int
list_next(const char **p, const char **start, int *len)
{
	list_skip_space(p);

	if (**p == '\0') return 0;

	if (**p == '{') {
		/* Brace-quoted element */
		int level = 1;
		(*p)++;
		*start = *p;

		while (**p && level > 0) {
			if (**p == '{') level++;
			else if (**p == '}') level--;

			if (level > 0) (*p)++;
		}

		*len = (int)(*p - *start);

		if (**p == '}') (*p)++;
	} else if (**p == '"') {
		/* Quote-delimited element */
		(*p)++;
		*start = *p;

		while (**p && **p != '"') {
			if (**p == '\\' && *(*p + 1)) (*p)++;

			(*p)++;
		}

		*len = (int)(*p - *start);

		if (**p == '"') (*p)++;
	} else {
		/* Bare word */
		*start = *p;

		while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r')
			(*p)++;

		*len = (int)(*p - *start);
	}

	return 1;
}

/**
 * lindex list index
 *
 * Returns the element at the given index from a Tcl list.
 */
static int
cmd_lindex(struct picolInterp *i, int argc, char **argv,
                   struct picolCmd *cmd)
{
	if (argc != 3) return picolArityErr(i, argv[0]);

	const char *list = argv[1];
	int target = atoi(argv[2]);

	if (target < 0) {
		picolSetResult(i, "");
		return PICOL_OK;
	}

	const char *p = list;
	const char *start;
	int len, idx = 0;

	while (list_next(&p, &start, &len)) {
		if (idx == target) {
			char *elem = xmalloc(len + 1);
			memcpy(elem, start, len);
			elem[len] = '\0';
			picolSetResult(i, elem);
			free(elem);
			return PICOL_OK;
		}

		idx++;
	}

	picolSetResult(i, "");
	return PICOL_OK;
}

/**
 * llength list
 *
 * Returns the number of elements in a Tcl list.
 */
static int
cmd_llength(struct picolInterp *i, int argc, char **argv,
                    struct picolCmd *cmd)
{
	if (argc != 2) return picolArityErr(i, argv[0]);

	const char *p = argv[1];
	const char *start;
	int len, count = 0;

	while (list_next(&p, &start, &len)) count++;

	char buf[32];
	snprintf(buf, sizeof(buf), "%d", count);
	picolSetResult(i, buf);
	return PICOL_OK;
}

/**
 * glob ?pattern ...?
 *
 * Returns a space-separated list of matching files. Elements
 * containing whitespace are brace-quoted.
 */
static int
cmd_glob(struct picolInterp *i, int argc, char **argv,
                 struct picolCmd *cmd)
{
	if (argc < 2) return picolArityErr(i, argv[0]);

	char result[16384];
	int rlen = 0;
	result[0] = '\0';

	for (int a = 1; a < argc; a++) {
		char **matches;
		int count;

		if (kiln_glob(argv[a], &matches, &count) != OK) {
			picolSetResult(i, "glob: error");
			return PICOL_ERR;
		}

		for (int j = 0; j < count; j++) {
			int mlen = strlen(matches[j]);
			int need_brace = 0;

			for (int k = 0; k < mlen; k++) {
				if (matches[j][k] == ' ' || matches[j][k] == '\t') {
					need_brace = 1;
					break;
				}
			}

			if (rlen > 0 && rlen < (int)sizeof(result) - 1)
				result[rlen++] = ' ';

			if (need_brace && rlen < (int)sizeof(result) - 2) {
				result[rlen++] = '{';
				int copy = mlen;

				if (rlen + copy > (int)sizeof(result) - 2)
					copy = (int)sizeof(result) - 2 - rlen;

				memcpy(result + rlen, matches[j], copy);
				rlen += copy;
				result[rlen++] = '}';
			} else {
				int copy = mlen;

				if (rlen + copy > (int)sizeof(result) - 1)
					copy = (int)sizeof(result) - 1 - rlen;

				memcpy(result + rlen, matches[j], copy);
				rlen += copy;
			}
		}

		kiln_glob_free(matches, count);
	}

	result[rlen] = '\0';
	picolSetResult(i, result);
	return PICOL_OK;
}

/* Return a pointer to the last path separator, or NULL. */
static const char *
find_last_sep(const char *path)
{
	const char *last = NULL;

	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\') last = p;

	return last;
}

/**
 * file subcommand arg
 *
 * Subcommands: dirname, tail, extension, rootname.
 */
static int
cmd_file(struct picolInterp *i, int argc, char **argv,
                 struct picolCmd *cmd)
{
	if (argc != 3) return picolArityErr(i, argv[0]);

	const char *sub = argv[1];
	const char *path = argv[2];

	if (strcmp(sub, "dirname") == 0) {
		const char *sep = find_last_sep(path);

		if (!sep) {
			picolSetResult(i, ".");
		} else if (sep == path) {
			picolSetResult(i, "/");
		} else {
			char *buf = xmalloc(sep - path + 1);
			memcpy(buf, path, sep - path);
			buf[sep - path] = '\0';
			picolSetResult(i, buf);
			free(buf);
		}
	} else if (strcmp(sub, "tail") == 0) {
		const char *sep = find_last_sep(path);
		picolSetResult(i, (char *)(sep ? sep + 1 : path));
	} else if (strcmp(sub, "extension") == 0) {
		const char *sep = find_last_sep(path);
		const char *base = sep ? sep + 1 : path;
		const char *dot = strrchr(base, '.');
		picolSetResult(i, dot ? (char *)dot : "");
	} else if (strcmp(sub, "rootname") == 0) {
		const char *sep = find_last_sep(path);
		const char *base = sep ? sep + 1 : path;
		const char *dot = strrchr(base, '.');

		if (dot) {
			int len = (int)(dot - path);
			char *buf = xmalloc(len + 1);
			memcpy(buf, path, len);
			buf[len] = '\0';
			picolSetResult(i, buf);
			free(buf);
		} else {
			picolSetResult(i, (char *)path);
		}
	} else {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s:%d: file: unknown subcommand '%s'",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, sub);
		picolSetResult(i, buf);
		return PICOL_ERR;
	}

	return PICOL_OK;
}

/***************************************************************************
 * Register all extension commands
 **************************************************************************/
void
kiln_register_ext_commands(struct picolInterp *i)
{
	picolRegisterCommand(i, "glob", cmd_glob);
	picolRegisterCommand(i, "file", cmd_file);
	picolRegisterCommand(i, "lindex", cmd_lindex);
	picolRegisterCommand(i, "llength", cmd_llength);
}
