/* kiln.c : main entry point */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picol_ext.c"
#include "dag.c"
#include "hash.c"
#include "mapfile.c"
#include "cdb.c"
#include "depfile.c"
#include "term.c"
#include "execute.c"
#include "commands.c"

#include "kiln_stdlib.h"

static char *
read_file(const char *path)
{
	FILE *fp = fopen(path, "r");

	if (!fp) return NULL;

	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *buf = xmalloc(len + 1);
	size_t n = fread(buf, 1, len, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

// Evaluate a Tcl file in the interpreter. Returns 0 on success.
static int
eval_file(struct picolInterp *interp, const char *path)
{
	char *script = read_file(path);

	if (!script) {
		fprintf(stderr, "kiln: cannot open '%s'\n", path);
		return 1;
	}

	free(interp->current_file);
	interp->current_file = xstrdup(path);

	int ret = picolEval(interp, script);
	free(script);

	if (ret != PICOL_OK) {
		fprintf(stderr, "%s\n", interp->result);
		return 1;
	}

	return 0;
}

/*
 * mark_cone marks a node and its entire upstream cone (everything it
 * depends on, transitively) in the needed array.
 */
static void
mark_cone(struct dag *g, int id, int *needed)
{
	if (needed[id]) return;

	needed[id] = 1;

	for (struct dag_edge *e = g->nodes[id]->inputs; e; e = e->next)
		mark_cone(g, e->node_id, needed);
}

static int select_target(struct dag *g, const char *name, int *needed);

/*
 * select_target_list splits a whitespace-separated list of target names
 * and selects each one. Returns 0 if all resolved, 1 otherwise.
 */
static int
select_target_list(struct dag *g, const char *list, int *needed)
{
	const char *p = list;
	int err = 0;

	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n') p++;

		if (!*p) break;

		const char *start = p;

		while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;

		size_t len = p - start;
		char *word = xmalloc(len + 1);
		memcpy(word, start, len);
		word[len] = '\0';

		if (select_target(g, word, needed) != 0) err = 1;

		free(word);
	}

	return err;
}

/*
 * select_target resolves one target name and marks its build cone. An
 * alias expands (recursively) to its target list; otherwise the name is
 * looked up as a phony target, then as a file. Returns 0 on success, 1
 * if the name could not be resolved.
 */
static int
select_target(struct dag *g, const char *name, int *needed)
{
	const char *alias = kiln_lookup_alias(name);

	if (alias)
		return select_target_list(g, alias, needed);

	int id = dag_find_phony(g, name);

	if (id < 0) id = dag_find_file(g, name);

	/* A bare name may refer to a rule output that builddir prefixed, so
	 * fall back to the prefixed path the same way input resolution does. */
	if (id < 0 && kiln_get_builddir() && kiln_get_builddir()[0]) {
		char *prefixed = builddir_prefix(name);
		id = dag_find_file(g, prefixed);
		free(prefixed);
	}

	if (id < 0) {
		fprintf(stderr, "kiln: unknown target '%s'\n", name);
		return 1;
	}

	mark_cone(g, id, needed);
	return 0;
}

#define MAX_LIBS 64

static void
usage(void)
{
	fprintf(stderr, "Usage: kiln [options] [target...]\n"
	        "Options:\n"
	        "  -f, --file <file>    Read build description (default: Kilnfile;\n"
	        "                       repeatable)\n"
	        "  --triplet <triplet>  Set target triplet (skip cc -dumpmachine)\n"
	        "  --variant <name>     Set build variant (default: release)\n"
	        "  --builddir <path>    Set build directory prefix for rule outputs\n"
	        "  -a, --action <name>  Run a built-in action (clean, status)\n"
	        "  --lib <file.tcl>     Load a Tcl library before Kilnfiles\n"
	        "  --no-stdlib          Skip loading the built-in standard library\n"
	        "  -n, --dry-run        Print commands without executing\n"
	        "  -j N                 Parallel jobs (default: CPU count)\n"
	        "  -h, --help           Show this help\n");
}

int
main(int argc, char **argv)
{
	struct picolInterp *interp = picolInitInterp();
	picolRegisterCoreCommands(interp);
	kiln_register_ext_commands(interp);

	term_init();

	struct dag graph;
	dag_init(&graph);
	kiln_set_dag(&graph);
	kiln_register_commands(interp);

	/* Parse command-line arguments */
	const char *triplet = NULL;
	const char *variant = "release";
	const char *builddir = NULL;
	const char *action = NULL;
	const char *libs[MAX_LIBS];
	int lib_count = 0;
	const char *files[MAX_LIBS];
	int file_count = 0;
	const char **targets = xmalloc(sizeof(char *) * (argc > 0 ? argc : 1));
	int target_count = 0;
	int no_stdlib = 0;
	int dry_run = 0;
	int jobs = 0;
	int result = 0;
	int *needed = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--triplet") == 0 && i + 1 < argc) {
			triplet = argv[++i];
		} else if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
			variant = argv[++i];
		} else if (strcmp(argv[i], "--builddir") == 0 && i + 1 < argc) {
			builddir = argv[++i];
		} else if ((strcmp(argv[i], "-f") == 0 ||
		            strcmp(argv[i], "--file") == 0) && i + 1 < argc) {
			if (file_count >= MAX_LIBS) {
				fprintf(stderr, "kiln: too many -f arguments\n");
				result = 1;
				goto cleanup;
			}
			files[file_count++] = argv[++i];
		} else if ((strcmp(argv[i], "-a") == 0 ||
		            strcmp(argv[i], "--action") == 0) && i + 1 < argc) {
			action = argv[++i];
		} else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
			if (lib_count >= MAX_LIBS) {
				fprintf(stderr, "kiln: too many --lib arguments\n");
				result = 1;
				goto cleanup;
			}
			libs[lib_count++] = argv[++i];
		} else if (strcmp(argv[i], "--no-stdlib") == 0) {
			no_stdlib = 1;
		} else if (strcmp(argv[i], "-n") == 0 ||
		           strcmp(argv[i], "--dry-run") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
			jobs = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-h") == 0 ||
		           strcmp(argv[i], "--help") == 0) {
			usage();
			goto cleanup;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "kiln: unknown option '%s'\n", argv[i]);
			usage();
			result = 1;
			goto cleanup;
		} else {
			targets[target_count++] = argv[i];
		}
	}

	/* Default to a single "Kilnfile" when no -f was given */
	if (file_count == 0)
		files[file_count++] = "Kilnfile";

	/* Detect triplet */
	char triplet_buf[256];

	if (!triplet) {
		if (kiln_run_capture("cc -dumpmachine", triplet_buf, sizeof(triplet_buf)) != OK) {
			fprintf(stderr, "kiln: cc -dumpmachine failed. "
			        "Use --triplet to specify the target triplet.\n");
			result = 1;
			goto cleanup;
		}

		triplet = triplet_buf;
	}

	if (jobs <= 0) jobs = kiln_cpu_count();

	/* Export settings as Tcl variables */
	picolSetVar(interp, "Triplet", (char *)triplet);
	picolSetVar(interp, "Variant", (char *)variant);
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", jobs);
		picolSetVar(interp, "Jobs", buf);
	}
	picolSetVar(interp, "DryRun", dry_run ? "1" : "0");

	/* Set builddir from CLI flag (overrides any Kilnfile builddir) */
	if (builddir) {
		kiln_set_builddir_cli(builddir);
		picolSetVar(interp, "BuildDir", (char *)builddir);
	}

	/* Load standard library (unless --no-stdlib) */
	if (!no_stdlib) {
		if (picolEval(interp, kiln_stdlib_tcl) != PICOL_OK) {
			fprintf(stderr, "kiln: standard library error: %s\n",
			        interp->result);
			result = 1;
			goto cleanup;
		}
	}

	/* Load user-specified libraries */
	for (int j = 0; j < lib_count; j++) {
		if (eval_file(interp, libs[j]) != 0) {
			result = 1;
			goto cleanup;
		}
	}

	/* Evaluate build description file(s) */
	for (int j = 0; j < file_count; j++) {
		if (eval_file(interp, files[j]) != 0) {
			result = 1;
			goto cleanup;
		}
	}

	/* Topological sort */
	int cycle_node;

	if (dag_topo_sort(&graph, &cycle_node) != OK) {
		fprintf(stderr, "kiln: dependency cycle detected");

		if (cycle_node >= 0 && graph.nodes[cycle_node]->name)
			fprintf(stderr, " involving '%s'", graph.nodes[cycle_node]->name);

		fprintf(stderr, "\n");
		result = 1;
		goto cleanup;
	}

	/*
	 * Decide which targets to build. Explicit CLI targets win; otherwise
	 * a Kilnfile-declared default is used. With neither, needed stays
	 * NULL and the whole graph is built.
	 */
	if (target_count > 0) {
		needed = xmalloc(sizeof(int) * graph.count);
		memset(needed, 0, sizeof(int) * graph.count);

		for (int t = 0; t < target_count; t++) {
			if (select_target(&graph, targets[t], needed) != 0) {
				result = 1;
				goto cleanup;
			}
		}
	} else {
		const char *def = kiln_get_default();

		if (def && def[0]) {
			needed = xmalloc(sizeof(int) * graph.count);
			memset(needed, 0, sizeof(int) * graph.count);

			if (select_target_list(&graph, def, needed) != 0) {
				result = 1;
				goto cleanup;
			}
		}
	}

	/* Set up build config.
	 * Use builddir if set (from CLI or Kilnfile), otherwise default. */
	char build_dir[4096];
	char cdb_path[4096];
	const char *bd = kiln_get_builddir();
	if (bd && bd[0]) {
		snprintf(build_dir, sizeof(build_dir), "%s", bd);
		snprintf(cdb_path, sizeof(cdb_path), "%s/kiln.cdb", bd);
	} else {
		snprintf(build_dir, sizeof(build_dir), "build/%s", triplet);
		snprintf(cdb_path, sizeof(cdb_path), "build/%s/kiln.cdb", triplet);
	}

	struct build_config cfg;
	cfg.build_dir = build_dir;
	cfg.cdb_path = cdb_path;
	cfg.dry_run = dry_run;
	cfg.jobs = jobs;

	/* Dispatch action or build */
	if (action) {
		if (strcmp(action, "clean") == 0) {
			result = build_clean(&graph, &cfg);
		} else if (strcmp(action, "status") == 0) {
			result = build_status(&graph, &cfg);
		} else {
			fprintf(stderr, "kiln: unknown action '%s'\n", action);
			result = 1;
		}
	} else if (graph.order_count > 0) {
		result = build_execute(&graph, &cfg, needed);
	} else {
		printf("kiln: nothing to do\n");
	}

cleanup:
	free(needed);
	free(targets);
	kiln_free_commands();
	picolFreeInterp(interp);
	dag_free(&graph);
	return result;
}
