/* hash.h : djb2 hashing for content and command fingerprinting */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_HASH_H
#define KILN_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* djb2 hashes len bytes of data using the standard seed 5381. */
static inline uint32_t
djb2(const unsigned char *data, size_t len)
{
	uint32_t h = 5381;

	for (size_t i = 0; i < len; i++)
		h = ((h << 5) + h) ^ data[i];

	return h;
}

/* hash_file hashes a file's contents. Returns OK on success. */
int hash_file(const char *path, uint32_t *out);

/* hash_string hashes a null-terminated string. */
static inline uint32_t
hash_string(const char *s)
{
	return djb2((const unsigned char *)s, strlen(s));
}

#endif
