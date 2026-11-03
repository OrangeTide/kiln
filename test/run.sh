#!/bin/sh
#
# kiln test suite — smoke and integration tests for CI/CD.
# Usage: sh test/run.sh [path-to-kiln]
#
set -e

KILN="${1:-./kiln}"
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

pass() { PASS=$((PASS + 1)); printf "  PASS: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL: %s\n" "$1"; }

check() {
    # check "name" "actual" "expected"
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1"
        printf "    expected: %s\n    got:      %s\n" "$3" "$2"
    fi
}

check_contains() {
    # check_contains "name" "haystack" "needle"
    case "$2" in
        *"$3"*) pass "$1" ;;
        *) fail "$1"; printf "    expected to contain: %s\n    got: %s\n" "$3" "$2" ;;
    esac
}

check_not_contains() {
    case "$2" in
        *"$3"*) fail "$1"; printf "    expected NOT to contain: %s\n    got: %s\n" "$3" "$2" ;;
        *) pass "$1" ;;
    esac
}

check_file_exists() {
    if [ -f "$2" ]; then pass "$1"; else fail "$1"; printf "    missing: %s\n" "$2"; fi
}

check_file_missing() {
    if [ ! -f "$2" ]; then pass "$1"; else fail "$1"; printf "    should not exist: %s\n" "$2"; fi
}

# Resolve to absolute path
KILN=$(cd "$(dirname "$KILN")" && pwd)/$(basename "$KILN")

# =========================================================================
printf "=== CLI ===\n"
# =========================================================================

OUT=$("$KILN" --help 2>&1 || true)
check_contains "help flag" "$OUT" "Usage: kiln"

OUT=$("$KILN" --triplet test-triple -f /dev/null 2>&1 || true)
check_not_contains "triplet override" "$OUT" "cc -dumpmachine failed"

OUT=$("$KILN" --bogus 2>&1 || true)
check_contains "unknown option" "$OUT" "unknown option"

# =========================================================================
printf "=== Tcl extensions ===\n"
# =========================================================================

T="$TMPDIR/ext"
mkdir -p "$T"
cat > "$T/test.tcl" <<'TCLEOF'
# lindex / llength
set list "alpha bravo charlie"
puts [llength $list]
puts [lindex $list 0]
puts [lindex $list 2]

# brace-quoted elements
set list2 "{hello world} foo {bar baz}"
puts [llength $list2]
puts [lindex $list2 0]

# file commands
puts [file dirname src/lib/foo.c]
puts [file tail src/lib/foo.c]
puts [file extension src/lib/foo.c]
puts [file rootname src/lib/foo.c]
TCLEOF

OUT=$("$KILN" --triplet t -f "$T/test.tcl" 2>&1)
check "llength" "$(echo "$OUT" | sed -n '1p')" "3"
check "lindex 0" "$(echo "$OUT" | sed -n '2p')" "alpha"
check "lindex 2" "$(echo "$OUT" | sed -n '3p')" "charlie"
check "llength brace" "$(echo "$OUT" | sed -n '4p')" "3"
check "lindex brace elem" "$(echo "$OUT" | sed -n '5p')" "hello world"
check "file dirname" "$(echo "$OUT" | sed -n '6p')" "src/lib"
check "file tail" "$(echo "$OUT" | sed -n '7p')" "foo.c"
check "file extension" "$(echo "$OUT" | sed -n '8p')" ".c"
check "file rootname" "$(echo "$OUT" | sed -n '9p')" "src/lib/foo"

# =========================================================================
printf "=== Error reporting ===\n"
# =========================================================================

T="$TMPDIR/err"
mkdir -p "$T"

cat > "$T/test.tcl" <<'TCLEOF'
set x 1
set y 2
badcmd foo
TCLEOF

OUT=$("$KILN" --triplet t -f "$T/test.tcl" 2>&1 || true)
check_contains "error file:line" "$OUT" "test.tcl:3:"
check_contains "error command name" "$OUT" "badcmd"

cat > "$T/test2.tcl" <<'TCLEOF'
set x 1

# comment
set y 2

# another comment

puts $noexist
TCLEOF

OUT=$("$KILN" --triplet t -f "$T/test2.tcl" 2>&1 || true)
check_contains "error line with comments" "$OUT" "test2.tcl:8:"

# =========================================================================
printf "=== Source command ===\n"
# =========================================================================

T="$TMPDIR/source"
mkdir -p "$T/lib"

cat > "$T/lib/defs.tcl" <<'TCLEOF'
set GREETING hello
TCLEOF

cat > "$T/main.tcl" <<'TCLEOF'
set SourcePath LIB_PATH_PLACEHOLDER
source defs.tcl
puts $GREETING
TCLEOF
# Patch in the actual path (portable: BSD sed lacks GNU's in-place -i)
sed "s|LIB_PATH_PLACEHOLDER|$T/lib|" "$T/main.tcl" > "$T/main.tcl.new"
mv "$T/main.tcl.new" "$T/main.tcl"

OUT=$("$KILN" --triplet t -f "$T/main.tcl" 2>&1)
check_contains "source PATH search" "$OUT" "hello"

OUT=$("$KILN" --triplet t -n -f "$T/main.tcl" 2>&1)
check_contains "source with dry-run" "$OUT" "hello"

