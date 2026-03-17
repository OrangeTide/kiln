/* dag.c : dependency graph implementation */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "dag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
dag_strdup(const char *s)
{
	if (!s) return NULL;

	size_t l = strlen(s);
	char *d = malloc(l + 1);

	if (!d) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	memcpy(d, s, l + 1);
	return d;
}

void
dag_init(struct dag *g)
{
	g->capacity = 64;
	g->count = 0;
	g->nodes = malloc(sizeof(struct dag_node *) * g->capacity);
	g->order = NULL;
	g->order_count = 0;
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
}

static int
dag_add_node(struct dag *g, int type, const char *name)
{
	if (g->count >= g->capacity) {
		g->capacity *= 2;
		g->nodes = realloc(g->nodes, sizeof(struct dag_node *) * g->capacity);
	}

	struct dag_node *n = malloc(sizeof(*n));
	n->id = g->count;
	n->type = type;
	n->state = STATE_CLEAN;
	n->name = dag_strdup(name);
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
	return dag_add_node(g, NODE_FILE, path);
}

int
dag_add_cmd(struct dag *g, const char *command, const char *depfile)
{
	int id = dag_add_node(g, NODE_CMD, NULL);
	g->nodes[id]->command = dag_strdup(command);
	g->nodes[id]->depfile = dag_strdup(depfile);
	return id;
}

int
dag_find_file(struct dag *g, const char *path)
{
	for (int i = 0; i < g->count; i++) {
		if (g->nodes[i]->type == NODE_FILE &&
		    strcmp(g->nodes[i]->name, path) == 0)
			return i;
	}

	return -1;
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
	struct dag_edge *e = malloc(sizeof(*e));
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
	g->order = malloc(sizeof(int) * g->count);
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
