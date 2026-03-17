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

/*
 * This file is #included from kiln.c after picol_ext.c,
 * so all picol types, functions, and macros are already visible.
 */

/* Per-rule state, accumulated during body evaluation */
static char *rule_inputs;       /* space-separated input list */
static char *rule_command;      /* command string */
static char *rule_depfile;      /* depfile path or NULL */

static int
cmd_rule_inputs(struct picolInterp *i, int argc,
                       char **argv, struct picolCmd *cmd)
{
	if (argc < 2) return picolArityErr(i, argv[0]);

	/* Concatenate all args as space-separated list */
	size_t total = 0;

	for (int j = 1; j < argc; j++) total += strlen(argv[j]) + 1;

	size_t existing = rule_inputs ? strlen(rule_inputs) : 0;
	rule_inputs = realloc(rule_inputs, existing + total + 1);
	char *p = rule_inputs + existing;

	for (int j = 1; j < argc; j++) {
		if (existing > 0 || j > 1) *p++ = ' ';

		size_t l = strlen(argv[j]);
		memcpy(p, argv[j], l);
		p += l;
	}

	*p = '\0';

	/* Update $in variable */
	picolSetVar(i, "in", rule_inputs);
	return PICOL_OK;
}

static char *rule_command_template;  /* raw template, evaluated after body */

static int
cmd_rule_command(struct picolInterp *i, int argc,
                        char **argv, struct picolCmd *cmd)
{
	if (argc != 2) return picolArityErr(i, argv[0]);

	free(rule_command_template);
	rule_command_template = xstrdup(argv[1]);
	return PICOL_OK;
}

static int
cmd_rule_depfile(struct picolInterp *i, int argc,
                        char **argv, struct picolCmd *cmd)
{
	if (argc != 2) return picolArityErr(i, argv[0]);

	free(rule_depfile);
	rule_depfile = xstrdup(argv[1]);
	picolSetVar(i, "depfile", rule_depfile);
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
	if (argc != 3) return picolArityErr(i, argv[0]);

	char *output_list = argv[1];
	char *body = argv[2];

	/* Initialize per-rule state */
	rule_inputs = NULL;
	rule_command = NULL;
	rule_command_template = NULL;
	rule_depfile = NULL;

	/* Set $out before evaluating the body */
	picolSetVar(i, "out", output_list);
	picolSetVar(i, "in", "");
	picolSetVar(i, "depfile", "");

	/* Register sub-commands for the body */
	picolRegisterCommand(i, "inputs", cmd_rule_inputs);
	picolRegisterCommand(i, "command", cmd_rule_command);
	picolRegisterCommand(i, "depfile", cmd_rule_depfile);

	/* Evaluate the body - this collects inputs, depfile, and command template */
	int ret = picolEval(i, body);

	if (ret != PICOL_OK) {
		free(rule_inputs);
		free(rule_command_template);
		free(rule_depfile);
		return ret;
	}

	if (!rule_command_template) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s:%d: rule '%s': no command specified",
		         i->current_file ? i->current_file : "<eval>",
		         i->current_line, output_list);
		picolSetResult(i, buf);
		free(rule_inputs);
		free(rule_depfile);
		return PICOL_ERR;
	}

	/* Evaluate the command template with all variables ($out, $in, $depfile)
	 * now set. The template was passed in braces {}, so Tcl didn't substitute
	 * it yet. We wrap it as: set _ "..." to trigger substitution now. */
	{
		/* Need to escape any double-quotes or backslashes in the template
		 * for safe wrapping. Easier: use picolEval on a subst-like approach.
		 * Build: set _ "<template>" */
		size_t tlen = strlen(rule_command_template);
		char *eval_cmd = xmalloc(tlen + 9);
		memcpy(eval_cmd, "set _ \"", 7);
		memcpy(eval_cmd + 7, rule_command_template, tlen);
		eval_cmd[7 + tlen] = '"';
		eval_cmd[7 + tlen + 1] = '\0';
		ret = picolEval(i, eval_cmd);
		free(eval_cmd);
		free(rule_command_template);
		rule_command_template = NULL;

		if (ret != PICOL_OK) {
			free(rule_inputs);
			free(rule_depfile);
			return ret;
		}

		rule_command = xstrdup(i->result);
	}

	/* Build DAG nodes */
	int cmd_id = dag_add_cmd(g_dag, rule_command, rule_depfile);

	/* Parse and add input file nodes -> edges to command */
	if (rule_inputs) {
		const char *p = rule_inputs;
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

	/* Parse and add output file nodes - command -> edges to outputs */
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

	free(rule_inputs);
	free(rule_command);
	free(rule_depfile);
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

/***************************************************************************
 * Register all kiln build commands
 **************************************************************************/
void
kiln_register_commands(struct picolInterp *i)
{
	picolRegisterCommand(i, "rule", cmd_rule);
	picolRegisterCommand(i, "source", cmd_source);
}