# Source error in sourced file
cat > "$T/lib/bad.tcl" <<'TCLEOF'
set a 1
badcmd
TCLEOF

cat > "$T/err.tcl" <<'TCLEOF'
set SourcePath LIB_PATH_PLACEHOLDER
source bad.tcl
TCLEOF
sed "s|LIB_PATH_PLACEHOLDER|$T/lib|" "$T/err.tcl" > "$T/err.tcl.new"
mv "$T/err.tcl.new" "$T/err.tcl"

OUT=$("$KILN" --triplet t -f "$T/err.tcl" 2>&1 || true)
check_contains "error in sourced file" "$OUT" "bad.tcl:2:"

# =========================================================================
printf "=== DAG and cycle detection ===\n"
# =========================================================================

T="$TMPDIR/dag"
mkdir -p "$T"

cat > "$T/cycle.tcl" <<'TCLEOF'
rule a.o {
    inputs b.o
    command {cp $in $out}
}
rule b.o {
    inputs a.o
    command {cp $in $out}
}
TCLEOF

OUT=$("$KILN" --triplet t -f "$T/cycle.tcl" 2>&1 || true)
check_contains "cycle detected" "$OUT" "dependency cycle"

# Multiple outputs
cat > "$T/multi.tcl" <<'TCLEOF'
rule {parse.c parse.h} {
    inputs parse.y
    command {bison -d -o $out $in}
}
TCLEOF

OUT=$("$KILN" --triplet t -n -f "$T/multi.tcl" 2>&1)
check_contains "multi-output rule" "$OUT" "bison"

# =========================================================================
printf "=== Build: clean build ===\n"
# =========================================================================

T="$TMPDIR/build"
mkdir -p "$T"

cat > "$T/main.c" <<'EOF'
#include <stdio.h>
#include "util.h"
int main(void) { printf("%d\n", add(2, 3)); return 0; }
EOF
cat > "$T/util.c" <<'EOF'
#include "util.h"
int add(int a, int b) { return a + b; }
EOF
cat > "$T/util.h" <<'EOF'
int add(int a, int b);
EOF
cat > "$T/Kilnfile" <<'EOF'
rule main.o {
    inputs main.c
    depfile main.d
    command {cc -c -o $out $in -MMD -MF $depfile}
}
rule util.o {
    inputs util.c
    depfile util.d
    command {cc -c -o $out $in -MMD -MF $depfile}
}
rule hello {
    inputs {main.o util.o}
    command {cc -o $out $in}
}
EOF

cd "$T"

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "clean build count" "$OUT" "built 3 targets"
check_file_exists "clean build: main.o" "$T/main.o"
check_file_exists "clean build: util.o" "$T/util.o"
check_file_exists "clean build: hello" "$T/hello"

RESULT=$("$T/hello")
check "clean build output" "$RESULT" "5"

# =========================================================================
printf "=== Build: incremental no-op ===\n"
# =========================================================================

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "no-op rebuild" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: touch without content change ===\n"
# =========================================================================

sleep 1  # ensure mtime differs
touch "$T/util.c"

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "touch no-op" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: content change cascades ===\n"
# =========================================================================

cat > "$T/util.c" <<'EOF'
#include "util.h"
int add(int a, int b) { return a + b + 10; }
EOF

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "cascade: util.o rebuilt" "$OUT" "cc -c -o util.o"
check_contains "cascade: hello relinked" "$OUT" "cc -o hello"
check_not_contains "cascade: main.o skipped" "$OUT" "cc -c -o main.o"
check_contains "cascade: count" "$OUT" "built 2 targets"

RESULT=$("$T/hello")
check "cascade output" "$RESULT" "15"

# =========================================================================
printf "=== Build: header change via depfile ===\n"
# =========================================================================

cat > "$T/util.h" <<'EOF'
int add(int a, int b);
/* force rebuild */
EOF

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "header: main.o rebuilt" "$OUT" "cc -c -o main.o"
check_contains "header: util.o rebuilt" "$OUT" "cc -c -o util.o"
check_contains "header: hello relinked" "$OUT" "cc -o hello"
check_contains "header: count" "$OUT" "built 3 targets"

# =========================================================================
printf "=== Build: command string change ===\n"
# =========================================================================

# Change CFLAGS in the rule
cat > "$T/Kilnfile" <<'EOF'
rule main.o {
    inputs main.c
    depfile main.d
    command {cc -c -O2 -o $out $in -MMD -MF $depfile}
}
rule util.o {
    inputs util.c
    depfile util.d
    command {cc -c -O2 -o $out $in -MMD -MF $depfile}
}
rule hello {
    inputs {main.o util.o}
    command {cc -o $out $in}
}
EOF

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "cmd change: recompile" "$OUT" "-O2"
check_contains "cmd change: count" "$OUT" "built 3 targets"

# Restore and verify no-op
cat > "$T/Kilnfile" <<'EOF'
rule main.o {
    inputs main.c
    depfile main.d
    command {cc -c -O2 -o $out $in -MMD -MF $depfile}
}
rule util.o {
    inputs util.c
    depfile util.d
    command {cc -c -O2 -o $out $in -MMD -MF $depfile}
}
rule hello {
    inputs {main.o util.o}
    command {cc -o $out $in}
}
EOF

