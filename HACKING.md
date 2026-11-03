# Developer Guide

Internal notes for contributors working on kiln.

## Unity Build

kiln uses a unity build - `bootstrap.c` #includes all `.c` files into a
single translation unit. The include chain in `src/kiln.c` is
order-dependent:

    picol_ext.c -> dag.c -> hash.c -> mapfile.c -> cdb.c ->
    depfile.c -> term.c -> execute.c -> commands.c

`picol_ext.c` itself #includes `picol.c` with `PICOL_NO_MAIN` defined.
Do not compile any `src/*.c` file separately - they are not standalone
translation units.

Because everything shares one translation unit, static function names
must not collide across files.

## Include Guards

All headers use the `KILN_` prefix:

    #ifndef KILN_DAG_H
    #define KILN_DAG_H

## Module Prefixes

Exported functions are prefixed by their module name:

- `dag_` - dependency graph (dag.c)
- `cdb_` - constant database reader/writer (cdb.c)
- `hash_` - content hashing (hash.c)
- `mapfile_` - memory-mapped file I/O (mapfile.c)
- `depfile_` - gcc/clang depfile parsing (depfile.c)
- `term_` - terminal color and progress output (term.c)
- `kiln_` - platform abstraction and top-level operations

Static Tcl command handlers use the `cmd_` prefix:

- `cmd_rule`, `cmd_source`, `cmd_builddir` (commands.c)
- `cmd_rule_inputs`, `cmd_rule_uses`, `cmd_rule_command`,
  `cmd_rule_depfile` (commands.c; registered only while a `rule` body is
  evaluated)
- `cmd_lindex`, `cmd_llength`, `cmd_glob`, `cmd_file` (picol_ext.c)

Upstream picol functions keep their `picol` prefix - do not rename them.

## Tcl Command Registration

Commands are registered in three stages during startup (kiln.c main):

    picolRegisterCoreCommands(interp);   /* picol.c core */
    kiln_register_ext_commands(interp);  /* picol_ext.c */
    kiln_register_commands(interp);      /* commands.c */

Command handler signature:

    int cmd_xxx(struct picolInterp *i, int argc, char **argv,
                struct picolCmd *cmd)

Return `PICOL_OK` on success, `PICOL_ERR` on failure.

## OK/ERR Return Convention

`OK` (0) and `ERR` (-1) are defined in `platform.h` and used
project-wide for functions that return success or failure.

Tcl commands use picol's own convention: `PICOL_OK` / `PICOL_ERR`.

`build_execute` returns a process exit code (0/1), not OK/ERR.

## Error Reporting

- Main program and execution errors use `fprintf(stderr, ...)` with
  a `"kiln: "` prefix.
- Kilnfile parse errors include `file:line:` context.

## Memory

`xmalloc`, `xrealloc`, and `xstrdup` abort on allocation failure.
Prefer these over raw `malloc`/`realloc` in new code. The definitions
live in picol.c (upstream); `src/xmalloc.h` provides forward
declarations so modules included before picol.c in the unity build can
still use them.

## Actions

kiln uses `--action` (`-a`) for built-in operations that are not
targets, avoiding ambiguity when a target happens to share a name:

    kiln -a clean Kilnfile    # delete all DAG-declared outputs
    kiln -a status Kilnfile   # report out-of-date targets and stale CDB entries

`clean` walks all NODE_FILE nodes in the DAG, identifies outputs (files
with a NODE_CMD producer edge), deletes them, then removes empty parent
directories bottom-up. It also removes the CDB file and the build
directory if empty.

The existing `delete_outputs()` in execute.c handles per-command cleanup
on failure. `build_clean()` uses the same output identification pattern
but across the entire graph.

`build_status()` reports without building. It reuses `node_is_dirty()`
against a throwaway `cdb_writer` (so it stays read-only, and with nothing
building no node reaches STATE_BUILDING) to list out-of-date targets and
flag missing outputs, then walks the CDB with `cdb_iter_*` and reports
any `cmd:<output>` key whose output is no longer a file node (its rule
was removed).

## Terminal Output

`term.c` / `term.h` provide color and progress output. Call `term_init()`
once at startup. Detection order:

1. `CLICOLOR_FORCE` set and non-zero -> colors on
2. `NO_COLOR` set -> colors off
3. `!isatty(stdout)` -> colors off, progress rewriting off
4. `TERM` is NULL, empty, or `"dumb"` -> colors off
5. Otherwise -> colors on

On Windows, `term_init()` enables VT processing via `SetConsoleMode()`.

Colors use hardcoded ANSI SGR sequences (no terminfo dependency).
Progress uses `\r` to rewrite the current line on TTY; falls back to
plain newlines when piped. Call `term_clear_progress()` before printing
job output to avoid garbled lines.

## Kilnfile DSL

Build descriptions are written in Tcl and loaded from files named
`Kilnfile`. Available commands:

    rule <outputs> { body }    # define a build rule
      inputs <files...>        # declare inputs (sets $in)
      uses <files...>          # tool deps tracked like inputs, kept out of $in
      command <string>         # shell command (expands $out, $in, $depfile)
      depfile <path>           # optional gcc-style depfile

    source <filename>          # include another Kilnfile
    builddir <path>            # set output prefix (sets $BuildDir)

Predefined variables: `$Triplet`, `$Jobs`, `$DryRun`, `$SourcePath`,
and `$BuildDir` (set once `builddir` or `--builddir` is used).

The `--builddir` command-line flag takes precedence over a `builddir`
command in the Kilnfile.

## Standard Library

The standard library is written in Tcl in `lib/kiln.tcl` and embedded
into the binary as a C string. `bin/embed.sh` regenerates the committed
header `src/kiln_stdlib.h` from it:

    sh bin/embed.sh    # after editing lib/kiln.tcl

`kiln.c` includes that header and evaluates `kiln_stdlib_tcl` in the
interpreter at startup, before user `--lib` files and Kilnfiles, unless
`--no-stdlib` is given. The amalgamation inlines the header like any
other local include. Both `lib/kiln.tcl` and `src/kiln_stdlib.h` are
committed; regenerate and commit them together.

Helpers must use only the commands kiln exposes today. In a helper that
builds a rule body, escape `$out`, `$in`, and `$depfile` as `\$out` so
they reach the rule unexpanded; a helper's own arguments (`$src` and so
on) are substituted when the helper runs.

## Platform Abstraction

Platform-specific code lives in `platform_unix.c` and
`platform_win32.c`. Both are always compiled; `#if defined(_WIN32)`
guards select the active implementation. The interface is declared
in `platform.h`.

`mapfile.c` similarly uses `#ifdef` for POSIX mmap vs Win32
CreateFileMapping.
