/* platform.h : portable filesystem, process, and directory operations */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_PLATFORM_H
#define KILN_PLATFORM_H

#include <stddef.h>

#define OK  (0)
#define ERR (-1)

/*
 * kiln_stat holds a filesystem fingerprint used for staleness detection.
 * ino is the Unix inode, or on Windows the NTFS file index. A volume that
 * has no stable index (FAT, some network shares) reports 0, which leaves
 * the fingerprint resting on mtime, size, and ctime.
 */
struct kiln_stat {
	long long mtime_sec;
	long mtime_nsec;
	long long size;
	unsigned long long ino;
	long long ctime_sec;
	long ctime_nsec;
};

/* kiln_stat_file fills st with the stat fingerprint for path. */
int kiln_stat_file(const char *path, struct kiln_stat *st);

/* kiln_rename replaces newpath with oldpath. */
int kiln_rename(const char *oldpath, const char *newpath);

/* kiln_unlink removes a file. */
int kiln_unlink(const char *path);

/* kiln_mkdir_p creates a directory and all parent directories. */
int kiln_mkdir_p(const char *path);

/*
 * kiln_copy_file copies src to dst, overwriting dst, and preserves the
 * source's permission bits (so an installed program keeps its +x). The
 * destination directory must already exist. Returns OK or ERR.
 */
int kiln_copy_file(const char *src, const char *dst);

/* kiln_rmdir removes an empty directory. Returns OK or ERR. */
int kiln_rmdir(const char *path);

/* kiln_dir is an opaque handle for directory iteration. */
typedef struct kiln_dir kiln_dir;

/* kiln_dir_open opens a directory for iteration. */
int kiln_dir_open(kiln_dir **d, const char *path);

/* kiln_dir_next returns the next entry name, or NULL at end. Skips "." and "..". */
const char *kiln_dir_next(kiln_dir *d);

/* kiln_dir_close releases resources for a directory handle. */
void kiln_dir_close(kiln_dir *d);

/* kiln_glob expands a shell glob pattern into a heap-allocated array of matches. */
int kiln_glob(const char *pattern, char ***results, int *count);

/* kiln_glob_free releases the array returned by kiln_glob. */
void kiln_glob_free(char **results, int count);

/* kiln_proc is an opaque handle for a spawned child process. */
typedef struct kiln_proc kiln_proc;

/* kiln_spawn starts a shell command, capturing stdout and stderr. */
int kiln_spawn(kiln_proc **proc, const char *cmd, const char *cwd);

/* kiln_wait drains output and waits for the process to exit. */
int kiln_wait(kiln_proc *proc, int *exitcode);

/* kiln_proc_output returns the captured stdout+stderr buffer. */
void kiln_proc_output(kiln_proc *proc, const char **out, size_t *len);

/* kiln_proc_free releases all resources for a process handle. */
void kiln_proc_free(kiln_proc *proc);

/* kiln_cpu_count returns the number of online CPUs. */
int kiln_cpu_count(void);

/*
 * kiln_run_capture runs a command and captures its output into buf,
 * stripping trailing newlines. Returns OK on success.
 */
int kiln_run_capture(const char *cmd, char *buf, size_t bufsz);

#endif
