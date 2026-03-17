# PLAN.md

## Goal

Design and implement a build system, informed by research into existing systems (tup, make, etc.).

## Findings

### Tup  - Core Design

1. File-based build system with a persistent DAG stored in a database (not rescanned each run)
2. Tupfile syntax: `: [foreach] [inputs] [| order-only inputs] |> command |> [outputs]`
3. `%-flags` for substitution in commands: `%f` (inputs), `%b` (basename), `%B` (no ext), `%o` (outputs), `%e` (ext)
4. `$-variables` expand immediately (except in `!-macros`); `@-variables` read from `tup.config`
5. Only one command can write to a given output file  - enforced, not just convention
6. Runtime file-access monitoring intercepts reads/writes during command execution to verify declared deps
7. Undeclared reads from generated files and undeclared writes both trigger errors
8. DAG nodes are files and commands; edges are dependencies
9. Update algorithm: scan for changes -> mark affected DAG nodes -> execute in parallel respecting ordering
10. No `clean` target needed  - the DB tracks all generated files and can delete stale ones automatically
11. `!-macros` for reusable command templates; `{bins}` for output grouping across rules
12. Variants via separate directories each with their own `tup.config`, all updated in parallel
13. Linux monitor mode uses inotify to skip filesystem scanning entirely  - sub-millisecond no-op updates
14. `^o` flag: skip re-running dependents if output is unchanged (content-aware)
15. `^t` flag: transient outputs deleted after dependents run (intermediate files)

### Tup vs Make

