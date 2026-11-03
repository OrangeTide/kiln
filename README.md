# kiln

[![CI](https://github.com/OrangeTide/kiln/actions/workflows/ci.yml/badge.svg)](https://github.com/OrangeTide/kiln/actions/workflows/ci.yml)
[![License: MIT OR Apache-2.0](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg)](#license)
[![C99+](https://img.shields.io/badge/C-99+-brightgreen.svg)](#)
[![Platforms: Linux, macOS, Windows](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](#)

A build system with Tcl-based build descriptions, hybrid staleness
detection, and zero dependencies.

<p align="center">
  <img src="doc/screenshot.svg" alt="kiln building a project" width="720">
</p>

## Features

- **Tcl build descriptions** - write build rules in a real language, not
  a DSL with sharp edges
- **Hybrid staleness** - stat fingerprint (fast path) + content hash +
  command string hash. Changing compiler flags triggers a rebuild
  automatically
- **Compiler depfile support** - parses `cc -MMD` output for automatic
  header dependency tracking
- **Tool tracking** - declare the compiler or script a rule runs with
  `uses`; changing that tool triggers a rebuild
- **Cross-compilation** - `--triplet` flag, artifacts in
  `build/<triplet>/` so builds don't collide
- **Zero dependencies** - single static binary, bootstraps with one
  `cc` command
- **Colored progress output** - `[n/m]` counter with automatic TTY
  detection, respects `NO_COLOR`

## Quick Start

```sh
# Bootstrap kiln (requires a C compiler)
sh bin/bootstrap.sh

# Or build manually
cc -O2 -o kiln bootstrap.c
```

## Usage

```sh
kiln Kilnfile                # build all targets
kiln -a clean Kilnfile       # delete all outputs, remove empty dirs
kiln -n Kilnfile             # dry run (print commands only)
kiln --triplet arm-linux-gnueabihf Kilnfile   # cross-compile
```

## Example Kilnfile

```tcl
set cflags "-Wall -O2"

rule build/$Triplet/greet.o {
    inputs greet.c
    depfile build/$Triplet/greet.d
    command {cc $cflags -c -o $out $in -MMD -MF $depfile}
}

rule build/$Triplet/main.o {
    inputs main.c
    depfile build/$Triplet/main.d
    command {cc $cflags -c -o $out $in -MMD -MF $depfile}
}

rule build/$Triplet/hello {
    inputs build/$Triplet/main.o build/$Triplet/greet.o
    command {cc -o $out $in}
}
```

The `rule` command declares outputs, inputs, an optional depfile, and a
shell command. Variables like `$out`, `$in`, and `$depfile` are set
automatically. Since the body is Tcl, you have control flow (`if`,
`while`, `for`, `foreach`), list commands (`list`, `lappend`, `lindex`,
`llength`), `string`, `proc`, `catch`, `glob`, `file`, and `exec` (run a
program at parse time and capture its output, for compiler or host
probing). A rule per source is just a loop:

```tcl
set objs {}
foreach s [glob *.c] {
    set o build/$Triplet/[file rootname $s].o
    lappend objs $o
    cc_object $o $s $o.d "-Wall -O2"
}
rule build/$Triplet/app {
    inputs $objs
    command {cc -o $out $in}
}
```

Use `uses` to declare a program a rule runs, such as a compiler or a
generator script, when it is not already an input. kiln tracks it for
staleness like an input, so changing the tool rebuilds, and if the tool
is itself a build output it is built first:

```tcl
rule build/$Triplet/parser.c {
    inputs parser.peg
    uses build/$Triplet/peg
    command {build/$Triplet/peg $in > $out}
}
```

To avoid repeating `build/$Triplet/` on every path, set a build
directory prefix with the `builddir` command (or the `--builddir`
flag) and write outputs as plain paths. kiln prepends the prefix to
rule outputs and depfiles automatically, and resolves inputs that name
a prefixed build output.

## Standard Library

A small standard library (`lib/kiln.tcl`) is bundled into the binary and
loaded before your Kilnfiles unless you pass `--no-stdlib`. It provides
helper procs that cut rule boilerplate:

```tcl
builddir build/$Triplet

# Compile every C source and link them into one program
cc_program app [glob *.c] "-Wall -O2"
```

The helpers are:

- `cc_program out sources flags` - compile each source and link into `out`
- `cc_objects sources flags` - compile each source, returning the object list
- `cc_object obj src dep flags` - compile one source with depfile tracking
- `cp_rule dst src` - copy as a build step

The library is edited as `lib/kiln.tcl` and embedded with
`sh bin/embed.sh`, which regenerates `src/kiln_stdlib.h`. The helper set
will grow as kiln's Tcl surface does.

## How It Works

1. **Parse** - evaluate Kilnfiles in a Tcl interpreter (Picol 2),
   building a DAG of file and command nodes
2. **Sort** - topological sort with cycle detection
3. **Execute** - walk the DAG in order, checking each command for
   staleness before running it

Staleness is checked in three ways:

- **Stat fingerprint** (mtime, size, inode, ctime) - if unchanged, skip
  the file entirely (the fast path, one `stat()` call)
- **Content hash** - if stat changed but hash matches, the file was
  touched but not modified (e.g. `touch`)
- **Command string hash** - if the command changed (new flags, different
  compiler), rebuild even if inputs are identical

State is persisted in `build/<triplet>/kiln.cdb` using DJB's CDB format,
crash-safe via atomic rename.

## Documentation

- Online: <https://orangetide.github.io/kiln/> - landing page and
  rendered man page, published from `site/` by CI
- `doc/kiln.1` - man page (`man -l doc/kiln.1`)
- `HACKING.md` - developer guide
- `STYLE.md` - coding conventions

Build and preview the documentation site locally with
`make -C site serve` (requires `pandoc`).

The screenshot above is generated from real kiln output:

```sh
sh doc/gen-screenshot.sh
```

## Options

| Flag | Description |
|------|-------------|
| `-a clean` | Delete all declared outputs and empty parent dirs |
| `-a status` | Report out-of-date targets and stale database entries |
| `--triplet T` | Set target triplet (skip `cc -dumpmachine`) |
| `--builddir P` | Set build directory prefix for rule outputs |
| `--lib F` | Load a Tcl library before Kilnfiles (repeatable) |
| `--no-stdlib` | Skip the built-in standard library (`lib/kiln.tcl`) |
| `-n`, `--dry-run` | Print commands without running them |
| `-j N` | Run up to N commands in parallel (default: CPU count) |
| `-h`, `--help` | Show usage |

## Environment Variables

| Variable | Effect |
|----------|--------|
| `NO_COLOR` | Disable colored output ([no-color.org](https://no-color.org/)) |
| `CLICOLOR_FORCE` | Force color even when not a TTY |
| `TERM` | Color disabled when `dumb`, empty, or unset |

## Amalgamation Build

A single-file amalgamation can be generated for easy distribution:

```sh
sh bin/amalgamation.sh > kiln-amalgamation.c
cc -O2 -o kiln kiln-amalgamation.c
```

The amalgamation resolves all local `#include "..."` directives into
one self-contained C file. No source tree needed to compile it.

## License

Dual-licensed under [MIT](https://opensource.org/licenses/MIT) OR
[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0), at your option.
