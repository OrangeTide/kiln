/* picol_ext.c : Tcl extension commands (glob, file, lindex, llength) */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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
	(void)cmd;
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
	(void)cmd;
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
	(void)cmd;
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
	(void)cmd;
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
 * Growable string buffer and list-element helpers
 **************************************************************************/

struct sbuf {
	char *data;
	size_t len;
	size_t cap;
};

static void
sbuf_init(struct sbuf *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static void
sbuf_putc(struct sbuf *b, char c)
{
	if (b->len + 1 > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 64;
		b->data = xrealloc(b->data, b->cap);
	}

	b->data[b->len++] = c;
}

static void
sbuf_puts(struct sbuf *b, const char *s)
{
	while (*s) sbuf_putc(b, *s++);
}

/* NUL-terminate and return the owned buffer; caller frees. */
static char *
sbuf_done(struct sbuf *b)
{
	sbuf_putc(b, '\0');
	return b->data;
}

/* Append elem as one Tcl list element, brace-quoting it when it is empty
 * or contains whitespace. */
static void
sbuf_put_element(struct sbuf *b, const char *elem)
{
	int quote = (elem[0] == '\0');

	for (const char *p = elem; *p && !quote; p++)
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			quote = 1;

	if (quote) {
		sbuf_putc(b, '{');
		sbuf_puts(b, elem);
		sbuf_putc(b, '}');
	} else {
		sbuf_puts(b, elem);
	}
}

/* Glob matcher for "string match": supports *, ?, and [set]. */
static int
glob_match(const char *pat, const char *str)
{
	while (*pat) {
		if (*pat == '*') {
			while (*pat == '*') pat++;

			if (!*pat) return 1;

			for (; *str; str++)
				if (glob_match(pat, str)) return 1;

			return 0;
		} else if (*pat == '?') {
			if (!*str) return 0;

			pat++;
			str++;
		} else if (*pat == '[') {
			const char *p = pat + 1;
			int neg = 0, match = 0;

			if (*p == '!') { neg = 1; p++; }

			while (*p && *p != ']') {
				if (p[1] == '-' && p[2] && p[2] != ']') {
					if ((unsigned char)*str >= (unsigned char)p[0] &&
					    (unsigned char)*str <= (unsigned char)p[2])
						match = 1;

					p += 3;
				} else {
					if (*str == *p) match = 1;

					p++;
				}
			}

			if (*p == ']') p++;

			if (!*str || match == neg) return 0;

			pat = p;
			str++;
		} else {
			if (*str != *pat) return 0;

			pat++;
			str++;
		}
	}

	return *str == '\0';
}

/***************************************************************************
 * List commands: list, lappend
 **************************************************************************/

static int
cmd_list(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;

	struct sbuf b;
	sbuf_init(&b);

	for (int j = 1; j < argc; j++) {
		if (j > 1) sbuf_putc(&b, ' ');

		sbuf_put_element(&b, argv[j]);
	}

	char *s = sbuf_done(&b);
	picolSetResult(i, s);
	free(s);
	return PICOL_OK;
}

static int
cmd_lappend(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	struct picolVar *v = picolGetVar(i, argv[1]);
	struct sbuf b;
	sbuf_init(&b);

	if (v && v->val[0]) sbuf_puts(&b, v->val);

	for (int j = 2; j < argc; j++) {
		if (b.len > 0) sbuf_putc(&b, ' ');

		sbuf_put_element(&b, argv[j]);
	}

	char *s = sbuf_done(&b);
	picolSetVar(i, argv[1], s);
	picolSetResult(i, s);
	free(s);
	return PICOL_OK;
}

/***************************************************************************
 * String accumulation: append, incr
 **************************************************************************/

static int
cmd_append(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	struct picolVar *v = picolGetVar(i, argv[1]);
	struct sbuf b;
	sbuf_init(&b);

	if (v && v->val[0]) sbuf_puts(&b, v->val);

	for (int j = 2; j < argc; j++)
		sbuf_puts(&b, argv[j]);

	char *s = sbuf_done(&b);
	picolSetVar(i, argv[1], s);
	picolSetResult(i, s);
	free(s);
	return PICOL_OK;
}

static int
cmd_incr(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2 || argc > 3) return picolArityErr(i, argv[0]);

	struct picolVar *v = picolGetVar(i, argv[1]);
	long cur = v ? atol(v->val) : 0;
	long amt = (argc == 3) ? atol(argv[2]) : 1;
	cur += amt;

	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", cur);
	picolSetVar(i, argv[1], buf);
	picolSetResult(i, buf);
	return PICOL_OK;
}