OUT=$("$KILN" --triplet test -f Kilnfile 2>&1)
check_contains "cmd same: no-op" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: parallel (-j) ===\n"
# =========================================================================

TP="$TMPDIR/par"
mkdir -p "$TP"

cat > "$TP/a.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$TP/b.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$TP/Kilnfile" <<'EOF'
rule a {
    inputs a.c
    command {cc -o $out $in}
}
rule b {
    inputs b.c
    command {cc -o $out $in}
}
EOF

cd "$TP"

OUT=$("$KILN" --triplet test -j 4 -f Kilnfile 2>&1)
check_contains "parallel build count" "$OUT" "built 2 targets"
check_file_exists "parallel build: a" "$TP/a"
check_file_exists "parallel build: b" "$TP/b"

OUT=$("$KILN" --triplet test -j 4 -f Kilnfile 2>&1)
check_contains "parallel no-op rebuild" "$OUT" "nothing to do"

OUT=$("$KILN" --triplet test -j 1 -f Kilnfile 2>&1)
check_contains "serial (-j 1) no-op" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: delete outputs on failure ===\n"
# =========================================================================

T2="$TMPDIR/fail"
mkdir -p "$T2"

echo "syntax error!!!" > "$T2/bad.c"
touch "$T2/bad.o"

cat > "$T2/Kilnfile" <<'EOF'
rule bad.o {
    inputs bad.c
    command {cc -c -o $out $in}
}
EOF

cd "$T2"
OUT=$("$KILN" --triplet test -f Kilnfile 2>&1 || true)
check_contains "failure names target" "$OUT" "bad.o failed with exit code"
check_contains "failure reports command" "$OUT" "command:"
check_file_missing "output deleted on fail" "$T2/bad.o"

# =========================================================================
printf "=== Build: dry-run ===\n"
# =========================================================================

cd "$T"
rm -f "$T/main.o" "$T/util.o" "$T/hello" "$T/main.d" "$T/util.d"
rm -rf "$T/build"

OUT=$("$KILN" --triplet test -n -f Kilnfile 2>&1)
check_contains "dry-run shows commands" "$OUT" "cc -c"
check_file_missing "dry-run: no main.o" "$T/main.o"
check_file_missing "dry-run: no hello" "$T/hello"

# =========================================================================
printf "=== Build: quoted command ===\n"
# =========================================================================

TQ="$TMPDIR/quote"
mkdir -p "$TQ"
cd "$TQ"

echo "hi" > src.txt

cat > Kilnfile <<'EOF'
rule q.txt {
    inputs src.txt
    command {sh -c "cp $in q.txt"}
}
EOF

# $in expands and the literal double quotes survive into the command.
OUT=$("$KILN" --triplet t -n -f Kilnfile 2>&1)
check_contains "quoted command expands \$in" "$OUT" "cp src.txt q.txt"
check_contains "quoted command keeps quotes" "$OUT" '"cp src.txt q.txt"'

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "quoted command builds" "$OUT" "built 1 target"
check "quoted command output" "$(cat q.txt)" "hi"

# =========================================================================
printf "=== Build: tool dependency (uses) ===\n"
# =========================================================================

TU="$TMPDIR/uses"
mkdir -p "$TU"
cd "$TU"

echo "hello" > in.txt
printf 'cp "$1" "$2"\n' > gen.sh

cat > Kilnfile <<'EOF'
rule out.txt {
    inputs in.txt
    uses gen.sh
    command {sh gen.sh $in out.txt}
}
rule probe.txt {
    inputs in.txt
    uses gen.sh
    command {echo $in > probe.txt}
}
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "uses: initial build" "$OUT" "built 2 targets"
check_file_exists "uses: out.txt" "$TU/out.txt"
check "uses: tool kept out of \$in" "$(cat probe.txt)" "in.txt"

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "uses: no-op when tool unchanged" "$OUT" "nothing to do"

# Changing the tool forces a rebuild even though inputs are unchanged.
sleep 1
printf 'cp "$1" "$2"; echo TOOL2 >> "$2"\n' > gen.sh
OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_not_contains "uses: tool change rebuilds" "$OUT" "nothing to do"
check_contains "uses: output regenerated" "$(cat out.txt)" "TOOL2"

# A tool that is itself a build output: it must build first, and changing
# its source must cascade to the command that uses it.
TV="$TMPDIR/uses2"
mkdir -p "$TV"
cd "$TV"

printf 'cp "$1" "$2"\n' > tool.src
echo "data" > data.txt

cat > Kilnfile <<'EOF'
rule tool.sh {
    inputs tool.src
    command {cp $in $out}
}
rule out.txt {
    inputs data.txt
    uses tool.sh
    command {sh tool.sh data.txt out.txt}
}
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "uses in-tree: tool and user built" "$OUT" "built 2 targets"
check_file_exists "uses in-tree: out.txt" "$TV/out.txt"

sleep 1
printf 'cp "$1" "$2"; echo TOOL2 >> "$2"\n' > tool.src
OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "uses in-tree: tool rebuild cascades" "$OUT" "built 2 targets"
check_contains "uses in-tree: output regenerated" "$(cat out.txt)" "TOOL2"

# =========================================================================
printf "=== Standard library (kiln.tcl) ===\n"
# =========================================================================

TS="$TMPDIR/stdlib"
mkdir -p "$TS"
cd "$TS"

