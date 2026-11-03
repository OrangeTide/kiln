/* commands.c : Kilnfile commands (rule, source) */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "dag.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global DAG - set by kiln main before evaluating Kilnfiles */
static struct dag *g_dag;

void
kiln_set_dag(struct dag *g)
{
	g_dag = g;
}

/* Global build directory prefix - set by builddir command or --builddir flag.
 * When non-NULL, rule outputs and depfiles are automatically prefixed. */
static char *g_builddir;
static int g_builddir_from_cli;  // set when --builddir was used

void
kiln_set_builddir(const char *dir)
{
	free(g_builddir);
	g_builddir = dir ? xstrdup(dir) : NULL;
}

void
kiln_set_builddir_cli(const char *dir)
{
	kiln_set_builddir(dir);
	g_builddir_from_cli = 1;
}

const char *
kiln_get_builddir(void)
{
	return g_builddir;
}

// Prefix a path with builddir. Caller must free the result.
static char *
builddir_prefix(const char *path)
{
	if (!g_builddir || !g_builddir[0])
		return xstrdup(path);

	size_t blen = strlen(g_builddir);
	size_t plen = strlen(path);
	char *result = xmalloc(blen + 1 + plen + 1);
	memcpy(result, g_builddir, blen);
	result[blen] = '/';
	memcpy(result + blen + 1, path, plen + 1);
	return result;
}

/*
 * This file is #included from kiln.c after picol_ext.c,
 * so all picol types, functions, and macros are already visible.
 */

// Per-rule state, accumulated during body evaluation.
// A single static instance is used since rules are evaluated sequentially.
struct rule_state {
	char *inputs;           // space-separated input list
	char *tools;            // space-separated tool deps (not in $in)
	char *command;          // final expanded command string
	char *command_template; // raw template, evaluated after body
	char *depfile;          // depfile path or NULL
};

static struct rule_state g_rule;

static void
rule_state_clear(struct rule_state *rs)
{
	rs->inputs = NULL;
	rs->tools = NULL;
	rs->command = NULL;
	rs->command_template = NULL;
	rs->depfile = NULL;
}

static void
rule_state_free(struct rule_state *rs)
{
	free(rs->inputs);
	free(rs->tools);
	free(rs->command);
	free(rs->command_template);
	free(rs->depfile);
	rule_state_clear(rs);
}

/* Append one resolved path to a space-separated list in *dst.
 * When builddir is set and the prefixed path names a known build output,
 * the prefixed form is stored; otherwise the path is stored as given. */
static void
append_path(char **dst, const char *arg)
{
	char *resolved = NULL;

	if (g_builddir && g_builddir[0]) {
		char *prefixed = builddir_prefix(arg);
		int fid = dag_find_file(g_dag, prefixed);

		if (fid >= 0 && dag_has_producer(g_dag, fid))
			resolved = prefixed;
		else
			free(prefixed);
	}

	const char *name = resolved ? resolved : arg;
	size_t nlen = strlen(name);
	size_t existing = *dst ? strlen(*dst) : 0;
	int need_sep = (existing > 0) ? 1 : 0;

	*dst = xrealloc(*dst, existing + need_sep + nlen + 1);

	if (need_sep)
		(*dst)[existing++] = ' ';

	memcpy(*dst + existing, name, nlen + 1);
	free(resolved);
}

static int
cmd_rule_inputs(struct picolInterp *i, int argc,
                char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	for (int j = 1; j < argc; j++)
		append_path(&g_rule.inputs, argv[j]);

	picolSetVar(i, "in", g_rule.inputs);
	return PICOL_OK;
}

/**
 * uses file ...
 *
 * Declare programs (or other files) a command depends on but that are
 * not passed as inputs. They are tracked for staleness exactly like
 * inputs, so a changed compiler or assembler forces a rebuild, and when
 * a tool is itself a build output it is ordered before the commands that
 * use it. They are kept out of the $in variable.
 */
static int
cmd_rule_uses(struct picolInterp *i, int argc,
              char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	for (int j = 1; j < argc; j++)
		append_path(&g_rule.tools, argv[j]);

	return PICOL_OK;
}

