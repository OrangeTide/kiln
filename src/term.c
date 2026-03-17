/* term.c : terminal color and progress output */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */

#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

static int color_enabled;
static int tty_enabled;
static int progress_line_active; // true if a \r progress line needs clearing

void
term_init(void)
{
	const char *env;

	// CLICOLOR_FORCE overrides everything
	env = getenv("CLICOLOR_FORCE");
	if (env && env[0] != '\0' && strcmp(env, "0") != 0) {
		color_enabled = 1;
		tty_enabled = isatty(fileno(stdout));
		goto done;
	}

	// NO_COLOR disables color
	env = getenv("NO_COLOR");
	if (env) {
		color_enabled = 0;
		tty_enabled = isatty(fileno(stdout));
		goto done;
	}

	// Must be a TTY
	if (!isatty(fileno(stdout))) {
		color_enabled = 0;
		tty_enabled = 0;
		goto done;
	}

	tty_enabled = 1;

	// TERM must be set and not "dumb"
	env = getenv("TERM");
	if (!env || env[0] == '\0' || strcmp(env, "dumb") == 0) {
		color_enabled = 0;
		goto done;
	}

	color_enabled = 1;

done:
#if defined(_WIN32)
	// Enable VT processing on Windows 10+
	if (color_enabled) {
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (GetConsoleMode(h, &mode)) {
			mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
			if (!SetConsoleMode(h, mode))
				color_enabled = 0;
		}
	}
#endif
	progress_line_active = 0;
}

int
term_has_color(void)
{
	return color_enabled;
}

int
term_is_tty(void)
{
	return tty_enabled;
}

const char *
term_bold(void)
{
	return color_enabled ? "\033[1m" : "";
}

const char *
term_red(void)
{
	return color_enabled ? "\033[31m" : "";
}

const char *
term_green(void)
{
	return color_enabled ? "\033[32m" : "";
}

const char *
term_yellow(void)
{
	return color_enabled ? "\033[33m" : "";
}

const char *
term_reset(void)
{
	return color_enabled ? "\033[0m" : "";
}

void
term_progress(int done, int total, const char *msg)
{
	if (tty_enabled) {
		// Overwrite current line
		printf("\r%s%s[%d/%d]%s %s\033[K",
		       term_bold(), term_green(),
		       done, total,
		       term_reset(), msg);
		fflush(stdout);
		progress_line_active = 1;
	} else {
		printf("%s%s[%d/%d]%s %s\n",
		       term_bold(), term_green(),
		       done, total,
		       term_reset(), msg);
	}
}

void
term_clear_progress(void)
{
	if (tty_enabled && progress_line_active) {
		printf("\r\033[K");
		fflush(stdout);
		progress_line_active = 0;
	}
}
