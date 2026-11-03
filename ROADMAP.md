# kiln Roadmap

This roadmap sets out the work needed to make kiln a build system that can
replace a large hand-written Makefile without inheriting its problems.

## North star

The reference target is a real toolchain build (the Skjegg compiler), whose
Makefile is 7258 lines and 327 rules. A sampling of that file shows the
classes of problem a good build system should remove, not reproduce:

- 197 `qemu` invocations to run freshly built cross binaries, plus 43
  `addr2line` calls to symbolize their crashes. Building and running are
  tangled together by hand.
- 35 `$(wildcard)`, 16 `$(filter)`, and 14 `$(call)` uses to fake input
  discovery and rule templating that Make does not offer directly.
- 12 `ifeq`/`ifdef` blocks plus `$(shell uname ...)` for host arch and OS
  detection.
- `.SECONDEXPANSION` and 8 comments that flag manual ordering hazards
  ("deferred (= vs :=)", "defined further down the Makefile", "must come
  before"). The build is correct only because a human tracked the order.
- A version string derived from `git describe` with a `cmp` guard so a
  rebuild does not cascade on an unchanged header.

kiln already answers some of this: a real language in the Kilnfile, a DAG
with cycle detection, content and command hashing so an unchanged file does
not cascade (this removes the `cmp`-guard hack outright), and depfile parsing
for header dependencies. The gap is everything below.

The acceptance test for "complete" is a single example under `examples/`
that cross-compiles a small program for two architectures, runs each result
under an emulator, and rebuilds correctly when the compiler, the assembler,
a source file, or a header changes. If that example is shorter and clearer
than the equivalent Makefile fragment, kiln has met its goal.

## Phase 0: make the core claims true

These items close gaps between what kiln advertises and what it does. They
come first because later phases depend on them.

1. **Parallel execution. (done)** A job scheduler in `execute.c` walks the
   DAG by producer-count readiness and runs up to `-j N` ready commands at
   once (default: CPU count), printing each command's captured output as a
   group. `-j 1` reproduces the old sequential behavior. The orchestrator is
   single-threaded, so the DAG and CDB are never touched concurrently; only
   the shell commands run in parallel. Jobs are reaped in dispatch order,
   which is simple and needs no platform wait-for-any; a poll-based wait-any,
   so a slow job cannot hold finished slots idle, is a future optimization.

2. **Tool-binary staleness.** Staleness hashes the command string only, so
   rebuilding an in-tree compiler or assembler does not re-run the commands
   that use it. The Skjegg build bootstraps its own assembler and then
   assembles with it, which Make cannot get right either. Let a rule name the
   program it runs as an implicit dependency (for example a `uses <path>`
   declaration, or auto-detection of the first argv token when it resolves to
   a build output), and include that file in the staleness check.
   - Done when: changing a built tool forces every dependent command to
     re-run, and nothing else does.

3. **`-a status`.** Implement the stubbed action: report files in the CDB
   that are no longer in the DAG, outputs missing on disk, and stale entries.
   This is the debugging aid the Skjegg ordering comments are crying out for.

4. **Bundle the standard library.** `--no-stdlib` and the `kiln.tcl` loader
   are documented but `lib/` is empty and the loader is a TODO. Ship an
   embedded `kiln.tcl` with the common helpers the later phases introduce,
   and make `--no-stdlib` meaningful.

## Phase 1: expressive build descriptions

Today a Kilnfile can call `break continue expr if proc puts return set while`
plus `glob file lindex llength`. That is too thin to express a real build
without repetition. The Skjegg Makefile leans on `$(call)`, `$(foreach)`,
and list filtering precisely because the base language is weak.

5. **Grow the Tcl surface.** Add the commands a description actually needs:
   `foreach`, `for`, `incr`, `append`, `lappend`, `list`, `lrange`,
   `lsort`, `string` (at least `match`, `map`, `range`, `first`, `last`),
   `switch`, `catch`, `eval`, and `format`. Document the supported set so
   authors are not guessing.
   - Done when: a pattern-rule-style description over a list of targets reads
     as a `foreach` loop with no external tricks.

6. **Config-time `exec`.** Expose a command that runs a program while the
   Kilnfile is being evaluated and returns its output, the counterpart to
   `cc -dumpmachine` that kiln already runs internally. This replaces
   `$(shell uname -m)` and lets a Kilnfile probe the compiler, detect the
   host, or read a tool version before declaring rules.

7. **Rule templates.** Provide an ergonomic way to declare one rule shape and
   apply it across many inputs or targets, keyed by a suffix or an
   architecture name. With `foreach` plus `proc` this can be a stdlib
   helper rather than new syntax. The goal is that the per-arch tool rules
   that Make writes as a pattern rule plus `$(call)` become a short loop.

8. **Toolchain configuration.** Offer a convention for declaring a toolchain
   (compiler, assembler, linker, emulator) with per-target overrides from the
   environment and the command line, replacing the wall of `?=` defaults. A
   stdlib helper that reads env vars with fallbacks is likely enough.

## Phase 2: targets that are not files

A build system for a compiler spends most of its rules running things, not
producing files. The Skjegg Makefile has 327 rules, many of them phony test
and run targets. kiln today requires every rule to produce a file output,
which rules out `check`, `run`, and `install` as first-class targets.

9. **Phony and task targets.** Support named targets that run commands and
   produce no file, participating in the DAG by name. This is the
   prerequisite for everything in this phase.

10. **Run and test integration.** Give a first-class way to express "build
    X, then run it, optionally under a wrapper such as an emulator, and check
    the exit status or output." This is what the 197 `qemu` lines and the
    `addr2line` symbolization do by hand. A failing run should report the
    command and its output the way a failing build command does.

11. **Default targets, aliases, and groups.** Let a Kilnfile declare which
    targets build by default and define group aliases (one name that expands
    to many), so `check` can mean "run every test" without the caller listing
    them.

## Phase 3: variants, generation, and multiple targets

12. **Build variants.** Make debug, release, and sanitizer builds first-class,
    each in its own build directory, without duplicating rules. The Skjegg
    sanitizer build is a separate `CFLAGS` set bolted on; it should be a
    variant selector instead.

13. **Generated files and codegen.** Bless a pattern for generating a source
    file from a command (a version header, a parser table). kiln's content
    hashing already avoids the mtime cascade that forces the Makefile's `cmp`
    guard, so the stdlib helper mainly needs to make this easy and document
    the win.

14. **Multiple targets in one run.** Allow building several triplets or
    architectures in a single invocation with per-target toolchains, rather
    than one `--triplet` per run. The reference build produces m68k, RISC-V,
    arm64, and x86-64 artifacts together.

## Phase 4: distribution and lifecycle

15. **Install and packaging.** Add an `install` action with prefix and
    `DESTDIR` support, and handle relocation: a path baked in at build time
    with a runtime override, as the Makefile does for its include directory.

16. **Version integration.** Provide a stdlib helper for the git-describe
    version pattern (authoritative version file, git only refines it), so
    every project does not re-derive it. The pattern is already documented in
    the project's git workflow; package it.

17. **Explain and diagnose.** Add a mode that answers "why is this target
    rebuilding?" by naming the input, command, or implicit dependency that
    changed. This directly addresses the ordering fragility the Makefile
    comments admit to.

## Phase 5: robustness and portability

18. **Windows staleness.** On Windows the stat fingerprint sets `ino` to 0,
    weakening change detection. Use the file index from
    `GetFileInformationByHandle` or fall back to content hashing.

19. **Diagnostics.** Improve error context: report the full cycle path, not a
    single node, and attach `file:line` to more failures.

20. **Scale.** Benchmark and tune for the thousands of nodes a real toolchain
    build reaches. The open-addressing name table and CDB should hold up;
    confirm it and fix what does not.

## Cross-cutting

- Every item ships with tests in `test/run.sh` and an update to the man page,
  `README.md`, and `HACKING.md` in the same change.
- Each phase should extend the `examples/` tree toward the north-star example,
  so progress is demonstrable and not just internal.
- Keep the zero-dependency, single-translation-unit, bootstrap-from-`cc`
  property. No feature is worth a required external library.