static int
cmd_rule_command(struct picolInterp *i, int argc,
                 char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 2) return picolArityErr(i, argv[0]);

	free(g_rule.command_template);
	g_rule.command_template = xstrdup(argv[1]);
	return PICOL_OK;
}

static int
cmd_rule_depfile(struct picolInterp *i, int argc,
                 char **argv, struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 2) return picolArityErr(i, argv[0]);

	free(g_rule.depfile);
	g_rule.depfile = builddir_prefix(argv[1]);
	picolSetVar(i, "depfile", g_rule.depfile);
	return PICOL_OK;
}

/* Expand a command template with $out/$in/$depfile set. The template
 * arrived in braces (unsubstituted), so wrap it as a double-quoted string
 * to expand $var and [cmd], escaping embedded double quotes so they stay
 * literal. Returns the expanded command (caller frees) or NULL, storing
 * the picol return code in *ret. */
static char *
expand_command(struct picolInterp *i, const char *t, int *ret)
{
	size_t tlen = strlen(t);
	size_t quotes = 0;

	for (size_t k = 0; k < tlen; k++)
		if (t[k] == '"') quotes++;

	char *eval_cmd = xmalloc(7 + tlen + quotes + 2);
	char *w = eval_cmd;
	memcpy(w, "set _ \"", 7);
	w += 7;

	for (size_t k = 0; k < tlen; k++) {
		if (t[k] == '"') *w++ = '\\';

		*w++ = t[k];
	}

	*w++ = '"';
	*w = '\0';
	*ret = picolEval(i, eval_cmd);
	free(eval_cmd);

	if (*ret != PICOL_OK) return NULL;

	return xstrdup(i->result);
}

/* Add a file->command edge for each name in a space-separated list. */
static void
add_input_edges(int cmd_id, const char *names)
{
	if (!names) return;

	const char *p = names;
	const char *start;
	int len;

	while (list_next(&p, &start, &len)) {
		char *name = xmalloc(len + 1);
		memcpy(name, start, len);
		name[len] = '\0';
		int file_id = dag_file(g_dag, name);
		dag_add_edge(g_dag, file_id, cmd_id);
		free(name);
	}
}

/**
 * rule output_or_list { body }
 *
 * Body is evaluated in Tcl context. Within the body, the user
 * calls inputs, command, and depfile to accumulate build state.
 * Before body evaluation, $out is set to the output list and
 * $in/$depfile are empty.
 */
static int
cmd_rule(struct picolInterp *i, int argc, char **argv,
         struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 3) return picolArityErr(i, argv[0]);

	char *body = argv[2];

	/* Capture the rule's definition line before the body advances it. */
	const char *deffile = i->current_file;
	int defline = i->current_line;

	rule_state_clear(&g_rule);

	// Build prefixed output list when builddir is set.
	// Each output in the list gets $builddir/ prepended.
	char *output_list;

	if (g_builddir && g_builddir[0]) {
		const char *p = argv[1];
		const char *start;
		int len;
		size_t total = 0;

		// First pass: compute size
		const char *p2 = argv[1];

		while (list_next(&p2, &start, &len))
			total += strlen(g_builddir) + 1 + len + 1;

		output_list = xmalloc(total + 1);
		char *wp = output_list;
		int first = 1;

		while (list_next(&p, &start, &len)) {
			if (!first) *wp++ = ' ';

			first = 0;
			size_t blen = strlen(g_builddir);
			memcpy(wp, g_builddir, blen);
			wp[blen] = '/';
			memcpy(wp + blen + 1, start, len);
			wp += blen + 1 + len;
		}

		*wp = '\0';
	} else {
		output_list = xstrdup(argv[1]);
	}

	// Set $out before evaluating the body
	picolSetVar(i, "out", output_list);
	picolSetVar(i, "in", "");
	picolSetVar(i, "depfile", "");

	// Register sub-commands for the body
	picolRegisterCommand(i, "inputs", cmd_rule_inputs);
	picolRegisterCommand(i, "uses", cmd_rule_uses);
	picolRegisterCommand(i, "command", cmd_rule_command);
	picolRegisterCommand(i, "depfile", cmd_rule_depfile);

	// Evaluate the body -- this collects inputs, depfile, and command template
	int ret = picolEval(i, body);

	if (ret != PICOL_OK) {
		free(output_list);
		rule_state_free(&g_rule);
		return ret;
	}

	if (!g_rule.command_template) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s:%d: rule '%s': no command specified",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, output_list);
		picolSetResult(i, buf);
		free(output_list);
		rule_state_free(&g_rule);
		return PICOL_ERR;
	}

	g_rule.command = expand_command(i, g_rule.command_template, &ret);
	free(g_rule.command_template);
	g_rule.command_template = NULL;

	if (!g_rule.command) {
		free(output_list);
		rule_state_free(&g_rule);
		return ret;
	}

	// Build DAG nodes
	int cmd_id = dag_add_cmd(g_dag, g_rule.command, g_rule.depfile);
	dag_set_def(g_dag, cmd_id, deffile, defline);

	// Inputs (set $in) and tool deps (kept out of $in) both become
	// file->command edges so staleness and ordering apply.
	add_input_edges(cmd_id, g_rule.inputs);
	add_input_edges(cmd_id, g_rule.tools);

	// Parse and add output file nodes -- command -> edges to outputs
	{
		const char *p = output_list;
		const char *start;
		int len;

		while (list_next(&p, &start, &len)) {
			char *name = xmalloc(len + 1);
			memcpy(name, start, len);
			name[len] = '\0';
			int file_id = dag_file(g_dag, name);
			dag_add_edge(g_dag, cmd_id, file_id);
			free(name);
		}
	}

	free(output_list);
	rule_state_free(&g_rule);
	return PICOL_OK;
}