echo "payload" > src.txt

cat > Kilnfile <<'EOF'
cp_rule copy.txt src.txt
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "stdlib cp_rule builds" "$OUT" "built 1 target"
check "stdlib cp_rule output" "$(cat copy.txt)" "payload"

# --no-stdlib must leave the helpers undefined.
OUT=$("$KILN" --no-stdlib --triplet t -f Kilnfile 2>&1 || true)
check_contains "no-stdlib disables helpers" "$OUT" "No such command"

cat > o.c <<'EOF'
int answer(void) { return 42; }
EOF
cat > Kilnfile <<'EOF'
cc_object o.o o.c o.d "-O2"
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "stdlib cc_object builds" "$OUT" "built 1 target"
check_file_exists "stdlib cc_object output" "$TS/o.o"
check_file_exists "stdlib cc_object depfile" "$TS/o.d"

# cc_program: compile several sources and link, in one call.
cat > prog.c <<'EOF'
int helper(void);
int main(void) { return helper(); }
EOF
cat > helper.c <<'EOF'
int helper(void) { return 0; }
EOF
cat > Kilnfile <<'EOF'
cc_program app "prog.c helper.c" "-O2"
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "stdlib cc_program builds" "$OUT" "built 3 targets"
check_file_exists "stdlib cc_program output" "$TS/app"
check_file_exists "stdlib cc_program object" "$TS/helper.o"

# =========================================================================
printf "=== Status action ===\n"
# =========================================================================

TST="$TMPDIR/status"
mkdir -p "$TST"
cd "$TST"

echo a > a.c
echo b > b.c

cat > Kilnfile <<'EOF'
rule a.o {
    inputs a.c
    command {cp $in $out}
}
rule b.o {
    inputs b.c
    command {cp $in $out}
}
EOF

OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "status: no build state" "$OUT" "no build state"

"$KILN" --triplet t -f Kilnfile >/dev/null 2>&1

OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "status: all up to date" "$OUT" "2 up to date"

# A changed input is reported, and status must not build it.
sleep 1
echo changed > a.c
OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "status: out of date count" "$OUT" "1 out of date"
check_contains "status: names target" "$OUT" "a.o"
check "status: did not build" "$(cat a.o)" "a"

# A missing output is flagged.
echo a > a.c
"$KILN" --triplet t -f Kilnfile >/dev/null 2>&1
rm -f b.o
OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "status: missing output" "$OUT" "missing output"

# A removed rule leaves a stale CDB entry.
"$KILN" --triplet t -f Kilnfile >/dev/null 2>&1
cat > Kilnfile <<'EOF'
rule a.o {
    inputs a.c
    command {cp $in $out}
}
EOF
OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "status: stale entry header" "$OUT" "stale entries"
check_contains "status: stale entry name" "$OUT" "b.o"

# =========================================================================
printf "=== Tcl surface (foreach, list, string, ...) ===\n"
# =========================================================================

TX="$TMPDIR/tclx"
mkdir -p "$TX"

cat > "$TX/t.tcl" <<'TCLEOF'
set xs [list a b "c d"]
puts [llength $xs]
set acc ""
lappend acc one
lappend acc two
puts $acc
set n 0
foreach x $xs { incr n }
puts $n
set sum 0
for {set k 1} {$k <= 3} {incr k} { incr sum $k }
puts $sum
puts [string range hello 1 3]
puts [string match f*.c foo.c]
puts [string map {a X} banana]
puts [catch {nosuchcmd}]
TCLEOF

OUT=$("$KILN" --triplet t -f "$TX/t.tcl" 2>&1)
check "list/llength" "$(echo "$OUT" | sed -n 1p)" "3"
check "lappend" "$(echo "$OUT" | sed -n 2p)" "one two"
check "foreach count" "$(echo "$OUT" | sed -n 3p)" "3"
check "for sum" "$(echo "$OUT" | sed -n 4p)" "6"
check "string range" "$(echo "$OUT" | sed -n 5p)" "ell"
check "string match" "$(echo "$OUT" | sed -n 6p)" "1"
check "string map" "$(echo "$OUT" | sed -n 7p)" "bXnXnX"
check "catch error" "$(echo "$OUT" | sed -n 8p)" "1"

# A pattern-rule-style build: generate a rule per source with foreach.
TXR="$TMPDIR/tclrules"
mkdir -p "$TXR"
cd "$TXR"

echo a > a.c
echo b > b.c

cat > Kilnfile <<'EOF'
set objs ""
foreach s "a b" {
    lappend objs $s.o
    rule $s.o {
        inputs $s.c
        command {cp $in $out}
    }
}
rule all.txt {
    inputs $objs
    command {cat $in > $out}
}
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "foreach-generated rules build" "$OUT" "built 3 targets"
check_file_exists "foreach rule output a.o" "$TXR/a.o"
check_file_exists "foreach collected link" "$TXR/all.txt"

# =========================================================================
printf "=== Config-time exec ===\n"
# =========================================================================

TE="$TMPDIR/exec"
mkdir -p "$TE"

cat > "$TE/t.tcl" <<'TCLEOF'
puts [exec echo hello]
puts [exec echo a b c]
puts [catch {exec false}]
TCLEOF

