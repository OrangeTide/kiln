/* execute.c : build execution with hybrid staleness detection */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "execute.h"
#include "cdb.h"
#include "hash.h"
#include "depfile.h"
#include "platform.h"
#include "dag.h"
#include "term.h"
#include "xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * CDB key/value layout:
 *
 * Key: "stat:<path>"
 * Value: struct stored_stat (binary)
 *
 * Key: "hash:<path>"
 * Value: uint32_t (content hash)
 *
 * Key: "cmd:<output>"
 * Value: uint32_t (command string hash)
 *
 * Key: "deps:<output>"
 * Value: null-separated list of implicit dep paths
 */

struct stored_stat {
	long long mtime_sec;
	long mtime_nsec;
	long long size;
	unsigned long long ino;
	long long ctime_sec;
	long ctime_nsec;
};

static void
stat_to_stored(const struct kiln_stat *s, struct stored_stat *st)
{
	st->mtime_sec = s->mtime_sec;
	st->mtime_nsec = s->mtime_nsec;
	st->size = s->size;
	st->ino = s->ino;
	st->ctime_sec = s->ctime_sec;
	st->ctime_nsec = s->ctime_nsec;
}

static int
stat_equal(const struct stored_stat *a, const struct stored_stat *b)
{
	return a->mtime_sec == b->mtime_sec &&
	       a->mtime_nsec == b->mtime_nsec &&
	       a->size == b->size &&
	       a->ino == b->ino &&
	       a->ctime_sec == b->ctime_sec &&
	       a->ctime_nsec == b->ctime_nsec;
}

/**
 * file_is_dirty checks whether a file has changed since the last build.
 *
 * Uses a two-level check: stat fingerprint first (fast path), then
 * content hash if stat differs. Records new state in new_db regardless.
 * Returns 1 if dirty, 0 if unchanged.
 */
static int
file_is_dirty(const char *path, struct cdb *old_db,
              struct cdb_writer *new_db)
{
	struct kiln_stat st;

	if (kiln_stat_file(path, &st) != OK) {
		/* Can't stat - treat as dirty (might be a source file that
		 * doesn't exist yet, which is an error, but let the command
		 * fail with a clear message). */
		return 1;
	}

	struct stored_stat current;

	stat_to_stored(&st, &current);

	/* Look up old stat fingerprint */
	char key[4096];

	snprintf(key, sizeof(key), "stat:%s", path);

	size_t vlen;

	const void *old_val = cdb_find(old_db, key, strlen(key), &vlen);

	if (old_val && vlen == sizeof(struct stored_stat)) {
		const struct stored_stat *old_stat = old_val;

		if (stat_equal(&current, old_stat)) {
			/* Stat matches - file unchanged (fast path) */
			/* Carry forward to new DB */
			cdb_writer_add(new_db, key, strlen(key), &current, sizeof(current));

			/* Also carry forward content hash */
			snprintf(key, sizeof(key), "hash:%s", path);
			old_val = cdb_find(old_db, key, strlen(key), &vlen);

			if (old_val && vlen == sizeof(uint32_t))
				cdb_writer_add(new_db, key, strlen(key), old_val, vlen);

			return 0;
		}
	}

	/* Stat differs - check content hash */
	uint32_t new_hash;

	if (hash_file(path, &new_hash) != OK)
		return 1; /* can't hash, treat as dirty */

	snprintf(key, sizeof(key), "hash:%s", path);
	old_val = cdb_find(old_db, key, strlen(key), &vlen);

	/* Store new stat + hash regardless */
	snprintf(key, sizeof(key), "stat:%s", path);
	cdb_writer_add(new_db, key, strlen(key), &current, sizeof(current));
	snprintf(key, sizeof(key), "hash:%s", path);
	cdb_writer_add(new_db, key, strlen(key), &new_hash, sizeof(new_hash));

	if (old_val && vlen == sizeof(uint32_t)) {
		uint32_t old_hash;
		memcpy(&old_hash, old_val, sizeof(old_hash));

		if (old_hash == new_hash)
			return 0; /* Content unchanged despite stat change (e.g. touch) */
	}

	return 1; /* Genuinely dirty */
}

/* Delete all output files for a command node */
static void
delete_outputs(struct dag *g, struct dag_node *cmd_node)
{
	struct dag_edge *e = cmd_node->outputs;

	while (e) {
		struct dag_node *out = g->nodes[e->node_id];

		if (out->type == NODE_FILE) {
			kiln_unlink(out->name);
		}

		e = e->next;
	}
}