/**
 * phony name { body }
 *
 * Declare a named target that runs a command but produces no file.
 * The body uses inputs, uses, and command as in rule, and $out is set
 * to the target name. A phony target has no output to prove it is up to
 * date, so it runs whenever the build reaches it.
 */
static int
cmd_phony(struct picolInterp *i, int argc, char **argv,
          struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 3) return picolArityErr(i, argv[0]);

	const char *name = argv[1];
	char *body = argv[2];
	const char *deffile = i->current_file;
	int defline = i->current_line;

	rule_state_clear(&g_rule);

	picolSetVar(i, "out", (char *)name);
	picolSetVar(i, "in", "");
	picolSetVar(i, "depfile", "");

	picolRegisterCommand(i, "inputs", cmd_rule_inputs);
	picolRegisterCommand(i, "uses", cmd_rule_uses);
	picolRegisterCommand(i, "command", cmd_rule_command);
	picolRegisterCommand(i, "depfile", cmd_rule_depfile);

	int ret = picolEval(i, body);

	if (ret != PICOL_OK) {
		rule_state_free(&g_rule);
		return ret;
	}

	if (!g_rule.command_template) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s:%d: phony '%s': no command specified",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, name);
		picolSetResult(i, buf);
		rule_state_free(&g_rule);
		return PICOL_ERR;
	}

	g_rule.command = expand_command(i, g_rule.command_template, &ret);
	free(g_rule.command_template);
	g_rule.command_template = NULL;

	if (!g_rule.command) {
		rule_state_free(&g_rule);
		return ret;
	}

	int cmd_id = dag_add_cmd(g_dag, g_rule.command, g_rule.depfile);
	dag_set_def(g_dag, cmd_id, deffile, defline);
	add_input_edges(cmd_id, g_rule.inputs);
	add_input_edges(cmd_id, g_rule.tools);

	int phony_id = dag_add_phony(g_dag, name);
	dag_add_edge(g_dag, cmd_id, phony_id);

	rule_state_free(&g_rule);
	return PICOL_OK;
}

/**
 * source filename
 *
 * Searches for filename in:
 *   1. Current directory
 *   2. Directories in $SourcePath (space-separated list)
 */
