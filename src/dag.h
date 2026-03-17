/* dag.h : dependency graph types and operations */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_DAG_H
#define KILN_DAG_H

enum {
	NODE_FILE,
	NODE_CMD
};

/*
 * Node states track progress during a single build run.
 * STATE_BUILDING specifically means "executed this run" and is used
 * to propagate dirtiness to downstream commands (cascading rebuilds).
 */
enum {
	STATE_CLEAN,
	STATE_DIRTY,
	STATE_BUILDING,
	STATE_DONE,
	STATE_FAILED
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

	/* Graph edges */
	struct dag_edge *inputs;    /* nodes this depends on */
	struct dag_edge *outputs;   /* nodes that depend on this */

	/* Topo sort */
	int in_degree;
	int visited;        /* 0=unvisited, 1=in-progress, 2=done */
};

/* dag holds the complete dependency graph and its topological ordering. */
struct dag {
	struct dag_node **nodes;
	int count;
	int capacity;

	int *order;         /* topo-sorted node indices */
	int order_count;
};

/* dag_init initializes an empty graph. */
void dag_init(struct dag *g);

/* dag_free releases all memory owned by the graph. */
void dag_free(struct dag *g);

/* dag_add_file adds a file node and returns its id. */
int dag_add_file(struct dag *g, const char *path);

/* dag_add_cmd adds a command node and returns its id. */
int dag_add_cmd(struct dag *g, const char *command, const char *depfile);

/* dag_find_file returns the id of a file node, or -1 if not found. */
int dag_find_file(struct dag *g, const char *path);

/* dag_file finds or creates a file node, returning its id. */
int dag_file(struct dag *g, const char *path);

/* dag_add_edge creates a directed edge from from_id to to_id. */
void dag_add_edge(struct dag *g, int from_id, int to_id);

/*
 * dag_topo_sort performs a DFS-based topological sort. Returns OK on
 * success or ERR if a cycle is detected, in which case cycle_node is
 * set to the id of a node involved in the cycle.
 */
int dag_topo_sort(struct dag *g, int *cycle_node);

/* dag_print_plan prints the execution plan to stdout for debugging. */
void dag_print_plan(struct dag *g);

#endif