/* Store implicit deps from depfile into CDB writer, and snapshot their stats. */
static void
store_depfile_deps(struct cdb_writer *new_db, struct cdb *old_db,
                   const char *output, const char *depfile_path)
{
	if (!depfile_path || !depfile_path[0]) return;

	int dep_count;
	char **deps = depfile_parse(depfile_path, &dep_count);

	if (!deps || dep_count == 0) {
		depfile_free(deps, dep_count);
		return;
	}

	/* Serialize as null-separated list */
	size_t total = 0;

	for (int i = 0; i < dep_count; i++)
		total += strlen(deps[i]) + 1;

	char *val = xmalloc(total);
	char *p = val;

	for (int i = 0; i < dep_count; i++) {
		size_t l = strlen(deps[i]);
		memcpy(p, deps[i], l + 1);
		p += l + 1;
	}

	char key[4096];
	snprintf(key, sizeof(key), "deps:%s", output);
	cdb_writer_add(new_db, key, strlen(key), val, total);
	free(val);

	/* Snapshot stat+hash for each implicit dep so staleness works next run */
	for (int i = 0; i < dep_count; i++)
		file_is_dirty(deps[i], old_db, new_db);

	depfile_free(deps, dep_count);
}

/* Check if any implicit dep (from previous depfile) is dirty */
static int
implicit_deps_dirty(const char *output, struct cdb *old_db,
                    struct cdb_writer *new_db)
{
	char key[4096];
	snprintf(key, sizeof(key), "deps:%s", output);
	size_t vlen;
	const void *val = cdb_find(old_db, key, strlen(key), &vlen);

	if (!val || vlen == 0) return 0; /* No stored deps - not dirty from this */

	const char *p = val;
	const char *end = p + vlen;

	while (p < end) {
		size_t l = strlen(p);

		if (l > 0 && file_is_dirty(p, old_db, new_db))
			return 1;

		p += l + 1;
	}

	return 0;
}

/* Try to remove path and then each parent directory, stopping when a
 * removal fails (directory not empty) or we reach stop_at. */
static void
remove_empty_parents(const char *path, const char *stop_at)
{
	char buf[4096];
	size_t len = strlen(path);

	if (len >= sizeof(buf)) return;

	memcpy(buf, path, len + 1);

	/* Walk up from the path itself */
	while (len > 0) {
		/* Try removing the current directory */
		if (kiln_rmdir(buf) != OK) break;

		/* Find parent */
		while (len > 0 && buf[len - 1] != '/')
			len--;

		if (len > 0) len--; /* Remove trailing slash */

		buf[len] = '\0';

		/* Stop if we've reached the stop directory */
		if (len == 0) break;

		if (stop_at && strcmp(buf, stop_at) == 0) {
			kiln_rmdir(buf);
			break;
		}
	}
}

int
build_clean(struct dag *g, struct build_config *cfg)
{
	int removed = 0;

	for (int i = 0; i < g->count; i++) {
		struct dag_node *n = g->nodes[i];

		if (n->type != NODE_FILE) continue;
		if (!dag_has_producer(g, n->id)) continue;

		struct kiln_stat st;

		if (kiln_stat_file(n->name, &st) == OK) {
			if (kiln_unlink(n->name) == OK) {
				printf("%srm%s %s\n",
				       term_red(), term_reset(), n->name);
				removed++;
			}
		}
	}

	/* Remove CDB file */
	kiln_unlink(cfg->cdb_path);

	/* Remove empty parent directories, walking up to "build" */
	for (int i = 0; i < g->count; i++) {
		struct dag_node *n = g->nodes[i];

		if (n->type != NODE_FILE) continue;
		if (!dag_has_producer(g, n->id)) continue;

		/* Find the directory portion */
		const char *last_sep = NULL;

		for (const char *p = n->name; *p; p++)
			if (*p == '/') last_sep = p;

		if (last_sep) {
			char dir[4096];
			size_t dlen = last_sep - n->name;

			if (dlen < sizeof(dir)) {
				memcpy(dir, n->name, dlen);
				dir[dlen] = '\0';
				remove_empty_parents(dir, NULL);
			}
		}
	}

	/* Try to remove the build directory itself */
	remove_empty_parents(cfg->build_dir, NULL);

	if (removed > 0)
		printf("%skiln:%s removed %d file%s\n",
		       term_green(), term_reset(),
		       removed, removed == 1 ? "" : "s");
	else
		printf("%skiln:%s nothing to clean\n",
		       term_green(), term_reset());

	return 0;
}

