/* cdb.h : constant database (cdb) reader and writer */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_CDB_H
#define KILN_CDB_H

#include <stddef.h>
#include <stdint.h>
#include "mapfile.h"

/* cdb is a read-only handle to a memory-mapped CDB file. */
struct cdb {
	struct mapfile mf;
};

/* cdb_open opens a CDB file for reading. Returns OK on success. */
int cdb_open(struct cdb *c, const char *path);

/* cdb_close releases resources associated with the reader. */
void cdb_close(struct cdb *c);

/*
 * cdb_find looks up a key and returns a pointer to the value,
 * setting vlen to the value's length. Returns NULL if not found.
 */
const void *cdb_find(struct cdb *c, const void *key, size_t klen, size_t *vlen);

/* cdb_record is a single key-value pair queued for writing. */
struct cdb_record {
	uint32_t hash;
	uint32_t offset;
	char *key;
	size_t klen;
	char *val;
	size_t vlen;
};

/* cdb_writer accumulates records and writes them as a CDB file. */
struct cdb_writer {
	struct cdb_record *records;
	int count;
	int capacity;
	unsigned char *data;
	size_t data_len;
	size_t data_cap;
};

/* cdb_writer_init prepares a writer for use. */
void cdb_writer_init(struct cdb_writer *w);

/* cdb_writer_free releases all memory owned by the writer. */
void cdb_writer_free(struct cdb_writer *w);

/* cdb_writer_add queues a key-value pair for writing. */
void cdb_writer_add(struct cdb_writer *w, const void *key, size_t klen,
                    const void *val, size_t vlen);

/* cdb_writer_finish writes all queued records to path in CDB format. */
int cdb_writer_finish(struct cdb_writer *w, const char *path);

#endif
