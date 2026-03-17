/* execute.h : build execution with hybrid staleness detection */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_EXECUTE_H
#define KILN_EXECUTE_H

#include "dag.h"

/* build_config holds the paths and flags for a single build invocation. */
struct build_config {
	const char *build_dir;  /* e.g. "build/x86_64-linux-gnu" */
	const char *cdb_path;   /* e.g. "build/x86_64-linux-gnu/kiln.cdb" */
	int dry_run;
};

/* build_execute runs the build plan. Returns 0 on success, 1 on failure. */
int build_execute(struct dag *g, struct build_config *cfg);

/* build_clean deletes all output files declared in the DAG and removes
 * empty parent directories. Also removes the CDB file. */
int build_clean(struct dag *g, struct build_config *cfg);

#endif
