/* dag.c : dependency graph implementation */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "dag.h"
#include "hash.h"
#include "xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/***************************************************************************
 * File-name hash table (open-addressing, linear probing)
 **************************************************************************/

#define HTAB_INITIAL_CAP 128
#define HTAB_LOAD_NUM    7   // resize when count * 10 >= cap * LOAD_NUM
#define HTAB_LOAD_DEN    10

static void
htab_init(struct dag *g)
{
	g->htab_cap = HTAB_INITIAL_CAP;
	g->htab_count = 0;
	g->htab = xmalloc(sizeof(struct dag_htab_entry) * g->htab_cap);

	for (int i = 0; i < g->htab_cap; i++)
		g->htab[i].node_id = -1;
}

static void
htab_resize(struct dag *g)
{
	int old_cap = g->htab_cap;
	struct dag_htab_entry *old = g->htab;

	g->htab_cap *= 2;
	g->htab = xmalloc(sizeof(struct dag_htab_entry) * g->htab_cap);

	for (int i = 0; i < g->htab_cap; i++)
		g->htab[i].node_id = -1;

	for (int i = 0; i < old_cap; i++) {
		if (old[i].node_id < 0) continue;

		uint32_t h = djb2((const unsigned char *)old[i].key,
		                   strlen(old[i].key));
		int slot = h & (g->htab_cap - 1);

		while (g->htab[slot].node_id >= 0)
			slot = (slot + 1) & (g->htab_cap - 1);

		g->htab[slot] = old[i];
	}

	free(old);
}

static void
htab_insert(struct dag *g, const char *name, int node_id)
{
	if (g->htab_count * HTAB_LOAD_DEN >= g->htab_cap * HTAB_LOAD_NUM)
		htab_resize(g);

	uint32_t h = djb2((const unsigned char *)name, strlen(name));
	int slot = h & (g->htab_cap - 1);

	while (g->htab[slot].node_id >= 0)
		slot = (slot + 1) & (g->htab_cap - 1);

	g->htab[slot].key = name;
	g->htab[slot].node_id = node_id;
	g->htab_count++;
}

static int
htab_find(struct dag *g, const char *name)
{
	uint32_t h = djb2((const unsigned char *)name, strlen(name));
	int slot = h & (g->htab_cap - 1);

	while (g->htab[slot].node_id >= 0) {
		if (strcmp(g->htab[slot].key, name) == 0)
			return g->htab[slot].node_id;

		slot = (slot + 1) & (g->htab_cap - 1);
	}

	return -1;
}

/***************************************************************************
 * DAG construction
 **************************************************************************/

void
dag_init(struct dag *g)
{
	g->capacity = 64;
	g->count = 0;
	g->nodes = xmalloc(sizeof(struct dag_node *) * g->capacity);
	g->order = NULL;
	g->order_count = 0;
	htab_init(g);
}

static void
free_edges(struct dag_edge *e)
{
	while (e) {
		struct dag_edge *next = e->next;
		free(e);
		e = next;
	}
}

void
dag_free(struct dag *g)
{
	for (int i = 0; i < g->count; i++) {
		struct dag_node *n = g->nodes[i];
		free(n->name);
		free(n->command);
		free(n->depfile);
		free_edges(n->inputs);
		free_edges(n->outputs);
		free(n);
	}

	free(g->nodes);
	free(g->order);
	free(g->htab);
}

static int
dag_add_node(struct dag *g, int type, const char *name)
{
	if (g->count >= g->capacity) {
		g->capacity *= 2;
		g->nodes = xrealloc(g->nodes, sizeof(struct dag_node *) * g->capacity);
	}

	struct dag_node *n = xmalloc(sizeof(*n));
	n->id = g->count;
	n->type = type;
	n->state = STATE_CLEAN;
	n->name = name ? xstrdup(name) : NULL;
	n->command = NULL;
	n->depfile = NULL;
	n->inputs = NULL;
	n->outputs = NULL;
	n->in_degree = 0;
	n->visited = 0;
	g->nodes[g->count] = n;
	return g->count++;
}

