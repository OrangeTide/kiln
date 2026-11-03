/* dag.h : dependency graph types and operations */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_DAG_H
#define KILN_DAG_H

enum {
	NODE_FILE,
	NODE_CMD,
	NODE_PHONY,
};

/*
 * Node states track progress during a single build run.
 * STATE_BUILDING specifically means "executed this run" and is used
 * to propagate dirtiness to downstream commands (cascading rebuilds).
 */
enum {
	STATE_CLEAN,
	STATE_BUILDING,
	STATE_DONE,
	STATE_FAILED,
};

/* dag_edge is a singly-linked edge list used for both inputs and outputs. */
struct dag_edge {
	int node_id;
	struct dag_edge *next;
};

/* dag_node represents either a file or a build command in the graph. */
struct dag_node {
	int id;
	int type;           /* NODE_FILE or NODE_CMD */
	int state;
	char *name;         /* file path (FILE) or NULL (CMD) */

	/* CMD nodes only */
	char *command;      /* shell command to execute */
	char *depfile;      /* depfile path, or NULL */
	char *deffile;      /* Kilnfile where the rule was defined, or NULL */
	int defline;        /* line of the rule's definition, or 0 */

	/* Graph edges */
	struct dag_edge *inputs;    /* nodes this depends on */
	struct dag_edge *outputs;   /* nodes that depend on this */

	/* Topo sort */
	int in_degree;
	int visited;        /* 0=unvisited, 1=in-progress, 2=done */
};

/* dag_htab_entry is a slot in the file-name hash table. */
struct dag_htab_entry {
	const char *key;    /* points into the dag_node's name (not owned) */
	int node_id;        /* -1 = empty slot */
};

/* dag holds the complete dependency graph and its topological ordering. */
struct dag {
	struct dag_node **nodes;
	int count;
	int capacity;

	int *order;         /* topo-sorted node indices */
	int order_count;

	/* File name -> node ID hash table (open-addressing, linear probe) */
	struct dag_htab_entry *htab;
	int htab_cap;
	int htab_count;
};

/* dag_init initializes an empty graph. */
void dag_init(struct dag *g);

/* dag_free releases all memory owned by the graph. */
void dag_free(struct dag *g);

/* dag_add_file adds a file node and returns its id. */
int dag_add_file(struct dag *g, const char *path);

/* dag_add_cmd adds a command node and returns its id. */
int dag_add_cmd(struct dag *g, const char *command, const char *depfile);

/* dag_set_def records the Kilnfile location where a node was defined. */
void dag_set_def(struct dag *g, int id, const char *file, int line);

/* dag_add_phony adds a named phony-target node and returns its id. */
int dag_add_phony(struct dag *g, const char *name);

/* dag_find_file returns the id of a file node, or -1 if not found. */
int dag_find_file(struct dag *g, const char *path);

/* dag_find_phony returns the id of a phony node by name, or -1. */
int dag_find_phony(struct dag *g, const char *name);

/* dag_file finds or creates a file node, returning its id. */
int dag_file(struct dag *g, const char *path);

/* dag_add_edge creates a directed edge from from_id to to_id. */
void dag_add_edge(struct dag *g, int from_id, int to_id);

/*
 * dag_topo_sort performs a DFS-based topological sort. Returns OK on
 * success or ERR if a cycle is detected. On a cycle, the node ids forming
 * it are written to cycle (a caller buffer of at least g->count ints) in
 * order, and *cycle_len is set to their count; the cycle closes from the
 * last back to the first.
 */
int dag_topo_sort(struct dag *g, int *cycle, int *cycle_len);

/* dag_has_producer returns 1 if the file node has a CMD input (is a build output). */
int dag_has_producer(struct dag *g, int file_id);

/* dag_print_plan prints the execution plan to stdout for debugging. */
void dag_print_plan(struct dag *g);

#endif
