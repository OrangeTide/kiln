/* depfile.h : gcc/clang -MMD depfile parser */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_DEPFILE_H
#define KILN_DEPFILE_H

/*
 * depfile_parse reads a depfile and returns a heap-allocated array
 * of dependency paths. The caller must free the result with
 * depfile_free. Sets count to the number of paths returned.
 */
char **depfile_parse(const char *path, int *count);

/* depfile_free releases the array returned by depfile_parse. */
void depfile_free(char **deps, int count);

#endif
