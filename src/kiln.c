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

#define MAX_LIBS 64

static void
usage(void)
{
	fprintf(stderr, "Usage: kiln [options] [target...]\n"
	        "Options:\n"
	        "  --triplet <triplet>  Set target triplet (skip cc -dumpmachine)\n"
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
	const char *builddir = NULL;
	const char *action = NULL;
	const char *libs[MAX_LIBS];
	int lib_count = 0;
	int no_stdlib = 0;
	int dry_run = 0;
	int jobs = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--triplet") == 0 && i + 1 < argc) {
			triplet = argv[++i];
		} else if (strcmp(argv[i], "--builddir") == 0 && i + 1 < argc) {
			builddir = argv[++i];
		} else if ((strcmp(argv[i], "-a") == 0 ||
		            strcmp(argv[i], "--action") == 0) && i + 1 < argc) {
			action = argv[++i];
		} else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
			if (lib_count >= MAX_LIBS) {
				fprintf(stderr, "kiln: too many --lib arguments\n");
				picolFreeInterp(interp);
				dag_free(&graph);
				return 1;
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
			picolFreeInterp(interp);
			dag_free(&graph);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "kiln: unknown option '%s'\n", argv[i]);
			usage();
			picolFreeInterp(interp);
			dag_free(&graph);
			return 1;
		} else {
			break; /* remaining args are targets */
		}
	}

	/* Detect triplet */
	char triplet_buf[256];

	if (!triplet) {
		if (kiln_run_capture("cc -dumpmachine", triplet_buf, sizeof(triplet_buf)) != OK) {
			fprintf(stderr, "kiln: cc -dumpmachine failed. "
			        "Use --triplet to specify the target triplet.\n");
			picolFreeInterp(interp);
			dag_free(&graph);
			return 1;
		}

		triplet = triplet_buf;
	}

	if (jobs <= 0) jobs = kiln_cpu_count();

	/* Export settings as Tcl variables */
	picolSetVar(interp, "Triplet", (char *)triplet);
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
		// TODO: load embedded kiln.tcl when available
	}

	/* Load user-specified libraries */
	for (int j = 0; j < lib_count; j++) {
		if (eval_file(interp, libs[j]) != 0) {
			picolFreeInterp(interp);
			dag_free(&graph);
			return 1;
		}
	}

	/* Evaluate Kilnfile(s) passed as arguments */
	for (; i < argc; i++) {
		if (eval_file(interp, argv[i]) != 0) {
			picolFreeInterp(interp);
			dag_free(&graph);
			return 1;
		}
	}

	/* Topological sort */
	int cycle_node;

	if (dag_topo_sort(&graph, &cycle_node) != OK) {
		fprintf(stderr, "kiln: dependency cycle detected");

		if (cycle_node >= 0 && graph.nodes[cycle_node]->name)
			fprintf(stderr, " involving '%s'", graph.nodes[cycle_node]->name);

		fprintf(stderr, "\n");
		picolFreeInterp(interp);
		dag_free(&graph);
		return 1;
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

	/* Dispatch action or build */
	int result = 0;

	if (action) {
		if (strcmp(action, "clean") == 0) {
			result = build_clean(&graph, &cfg);
		} else if (strcmp(action, "status") == 0) {
			fprintf(stderr, "kiln: status action not yet implemented\n");
			result = 1;
		} else {
			fprintf(stderr, "kiln: unknown action '%s'\n", action);
			result = 1;
		}
	} else if (graph.order_count > 0) {
		result = build_execute(&graph, &cfg);
	} else {
		printf("kiln: nothing to do\n");
	}

	picolFreeInterp(interp);
	dag_free(&graph);
	return result;
}