// Find the first FILE output of a command node.
static const char *
first_file_output(struct dag *g, struct dag_node *n)
{
	struct dag_edge *e = n->outputs;

	while (e) {
		if (g->nodes[e->node_id]->type == NODE_FILE)
			return g->nodes[e->node_id]->name;

		e = e->next;
	}

	return NULL;
}

// Check whether a command node needs to be rebuilt.
// Sets *out_first_output and *out_cmd_hash for use by the caller.
// When not dirty, carries forward the old CDB state into new_db.
// Returns 1 if dirty, 0 if clean.
static int
node_is_dirty(struct dag *g, struct dag_node *n,
              struct cdb *old_db, int have_old_db,
              struct cdb_writer *new_db,
              const char **out_first_output, uint32_t *out_cmd_hash)
{
	uint32_t cmd_hash = hash_string(n->command);
	const char *first_output = first_file_output(g, n);

	*out_first_output = first_output;
	*out_cmd_hash = cmd_hash;

	// Check if command string changed
	int cmd_dirty = 0;

	if (first_output && have_old_db) {
		char key[4096];
		snprintf(key, sizeof(key), "cmd:%s", first_output);
		size_t vlen;
		const void *old_val = cdb_find(old_db, key, strlen(key), &vlen);

		if (!old_val || vlen != sizeof(uint32_t)) {
			cmd_dirty = 1;
		} else {
			uint32_t old_hash;
			memcpy(&old_hash, old_val, sizeof(old_hash));

			if (old_hash != cmd_hash) cmd_dirty = 1;
		}
	} else {
		cmd_dirty = 1; // No old DB or no output -- always build
	}

	// Check explicit inputs.
	// Two conditions make an input dirty:
	// 1. The file's stat/hash changed on disk
	// 2. The file was produced by a command that ran this build
	int input_dirty = 0;
	{
		struct dag_edge *e = n->inputs;

		while (e) {
			struct dag_node *in = g->nodes[e->node_id];

			if (in->type == NODE_FILE) {
				// Check if this file was just rebuilt
				struct dag_edge *pe = in->inputs;

				while (pe) {
					struct dag_node *producer = g->nodes[pe->node_id];

					if (producer->type == NODE_CMD &&
					    producer->state == STATE_BUILDING) {
						input_dirty = 1;
						break;
					}

					pe = pe->next;
				}

				// Check stat/hash if we have an old DB
				if (!input_dirty && !cmd_dirty && have_old_db) {
					if (file_is_dirty(in->name, old_db, new_db))
						input_dirty = 1;
				}
			}

			if (input_dirty) break;

			e = e->next;
		}
	}

	// Check implicit deps (from previous depfile)
	int implicit_dirty = 0;

	if (!cmd_dirty && !input_dirty && have_old_db && first_output) {
		implicit_dirty = implicit_deps_dirty(first_output, old_db, new_db);
	}

	// Check if outputs exist
	int output_missing = 0;
	{
		struct dag_edge *e = n->outputs;

		while (e) {
			struct dag_node *out = g->nodes[e->node_id];

			if (out->type == NODE_FILE) {
				struct kiln_stat st;

				if (kiln_stat_file(out->name, &st) != OK)
					output_missing = 1;
			}

			e = e->next;
		}
	}

	int dirty = cmd_dirty || input_dirty || implicit_dirty || output_missing;

	if (!dirty && first_output) {
		// Carry forward command hash
		char key[4096];
		snprintf(key, sizeof(key), "cmd:%s", first_output);
		cdb_writer_add(new_db, key, strlen(key),
		               &cmd_hash, sizeof(cmd_hash));
		// Carry forward depfile deps
		snprintf(key, sizeof(key), "deps:%s", first_output);
		size_t vlen;
		const void *val = cdb_find(old_db, key, strlen(key), &vlen);

		if (val && vlen > 0)
			cdb_writer_add(new_db, key, strlen(key), val, vlen);
	}

	return dirty;
}

/* Record a successfully built command's new state into the CDB writer:
 * its command hash, any depfile-discovered implicit deps, and the stat
 * plus content hash of every output and explicit input. */