OUT=$("$KILN" --triplet t -f "$TE/t.tcl" 2>&1)
check "exec output" "$(echo "$OUT" | sed -n 1p)" "hello"
check "exec joins args" "$(echo "$OUT" | sed -n 2p)" "a b c"
check "exec failure via catch" "$(echo "$OUT" | sed -n 3p)" "1"

cd "$TE"
cat > Kilnfile <<'EOF'
set greeting [exec echo hi]
rule out.txt {
    command {echo $greeting > out.txt}
}
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "exec drives a rule" "$OUT" "built 1 target"
check "exec rule output" "$(cat out.txt)" "hi"

# =========================================================================
printf "=== Toolchain configuration ===\n"
# =========================================================================

TTC="$TMPDIR/toolchain"
mkdir -p "$TTC"
cd "$TTC"

echo 'int main(void) { return 0; }' > m.c

cat > g.tcl <<'TCLEOF'
puts [getenv KILN_NOPE_VAR fallback]
puts [string length [getenv KILN_NOPE_VAR]]
TCLEOF

OUT=$("$KILN" --triplet t -f g.tcl 2>&1)
check "getenv default" "$(echo "$OUT" | sed -n 1p)" "fallback"
check "getenv unset is empty" "$(echo "$OUT" | sed -n 2p)" "0"

# A Kilnfile can override the compiler the cc_* helpers use.
cat > Kilnfile <<'EOF'
set CC MYCOMPILER
cc_program app m.c "-O2"
EOF
OUT=$("$KILN" --triplet t -n -f Kilnfile 2>&1)
check_contains "CC override in Kilnfile" "$OUT" "MYCOMPILER -O2 -c"

# The environment overrides CC when the Kilnfile does not set it.
cat > Kilnfile2 <<'EOF'
cc_program app m.c "-O2"
EOF
OUT=$(CC=ENVCC "$KILN" --triplet t -n -f Kilnfile2 2>&1)
check_contains "CC override from env" "$OUT" "ENVCC -o app"

# =========================================================================
printf "=== Phony targets ===\n"
# =========================================================================

TPH="$TMPDIR/phony"
mkdir -p "$TPH"
cd "$TPH"

echo hi > src.txt

cat > Kilnfile <<'EOF'
rule out.txt {
    inputs src.txt
    command {cp $in $out}
}
phony show {
    inputs out.txt
    command {cat out.txt}
}
EOF

OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "phony: first build runs file and phony" "$OUT" "built 2 targets"
check_contains "phony: command output" "$OUT" "hi"

# The file target is up to date, but the phony still runs.
OUT=$("$KILN" --triplet t -f Kilnfile 2>&1)
check_contains "phony: reruns when file up to date" "$OUT" "built 1 target"
check_contains "phony: still produces output" "$OUT" "hi"

OUT=$("$KILN" --triplet t -a status -f Kilnfile 2>&1)
check_contains "phony: status labels by name" "$OUT" "show"

cat > Kilnfile <<'EOF'
phony bad {
    inputs src.txt
}
EOF
OUT=$("$KILN" --triplet t -f Kilnfile 2>&1 || true)
check_contains "phony: missing command errors" "$OUT" "no command specified"

# =========================================================================
printf "=== Target selection ===\n"
# =========================================================================

TSEL="$TMPDIR/select"
mkdir -p "$TSEL"
cd "$TSEL"

cat > Kilnfile <<'EOF'
rule dep.txt {
    command {sh -c "echo D > dep.txt"}
}
rule app {
    inputs dep.txt
    command {sh -c "echo A > app"}
}
rule other.txt {
    command {sh -c "echo O > other.txt"}
}
default app
alias everything {app other.txt}
EOF

# An explicit target builds only its own cone: app pulls in dep.txt but
# leaves the unrelated other.txt alone.
OUT=$("$KILN" --triplet t app 2>&1)
check_contains "select: requested target builds its cone" "$OUT" "built 2 targets"
check_file_exists "select: app built" "$TSEL/app"
check_file_exists "select: dependency built" "$TSEL/dep.txt"
check_file_missing "select: unrelated target skipped" "$TSEL/other.txt"

# A bare invocation builds the declared default.
rm -f app dep.txt other.txt
OUT=$("$KILN" --triplet t 2>&1)
check_contains "select: default target used" "$OUT" "built 2 targets"
check_file_exists "select: default built app" "$TSEL/app"
check_file_missing "select: default skips other" "$TSEL/other.txt"

# An alias expands to several targets.
rm -f app dep.txt other.txt
OUT=$("$KILN" --triplet t everything 2>&1)
check_contains "select: alias builds all members" "$OUT" "built 3 targets"
check_file_exists "select: alias built app" "$TSEL/app"
check_file_exists "select: alias built other" "$TSEL/other.txt"

# An unknown target is a hard error.
OUT=$("$KILN" --triplet t nosuch 2>&1 || true)
check_contains "select: unknown target errors" "$OUT" "unknown target 'nosuch'"

# With builddir active, a bare target name resolves to the prefixed output.
TSELB="$TMPDIR/selectbd"
mkdir -p "$TSELB"
cd "$TSELB"
cat > Kilnfile <<'EOF'
builddir out
rule prog {
    command {sh -c "echo P > $out"}
}
EOF
OUT=$("$KILN" --triplet t prog 2>&1)
check_contains "select: bare name resolves under builddir" "$OUT" "built 1 target"
check_file_exists "select: builddir output created" "$TSELB/out/prog"

