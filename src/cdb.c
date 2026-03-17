/* cdb.c : constant database (cdb) reader and writer */
/* CDB format by Daniel J. Bernstein.
 * Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "cdb.h"
#include "hash.h"
#include "mapfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LE read/write helpers */
static uint32_t
le32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16)
		| ((uint32_t)p[3] << 24);
}

static void
put_le32(unsigned char *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

/***************************************************************************
 * Reader
 **************************************************************************/

int
cdb_open(struct cdb *c, const char *path)
{
	if (mapfile_open(&c->mf, path) != OK)
		return ERR;

	if (c->mf.len < 2048) {
		mapfile_close(&c->mf);
		return ERR;
	}

	return OK;
}

void
cdb_close(struct cdb *c)
{
	mapfile_close(&c->mf);
}

const void *
cdb_find(struct cdb *c, const void *key, size_t klen,
         size_t *vlen)
{
	if (!c->mf.data) return NULL;

	uint32_t h = djb2(key, klen);
	uint32_t slot = (h & 255) * 8;
	uint32_t tpos = le32(c->mf.data + slot);
	uint32_t tlen = le32(c->mf.data + slot + 4);

	if (tlen == 0) return NULL;

	uint32_t start = (h >> 8) % tlen;

	for (uint32_t i = 0; i < tlen; i++) {
		uint32_t idx = (start + i) % tlen;
		uint32_t entry_off = tpos + idx * 8;

		if (entry_off + 8 > c->mf.len) return NULL;

		uint32_t eh = le32(c->mf.data + entry_off);
		uint32_t ep = le32(c->mf.data + entry_off + 4);

		if (ep == 0) return NULL; /* empty slot */

		if (eh == h) {
			/* Check key */
			if (ep + 8 > c->mf.len) continue;

			uint32_t rklen = le32(c->mf.data + ep);
			uint32_t rvlen = le32(c->mf.data + ep + 4);

			if (rklen != klen) continue;

			if (ep + 8 + rklen + rvlen > c->mf.len)
				continue;

			if (memcmp(c->mf.data + ep + 8, key, klen) == 0) {
				*vlen = rvlen;
				return c->mf.data + ep + 8 + rklen;
			}
		}
	}

	return NULL;
}

/***************************************************************************
 * Writer
 **************************************************************************/

void
cdb_writer_init(struct cdb_writer *w)
{
	w->capacity = 256;
	w->count = 0;
	w->records = malloc(sizeof(struct cdb_record) * w->capacity);
	w->data = NULL;
	w->data_len = 0;
	w->data_cap = 0;
}

void
cdb_writer_free(struct cdb_writer *w)
{
	for (int i = 0; i < w->count; i++) {
		free(w->records[i].key);
		free(w->records[i].val);
	}

	free(w->records);
	free(w->data);
}

void
cdb_writer_add(struct cdb_writer *w, const void *key,
               size_t klen, const void *val, size_t vlen)
{
	if (w->count >= w->capacity) {
		w->capacity *= 2;
		w->records = realloc(w->records,
			sizeof(struct cdb_record) * w->capacity);
	}

	struct cdb_record *r = &w->records[w->count++];
	r->hash = djb2(key, klen);
	r->klen = klen;
	r->vlen = vlen;
	r->key = malloc(klen);
	memcpy(r->key, key, klen);
	r->val = malloc(vlen);
	memcpy(r->val, val, vlen);
	r->offset = 0; /* filled during finish */
}

int
cdb_writer_finish(struct cdb_writer *w, const char *path)
{
	FILE *fp = fopen(path, "wb");

	if (!fp) return ERR;

	/* Phase 1: write placeholder header + records */
	unsigned char header[2048];
	memset(header, 0, sizeof(header));
	fwrite(header, 1, 2048, fp);

	/* Count entries per hash table */
	uint32_t table_count[256];
	memset(table_count, 0, sizeof(table_count));

	for (int i = 0; i < w->count; i++) {
		w->records[i].offset = (uint32_t)ftell(fp);
		unsigned char rec_hdr[8];
		put_le32(rec_hdr, w->records[i].klen);
		put_le32(rec_hdr + 4, w->records[i].vlen);
		fwrite(rec_hdr, 1, 8, fp);
		fwrite(w->records[i].key, 1, w->records[i].klen, fp);
		fwrite(w->records[i].val, 1, w->records[i].vlen, fp);
		table_count[w->records[i].hash & 255]++;
	}

	/* Phase 2: write hash tables */
	uint32_t table_pos[256];
	uint32_t table_len[256];

	for (int t = 0; t < 256; t++) {
		table_pos[t] = (uint32_t)ftell(fp);
		table_len[t] = table_count[t] * 2; /* ~50% load */

		if (table_len[t] == 0) {
			table_pos[t] = 0;
			continue;
		}

		/* Build hash table for this slot */
		size_t tsize = table_len[t] * 8;
		unsigned char *table = calloc(1, tsize);

		for (int i = 0; i < w->count; i++) {
			if ((w->records[i].hash & 255) != (uint32_t)t)
				continue;

			uint32_t slot = (w->records[i].hash >> 8)
				% table_len[t];

			/* Linear probing */
			while (le32(table + slot * 8 + 4) != 0)
				slot = (slot + 1) % table_len[t];

			put_le32(table + slot * 8,
				w->records[i].hash);
			put_le32(table + slot * 8 + 4,
				w->records[i].offset);
		}

		fwrite(table, 1, tsize, fp);
		free(table);
	}

	/* Phase 3: rewrite header */
	fseek(fp, 0, SEEK_SET);

	for (int t = 0; t < 256; t++) {
		unsigned char entry[8];
		put_le32(entry, table_pos[t]);
		put_le32(entry + 4, table_len[t]);
		fwrite(entry, 1, 8, fp);
	}

	fclose(fp);
	return OK;
}
