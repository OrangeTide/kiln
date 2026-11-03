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

	// Evaluate the command template with all variables ($out, $in, $depfile)
	// now set. The template was passed in braces {}, so Tcl didn't substitute
	// it yet. We wrap it as: set _ "..." to trigger substitution now.
	{
		size_t tlen = strlen(g_rule.command_template);
		char *eval_cmd = xmalloc(tlen + 9);
		memcpy(eval_cmd, "set _ \"", 7);
		memcpy(eval_cmd + 7, g_rule.command_template, tlen);
		eval_cmd[7 + tlen] = '"';
		eval_cmd[7 + tlen + 1] = '\0';
		ret = picolEval(i, eval_cmd);
		free(eval_cmd);
		free(g_rule.command_template);
		g_rule.command_template = NULL;

		if (ret != PICOL_OK) {
			free(output_list);
			rule_state_free(&g_rule);
			return ret;
		}

		g_rule.command = xstrdup(i->result);
	}

	// Build DAG nodes
	int cmd_id = dag_add_cmd(g_dag, g_rule.command, g_rule.depfile);

	// Parse and add input file nodes -> edges to command
	if (g_rule.inputs) {
		const char *p = g_rule.inputs;
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

	// Tool dependencies: added as command inputs so staleness and
	// ordering apply, but they were never added to $in.
	if (g_rule.tools) {
		const char *p = g_rule.tools;
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
 * Register all kiln build commands
 **************************************************************************/
void
kiln_register_commands(struct picolInterp *i)
{
	picolRegisterCommand(i, "rule", cmd_rule);
	picolRegisterCommand(i, "source", cmd_source);
	picolRegisterCommand(i, "builddir", cmd_builddir);
}