16. Make rescans the entire project to find what changed; tup uses its DB  - O(changed) not O(total)
17. At 100k files, make takes ~30 min to figure out what to rebuild; tup does it in ~0.1s
18. Recursive make is both slow and incorrect (changing a header doesn't reliably rebuild dependents)
19. Tup is 5–30% slower on clean/initial builds (one-time cost, acceptable tradeoff)
20. Tup's correctness guarantee: N iterations of updates always produce same result as a clean build

### Tup  - Algorithms (paper)

21. DAG at file-level granularity (not task-level like make) prevents unnecessary rebuilds
22. Content-based change detection vs make's timestamp-only approach
23. Dependencies inferred automatically during execution, not manually enumerated
24. Parallel execution falls naturally out of the DAG  - independent nodes run concurrently
25. Idempotency: running the build twice produces identical results with no redundant work

### Plan 9 mk

26. Rule syntax: `targets:attributes:prerequisites` + recipe body (3-part header, not 2 like make)
27. No built-in rules at all  - generic rules imported via `<` include from prototype mkfiles
28. Recipes passed verbatim to the shell (rc) uninterpreted  - mk doesn't parse them
29. Metarules: `%` matches anything, `&` matches non-`/` non-`.`; matched text available as `$stem`
30. R attribute enables regex metarules with `\1`–`\9` subexpressions (`$stem1`–`$stem9` in recipes)
31. Full dependency graph built before any execution (make builds incrementally)
32. Transitive closure on metarules to derive all reachable targets  - but evaluated once per rule, no infinite loops
33. Cycles prohibited (make allows them)
34. Parallel execution: `$NPROC` controls concurrency; independent recipes run in any order
35. `$nproc` gives each parallel recipe a unique processor ID for temp file naming
36. Attributes instead of special targets: V (virtual), Q (quiet), N (no-exist ok), E (continue on error), D (delete on failure), P (custom staleness check)
37. P attribute: runs a command to decide if target is out-of-date  - fully programmable staleness
38. Variable substitution patterns: `${var:A%B=C%D}`  - ed-style transforms on namelists
39. Variables in rule headers resolve at parse time; variables in recipes resolve at execution time
40. Missing intermediates: won't rebuild `.o` if final binary is up-to-date with original sources  - `-i` overrides
41. Archive syntax `lib(member)` with N attribute for member-level dependency tracking
42. `-w file` flag: treat named files as recently modified to force selective rebuilds
43. D attribute auto-deletes partial targets on recipe failure (prevents stale outputs)
44. Command output capture in variables: `` var=`{cmd} ``
45. Environment variables and mk variables are unified  - no distinction
46. Transitive closure of metarules causes rapid graph growth, but pruning to the desired target keeps it tractable
47. Ambiguity resolution: explicit rules beat metarules; if two metarules both match (e.g. foo.c and foo.s both exist for foo.o), mk errors out rather than guessing
48. `&` pattern (matches non-`/` non-`.`) prevents cross-directory/cross-extension false matches  - more restrictive than `%`
49. Recipes cannot read stdin  - mk uses it internally to pipe recipes to the shell
50. `$newprereq` in recipes contains only the out-of-date prerequisites (not all prereqs)
51. Archive pattern: compile in parallel, archive serially in one recipe  - avoids corruption from concurrent ar updates
52. Adding a prerequisite to a metarule header can create unintended ambiguity  - splitting into separate rule headers avoids it but changes semantics
53. Designed for Plan 9's heterogeneous architecture  - prototype mkfiles per `$objtype` replace make's built-in rules

### Perforce Jam

54. Four-phase execution: startup -> parsing -> binding -> updating (clean separation of concerns)
55. Only data type is a list of strings  - no maps, no integers, no structs
56. Rules are procedures (jam statements); actions are shell commands attached to rules  - these are separate concepts
57. Action modifiers: `quietly` (suppress output), `piecemeal` (chunk large file lists), `existing` (filter to present files), `bind` (substitute bound paths)
58. Variable expansion is a cartesian product: `t$(X)` where X="a b c" produces "ta tb tc"
59. Variable modifiers extract path components: `:D` (dir), `:B` (base), `:S` (suffix), etc.
60. Target-specific variables override globals during that target's processing  - dynamic scoping
61. DEPENDS builds the dependency graph; INCLUDES creates sibling deps (if A includes B, anything depending on A also depends on B)
62. Binding phase: `$(SEARCH)` locates existing sources, `$(LOCATE)` fixes output locations  - explicit path management
63. "Grist" (e.g. `<src!util>filename`) disambiguates same-named targets across directories
64. Header scanning: `$(HDRSCAN)` regex + `$(HDRRULE)` callback  - dynamic, not exact, errs toward over-inclusion
65. Modifier rules: ALWAYS, LEAVES, NOCARE, NOTFILE, NOUPDATE, TEMPORARY  - each changes one aspect of update behavior
66. `-j N` for parallel execution; `-g` reorders by source recency instead of file order
67. `$(JAMSHELL)` controls action execution  - `%` replaced with action text, `!` with process slot number
68. Jambase provides all default rules (Main, Library, Objects, etc.)  - jam itself has no built-in build knowledge
69. Rule hierarchy: Main/Library -> Objects -> Object -> Cc/C++/Yacc/Lex  - suffix-based dispatch with UserObject extensibility hook
70. Pseudotargets (`exe`, `lib`, `obj`, `file`, `shell`) group related file targets  - `all` depends on these groups
71. SubDir rule initializes per-directory SEARCH_SOURCE, LOCATE_TARGET, SOURCE_GRIST  - enables tree-wide builds from any directory
72. Platform abstraction via conditional variables (SUFEXE, SUFLIB, SUFOBJ) and platform flags (UNIX, NT, MAC, etc.)
73. Rules can be invoked in any order  - jam resolves build sequence from the dependency graph, not from declaration order

### Design Decision: Tcl over a custom language

74. Tcl's "everything is a string" matches Jam's "everything is a list of strings"  - same radical simplicity
75. Tcl is a known language with documented semantics; Jam's language is a one-off only Jam users learn
76. Command substitution (`[exec ...]`, `[glob ...]`) maps naturally to build tasks  - Jam needs special syntax
77. Tcl's `{braces}` for literal blocks handle recipe bodies cleanly without inventing new quoting rules
78. Path manipulation already exists in Tcl (`file dirname`, `file tail`, `file extension`)  - better than Jam's `:D`/`:B`/`:S` modifiers
79. Target-specific variables can use Tcl arrays (`set CFLAGS(main.o) "-O2"`) instead of Jam's implicit dynamic scoping
80. Picol 2 (~670 lines, github.com/antirez/picol) provides the Tcl interpreter  - includes expr with Pratt parser, elseif, escape sequences, picolFreeInterp(), PICOL_NO_MAIN for library embedding, capital-letter globals, recursion depth limit
81. What must be added as new Tcl commands: dependency graph primitives (depends, includes, always, notfile, temporary, etc.), action registration (deferred shell commands)
82. Key design question: parse-time (graph building) vs update-time (shell execution) split  - `depends` runs immediately, `action` registers a deferred command

### Ninja

83. Designed as a "build assembler"  - meant to be generated, not hand-written
84. Core insight: "build systems get slow when they need to make decisions"  - eliminate decisions at build time, push them to the generator
85. Variables are bindings (immutable, can only be shadowed in narrower scopes), not mutable variables
86. Commands implicitly depend on their own command string  - changing flags triggers rebuilds with no extra mechanism
87. Three dependency types: explicit (in `$in`), implicit (`| dep`  - triggers rebuild but not in `$in`), order-only (`|| dep`  - must exist before running but changes don't trigger rebuild)
88. Header deps via compiler-generated depfiles (`-M` flags)  - stored compacted in `.ninja_deps` binary log, not rescanned
89. `deps = gcc` processes depfile after compilation; `deps = msvc` parses `/showIncludes` from stdout  - both compiler-native
90. Dynamic dependencies (dyndep, v1.10): load implicit inputs/outputs from generated files during the build  - enables correct first builds when deps are only discoverable by analyzing sources
91. Self-regeneration: if `build.ninja` is a build output and is stale, ninja rebuilds and reloads it before doing anything else
92. Pools limit concurrency for specific rules; `console` pool (depth=1) grants stdin/stdout access
93. Response files (`rspfile`) for Windows command-line length limits  - written before command, deleted after
94. Parallel by default (scaled to CPU count); underspecified deps produce incorrect builds rather than falling back to serial
95. Output buffering: parallel command outputs never interleave; on failure, full command + output shown together
96. Phony rule inputs are "passed through"  - a build depending on a phony target semantically depends on the phony's inputs
97. `subninja` for scoped includes (child can read parent vars, changes don't propagate back) vs `include` for flat inclusion
98. GNU jobserver integration  - respects `MAKEFLAGS` when invoked as part of a larger make-driven build

### Design Decision: Three-phase execution

99. Multi-directory builds (per-directory Kilnfiles) require the full dependency graph before binding  - cross-directory targets are unresolvable during parse
100. Three phases: **parse** (walk tree, evaluate all Kilnfiles, collect targets and dep edges as names) -> **bind** (resolve names to filesystem paths, detect ambiguities/missing targets, compute staleness) -> **update** (execute actions in dependency order)
101. This avoids recursive make's fundamental flaw (partial graph visibility) while keeping per-directory build files
102. Subdirectory discovery: auto-discover Kilnfiles by default, allow explicit overrides (e.g. `subdirs` command to include/exclude)

### Design Decision: C implementation with embedded Tcl

103. Implement kiln in C with Picol 2 (extended) as the embedded Tcl interpreter for build description files
104. Single static binary, zero runtime dependencies  - matches existing `--static-pie` build approach
105. Picol 2 (~670 lines) provides the base interpreter; extend by registering C commands (`depends`, `rule`, `glob`, `file`, etc.)
106. Phase mapping: parse phase runs Tcl interpreter (picol), bind and update phases are pure C on the graph
107. C gives direct fork/exec/waitpid for parallel jobs, stat for timestamps, mmap for depfile parsing
108. User writes Tcl (Kilnfiles); kiln binary handles DAG, binding, staleness, scheduling, output buffering
109. Same architecture as Jam (interpreted language + C engine) but with a known language instead of a one-off

### Design Decision: Source-embedded build directives

110. When no Kilnfile is present in a directory, kiln scans source files for `kiln:` directives in comments
111. Prefix-based: look for `kiln:` after a comment leader  - simple pattern match, no real parsing
112. Comment styles: `//`, `#`, `/* */`, `(* *)`, `--`
113. Minimum directives: `cflags`, `ldflags`, `depends`, `type` (program/library/object, default inferred), `output` (override name)
114. Fallback only  - if a Kilnfile exists, it's authoritative; source annotations only used for unclaimed files
115. Zero-config path for single-file utilities and small libraries  - self-documenting even without kiln installed
116. Scanning cost acceptable since this only triggers in directories without Kilnfile (likely small)
117. Similar precedent: Go's `//go:build` constraints, Rust's cargo-script doc-comment dependencies

### Design Decision: Hybrid staleness detection (stat fingerprint + cached content hash)

118. Store both content hash and stat fingerprint (mtime, size, inode, ctime) per input file after each build
119. On next build: stat() each input, compare against stored fingerprint
120. If stat matches -> file unchanged, skip hash (the 99% fast path  - one stat() call, essentially free)
121. If stat differs -> rehash file, compare against stored hash
122. If hash also differs -> genuinely dirty, rebuild. If hash matches -> update stored stat, skip rebuild (handles touch/no-op edits)
123. Also store command string hash per action  - if CFLAGS etc. change, command string changes, dependents rebuild automatically
124. Racy edge case (file modified within same second as build): fall back to content comparison when mtime equals build timestamp  - rare in practice
125. Full staleness check: command_string changed? -> dirty. For each input: stat changed? -> hash changed? -> dirty. Otherwise skip.
126. Cost: one stat() per input (unavoidable), occasional hash() for touched files, one string compare per command. No full-tree hashing.
127. Inspired by Git's index stat cache (mtime/size/inode/ctime fingerprint to avoid rehashing) and Ninja's command string hashing

### Design Decision: Compiler depfiles for header dependency discovery

128. Parse compiler-generated depfiles (gcc/clang `-MMD` flags) for implicit dependencies
129. If no depfile exists yet -> target is dirty (first build always compiles)
130. After compilation, read the .d file and store implicit deps in kiln's database
131. On subsequent builds, check staleness against both explicit deps (from Kilnfile) and stored implicit deps (from depfile)
132. After each rebuild, re-read the depfile and update stored deps  - the set of headers may have changed
133. If file type doesn't support depfile generation (protobuf, LaTeX, codegen, etc.) -> user must declare deps explicitly in Kilnfile
134. No runtime interception (strace/ptrace/FUSE)  - not portable across Linux/macOS/etc.
135. No regex-based header scanning  - misses conditional includes, computed includes, generated headers; the compiler already solves this correctly
136. Store depfile results compactly in kiln's database (like Ninja's .ninja_deps) to avoid reparsing .d files each run

