/* hash.c : file content hashing */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "hash.h"
#include <stdio.h>

int
hash_file(const char *path, uint32_t *out)
{
	FILE *fp = fopen(path, "rb");

	if (!fp) return ERR;

	uint32_t h = 5381;
	unsigned char buf[8192];
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		for (size_t i = 0; i < n; i++)
			h = ((h << 5) + h) ^ buf[i];
	}

	fclose(fp);
	*out = h;
	return OK;
}
