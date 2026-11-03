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

2. **Tool-binary staleness. (done)** A rule can declare the programs it runs
   with `uses <files...>`. Those files are added as command inputs, so the
   existing staleness check, ordering, parallel readiness, and CDB snapshot
   all apply, while they are kept out of `$in`. Changing a tool (an in-tree
   assembler or an external compiler named by path) forces the commands that
   use it to re-run, and a tool that is itself a build output is built first.
   Hashing a tool resolved from `$PATH` still needs the config-time `exec` in
   item 6 to turn a bare `cc` into a path.

3. **`-a status`. (done)** Reports, without building, which targets are out
   of date (reusing `node_is_dirty` read-only) and flags missing outputs,
   then walks the CDB (via a new `cdb_iter`) and lists `cmd:` entries whose
   rule has been removed. A per-reason "why" breakdown is the fuller explain
   mode in item 17.

4. **Bundle the standard library. (done)** `lib/kiln.tcl` is embedded into
   the binary by `bin/embed.sh` (generating the committed `src/kiln_stdlib.h`)
   and evaluated at startup before Kilnfiles, unless `--no-stdlib` is given,
   which now genuinely skips it. It ships starter helpers (`cc_object`,
   `cp_rule`); the set is limited by the thin Tcl surface and will grow with
   item 5.

## Phase 1: expressive build descriptions

Today a Kilnfile can call `break continue expr if proc puts return set while`
plus `glob file lindex llength`. That is too thin to express a real build
without repetition. The Skjegg Makefile leans on `$(call)`, `$(foreach)`,
and list filtering precisely because the base language is weak.

5. **Grow the Tcl surface. (partly done)** Added as extensions in
   `picol_ext.c`: `list`, `lappend`, `append`, `incr`, `foreach`, `for`,
   `catch`, `eval`, and `string` (`length`, `index`, `range`, `match`, `map`,
   `first`, `last`, `equal`, `tolower`, `toupper`). A pattern-rule-style
   description over a list of sources now reads as a `foreach` loop with no
   external tricks. Still missing and worth adding as needs arise: `lrange`,
   `lsort`, `switch`, `format`, and multi-variable `foreach`.

6. **Config-time `exec`. (done)** `exec command ...` runs a command through
   the shell while the Kilnfile is evaluated and returns its trimmed output
   (wrapping `kiln_run_capture`); a failure raises a Tcl error that `catch`
   can handle. This replaces `$(shell uname -m)` and lets a Kilnfile probe
   the compiler or host before declaring rules. It also enables the item 2
   pattern of tracking a `$PATH` tool: `uses [exec command -v cc]`. Output is
   capped at 64 KiB, which suits probes, not bulk data.

7. **Rule templates. (done)** Done as stdlib helpers rather than new syntax,
   on top of `foreach`/`proc`/`file`: `cc_objects sources flags` returns the
   object list after defining a compile rule per source (Picol has no
   `upvar`, so it returns rather than mutating a caller variable), and
   `cc_program out sources flags` compiles and links in one call. Combined
   with `builddir` and a `foreach` over targets, the per-target rules Make
   writes with a pattern rule plus `$(call)` are now a short loop. The
   compiler is still hard-coded as `cc`; item 8 parameterizes it.

   `cc_objects` mirrors the source tree into the build directory (kiln
   creates each output's directory as needed; see item 18a), so two sources
   with the same base name in different directories do not collide.

8. **Toolchain configuration. (done)** A `getenv name ?default?` command
   reads environment variables with fallbacks, so the wall of `?=` becomes
   `set CC [getenv CC cc]`. The stdlib defaults the global `CC` this way and
   the `cc_*` helpers compile and link with `$CC` (uppercase, so a Kilnfile
   can override the global before a rule, and setting it per target gives
   cross-compilation). Precedence is Kilnfile `set` > environment > the `cc`
   default. Assembler and linker toolchain vars follow the same `getenv`
   convention when helpers need them; an emulator/run wrapper waits on the
   run targets in item 10.

## Phase 2: targets that are not files

A build system for a compiler spends most of its rules running things, not
producing files. The Skjegg Makefile has 327 rules, many of them phony test
and run targets. kiln today requires every rule to produce a file output,
which rules out `check`, `run`, and `install` as first-class targets.

9. **Phony and task targets. (done)** A `phony name { body }` command
   declares a named target that runs a command but produces no file. It is a
   command node with a `NODE_PHONY` output carrying the name; because it has
   no file output, `node_is_dirty` already treats it as always out of date,
   so it runs whenever the build reaches it, and `status` labels it by name.
   The body reuses `rule`'s inputs/uses/command machinery (factored into
   `expand_command` and `add_input_edges`). Since kiln still builds the whole
   DAG, a phony runs on every build; per-target selection (item 11) is what
   makes `run`/`test` tasks run on demand rather than every time.

10. **Run and test integration. (done)** Two stdlib helpers on top of
    `phony`: `run name program opts` builds `program` and launches it, and
    `test name program opts` does the same for a test whose exit status is
    the verdict. The launch is wrapped by the global `RUN` (empty by default,
    `getenv RUN`), so a cross-compiled binary runs under an emulator by
    setting `RUN=qemu-arm`; this is what the 197 `qemu` lines do by hand. The
    program is a tracked input, so it is rebuilt before each run. A non-zero
    exit fails the build, and `build_execute` now reports the failing target
    (via `node_label`) and its command alongside the captured output, the
    same for a failing run as for a failing compile. Output comparison beyond
    exit status is left to the command itself (the template is arbitrary
    shell); a golden-output helper can follow if a real need appears.