static void
record_built(struct dag *g, struct dag_node *n, struct cdb *old_db,
             struct cdb_writer *new_db, const char *first_output,
             uint32_t cmd_hash)
{
	char key[4096];

	if (first_output) {
		snprintf(key, sizeof(key), "cmd:%s", first_output);
		cdb_writer_add(new_db, key, strlen(key),
		               &cmd_hash, sizeof(cmd_hash));
		store_depfile_deps(new_db, old_db, first_output, n->depfile);
	}

	for (struct dag_edge *e = n->outputs; e; e = e->next) {
		struct dag_node *out = g->nodes[e->node_id];

		if (out->type != NODE_FILE) continue;

		struct kiln_stat st;

		if (kiln_stat_file(out->name, &st) != OK) continue;

		struct stored_stat ss;
		stat_to_stored(&st, &ss);
		snprintf(key, sizeof(key), "stat:%s", out->name);
		cdb_writer_add(new_db, key, strlen(key), &ss, sizeof(ss));

		uint32_t h;

		if (hash_file(out->name, &h) == OK) {
			snprintf(key, sizeof(key), "hash:%s", out->name);
			cdb_writer_add(new_db, key, strlen(key), &h, sizeof(h));
		}
	}

	/* Snapshot explicit inputs so their state is in the new DB even when
	 * node_is_dirty short-circuited before checking them. */
	for (struct dag_edge *e = n->inputs; e; e = e->next) {
		struct dag_node *in = g->nodes[e->node_id];

		if (in->type == NODE_FILE)
			file_is_dirty(in->name, old_db, new_db);
	}
}

/*
 * The scheduler tracks, for each command node, how many producer commands
 * it is still waiting on (pending). A command is ready when pending hits 0.
 * sched_complete is called when a command finishes: it walks that command's
 * consumers and decrements their pending counts, pushing any that reach 0
 * onto the ready stack.
 *
 * mark/epoch deduplicate the consumer walk so a command that consumes two
 * outputs of the finished node is only decremented once.
 */
static void
sched_complete(struct dag *g, struct dag_node *n, int *mark, int *epoch,
               int *pending, int *ready, int *ready_count)
{
	(*epoch)++;

	for (struct dag_edge *e = n->outputs; e; e = e->next) {
		struct dag_node *f = g->nodes[e->node_id];

		if (f->type != NODE_FILE) continue;

		for (struct dag_edge *ce = f->outputs; ce; ce = ce->next) {
			struct dag_node *c = g->nodes[ce->node_id];

			if (c->type != NODE_CMD || mark[c->id] == *epoch) continue;

			mark[c->id] = *epoch;

			if (--pending[c->id] == 0)
				ready[(*ready_count)++] = c->id;
		}
	}
}

/*
 * A running job: the command node, the staleness data computed when it was
 * dispatched, and its child process handle.
 */
struct build_job {
	struct dag_node *n;
	const char *first_output;
	uint32_t cmd_hash;
	kiln_proc *proc;
};

