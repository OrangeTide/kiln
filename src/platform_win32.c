/* platform_win32.c : Windows platform implementation */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#if defined(_WIN32)

#include "platform.h"
#include "xmalloc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Convert a FILETIME (100ns intervals since 1601) to sec + nsec. */
static void
filetime_to_unix(FILETIME ft, long long *sec, long *nsec)
{
	ULARGE_INTEGER t;

	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	*sec = (long long)(t.QuadPart / 10000000ULL) - 11644473600LL;
	*nsec = (long)(t.QuadPart % 10000000ULL) * 100;
}

int
kiln_stat_file(const char *path, struct kiln_stat *st)
{
	/*
	 * Open a handle so GetFileInformationByHandle can supply the NTFS
	 * file index, the closest equivalent to a Unix inode. It fills the
	 * ino field so a content-preserving replace (same mtime and size,
	 * different file) still changes the fingerprint and forces a hash
	 * check. Zero access requests metadata only; BACKUP_SEMANTICS lets
	 * the handle name a directory as well as a file, and the full share
	 * mode avoids blocking a concurrent build step.
	 */
	HANDLE h = CreateFileA(path, 0,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE
	                           | FILE_SHARE_DELETE,
	                       NULL, OPEN_EXISTING,
	                       FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (h != INVALID_HANDLE_VALUE) {
		BY_HANDLE_FILE_INFORMATION info;

		if (!GetFileInformationByHandle(h, &info)) {
			CloseHandle(h);
			return ERR;
		}

		CloseHandle(h);

		filetime_to_unix(info.ftLastWriteTime,
		                 &st->mtime_sec, &st->mtime_nsec);
		filetime_to_unix(info.ftCreationTime,
		                 &st->ctime_sec, &st->ctime_nsec);
		st->size = ((long long)info.nFileSizeHigh << 32)
		           | info.nFileSizeLow;
		/*
		 * The 64-bit file index is unique within a volume. A volume
		 * without one (FAT, some network shares) reports 0, which
		 * leaves the fingerprint resting on mtime, size and ctime, as
		 * it did before.
		 */
		st->ino = ((unsigned long long)info.nFileIndexHigh << 32)
		          | info.nFileIndexLow;
		return OK;
	}

	/*
	 * CreateFileA can fail where GetFileAttributesExA still succeeds
	 * (certain locked or special files). Fall back to attribute data
	 * with no file index rather than reporting the file unstattable.
	 */
	WIN32_FILE_ATTRIBUTE_DATA attr;

	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attr))
		return ERR;

	filetime_to_unix(attr.ftLastWriteTime,
	                 &st->mtime_sec, &st->mtime_nsec);
	filetime_to_unix(attr.ftCreationTime,
	                 &st->ctime_sec, &st->ctime_nsec);
	st->size = ((long long)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
	st->ino = 0;
	return OK;
}

int
kiln_rename(const char *oldpath, const char *newpath)
{
	if (MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
		return OK;

	return ERR;
}

int
kiln_unlink(const char *path)
{
	if (DeleteFileA(path)) return OK;

	return ERR;
}

int
kiln_mkdir_p(const char *path)
{
	char tmp[4096];
	size_t len = strlen(path);

	if (len >= sizeof(tmp)) return ERR;

	memcpy(tmp, path, len + 1);

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char save = *p;
			*p = '\0';
			CreateDirectoryA(tmp, NULL);
			*p = save;
		}
	}

	if (!CreateDirectoryA(tmp, NULL)) {
		if (GetLastError() != ERROR_ALREADY_EXISTS) return ERR;
	}

	return OK;
}

int
kiln_rmdir(const char *path)
{
	if (RemoveDirectoryA(path)) return OK;

	return ERR;
}

int
kiln_copy_file(const char *src, const char *dst)
{
	/* CopyFileA overwrites dst and carries the source's attributes. */
	if (CopyFileA(src, dst, FALSE)) return OK;

	return ERR;
}

/* Directory iteration */
struct kiln_dir {
	HANDLE h;
	WIN32_FIND_DATAA data;
	int first;
};