11. **Default targets, aliases, and groups. (done)** Positional arguments are
    now targets, not Kilnfiles; `-f`/`--file` (repeatable, default `Kilnfile`)
    selects the build description. A requested target builds only its upstream
    cone: `main` marks that cone in a `needed[]` array that `build_execute`
    honors, so unrelated rules and phony tasks stay idle. `default <targets>`
    sets what builds when the command line names none, and `alias <name>
    { targets }` expands one name to many (recursively), so `check` can mean
    "run every test". With no CLI targets and no default, the whole graph
    builds as before.

## Phase 3: variants, generation, and multiple targets

12. **Build variants. (done)** A `--variant NAME` flag (default `release`)
    sets `$Variant`, and the stdlib `variant name flags` selector appends to
    the `CFLAGS`/`LDFLAGS` globals and places the build under
    `build/<triplet>/<name>` when its name is active. Debug, release, and
    sanitizer builds are declared as one `variant` line each, share the same
    rules, and keep separate output trees and staleness databases. Variant
    flags reach both compile and link, so the Skjegg sanitizer set becomes a
    single `variant asan {...}` rather than a bolted-on `CFLAGS` copy. The
    flag globals start empty, so a build depends on the Kilnfile, not the
    environment.

13. **Generated files and codegen. (done)** Two stdlib helpers. `generate out
    inputs command` is a thin, intent-naming wrapper over `rule` for a
    generated source such as a parser table. `version_header out macro`
    probes `git describe` with config-time `exec` and embeds the result in
    the generator command, so command hashing reruns it exactly when the
    version changes and an unchanged version rewrites nothing, with no
    `FORCE` rule or `cmp` guard. (The win is command and content hashing
    keeping an unchanged generator from running; kiln still cascades to
    consumers whenever a producer does run this build, so a forced generator
    was deliberately avoided.)

14. **Multiple targets in one run. (done)** `--triplet` is repeatable. The
    command line is parsed once into a `struct kiln_options`, and `build_one`
    runs the whole evaluate-and-build pipeline per triplet in its own
    interpreter and DAG, so `$Triplet` (and a `$Triplet`-derived toolchain
    and build directory) differ per build with no state leaking between them.
    A failure in one triplet does not stop the others; kiln exits nonzero if
    any failed. So one invocation produces the m68k, RISC-V, arm64, and
    x86-64 trees the reference build needs, each with its own toolchain.

## Phase 4: distribution and lifecycle

15. **Install and packaging. (done)** An `install src dest` command records
    what to install, and `kiln -a install` copies each output to
    `$DESTDIR$prefix/dest` (via a new `kiln_copy_file` platform primitive),
    creating directories and preserving the executable bit. `--prefix`
    (default `/usr/local`) and `--destdir` (or the `DESTDIR` environment
    variable) control the destination. For relocation, `$Prefix` is exported
    so a Kilnfile bakes the install path into the build with `define_header`
    (or a `-D` flag), and the program reads an environment override at
    runtime, so a staged or relocated install still finds its files.

16. **Version integration. (done)** `git_version base` packages the
    authoritative-file pattern from the project's git workflow: it refines a
    tracked base version with `git describe --match v<base>`, returning the
    base for a clean release, the base plus git's suffix past the tag,
    `base-dev-g<sha>` with no matching tag, and the bare base without git.
    Matching `v<base>` keeps a vendored copy from describing against a host
    project's tags. `define_header out macro value` writes any value into a
    guarded header, so `define_header ... [git_version [exec cat VERSION]]`
    is the whole pattern. (The simpler `version_header`, from item 13, stays
    for a quick bare `git describe`, and now shares `define_header`.)

17. **Explain and diagnose. (done)** `kiln -a explain` reports, without
    building, why each out-of-date target would rebuild, naming the cause: a
    changed or first-seen command, a changed or missing input, an input
    rebuilt by an out-of-date rule (a cascade), a changed implicit header, or
    a missing output. It computes direct dirtiness like `status`, then
    propagates cascades in topological order so the reported set matches what
    a build would run (a program is listed because its object will be
    rebuilt, not only the object). This answers the ordering questions the
    Makefile comments admit to.

## Phase 5: robustness and portability

18. **Windows staleness.** On Windows the stat fingerprint sets `ino` to 0,
    weakening change detection. Use the file index from
    `GetFileInformationByHandle` or fall back to content hashing.

18a. **Create output directories. (done)** Before running a command,
    `build_execute` now creates the parent directory of each of its file
    outputs and of its depfile (via `ensure_output_dirs`/`kiln_mkdir_p`), so
    a rule may write into a subdirectory of the build directory that does
    not yet exist. With that in place, `cc_objects` mirrors the source tree
    (`src/a/foo.c` -> `src/a/foo.o`) instead of flattening object names,
    which also stops same-named sources in different directories from
    colliding.

19. **Diagnostics.** Improve error context: report the full cycle path, not a
    single node, and attach `file:line` to more failures.

19a. **Command-template quoting. (done)** A `command {...}` template is
    expanded by wrapping it as `set _ "<template>"` to defer substitution,
    which let a literal double quote in the template close the string early
    ("Wrong number of args for set"). Embedded double quotes are now escaped
    when building that wrapper, so commands can contain quotes while `$var`
    and `[cmd]` still expand. Backslash handling still follows Tcl
    double-quote rules; revisit if that proves surprising.

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