/***************************************************************************
 * Control flow: foreach, for, catch, eval
 **************************************************************************/

static int
cmd_foreach(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 4) return picolArityErr(i, argv[0]);

	const char *p = argv[2];
	const char *start;
	int len;

	while (list_next(&p, &start, &len)) {
		char *elem = xmalloc(len + 1);
		memcpy(elem, start, len);
		elem[len] = '\0';
		picolSetVar(i, argv[1], elem);
		free(elem);

		int rc = picolEval(i, argv[3]);

		if (rc == PICOL_OK || rc == PICOL_CONTINUE) continue;

		if (rc == PICOL_BREAK) break;

		return rc; /* ERR or RETURN propagate */
	}

	picolSetResult(i, "");
	return PICOL_OK;
}

static int
cmd_for(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 5) return picolArityErr(i, argv[0]);

	int rc = picolEval(i, argv[1]); /* start */

	if (rc != PICOL_OK) return rc;

	while (1) {
		rc = picolExprExpansion(i, argv[2]); /* test */

		if (rc != PICOL_OK) return rc;

		if (!strtod(i->result, NULL)) break;

		rc = picolEval(i, argv[4]); /* body */

		if (rc == PICOL_BREAK) break;

		if (rc != PICOL_OK && rc != PICOL_CONTINUE) return rc;

		rc = picolEval(i, argv[3]); /* next */

		if (rc != PICOL_OK) return rc;
	}

	picolSetResult(i, "");
	return PICOL_OK;
}

static int
cmd_catch(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2 || argc > 3) return picolArityErr(i, argv[0]);

	int rc = picolEval(i, argv[1]);

	if (argc == 3) picolSetVar(i, argv[2], i->result);

	char buf[16];
	snprintf(buf, sizeof(buf), "%d", rc);
	picolSetResult(i, buf);
	return PICOL_OK;
}

static int
cmd_eval(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	struct sbuf b;
	sbuf_init(&b);

	for (int j = 1; j < argc; j++) {
		if (j > 1) sbuf_putc(&b, ' ');

		sbuf_puts(&b, argv[j]);
	}

	char *s = sbuf_done(&b);
	int rc = picolEval(i, s);
	free(s);
	return rc;
}

/***************************************************************************
 * string subcommand ...
 *
 * Subcommands: length, index, range, match, map, first, last, equal,
 * tolower, toupper.
 **************************************************************************/