### Design Decision: Parallelism

137. Parallel by default, scaled to CPU count
138. Rules can specify a pool; default pool is unlimited (up to -j N). `-P poolname=N` limits a named pool.
139. `-j N` sets the global cap across all pools
140. Example: link rules use a `link` pool; `-P link=2` allows 2 concurrent links while compiles run at full parallelism
141. On failure: let in-flight jobs finish, don't start new ones (Ninja behavior)
142. Output buffered per-job (no interleaving); on failure, print full command + output together
143. SIGUSR1 force-flushes all output buffers (debugging aid)

### Design Decision: DAG structure (file + command nodes)

144. Two node types: file nodes and command nodes
145. Graph: input_file -> command -> output_file
146. Multiple outputs are natural  - one command node, multiple output edges (e.g. compiler produces .o + .d)
147. Command string hash attaches to the command node (ties into staleness decision #123)

### Design Decision: V1 scope

148. Target: Tier 2 (actually usable) plus easy wins from Tier 3
149. V1 includes:
  - Picol 2 interpreter extended with build commands (rule, depends)
  - DAG construction with file + command nodes
  - Multi-directory support with auto-discovery of Kilnfiles
  - Full hybrid staleness (stat fingerprint + content hash + command string hash)
  - Compiler depfile parsing for C/C++
  - Parallel execution with buffered output, -j N, SIGUSR1 flush
  - Persistent database for deps/hashes/stat fingerprints
  - `kiln -a clean` (DAG-based clean with empty directory removal)
  - --dry-run / -n with optional script output
150. Stretch goals (if not too much trouble): variant builds, console/streaming pool

### Design Decision: CDB for persistent storage

151. Use djb's CDB (constant database) format for kiln's persistent database (fingerprints, hashes, depfile results)
152. CDB is write-once, read-many  - matches kiln's pattern of rebuilding state each run
153. 2 disk accesses for a hit, 1 for a miss; 2048-byte header + hash tables + key/value records
154. Crash-safe: old kiln.cdb remains intact until new one is atomically renamed into place
155. Trivial to implement inline (~200–300 lines of C), zero external dependencies
156. Workflow: open existing kiln.cdb for staleness reads -> accumulate new entries in memory as jobs complete -> write kiln.cdb.tmp -> atomic rename
157. In-memory accumulation (not streaming cdbmake) because parallel execution means jobs complete in non-deterministic order
158. Reuse djb2 hash (`h = ((h << 5) + h) ^ c`, seed 5381) for both CDB lookups and content/command fingerprinting

### Design Decision: Standard library Tcl commands

211. Implement `foreach`, `append`, `lappend`, `string` as Tcl procs in kiln.tcl  - do not modify picol source
212. These are needed by the standard library procs (program, library) for iterating sources and building lists
213. `foreach` iterates a list calling a body for each element  - uses `lindex`/`llength` C commands for proper list parsing
214. `append` concatenates strings onto a variable
215. `lappend` appends elements to a list variable (space-separated)
216. `string` subcommands as needed (length, index, range, equal, match, etc.)

### Design Decision: Picol line number error reporting

217. Propose upstream change to picol: add `int line` to `struct picolParser`, increment on `\n`
218. Add `char *current_file` and `int current_line` to `struct picolInterp`, updated during eval
219. Error messages include file and line: e.g. "Kilnfile:12: No such command 'foo'"
220. Critical for multi-directory builds where errors could come from any Kilnfile

### Design Decision: Build file naming

248. Build description files are named `Kilnfile` (no extension), following the Makefile/Gemfile convention
249. Unambiguous ownership  - immediately signals this file belongs to kiln
250. Editors can use a Tcl modeline or filetype config for syntax highlighting

### Design Decision: Project root discovery

221. A `top` directive in Kilnfile marks the project root
222. If the current directory's Kilnfile has no `top`, walk up parent directories looking for a Kilnfile with `top`
223. If no `top` is found, error out  - kiln needs a defined project root for artifact directory placement and multi-directory traversal

### Design Decision: Triplet detection and fallback

224. Run `cc -dumpmachine` at startup to detect the native triplet
225. If `--triplet <value>` is specified on the command line, skip `cc -dumpmachine` entirely
226. If `cc -dumpmachine` fails (no compiler, unsupported CC), kiln errors out with a clear message suggesting `--triplet`

### Design Decision: Dry-run mode

227. `--dry-run` / `-n` flag prints commands that would execute without running them
228. Optionally output to a file: `--dry-run=build.sh` or `--dry-run=build.bat` to generate a standalone script
229. Generated script is a primitive but functional representation of the build  - useful for debugging and environments without kiln

### Design Decision: `source` command with search path

230. `source` command loads and evaluates a .tcl file  - standard Tcl semantics
231. Supports a PATH-like search: `source common.tcl` searches a list of directories (e.g. project root `lib/`, then system dirs)
232. Explicit paths (`source ../shared/defs.tcl`) bypass the search and load directly
233. Enables sharing common definitions (CFLAGS, helper procs) across multi-directory builds without duplicating code

### Design Decision: Rule command evaluation

234. The `command` keyword inside `rule` takes a brace-quoted template: `command {cc -c -o $out $in -MMD -MF $depfile}`
235. Braces defer substitution  - kiln evaluates the template *after* all keywords (`inputs`, `depfile`) have been processed, so all variables are set
236. `$out`, `$in`, `$depfile` are Tcl variables; `[cmd]` substitution also works (e.g. `[lindex $out 0]`, `[file rootname $in]`)
237. `rule` collects state from the body, then evaluates the command template in a context where all variables are available

### Design Decision: Multiple outputs from `rule`

238. Multiple outputs specified as a brace-quoted list: `rule {out1.o out2.o} { ... }`
239. Single outputs need no braces: `rule output.o { ... }`
240. Consistent with Tcl list conventions  - `rule` treats argv[1] as a list of output names

### Design Decision: Delete outputs on command failure

241. If a command fails (non-zero exit), kiln deletes all output files declared by that rule
242. Prevents stale partial outputs from appearing "built" on the next run
243. Same behavior as Ninja and mk's D attribute  - but unconditional, not opt-in

### Design Decision: `lindex` and `llength` as C commands

244. `lindex` and `llength` implemented as C-level commands in kiln's picol extensions (not modifying picol.c)
245. Both implement proper Tcl list parsing: split on whitespace, respect `{}` and `""` grouping for elements with spaces
246. `foreach` stays a Tcl proc in kiln.tcl, using `lindex`/`llength` to iterate
247. Keeps the "extend in Tcl where possible, provide primitives in C" philosophy

### Design Decision: Cross-platform support (Linux, macOS, Windows)

159. Thin platform abstraction layer (platform.h) with kiln_spawn(), kiln_wait(), kiln_stat(), etc.
160. Unix: fork/exec, waitpid, poll, sigaction, stat, rename, opendir/readdir, mmap
161. Windows: CreateProcess, WaitForMultipleObjects, _stat, MoveFileEx(MOVEFILE_REPLACE_EXISTING), FindFirstFile/FindNextFile, CreateFileMapping/MapViewOfFile
162. Skip inode (st_ino) on Windows  - use mtime+size+ctime only for stat fingerprint; document in manual
163. No SIGUSR1 on Windows  - skip that feature, it's a debugging convenience
164. Limit to 64 parallel tasks on Windows (WaitForMultipleObjects handle limit); no practical issue
165. No external dependencies on any platform  - no Cygwin/MSYS2 required on Windows

### Design Decision: Unity build for bootstrapping

166. bootstrap.c #includes all .c files into one translation unit  - single `cc bootstrap.c -o kiln` command
167. bootstrap.sh (Unix) and bootstrap.bat (Windows) run that same command with platform-appropriate compiler
168. Avoid static function name collisions across files  - use unique or prefix-namespaced names
169. Once kiln is bootstrapped, it builds itself properly (incremental, parallel, etc.)
170. Precedent: SQLite ships a single amalgamation file for the same reason

### Design Decision: Automatic response files

171. When a command line exceeds a length threshold (~8K), kiln automatically writes arguments to a temp file and passes @tempfile to the tool
172. Transparent to the user  - no special syntax or rule configuration needed
173. Format: arguments as written on command line, whitespace-separated, optional quoting  - same for GCC/Clang and MSVC
174. Supported by GCC, Clang, MSVC cl.exe, link.exe, GNU ld, ar
175. Essential for Windows (32K char command line limit), helpful on Linux for extreme cases (128K–2M ARG_MAX)
176. Temp file deleted after command completes

### Design Decision: Separate build artifact directory

177. Build artifacts go in a separate directory, not alongside sources (out-of-tree builds)
178. Default path: `build/<target-triplet>/` (e.g. `build/x86_64-linux-gnu/`, `build/aarch64-apple-darwin/`)
179. Override via command line (e.g. `kiln -o /tmp/kiln-build-XXXXX`) for CI/CD with concurrent builders sharing one source tree
180. kiln.cdb and all generated files (.o, .d, binaries) live under this directory
181. Source tree stays read-only from kiln's perspective  - only the artifact directory is written to
182. Mirror source tree structure under the artifact dir (e.g. `build/x86_64-linux-gnu/src/lib/foo.o`)
183. Triplet detection: run `cc -dumpmachine` at startup to get the toolchain's native triplet (e.g. `x86_64-linux-gnu`)
184. Ensures the artifact directory matches the actual compiler target, not just the OS/arch kiln happens to run on

### Design Decision: Cross-compilation and Canadian cross support

185. Cross-compilation configured via a cross file (just a .tcl file): sets CC, AR, SYSROOT, HOST_TRIPLET, etc.
186. Invoked as `kiln --cross cross/arm.tcl`  - evaluated before build files, overrides toolchain variables
187. Artifact directory automatically uses HOST_TRIPLET (e.g. `build/arm-linux-gnueabihf/`)
188. Three platform roles: `native` (runs on build machine), default/unmarked (runs on host machine), `target` (runs on target machine  - Canadian cross)
189. Platform is an attribute inside the target block, not a separate top-level command:
```tcl
program codegen {
    native
    sources codegen.c
}
program mycc {
    sources main.c parser.c
}
library runtime {
    target
    sources crt0.c libc.c
}
```
190. `program` and `library` are the only top-level declaration commands; `native`/`target` are attributes evaluated inside the block alongside `sources`, `depends`, etc.
191. Canadian cross adds `--target cross/arm.tcl` for the third toolchain; artifact dirs: `build/<build-triplet>/`, `build/<host-triplet>/`, `build/<target-triplet>/`
192. V1: always two passes when cross-compiling (native pass then host pass); three passes for Canadian cross
193. Future optimization: lazy/deferred  - only build native tools when actually needed during cross build
194. Platform dependencies are transitive  - if a native program depends on a library, that library is also built natively

### Design Decision: Rule primitive and extensibility

195. `rule` is the only build primitive implemented in C  - creates a command node in the DAG with inputs, outputs, and a shell command
196. `rule` syntax (keyword-based block, consistent with `program`/`library`):
```tcl
rule output.o {
    inputs input.c
    depfile output.d
    command {cc -c -o $out $in -MMD -MF $depfile}
}
rule {parse.c parse.h} {
    inputs parse.y
    command {bison -d -o [lindex $out 0] $in}
}
```
197. `command` uses braces to defer substitution; kiln evaluates the template after all keywords are processed
198. `program`, `library`, and other high-level commands are Tcl procs defined in a standard library (kiln.tcl), loaded before any Kilnfiles
199. Users extend kiln by writing Tcl procs that emit rules  - no C changes needed for new file types or tools
200. Standard library defines helper procs like `compile` (single .c -> .o) composed into higher-level procs like `program` and `library`
201. No pattern rules in the engine  - Tcl's foreach/proc/if replaces the need for make-style `%.o: %.c` patterns
202. Pattern rules add engine complexity (ambiguity detection, transitive closure, specificity ranking) that is unnecessary when the iteration and dispatch happen in Tcl
203. Suffix-based dispatch and conditional compilation are just Tcl control flow, not special build system features

### Design Decision: Project layout

204. Source code under `src/`, documentation under `doc/`, executable scripts under `bin/`
205. Top-level directory kept clean  - Kilnfile (kiln's own build file), bootstrap.c (unity build)
206. `bin/` for bootstrap.sh and bootstrap.bat
207. `lib/` for runtime resources like kiln.tcl (standard library with program, library, compile procs)
208. V1: embed kiln.tcl into the binary as a C string  - single self-contained binary, no runtime file dependencies
209. Future: consider loading from /usr/share/kiln/kiln.tcl as a system-wide override (not decided yet)
210. Documentation: README.md (what/why/quickstart, 1–2 pages), doc/kiln.1 (man page  - CLI flags, Kilnfile syntax, standard library, kiln: directives, env vars), doc/DESIGN.md (architecture and rationale for contributors)

### Design Decision: Actions via --action flag

251. Built-in operations (clean, status) use `-a`/`--action` instead of positional arguments
252. Positional arguments are exclusively for target names - no ambiguity if a target is named "clean"
253. `kiln -a clean` deletes all DAG-declared outputs (NODE_FILE with a NODE_CMD producer) and removes empty parent directories bottom-up
254. Clean also removes the CDB file and the build directory itself if empty
255. `kiln -a status` reserved for non-destructive CDB/DAG consistency reporting (not yet implemented)
256. `all` and `test` are not built-in actions - they are Kilnfile conventions (procs that emit rules, implicit default target)

## Tasks

- [x] Research tup (manual, make_vs_tup, build_system_rules_and_algorithms.pdf)
- [x] Research Plan 9 mk
- [x] Research Perforce Jam
- [x] Research Ninja
- [x] Decide: what to name the software -> **kiln**
- [ ] Define design goals and constraints for our build system
- [ ] Implement core build logic

## V1 Scope

### Included in V1
- Picol 2 interpreter extended with build commands (with upstream line number patch)
- `rule` primitive + standard library (`program`, `library`, `compile`, `foreach`, `append`, `lappend`, `string` procs)
- DAG with file + command nodes, cycle detection, topological sort
- Multi-directory support with auto-discovery of Kilnfiles, `top` directive for project root
- Full hybrid staleness (stat fingerprint + content hash + command string hash)
- Compiler depfile parsing for C/C++
- CDB persistent storage
- Parallel execution with `-j N`, buffered output, SIGUSR1 flush
- Build artifact directory with `cc -dumpmachine` triplet detection (or `--triplet` override)
- Cross-compilation via `--cross` (two passes: native then host)
- `native` attribute for build tool targets
- Automatic response files (command line length threshold)
- `--dry-run` / `-n` with optional script output (`--dry-run=build.sh`)
- Platform abstraction for Linux, macOS, Windows
- Unity build bootstrap (bootstrap.c, bootstrap.sh, bootstrap.bat)
- Embedded kiln.tcl standard library (baked into binary as C string)
- `kiln -a clean` (DAG-based clean with empty directory removal)
- Documentation: README.md, doc/DESIGN.md, doc/kiln.1
- kiln builds itself

### Deferred to post-V1
- Canadian cross (`--target`, three passes)  - rare, can be done manually with two kiln invocations
- Source-embedded `kiln:` directives  - users can write a trivial Kilnfile instead; easy to add later
- Named pools (`-P poolname=N`)  - `-j N` global limit is sufficient; pools add scheduler complexity
- Variant builds
- Console/streaming pool
- System-wide kiln.tcl override (/usr/share/kiln/kiln.tcl)

## Implementation Plan

### Milestone 1  - Foundation
- Project layout: `src/`, `doc/`, `bin/`, `lib/`
- Platform abstraction (`src/platform.h`, `src/platform_unix.c`, `src/platform_win32.c`)
- Picol 2 (from github.com/antirez/picol) with PICOL_NO_MAIN and upstream line number patch
- Extend picol with C commands: `glob`, `file` (dirname/tail/extension/rootname), `lindex`, `llength`
- `bootstrap.c`, `bin/bootstrap.sh`, `bin/bootstrap.bat`
- Verify: bootstraps and runs on Linux, macOS, Windows

### Milestone 2  - DAG engine
- File node and command node data structures
- `rule` command registered in picol (the only C-level build primitive)
- `rule` evaluates command block in Tcl context with `$out`, `$in`, `$depfile` pre-set; multiple outputs via brace-quoted list
- `source` command with PATH-like search for shared .tcl files
- Graph construction from evaluated Tcl
- Topological sort / dependency-order traversal
- Cycle detection
- Verify: can parse a Kilnfile and print the execution plan

### Milestone 3  - Staleness and execution (serial)
- `cc -dumpmachine` triplet detection at startup; `--triplet` override skips detection; error if no `--triplet` and `cc -dumpmachine` fails
- Build artifact directory creation (mirroring source tree)
- Stat fingerprinting (mtime, size, inode, ctime)
- djb2 hashing for content and command strings
- CDB reader and writer
- Staleness check (stat fingerprint -> content hash -> command string hash)
- Serial command execution (delete outputs on failure)
- Depfile parsing and storage
- Verify: can build a simple C project using raw `rule` commands, incremental rebuilds work correctly

### Milestone 4  - Standard library
- `lib/kiln.tcl` with `foreach`, `append`, `lappend`, `string`, `compile`, `program`, `library` procs
- Embed kiln.tcl into binary as C string (build step converts .tcl to C string literal)
- `native` attribute handling
- Verify: can build a multi-file C project using `program` and `library` syntax

### Milestone 5  - Multi-directory
- `top` directive for project root; walk up if not found
- Auto-discovery of Kilnfiles in subdirectories
- Explicit subdirectory override (`subdirs` command)
- Verify: can build a multi-directory project

### Milestone 6  - Parallel execution
- Fork/exec job scheduler with `-j N` (Unix) / CreateProcess (Windows)
- Output buffering per-job
- SIGUSR1 flush handler (Unix only)
- Failure handling (finish in-flight, stop starting new)
- 64-job cap on Windows
- Verify: parallel build of a real project, output doesn't interleave

### Milestone 7  - Cross-compilation
- `--cross` flag, cross file loading
- Two-pass build (native then host)
- Verify: cross-compile a simple project

### Milestone 8  - Polish
- Automatic response files (command line length threshold)
- `kiln -a clean` (DAG-based clean with empty directory removal)
- `--dry-run` / `-n` with optional script output
- README.md, doc/DESIGN.md, doc/kiln.1
- Integration test: kiln builds kiln (`Kilnfile` for kiln)
