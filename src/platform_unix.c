/* platform_unix.c : Unix platform implementation */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#if !defined(_WIN32)

#include "platform.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <errno.h>

int
kiln_stat_file(const char *path, struct kiln_stat *st)
{
	struct stat sb;

	if (stat(path, &sb) != 0) return ERR;

	st->mtime_sec = sb.st_mtime;
#if defined(__APPLE__)
	st->mtime_nsec = sb.st_mtimespec.tv_nsec;
	st->ctime_nsec = sb.st_ctimespec.tv_nsec;
#else
	st->mtime_nsec = sb.st_mtim.tv_nsec;
	st->ctime_nsec = sb.st_ctim.tv_nsec;
#endif
	st->size = sb.st_size;
	st->ino = sb.st_ino;
	st->ctime_sec = sb.st_ctime;
	return OK;
}

int
kiln_rename(const char *oldpath, const char *newpath)
{
	return rename(oldpath, newpath);
}

int
kiln_unlink(const char *path)
{
	return unlink(path);
}

int
kiln_mkdir_p(const char *path)
{
	char tmp[4096];
	size_t len = strlen(path);

	if (len >= sizeof(tmp)) return ERR;

	memcpy(tmp, path, len + 1);

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';

			if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return ERR;

			*p = '/';
		}
	}

	if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return ERR;

	return OK;
}

int
kiln_rmdir(const char *path)
{
	return rmdir(path);
}

/* Directory iteration */
struct kiln_dir {
	DIR *dp;
	struct dirent *ent;
};

int
kiln_dir_open(kiln_dir **d, const char *path)
{
	kiln_dir *dir = malloc(sizeof(*dir));

	if (!dir) return ERR;

	dir->dp = opendir(path);

	if (!dir->dp) {
		free(dir);
		return ERR;
	}

	dir->ent = NULL;
	*d = dir;
	return OK;
}

const char *
kiln_dir_next(kiln_dir *d)
{
	while ((d->ent = readdir(d->dp)) != NULL) {
		if (d->ent->d_name[0] == '.' &&
		    (d->ent->d_name[1] == '\0' ||
		     (d->ent->d_name[1] == '.' && d->ent->d_name[2] == '\0')))
			continue;

		return d->ent->d_name;
	}

	return NULL;
}

void
kiln_dir_close(kiln_dir *d)
{
	if (d) {
		if (d->dp) closedir(d->dp);

		free(d);
	}
}

/* Glob */
int
kiln_glob(const char *pattern, char ***results, int *count)
{
	glob_t g;
	int ret = glob(pattern, GLOB_NOSORT, NULL, &g);

	if (ret == GLOB_NOMATCH) {
		*results = NULL;
		*count = 0;
		return OK;
	}

	if (ret != 0) return ERR;

	*count = (int)g.gl_pathc;
	*results = malloc(sizeof(char *) * g.gl_pathc);

	if (!*results) {
		globfree(&g);
		return ERR;
	}

	for (size_t i = 0; i < g.gl_pathc; i++)
		(*results)[i] = strdup(g.gl_pathv[i]);

	globfree(&g);
	return OK;
}

void
kiln_glob_free(char **results, int count)
{
	for (int i = 0; i < count; i++) free(results[i]);

	free(results);
}

/* Process execution */
struct kiln_proc {
	pid_t pid;
	int pipefd;
	char *outbuf;
	size_t outlen;
	size_t outcap;
};

int
kiln_spawn(kiln_proc **proc, const char *cmd, const char *cwd)
{
	int pipefd[2];

	if (pipe(pipefd) != 0) return ERR;

	pid_t pid = fork();

	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return ERR;
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);

		if (cwd && chdir(cwd) != 0) _exit(127);

		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);

	kiln_proc *p = malloc(sizeof(*p));
	p->pid = pid;
	p->pipefd = pipefd[0];
	p->outbuf = NULL;
	p->outlen = 0;
	p->outcap = 0;
	*proc = p;
	return OK;
}

int
kiln_wait(kiln_proc *proc, int *exitcode)
{
	/* Drain output */
	char buf[4096];
	ssize_t n;

	while ((n = read(proc->pipefd, buf, sizeof(buf))) > 0) {
		if (proc->outlen + n > proc->outcap) {
			proc->outcap = (proc->outlen + n) * 2;

			if (proc->outcap < 4096) proc->outcap = 4096;

			proc->outbuf = realloc(proc->outbuf, proc->outcap);
		}

		memcpy(proc->outbuf + proc->outlen, buf, n);
		proc->outlen += n;
	}

	close(proc->pipefd);

	int status;

	if (waitpid(proc->pid, &status, 0) < 0) return ERR;

	if (WIFEXITED(status))
		*exitcode = WEXITSTATUS(status);
	else
		*exitcode = 128;

	return OK;
}

void
kiln_proc_output(kiln_proc *proc, const char **out, size_t *len)
{
	*out = proc->outbuf;
	*len = proc->outlen;
}

void
kiln_proc_free(kiln_proc *proc)
{
	if (proc) {
		free(proc->outbuf);
		free(proc);
	}
}

int
kiln_cpu_count(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n > 0) ? (int)n : 1;
}

int
kiln_run_capture(const char *cmd, char *buf, size_t bufsz)
{
	FILE *fp = popen(cmd, "r");

	if (!fp) return ERR;

	size_t total = 0;

	while (total < bufsz - 1) {
		size_t n = fread(buf + total, 1, bufsz - 1 - total, fp);

		if (n == 0) break;

		total += n;
	}

	buf[total] = '\0';
	int ret = pclose(fp);

	/* Strip trailing newline */
	while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
		buf[--total] = '\0';

	return ret == 0 ? OK : ERR;
}

#endif /* !_WIN32 */