static int
cmd_source(struct picolInterp *i, int argc, char **argv,
                   struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 2) return picolArityErr(i, argv[0]);

	char *filename = argv[1];
	char path[4096];
	FILE *fp = NULL;

	/* Try as-is first (absolute or relative path) */
	fp = fopen(filename, "r");

	/* If not found, search $SourcePath */
	if (!fp) {
		struct picolVar *v = picolGetVar(i, "SourcePath");

		if (v && v->val[0]) {
			const char *p = v->val;
			const char *start;
			int len;

			while (list_next(&p, &start, &len)) {
				if (len + 1 + (int)strlen(filename) >= (int)sizeof(path))
					continue;

				memcpy(path, start, len);
				path[len] = '/';
				strcpy(path + len + 1, filename);
				fp = fopen(path, "r");

				if (fp) {
					filename = path;
					break;
				}
			}
		}
	}

	if (!fp) {
		char buf[4096];
		snprintf(buf, sizeof(buf), "%s:%d: source: cannot open '%s'",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, argv[1]);
		picolSetResult(i, buf);
		return PICOL_ERR;
	}

	/* Read and evaluate */
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *script = xmalloc(sz + 1);
	size_t n = fread(script, 1, sz, fp);
	script[n] = '\0';
	fclose(fp);

	/* Save and set current_file for error reporting */
	char *saved_file = i->current_file;
	i->current_file = xstrdup(filename);

	int ret = picolEval(i, script);
	free(script);

	free(i->current_file);
	i->current_file = saved_file;

	return ret;
}

/**
 * builddir path
 *
 * Set the build directory prefix. Rule outputs and depfiles are
 * automatically prefixed with this path. Inputs that reference
 * known build outputs are resolved to the prefixed path.
 * Also sets $BuildDir for use in Tcl expressions.
 */
static int
cmd_builddir(struct picolInterp *i, int argc, char **argv,
                    struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 2) return picolArityErr(i, argv[0]);

	// CLI --builddir takes precedence over Kilnfile builddir
	if (!g_builddir_from_cli) {
		kiln_set_builddir(argv[1]);
		picolSetVar(i, "BuildDir", argv[1]);
	}
	return PICOL_OK;
}

/***************************************************************************
 * Target selection: default targets and aliases, consulted by main.
 **************************************************************************/

struct kiln_alias {
	char *name;
	char *targets;         // space-separated target names
	struct kiln_alias *next;
};

static struct kiln_alias *g_aliases;
static char *g_default;        // space-separated default target names

/* Install mappings: a build output and where it goes under the prefix. */
struct kiln_install {
	char *src;             // build output to install
	char *dest;            // destination path relative to the prefix
	struct kiln_install *next;
};

static struct kiln_install *g_installs;

/* Append a word to a space-separated list, no builddir resolution. */
static void
append_word(char **dst, const char *w)
{
	size_t wl = strlen(w);
	size_t ex = *dst ? strlen(*dst) : 0;
	int sep = ex > 0 ? 1 : 0;

	*dst = xrealloc(*dst, ex + sep + wl + 1);

	if (sep) (*dst)[ex++] = ' ';

	memcpy(*dst + ex, w, wl + 1);
}

/**
 * default target ...
 *
 * Declare the targets built when none are named on the command line.
 */
static int
cmd_default(struct picolInterp *i, int argc, char **argv,
            struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 2) return picolArityErr(i, argv[0]);

	for (int j = 1; j < argc; j++)
		append_word(&g_default, argv[j]);

	return PICOL_OK;
}

/**
 * alias name target ...
 *
 * Define a name that expands to one or more targets, so building the
 * name builds them all.
 */
static int
cmd_alias(struct picolInterp *i, int argc, char **argv,
          struct picolCmd *cmd)
{
	(void)cmd;
	if (argc < 3) return picolArityErr(i, argv[0]);

	char *targets = NULL;

	for (int j = 2; j < argc; j++)
		append_word(&targets, argv[j]);

	struct kiln_alias *a = xmalloc(sizeof(*a));
	a->name = xstrdup(argv[1]);
	a->targets = targets ? targets : xstrdup("");
	a->next = g_aliases;
	g_aliases = a;
	return PICOL_OK;
}

/* kiln_lookup_alias returns the targets an alias expands to, or NULL. */
const char *
kiln_lookup_alias(const char *name)
{
	for (struct kiln_alias *a = g_aliases; a; a = a->next)
		if (strcmp(a->name, name) == 0) return a->targets;

	return NULL;
}

