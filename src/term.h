/* term.h : terminal color and progress output */
/* Copyright (c) 2025-2026 Jon Mayo <jon@rm-f.net>
 * Licensed under MIT OR Apache-2.0 */
#ifndef KILN_TERM_H
#define KILN_TERM_H

// Detect terminal capabilities and initialize color/progress state.
// Call once at startup before any output.
void term_init(void);

// True if stdout supports color escape sequences.
int term_has_color(void);

// True if stdout is a TTY (progress rewriting is safe).
int term_is_tty(void);

// ANSI color codes - return empty string when color is off.
const char *term_bold(void);
const char *term_red(void);
const char *term_green(void);
const char *term_yellow(void);
const char *term_reset(void);

// Print a progress counter that overwrites the current line on a TTY.
// On non-TTY, prints a plain line with newline.
void term_progress(int done, int total, const char *msg);

// Clear the progress line (call before printing job output).
void term_clear_progress(void);

#endif
