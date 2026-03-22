/* xmalloc.h : allocation wrappers that abort on out-of-memory
 *
 * The actual definitions live in picol.c (upstream). This header provides
 * forward declarations so modules included before picol.c in the unity
 * build can use them. */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_XMALLOC_H
#define KILN_XMALLOC_H

#include <stddef.h>

void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

#define xmalloc(size) xrealloc(NULL,size)

#endif