# =========================================================================
printf "=== Run and test targets ===\n"
# =========================================================================

TRUN="$TMPDIR/run"
mkdir -p "$TRUN"
cd "$TRUN"

cat > Kilnfile <<'EOF'
rule prog {
    command {sh -c "printf '#!/bin/sh\necho RAN\n' > prog; chmod +x prog"}
}
rule badprog {
    command {sh -c "printf '#!/bin/sh\nexit 2\n' > badprog; chmod +x badprog"}
}
run go prog {}
run withargs prog {--flag value}
test bad badprog {}
EOF

# run builds the program, then launches it.
OUT=$("$KILN" --triplet t go 2>&1)
check_contains "run: builds and launches" "$OUT" "built 2 targets"
check_contains "run: program output shown" "$OUT" "RAN"

# opts are appended to the launch command.
OUT=$("$KILN" --triplet t -n withargs 2>&1)
check_contains "run: opts appended" "$OUT" "./prog --flag value"

# RUN wraps the launch (here echo, so the program itself never runs).
OUT=$(RUN=echo "$KILN" --triplet t go 2>&1)
check_contains "run: RUN wrapper prepended" "$OUT" "echo ./prog"

# A non-zero exit fails the build and names the target and command.
OUT=$("$KILN" --triplet t bad 2>&1 || true)
check_contains "test: failing exit fails build" "$OUT" "bad failed with exit code 2"
check_contains "test: failure reports command" "$OUT" "./badprog"

# =========================================================================
printf "=== Output directories ===\n"
# =========================================================================

TDIR="$TMPDIR/outdirs"
mkdir -p "$TDIR"
cd "$TDIR"

# A rule whose output lives in a not-yet-existing subdirectory builds.
cat > Kilnfile <<'EOF'
rule a/b/c.txt {
    command {sh -c "echo hi > $out"}
}
EOF
OUT=$("$KILN" --triplet t a/b/c.txt 2>&1)
check_contains "outdir: builds into new subdir" "$OUT" "built 1 target"
check_file_exists "outdir: nested output created" "$TDIR/a/b/c.txt"

# A rule whose depfile has its own subdirectory also works.
rm -rf obj dep
cat > foo.c <<'EOF'
int main(void){return 0;}
EOF
cat > Kilnfile <<'EOF'
rule obj/foo.o {
    inputs foo.c
    depfile dep/foo.d
    command {cc -c -o $out $in -MMD -MF $depfile}
}
EOF
OUT=$("$KILN" --triplet t obj/foo.o 2>&1)
check_contains "outdir: builds with depfile subdir" "$OUT" "built 1 target"
check_file_exists "outdir: output dir created" "$TDIR/obj/foo.o"
check_file_exists "outdir: depfile dir created" "$TDIR/dep/foo.d"

# cc_objects mirrors the source tree under the build directory instead of
# flattening object names, so same-named sources do not collide.
TMIR="$TMPDIR/mirror"
mkdir -p "$TMIR/src/math" "$TMIR/src/io"
cd "$TMIR"
cat > src/math/op.c <<'EOF'
int op(int a,int b){return a+b;}
EOF
cat > src/io/op.c <<'EOF'
void ioop(void){}
EOF
cat > main.c <<'EOF'
int op(int,int); void ioop(void);
int main(void){ioop();return op(2,3);}
EOF
cat > Kilnfile <<'EOF'
builddir build/$Triplet
cc_program app [list main.c src/math/op.c src/io/op.c] "-O2"
EOF
OUT=$("$KILN" --triplet t app 2>&1)
check_contains "mirror: program links" "$OUT" "built 4 targets"
check_file_exists "mirror: math object at mirrored path" "$TMIR/build/t/src/math/op.o"
check_file_exists "mirror: io object at mirrored path" "$TMIR/build/t/src/io/op.o"
check_file_exists "mirror: program built" "$TMIR/build/t/app"

# =========================================================================
printf "=== Clean action ===\n"
# =========================================================================

TCLN="$TMPDIR/clean"
mkdir -p "$TCLN/src/math"
cd "$TCLN"
echo 'int add(int a,int b){return a+b;}' > src/math/add.c
echo 'int add(int,int); int main(void){return add(1,2);}' > main.c
cat > Kilnfile <<'EOF'
builddir build/$Triplet
cc_program app [list main.c src/math/add.c] "-O2"
EOF

"$KILN" --triplet t app >/dev/null 2>&1
check_file_exists "clean: object built" "$TCLN/build/t/src/math/add.o"
check_file_exists "clean: depfile written" "$TCLN/build/t/src/math/add.d"

OUT=$("$KILN" --triplet t -a clean 2>&1)
check_contains "clean: reports depfile removal" "$OUT" "add.d"
check_file_missing "clean: output removed" "$TCLN/build/t/app"
check_file_missing "clean: object removed" "$TCLN/build/t/src/math/add.o"
check_file_missing "clean: depfile removed" "$TCLN/build/t/src/math/add.d"

# With outputs and depfiles gone, the nested build directory is removed too.
if [ -d "$TCLN/build" ]; then
    fail "clean: build directory removed"
    printf "    still present: %s\n" "$TCLN/build"
else
    pass "clean: build directory removed"
fi