static int
cmd_string(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 3) return picolArityErr(i, argv[0]);

	const char *op = argv[1];

	if (strcmp(op, "length") == 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", (int)strlen(argv[2]));
		picolSetResult(i, buf);
		return PICOL_OK;
	}

	if (strcmp(op, "tolower") == 0 || strcmp(op, "toupper") == 0) {
		int up = (strcmp(op, "toupper") == 0);
		struct sbuf b;
		sbuf_init(&b);

		for (const char *p = argv[2]; *p; p++)
			sbuf_putc(&b, (char)(up ? toupper((unsigned char)*p)
			                        : tolower((unsigned char)*p)));

		char *r = sbuf_done(&b);
		picolSetResult(i, r);
		free(r);
		return PICOL_OK;
	}

	if (strcmp(op, "index") == 0) {
		if (argc != 4) return picolArityErr(i, argv[0]);

		const char *s = argv[2];
		int slen = (int)strlen(s);
		int idx = (strcmp(argv[3], "end") == 0) ? slen - 1 : atoi(argv[3]);
		char ch[2] = { 0, 0 };

		if (idx >= 0 && idx < slen) ch[0] = s[idx];

		picolSetResult(i, ch);
		return PICOL_OK;
	}

	if (strcmp(op, "range") == 0) {
		if (argc != 5) return picolArityErr(i, argv[0]);

		const char *s = argv[2];
		int slen = (int)strlen(s);
		int a = (strcmp(argv[3], "end") == 0) ? slen - 1 : atoi(argv[3]);
		int b = (strcmp(argv[4], "end") == 0) ? slen - 1 : atoi(argv[4]);

		if (a < 0) a = 0;

		if (b >= slen) b = slen - 1;

		if (slen == 0 || a > b) {
			picolSetResult(i, "");
		} else {
			int n = b - a + 1;
			char *r = xmalloc(n + 1);
			memcpy(r, s + a, n);
			r[n] = '\0';
			picolSetResult(i, r);
			free(r);
		}

		return PICOL_OK;
	}

	if (strcmp(op, "match") == 0) {
		if (argc != 4) return picolArityErr(i, argv[0]);

		picolSetResult(i, glob_match(argv[2], argv[3]) ? "1" : "0");
		return PICOL_OK;
	}

	if (strcmp(op, "equal") == 0) {
		if (argc != 4) return picolArityErr(i, argv[0]);

		picolSetResult(i, strcmp(argv[2], argv[3]) == 0 ? "1" : "0");
		return PICOL_OK;
	}

	if (strcmp(op, "first") == 0 || strcmp(op, "last") == 0) {
		if (argc != 4) return picolArityErr(i, argv[0]);

		const char *needle = argv[2];
		const char *hay = argv[3];
		int found = -1;

		if (*needle) {
			if (strcmp(op, "first") == 0) {
				const char *f = strstr(hay, needle);

				if (f) found = (int)(f - hay);
			} else {
				const char *f = hay, *prev = NULL;

				while ((f = strstr(f, needle))) {
					prev = f;
					f++;
				}

				if (prev) found = (int)(prev - hay);
			}
		}

		char buf[16];
		snprintf(buf, sizeof(buf), "%d", found);
		picolSetResult(i, buf);
		return PICOL_OK;
	}

	if (strcmp(op, "map") == 0) {
		if (argc != 4) return picolArityErr(i, argv[0]);

		const char *s = argv[3];
		int slen = (int)strlen(s);
		int pos = 0;
		struct sbuf out;
		sbuf_init(&out);

		while (pos < slen) {
			const char *p = argv[2];
			const char *kstart, *vstart;
			int klen, vlen, matched = 0;

			while (list_next(&p, &kstart, &klen)) {
				if (!list_next(&p, &vstart, &vlen)) break;

				if (klen > 0 && pos + klen <= slen &&
				    memcmp(s + pos, kstart, klen) == 0) {
					for (int k = 0; k < vlen; k++)
						sbuf_putc(&out, vstart[k]);

					pos += klen;
					matched = 1;
					break;
				}
			}

			if (!matched) sbuf_putc(&out, s[pos++]);
		}

		char *r = sbuf_done(&out);
		picolSetResult(i, r);
		free(r);
		return PICOL_OK;
	}

	char buf[256];
	snprintf(buf, sizeof(buf), "%s:%d: string: unknown subcommand '%s'",
	         i->current_file ? i->current_file : "<eval>",
	         i->current_line, op);
	picolSetResult(i, buf);
	return PICOL_ERR;
}

/***************************************************************************
 * exec command ...
 *
 * Run a command through the shell while the Kilnfile is being evaluated
 * and return its standard output with trailing newlines stripped. Errors
 * (fails to launch or exits non-zero) raise a Tcl error, so wrap it in
 * catch to probe optionally. Intended for small probes such as compiler
 * version or host detection; output is capped.
 **************************************************************************/

static int
cmd_exec(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	struct sbuf b;
	sbuf_init(&b);

	for (int j = 1; j < argc; j++) {
		if (j > 1) sbuf_putc(&b, ' ');

		sbuf_puts(&b, argv[j]);
	}

	char *command = sbuf_done(&b);

	size_t cap = 65536;
	char *out = xmalloc(cap);
	int ok = (kiln_run_capture(command, out, cap) == OK);

	if (!ok) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s:%d: exec: command failed: %s",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, command);
		picolSetResult(i, buf);
		free(out);
		free(command);
		return PICOL_ERR;
	}

	picolSetResult(i, out);
	free(out);
	free(command);
	return PICOL_OK;
}

/***************************************************************************
 * getenv name ?default?
 *
 * Return the value of an environment variable, or the default (or an
 * empty string) when it is unset. Used to make toolchain settings
 * overridable from the environment.
 **************************************************************************/

static int
cmd_getenv(struct picolInterp *i, int argc, char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2 || argc > 3) return picolArityErr(i, argv[0]);

	char *v = getenv(argv[1]);

	if (v)
		picolSetResult(i, v);
	else
		picolSetResult(i, argc == 3 ? argv[2] : "");

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
	picolRegisterCommand(i, "list", cmd_list);
	picolRegisterCommand(i, "lappend", cmd_lappend);
	picolRegisterCommand(i, "append", cmd_append);
	picolRegisterCommand(i, "incr", cmd_incr);
	picolRegisterCommand(i, "foreach", cmd_foreach);
	picolRegisterCommand(i, "for", cmd_for);
	picolRegisterCommand(i, "catch", cmd_catch);
	picolRegisterCommand(i, "eval", cmd_eval);
	picolRegisterCommand(i, "string", cmd_string);
	picolRegisterCommand(i, "exec", cmd_exec);
	picolRegisterCommand(i, "getenv", cmd_getenv);
}