int
kiln_dir_open(kiln_dir **d, const char *path)
{
	char pattern[4096];
	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	kiln_dir *dir = xmalloc(sizeof(*dir));

	dir->h = FindFirstFileA(pattern, &dir->data);

	if (dir->h == INVALID_HANDLE_VALUE) {
		free(dir);  // xmalloc succeeded, free on FindFirstFile failure
		return ERR;
	}

	dir->first = 1;
	*d = dir;
	return OK;
}

const char *
kiln_dir_next(kiln_dir *d)
{
	while (1) {
		if (!d->first) {
			if (!FindNextFileA(d->h, &d->data)) return NULL;
		}

		d->first = 0;

		if (d->data.cFileName[0] == '.' &&
		    (d->data.cFileName[1] == '\0' ||
		     (d->data.cFileName[1] == '.' && d->data.cFileName[2] == '\0')))
			continue;

		return d->data.cFileName;
	}
}

void
kiln_dir_close(kiln_dir *d)
{
	if (d) {
		if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);

		free(d);
	}
}

/* Glob - simple implementation using FindFirstFile */
int
kiln_glob(const char *pattern, char ***results, int *count)
{
	WIN32_FIND_DATAA data;
	HANDLE h = FindFirstFileA(pattern, &data);

	if (h == INVALID_HANDLE_VALUE) {
		*results = NULL;
		*count = 0;
		return OK;
	}

	int cap = 16, n = 0;
	char **res = xmalloc(sizeof(char *) * cap);

	do {
		if (data.cFileName[0] == '.' &&
		    (data.cFileName[1] == '\0' ||
		     (data.cFileName[1] == '.' && data.cFileName[2] == '\0')))
			continue;

		if (n >= cap) {
			cap *= 2;
			res = xrealloc(res, sizeof(char *) * cap);
		}

		res[n++] = xstrdup(data.cFileName);
	} while (FindNextFileA(h, &data));

	FindClose(h);
	*results = res;
	*count = n;
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
	HANDLE process;
	HANDLE readpipe;
	char *outbuf;
	size_t outlen;
	size_t outcap;
};

int
kiln_spawn(kiln_proc **proc, const char *cmd, const char *cwd)
{
	HANDLE rd, wr;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

	if (!CreatePipe(&rd, &wr, &sa, 0)) return ERR;

	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	char cmdline[32768];
	snprintf(cmdline, sizeof(cmdline), "cmd /c %s", cmd);

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi)) {
		CloseHandle(rd);
		CloseHandle(wr);
		return ERR;
	}

	CloseHandle(wr);
	CloseHandle(pi.hThread);

	kiln_proc *p = xmalloc(sizeof(*p));
	p->process = pi.hProcess;
	p->readpipe = rd;
	p->outbuf = NULL;
	p->outlen = 0;
	p->outcap = 0;
	*proc = p;
	return OK;
}

int
kiln_wait(kiln_proc *proc, int *exitcode)
{
	char buf[4096];
	DWORD n;

	while (ReadFile(proc->readpipe, buf, sizeof(buf), &n, NULL) && n > 0) {
		if (proc->outlen + n > proc->outcap) {
			proc->outcap = (proc->outlen + n) * 2;

			if (proc->outcap < 4096) proc->outcap = 4096;

			proc->outbuf = xrealloc(proc->outbuf, proc->outcap);
		}

		memcpy(proc->outbuf + proc->outlen, buf, n);
		proc->outlen += n;
	}

	CloseHandle(proc->readpipe);

	WaitForSingleObject(proc->process, INFINITE);
	DWORD code;
	GetExitCodeProcess(proc->process, &code);
	*exitcode = (int)code;
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
		CloseHandle(proc->process);
		free(proc->outbuf);
		free(proc);
	}
}

int
kiln_cpu_count(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
}

int
kiln_run_capture(const char *cmd, char *buf, size_t bufsz)
{
	kiln_proc *proc;

	if (kiln_spawn(&proc, cmd, NULL) != OK) return ERR;

	int exitcode;
	kiln_wait(proc, &exitcode);
	const char *out;
	size_t len;
	kiln_proc_output(proc, &out, &len);

	if (len >= bufsz) len = bufsz - 1;

	memcpy(buf, out, len);
	buf[len] = '\0';

	/* Strip trailing newline */
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	kiln_proc_free(proc);
	return exitcode == 0 ? OK : ERR;
}

#endif /* _WIN32 */