OUT=$("$KILN" --triplet t -a clean 2>&1)
check_contains "clean: nothing left to clean" "$OUT" "nothing to clean"

# =========================================================================
printf "=== Build variants ===\n"
# =========================================================================

TVAR="$TMPDIR/variant"
mkdir -p "$TVAR"
cd "$TVAR"
echo 'int main(void){return 0;}' > m.c
cat > Kilnfile <<'EOF'
set CFLAGS "-Wall"
variant release {-O2 -DNDEBUG}
variant debug   {-O0 -g}
variant asan    {-O1 -fsanitize=address}
cc_program app [glob *.c] $CFLAGS
EOF

# The default variant is release: its flags and build directory are used.
OUT=$("$KILN" --triplet t -n 2>&1)
check_contains "variant: default uses release flags" "$OUT" "-O2 -DNDEBUG -c"
check_contains "variant: default build dir" "$OUT" "build/t/release/app"

# --variant debug selects the debug flag set and its own directory.
OUT=$("$KILN" --triplet t --variant debug -n 2>&1)
check_contains "variant: debug flags" "$OUT" "-O0 -g -c"
check_contains "variant: debug build dir" "$OUT" "build/t/debug/app"
check_not_contains "variant: debug drops release flags" "$OUT" "-DNDEBUG"

# A sanitizer variant passes its flag to the link step as well as compile.
OUT=$("$KILN" --triplet t --variant asan -n 2>&1)
check_contains "variant: asan links with sanitizer" "$OUT" \
    "-fsanitize=address -o build/t/asan/app"

# Variants keep separate trees, so two built variants coexist.
"$KILN" --triplet t >/dev/null 2>&1
"$KILN" --triplet t --variant debug >/dev/null 2>&1
check_file_exists "variant: release artifact" "$TVAR/build/t/release/app"
check_file_exists "variant: debug artifact" "$TVAR/build/t/debug/app"

# =========================================================================
printf "=== Code generation ===\n"
# =========================================================================

TGEN="$TMPDIR/codegen"
mkdir -p "$TGEN"
cd "$TGEN"
echo A > seed.txt
cat > Kilnfile <<'EOF'
generate build/$Triplet/gen.txt {seed.txt} {sh -c "cp $in $out"}
rule build/$Triplet/app {
    inputs build/$Triplet/gen.txt
    command {sh -c "cp $in $out"}
}
default build/$Triplet/app
EOF

OUT=$("$KILN" --triplet t 2>&1)
check_contains "codegen: generate builds output and consumer" "$OUT" "built 2 targets"
check_file_exists "codegen: generated file created" "$TGEN/build/t/gen.txt"

# An unchanged rebuild does nothing: the generator is not force-run, so
# nothing downstream rebuilds. This is the win over a make FORCE rule.
OUT=$("$KILN" --triplet t 2>&1)
check_contains "codegen: unchanged rebuild is a no-op" "$OUT" "nothing to do"

# Touching the input without changing its contents is also a no-op:
# content hashing sees the input is unmodified and skips regeneration.
sleep 1
touch seed.txt
OUT=$("$KILN" --triplet t 2>&1)
check_contains "codegen: touch without change is a no-op" "$OUT" "nothing to do"

# A real content change regenerates and cascades to the consumer.
echo B > seed.txt
OUT=$("$KILN" --triplet t 2>&1)
check_contains "codegen: content change regenerates and cascades" "$OUT" "built 2 targets"

# version_header needs git; skip gracefully where it is unavailable.
if command -v git >/dev/null 2>&1; then
    TVH="$TMPDIR/version"
    mkdir -p "$TVH"
    cd "$TVH"
    git init -q
    git -c user.email=t@example.com -c user.name=test commit -q --allow-empty -m init
    git tag v0.1.0
    cat > Kilnfile <<'EOF'
version_header build/$Triplet/version.h APP_VERSION
default build/$Triplet/version.h
EOF
    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "version: header generated" "$OUT" "built 1 target"
    check_contains "version: guard present" "$(cat build/t/version.h)" "#ifndef APP_VERSION_H"
    check_contains "version: macro and tag present" "$(cat build/t/version.h)" "v0.1.0"

    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "version: unchanged is a no-op" "$OUT" "nothing to do"

    # A new tag on a new commit changes the version, so the header
    # regenerates without any FORCE/cmp machinery.
    git -c user.email=t@example.com -c user.name=test commit -q --allow-empty -m next
    git tag v0.2.0
    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "version: regenerates on new tag" "$OUT" "built 1 target"
    check_contains "version: header has new tag" "$(cat build/t/version.h)" "v0.2.0"
else
    printf "  SKIP: version_header tests (git not found)\n"
fi

# =========================================================================
printf "=== Multiple triplets ===\n"
# =========================================================================

TMT="$TMPDIR/multi"
mkdir -p "$TMT"
cd "$TMT"
echo 'int main(void){return 0;}' > m.c
cat > Kilnfile <<'EOF'
builddir build/$Triplet
cc_program app [glob *.c] "-O2"
default app
EOF

# Two triplets in one invocation build into separate trees.
OUT=$("$KILN" --triplet aa --triplet bb 2>&1)
check_contains "multi: first triplet banner" "$OUT" "== aa =="
check_contains "multi: second triplet banner" "$OUT" "== bb =="
check_file_exists "multi: first triplet output" "$TMT/build/aa/app"
check_file_exists "multi: second triplet output" "$TMT/build/bb/app"