int
dag_add_file(struct dag *g, const char *path)
{
	int id = dag_add_node(g, NODE_FILE, path);
	htab_insert(g, g->nodes[id]->name, id);
	return id;
}

int
dag_add_cmd(struct dag *g, const char *command, const char *depfile)
{
	int id = dag_add_node(g, NODE_CMD, NULL);
	g->nodes[id]->command = command ? xstrdup(command) : NULL;
	g->nodes[id]->depfile = depfile ? xstrdup(depfile) : NULL;
	return id;
}

int
dag_add_phony(struct dag *g, const char *name)
{
	return dag_add_node(g, NODE_PHONY, name);
}

int
dag_find_file(struct dag *g, const char *path)
{
	return htab_find(g, path);
}

int
dag_file(struct dag *g, const char *path)
{
	int id = dag_find_file(g, path);

	if (id >= 0) return id;

	return dag_add_file(g, path);
}

static void
add_edge_to_list(struct dag_edge **list, int node_id)
{
	struct dag_edge *e = xmalloc(sizeof(*e));
	e->node_id = node_id;
	e->next = *list;
	*list = e;
}

void
dag_add_edge(struct dag *g, int from_id, int to_id)
{
	add_edge_to_list(&g->nodes[from_id]->outputs, to_id);
	add_edge_to_list(&g->nodes[to_id]->inputs, from_id);
	g->nodes[to_id]->in_degree++;
}

/* DFS-based cycle detection and topological sort */
static int
dfs_visit(struct dag *g, int id, int *stack, int *sp,
          int *cycle_node)
{
	struct dag_node *n = g->nodes[id];

	if (n->visited == 1) {
		/* Back edge - cycle detected */
		*cycle_node = id;
		return ERR;
	}

	if (n->visited == 2) return OK; /* Already processed */

	n->visited = 1; /* Mark in-progress */

	struct dag_edge *e = n->outputs;

	while (e) {
		if (dfs_visit(g, e->node_id, stack, sp, cycle_node) != OK)
			return ERR;

		e = e->next;
	}

	n->visited = 2; /* Done */
	stack[(*sp)++] = id;
	return OK;
}

int
dag_topo_sort(struct dag *g, int *cycle_node)
{
	free(g->order);
	g->order = xmalloc(sizeof(int) * g->count);
	g->order_count = 0;

	for (int i = 0; i < g->count; i++)
		g->nodes[i]->visited = 0;

	int sp = 0;
	*cycle_node = -1;

	for (int i = 0; i < g->count; i++) {
		if (g->nodes[i]->visited == 0) {
			if (dfs_visit(g, i, g->order, &sp, cycle_node) != OK)
				return ERR;
		}
	}

	/* Reverse the stack to get dependency order */
	for (int i = 0; i < sp / 2; i++) {
		int tmp = g->order[i];
		g->order[i] = g->order[sp - 1 - i];
		g->order[sp - 1 - i] = tmp;
	}

	g->order_count = sp;
	return OK;
}

int
dag_has_producer(struct dag *g, int file_id)
{
	struct dag_edge *e = g->nodes[file_id]->inputs;

	while (e) {
		if (g->nodes[e->node_id]->type == NODE_CMD)
			return 1;

		e = e->next;
	}

	return 0;
}

void
dag_print_plan(struct dag *g)
{
	for (int i = 0; i < g->order_count; i++) {
		struct dag_node *n = g->nodes[g->order[i]];

		if (n->type == NODE_CMD) {
			/* Print inputs */
			printf("  inputs:");
			struct dag_edge *e = n->inputs;

			while (e) {
				struct dag_node *in = g->nodes[e->node_id];

				if (in->type == NODE_FILE)
					printf(" %s", in->name);

				e = e->next;
			}

			printf("\n");

			/* Print command */
			printf("  command: %s\n", n->command);

			/* Print outputs */
			printf("  outputs:");
			e = n->outputs;

			while (e) {
				struct dag_node *out = g->nodes[e->node_id];

				if (out->type == NODE_FILE)
					printf(" %s", out->name);

				e = e->next;
			}

			printf("\n\n");
		}
	}
}