/* kiln_get_default returns the declared default targets, or NULL. */
const char *
kiln_get_default(void)
{
	return g_default;
}

/**
 * install src dest
 *
 * Record that the build output src is installed to dest, a path relative
 * to the install prefix. The `install` action copies it there.
 */
static int
cmd_install(struct picolInterp *i, int argc, char **argv,
            struct picolCmd *cmd)
{
	(void)cmd;
	if (argc != 3) return picolArityErr(i, argv[0]);

	struct kiln_install *n = xmalloc(sizeof(*n));
	n->src = xstrdup(argv[1]);
	n->dest = xstrdup(argv[2]);
	n->next = g_installs;
	g_installs = n;
	return PICOL_OK;
}

/*
 * kiln_install copies every declared install mapping to its destination
 * under destdir + prefix, creating directories and preserving modes. src
 * is resolved against the build directory the same way inputs are, so a
 * bare output name works. Returns 0 on success, 1 if any copy failed.
 */
int
kiln_install(struct dag *g, const char *prefix, const char *destdir)
{
	if (!g_installs) {
		fprintf(stderr, "kiln: nothing to install "
		        "(no 'install' declarations)\n");
		return 1;
	}

	int failed = 0;

	/* Reverse into declaration order for stable, readable output. */
	struct kiln_install *rev = NULL;

	for (struct kiln_install *n = g_installs; n; n = n->next) {
		struct kiln_install *c = xmalloc(sizeof(*c));
		c->src = n->src;
		c->dest = n->dest;
		c->next = rev;
		rev = c;
	}

	for (struct kiln_install *n = rev; n;) {
		/* Resolve src to the built file: a bare name becomes the
		 * builddir-prefixed output when that names a known node. */
		char *resolved = NULL;

		if (g_builddir && g_builddir[0]) {
			char *prefixed = builddir_prefix(n->src);

			if (dag_find_file(g, prefixed) >= 0)
				resolved = prefixed;
			else
				free(prefixed);
		}

		const char *src = resolved ? resolved : n->src;

		char dst[4096];
		snprintf(dst, sizeof(dst), "%s%s/%s", destdir, prefix, n->dest);

		/* Create the destination directory. */
		const char *last_sep = NULL;

		for (const char *p = dst; *p; p++)
			if (*p == '/') last_sep = p;

		if (last_sep) {
			char dir[4096];
			size_t dlen = last_sep - dst;

			if (dlen < sizeof(dir)) {
				memcpy(dir, dst, dlen);
				dir[dlen] = '\0';
				kiln_mkdir_p(dir);
			}
		}

		if (kiln_copy_file(src, dst) == OK) {
			printf("%sinstall%s %s -> %s\n",
			       term_green(), term_reset(), src, dst);
		} else {
			fprintf(stderr, "%skiln: cannot install %s -> %s%s\n",
			        term_red(), src, dst, term_reset());
			failed = 1;
		}

		free(resolved);

		struct kiln_install *next = n->next;
		free(n);
		n = next;
	}

	return failed ? 1 : 0;
}

/* kiln_free_commands releases alias and default storage. */
void
kiln_free_commands(void)
{
	struct kiln_alias *a = g_aliases;

	while (a) {
		struct kiln_alias *next = a->next;
		free(a->name);
		free(a->targets);
		free(a);
		a = next;
	}

	g_aliases = NULL;
	free(g_default);
	g_default = NULL;

	struct kiln_install *n = g_installs;

	while (n) {
		struct kiln_install *next = n->next;
		free(n->src);
		free(n->dest);
		free(n);
		n = next;
	}

	g_installs = NULL;
}

/***************************************************************************
 * Register all kiln build commands
 **************************************************************************/
void
kiln_register_commands(struct picolInterp *i)
{
	picolRegisterCommand(i, "rule", cmd_rule);
	picolRegisterCommand(i, "phony", cmd_phony);
	picolRegisterCommand(i, "default", cmd_default);
	picolRegisterCommand(i, "alias", cmd_alias);
	picolRegisterCommand(i, "install", cmd_install);
	picolRegisterCommand(i, "source", cmd_source);
	picolRegisterCommand(i, "builddir", cmd_builddir);
}