# $Triplet drives each build independently (toolchain and directory).
OUT=$("$KILN" --triplet x1 --triplet x2 -n 2>&1)
check_contains "multi: per-triplet path x1" "$OUT" "build/x1/app"
check_contains "multi: per-triplet path x2" "$OUT" "build/x2/app"

# A single triplet prints no banner (unchanged behavior).
OUT=$("$KILN" --triplet solo -n 2>&1)
check_not_contains "multi: no banner for one triplet" "$OUT" "== solo =="

# A failure in one triplet does not stop the others; the overall exit
# status is nonzero.
cat > Kilnfile <<'EOF'
phony check {
    command {sh -c "test $Triplet != fail"}
}
default check
EOF
OUT=$("$KILN" --triplet fail --triplet ok 2>&1) && rc=0 || rc=$?
check_contains "multi: failing triplet reported" "$OUT" "== fail =="
check_contains "multi: later triplet still runs" "$OUT" "== ok =="
check "multi: overall failure exit status" "$rc" "1"

# =========================================================================
printf "=== Version integration ===\n"
# =========================================================================

TVI="$TMPDIR/verint"
mkdir -p "$TVI"
cd "$TVI"
cat > Kilnfile <<'EOF'
define_header build/$Triplet/ver.h PROJECT_VERSION 9.9.9-custom
default build/$Triplet/ver.h
EOF
OUT=$("$KILN" --triplet t 2>&1)
check_contains "verint: define_header builds" "$OUT" "built 1 target"
check_contains "verint: guard from macro" "$(cat build/t/ver.h)" "#ifndef PROJECT_VERSION_H"
check_contains "verint: value defined" "$(cat build/t/ver.h)" "#define PROJECT_VERSION \"9.9.9-custom\""

if command -v git >/dev/null 2>&1; then
    TGV="$TMPDIR/gitver"
    mkdir -p "$TGV"
    cd "$TGV"
    git init -q
    git -c user.email=t@example.com -c user.name=test commit -q --allow-empty -m init
    git tag v1.4.0

    printf 'puts "V=[git_version 1.4.0]"\n' > Kilnfile
    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "gitver: clean release is base" "$OUT" "V=1.4.0"

    git -c user.email=t@example.com -c user.name=test commit -q --allow-empty -m next
    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "gitver: past tag refines base" "$OUT" "V=1.4.0-1-g"

    printf 'puts "V=[git_version 7.7.7]"\n' > Kilnfile
    OUT=$("$KILN" --triplet t 2>&1)
    check_contains "gitver: no matching tag falls back" "$OUT" "V=7.7.7-dev-g"
else
    printf "  SKIP: git_version tests (git not found)\n"
fi

# Outside any git repo, git_version returns the authoritative base.
TNG="$TMPDIR/nogit"
mkdir -p "$TNG"
cd "$TNG"
printf 'puts "V=[git_version 3.0.1]"\n' > Kilnfile
OUT=$("$KILN" --triplet t 2>&1)
check_contains "verint: base without git" "$OUT" "V=3.0.1"

# =========================================================================
printf "=== Explain ===\n"
# =========================================================================

TEX="$TMPDIR/explain"
mkdir -p "$TEX"
cd "$TEX"
printf 'int add(int,int);\n' > util.h
printf '#include "util.h"\nint add(int a,int b){return a+b;}\n' > util.c
printf '#include "util.h"\nint main(void){return add(1,2);}\n' > main.c
cat > Kilnfile <<'EOF'
builddir build/$Triplet
cc_program app [list main.c util.c] "-Wall"
default app
EOF

# Before any build there is no state.
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: no build state" "$OUT" "no build state"

# After a build everything is up to date.
"$KILN" --triplet t >/dev/null 2>&1
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: up to date" "$OUT" "up to date"

# A header change explains the objects and the cascade to the program.
sleep 1
echo "/* touch */" >> util.h
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: header reason" "$OUT" "header util.h changed"
check_contains "explain: cascade reason" "$OUT" "rebuilt by an out-of-date rule"
check_contains "explain: program listed via cascade" "$OUT" "build/t/app"

# A source change is reported as a changed input (not a duplicate header).
"$KILN" --triplet t >/dev/null 2>&1
sleep 1
printf '#include "util.h"\nint add(int a,int b){return a+b+1;}\n' > util.c
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: input change" "$OUT" "input util.c changed"
check_not_contains "explain: source not doubled as header" "$OUT" "header util.c changed"

# A missing output is reported.
"$KILN" --triplet t >/dev/null 2>&1
rm -f build/t/app
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: missing output" "$OUT" "output build/t/app is missing"

# Changing a compile flag is reported as a command change.
"$KILN" --triplet t >/dev/null 2>&1
cat > Kilnfile <<'EOF'
builddir build/$Triplet
cc_program app [list main.c util.c] "-Wall -O2"
default app
EOF
OUT=$("$KILN" --triplet t -a explain 2>&1)
check_contains "explain: command change" "$OUT" "command changed"

# =========================================================================
printf "\n=== Results ===\n"
printf "  %d passed, %d failed\n" "$PASS" "$FAIL"
# =========================================================================

[ "$FAIL" -eq 0 ]
