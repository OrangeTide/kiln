# kiln

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
automatically. Since the body is Tcl, you can use `glob`, `foreach`,
`file`, and any other Tcl command.

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

- `doc/kiln.1` - man page (`man -l doc/kiln.1`)
- `HACKING.md` - developer guide
- `STYLE.md` - coding conventions

The screenshot above is generated from real kiln output:

```sh
sh doc/gen-screenshot.sh
```

## Options

| Flag | Description |
|------|-------------|
| `-a clean` | Delete all declared outputs and empty parent dirs |
| `-a status` | Show CDB/DAG inconsistencies *(not yet implemented)* |
| `--triplet T` | Set target triplet (skip `cc -dumpmachine`) |
| `-n`, `--dry-run` | Print commands without running them |
| `-j N` | Parallel jobs (default: CPU count) |
| `-h`, `--help` | Show usage |

## Environment Variables

| Variable | Effect |
|----------|--------|
| `NO_COLOR` | Disable colored output ([no-color.org](https://no-color.org/)) |
| `CLICOLOR_FORCE` | Force color even when not a TTY |
| `TERM` | Color disabled when `dumb`, empty, or unset |

## License

Dual-licensed under [MIT](https://opensource.org/licenses/MIT) OR
[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0), at your option.
