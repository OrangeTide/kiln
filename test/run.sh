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

OUT=$("$KILN" --triplet test-triple /dev/null 2>&1 || true)
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

OUT=$("$KILN" --triplet t "$T/test.tcl" 2>&1)
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

OUT=$("$KILN" --triplet t "$T/test.tcl" 2>&1 || true)
check_contains "error file:line" "$OUT" "test.tcl:3:"
check_contains "error command name" "$OUT" "badcmd"

cat > "$T/test2.tcl" <<'TCLEOF'
set x 1

# comment
set y 2

# another comment

puts $noexist
TCLEOF

OUT=$("$KILN" --triplet t "$T/test2.tcl" 2>&1 || true)
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

OUT=$("$KILN" --triplet t "$T/main.tcl" 2>&1)
check_contains "source PATH search" "$OUT" "hello"

OUT=$("$KILN" --triplet t -n "$T/main.tcl" 2>&1)
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

OUT=$("$KILN" --triplet t "$T/err.tcl" 2>&1 || true)
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

OUT=$("$KILN" --triplet t "$T/cycle.tcl" 2>&1 || true)
check_contains "cycle detected" "$OUT" "dependency cycle"

# Multiple outputs
cat > "$T/multi.tcl" <<'TCLEOF'
rule {parse.c parse.h} {
    inputs parse.y
    command {bison -d -o $out $in}
}
TCLEOF

OUT=$("$KILN" --triplet t -n "$T/multi.tcl" 2>&1)
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

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
check_contains "clean build count" "$OUT" "built 3 targets"
check_file_exists "clean build: main.o" "$T/main.o"
check_file_exists "clean build: util.o" "$T/util.o"
check_file_exists "clean build: hello" "$T/hello"

RESULT=$("$T/hello")
check "clean build output" "$RESULT" "5"

# =========================================================================
printf "=== Build: incremental no-op ===\n"
# =========================================================================

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
check_contains "no-op rebuild" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: touch without content change ===\n"
# =========================================================================

sleep 1  # ensure mtime differs
touch "$T/util.c"

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
check_contains "touch no-op" "$OUT" "nothing to do"

# =========================================================================
printf "=== Build: content change cascades ===\n"
# =========================================================================

cat > "$T/util.c" <<'EOF'
#include "util.h"
int add(int a, int b) { return a + b + 10; }
EOF

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet test Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet test -j 4 Kilnfile 2>&1)
check_contains "parallel build count" "$OUT" "built 2 targets"
check_file_exists "parallel build: a" "$TP/a"
check_file_exists "parallel build: b" "$TP/b"

OUT=$("$KILN" --triplet test -j 4 Kilnfile 2>&1)
check_contains "parallel no-op rebuild" "$OUT" "nothing to do"

OUT=$("$KILN" --triplet test -j 1 Kilnfile 2>&1)
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
OUT=$("$KILN" --triplet test Kilnfile 2>&1 || true)
check_contains "failure message" "$OUT" "command failed"
check_file_missing "output deleted on fail" "$T2/bad.o"

# =========================================================================
printf "=== Build: dry-run ===\n"
# =========================================================================

cd "$T"
rm -f "$T/main.o" "$T/util.o" "$T/hello" "$T/main.d" "$T/util.d"
rm -rf "$T/build"

OUT=$("$KILN" --triplet test -n Kilnfile 2>&1)
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
OUT=$("$KILN" --triplet t -n Kilnfile 2>&1)
check_contains "quoted command expands \$in" "$OUT" "cp src.txt q.txt"
check_contains "quoted command keeps quotes" "$OUT" '"cp src.txt q.txt"'

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
check_contains "uses: initial build" "$OUT" "built 2 targets"
check_file_exists "uses: out.txt" "$TU/out.txt"
check "uses: tool kept out of \$in" "$(cat probe.txt)" "in.txt"

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
check_contains "uses: no-op when tool unchanged" "$OUT" "nothing to do"

# Changing the tool forces a rebuild even though inputs are unchanged.
sleep 1
printf 'cp "$1" "$2"; echo TOOL2 >> "$2"\n' > gen.sh
OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
check_contains "uses in-tree: tool and user built" "$OUT" "built 2 targets"
check_file_exists "uses in-tree: out.txt" "$TV/out.txt"

sleep 1
printf 'cp "$1" "$2"; echo TOOL2 >> "$2"\n' > tool.src
OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
check_contains "stdlib cp_rule builds" "$OUT" "built 1 target"
check "stdlib cp_rule output" "$(cat copy.txt)" "payload"

# --no-stdlib must leave the helpers undefined.
OUT=$("$KILN" --no-stdlib --triplet t Kilnfile 2>&1 || true)
check_contains "no-stdlib disables helpers" "$OUT" "No such command"

cat > o.c <<'EOF'
int answer(void) { return 42; }
EOF
cat > Kilnfile <<'EOF'
cc_object o.o o.c o.d "-O2"
EOF

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t -a status Kilnfile 2>&1)
check_contains "status: no build state" "$OUT" "no build state"

"$KILN" --triplet t Kilnfile >/dev/null 2>&1

OUT=$("$KILN" --triplet t -a status Kilnfile 2>&1)
check_contains "status: all up to date" "$OUT" "2 up to date"

# A changed input is reported, and status must not build it.
sleep 1
echo changed > a.c
OUT=$("$KILN" --triplet t -a status Kilnfile 2>&1)
check_contains "status: out of date count" "$OUT" "1 out of date"
check_contains "status: names target" "$OUT" "a.o"
check "status: did not build" "$(cat a.o)" "a"

# A missing output is flagged.
echo a > a.c
"$KILN" --triplet t Kilnfile >/dev/null 2>&1
rm -f b.o
OUT=$("$KILN" --triplet t -a status Kilnfile 2>&1)
check_contains "status: missing output" "$OUT" "missing output"

# A removed rule leaves a stale CDB entry.
"$KILN" --triplet t Kilnfile >/dev/null 2>&1
cat > Kilnfile <<'EOF'
rule a.o {
    inputs a.c
    command {cp $in $out}
}
EOF
OUT=$("$KILN" --triplet t -a status Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t "$TX/t.tcl" 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t "$TE/t.tcl" 2>&1)
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

OUT=$("$KILN" --triplet t Kilnfile 2>&1)
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

OUT=$("$KILN" --triplet t g.tcl 2>&1)
check "getenv default" "$(echo "$OUT" | sed -n 1p)" "fallback"
check "getenv unset is empty" "$(echo "$OUT" | sed -n 2p)" "0"

# A Kilnfile can override the compiler the cc_* helpers use.
cat > Kilnfile <<'EOF'
set CC MYCOMPILER
cc_program app m.c "-O2"
EOF
OUT=$("$KILN" --triplet t -n Kilnfile 2>&1)
check_contains "CC override in Kilnfile" "$OUT" "MYCOMPILER -O2 -c"

# The environment overrides CC when the Kilnfile does not set it.
cat > Kilnfile2 <<'EOF'
cc_program app m.c "-O2"
EOF
OUT=$(CC=ENVCC "$KILN" --triplet t -n Kilnfile2 2>&1)
check_contains "CC override from env" "$OUT" "ENVCC -o app"

# =========================================================================
printf "\n=== Results ===\n"
printf "  %d passed, %d failed\n" "$PASS" "$FAIL"
# =========================================================================

[ "$FAIL" -eq 0 ]
