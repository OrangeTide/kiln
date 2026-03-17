/* mapfile.h : read-only memory-mapped file helpers */
/* Copyright (c) 2006, 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT-0 OR PUBLIC DOMAIN */
#ifndef KILN_MAPFILE_H
#define KILN_MAPFILE_H

#include <stddef.h>

/*
 * mapfile holds a read-only memory mapping of a file.
 * Based on http://orangeti.de/code/mapfile.c
 */
struct mapfile {
	const unsigned char *data;
	size_t len;
#if defined(_WIN32)
	void *hmap; /* HANDLE, stored as void* to avoid windows.h in header */
#endif
};

/* mapfile_open maps a file read-only. Returns OK on success. */
int mapfile_open(struct mapfile *mf, const char *path);

/* mapfile_close releases the mapping. */
void mapfile_close(struct mapfile *mf);

#endif