int
build_execute(struct dag *g, struct build_config *cfg)
{
	/* Ensure build directory exists */
	kiln_mkdir_p(cfg->build_dir);

	/* Open old CDB for staleness reads */
	struct cdb old_db;
	int have_old_db = (cdb_open(&old_db, cfg->cdb_path) == OK);

	/* New CDB writer for accumulating state */
	struct cdb_writer new_db;
	cdb_writer_init(&new_db);

	int jobs = cfg->jobs > 0 ? cfg->jobs : 1;

	/*
	 * Readiness bookkeeping, indexed by node id (entries for FILE nodes
	 * are unused). pending[c] counts the distinct producer commands that
	 * command c still waits on; mark/epoch dedup the dependency walks.
	 */
	int *pending = xmalloc(sizeof(int) * g->count);
	int *mark = xmalloc(sizeof(int) * g->count);
	int *ready = xmalloc(sizeof(int) * g->count);
	int ready_count = 0;
	int epoch = 0;
	int total_cmds = 0;

	for (int i = 0; i < g->count; i++)
		mark[i] = 0;

	/* Compute each command's producer count and seed the ready stack. */
	for (int i = 0; i < g->count; i++) {
		struct dag_node *n = g->nodes[i];

		if (n->type != NODE_CMD) continue;

		total_cmds++;
		epoch++;
		int preds = 0;

		for (struct dag_edge *e = n->inputs; e; e = e->next) {
			struct dag_node *f = g->nodes[e->node_id];

			if (f->type != NODE_FILE) continue;

			for (struct dag_edge *pe = f->inputs; pe; pe = pe->next) {
				struct dag_node *p = g->nodes[pe->node_id];

				if (p->type != NODE_CMD || mark[p->id] == epoch)
					continue;

				mark[p->id] = epoch;
				preds++;
			}
		}

		pending[n->id] = preds;

		if (preds == 0)
			ready[ready_count++] = n->id;
	}

	struct build_job *run = xmalloc(sizeof(struct build_job) * jobs);
	int nrun = 0;
	int cmd_index = 0;
	int built = 0;
	int failed = 0;

	/*
	 * Run the DAG. Dispatch ready commands into free job slots up to the
	 * -j limit, then reap the oldest running job. Reaping in dispatch
	 * (FIFO) order keeps the scheduler simple and needs no platform
	 * wait-for-any; a slow job at the head can leave finished jobs
	 * unreaped, so a poll-based wait-any is a future optimization.
	 */
	while (ready_count > 0 || nrun > 0) {
		while (!failed && nrun < jobs && ready_count > 0) {
			struct dag_node *n = g->nodes[ready[--ready_count]];
			const char *first_output;
			uint32_t cmd_hash;

			cmd_index++;

			if (!node_is_dirty(g, n, &old_db, have_old_db, &new_db,
			                   &first_output, &cmd_hash)) {
				n->state = STATE_DONE;
				sched_complete(g, n, mark, &epoch, pending,
				               ready, &ready_count);
				continue;
			}

			if (cfg->dry_run) {
				printf("%s\n", n->command);
				n->state = STATE_DONE;
				sched_complete(g, n, mark, &epoch, pending,
				               ready, &ready_count);
				continue;
			}

			term_progress(cmd_index, total_cmds, n->command);

			kiln_proc *proc;

			if (kiln_spawn(&proc, n->command, NULL) != OK) {
				term_clear_progress();
				fprintf(stderr, "%skiln: failed to spawn:%s %s\n",
				        term_red(), term_reset(), n->command);
				failed = 1;
				break;
			}

			run[nrun].n = n;
			run[nrun].first_output = first_output;
			run[nrun].cmd_hash = cmd_hash;
			run[nrun].proc = proc;
			nrun++;
		}

		if (nrun == 0) break; /* nothing in flight: done or failed */

		/* Reap the oldest running job, then shift the queue down. */
		struct build_job j = run[0];

		for (int k = 1; k < nrun; k++)
			run[k - 1] = run[k];

		nrun--;

		int exitcode;
		kiln_wait(j.proc, &exitcode);

		const char *out;
		size_t outlen;
		kiln_proc_output(j.proc, &out, &outlen);

		if (outlen > 0) {
			term_clear_progress();
			fwrite(out, 1, outlen, exitcode == 0 ? stdout : stderr);
		}

		kiln_proc_free(j.proc);

		if (exitcode != 0) {
			term_clear_progress();
			fprintf(stderr, "%skiln: command failed with exit code %d%s\n",
			        term_red(), exitcode, term_reset());
			delete_outputs(g, j.n);
			j.n->state = STATE_FAILED;
			failed = 1;
			continue; /* stop dispatching; drain remaining jobs */
		}

		j.n->state = STATE_BUILDING; /* marks "executed this run" */
		built++;
		record_built(g, j.n, &old_db, &new_db, j.first_output, j.cmd_hash);
		sched_complete(g, j.n, mark, &epoch, pending, ready, &ready_count);
	}

	free(pending);
	free(mark);
	free(ready);
	free(run);

	/* Write new CDB */
	if (!cfg->dry_run) {
		char tmp_path[4096];
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cfg->cdb_path);

		if (cdb_writer_finish(&new_db, tmp_path) == OK) {
			kiln_rename(tmp_path, cfg->cdb_path);
		}
	}

	cdb_writer_free(&new_db);

	if (have_old_db) cdb_close(&old_db);

	term_clear_progress();

	if (!failed && built == 0 && !cfg->dry_run)
		printf("%skiln:%s nothing to do (all targets up to date)\n",
		       term_green(), term_reset());
	else if (!failed && built > 0)
		printf("%skiln:%s built %d target%s\n",
		       term_green(), term_reset(),
		       built, built == 1 ? "" : "s");

	return failed ? 1 : 0;
}
