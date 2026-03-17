#!/bin/sh
# gen-screenshot.sh : generate doc/screenshot.svg from actual kiln output
# Run from the project root: sh doc/gen-screenshot.sh
set -e

KILN=./kiln
DIR=$(mktemp -d)
OUT=doc/screenshot.svg

trap 'rm -rf "$DIR"' EXIT

# Build kiln if needed
if [ ! -x "$KILN" ]; then
    cc -O2 -o "$KILN" bootstrap.c
fi

# Create example project
cat > "$DIR/greet.c" << 'EOF'
#include <stdio.h>
void greet(const char *name) { printf("Hello, %s!\n", name); }
EOF

cat > "$DIR/greet.h" << 'EOF'
#ifndef GREET_H
#define GREET_H
void greet(const char *name);
#endif
EOF

cat > "$DIR/main.c" << 'EOF'
#include "greet.h"
int main(void) { greet("world"); return 0; }
EOF

cat > "$DIR/Kilnfile" << 'KILNEOF'
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
KILNEOF

# Capture output with forced color (pipe won't be a TTY)
export CLICOLOR_FORCE=1

# Full build
build_out=$(cd "$DIR" && "$OLDPWD/$KILN" Kilnfile 2>&1)

# Incremental no-op
noop_out=$(cd "$DIR" && "$OLDPWD/$KILN" Kilnfile 2>&1)

# Clean
clean_out=$(cd "$DIR" && "$OLDPWD/$KILN" -a clean Kilnfile 2>&1)

# Convert ANSI to SVG spans
# Handles: ESC[0m (reset), ESC[1m (bold), ESC[31m (red), ESC[32m (green),
# ESC[K (clear line), ESC[1;32m / ESC[1m ESC[32m combos
ansi_to_svg() {
    printf '%s\n' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g' | awk '
    BEGIN {
        cls = "t"
    }
    {
        line = $0
        result = ""
        open_span = 0
        while (match(line, /\033\[[0-9;]*[mK]/)) {
            pre = substr(line, 1, RSTART - 1)
            code = substr(line, RSTART, RLENGTH)
            line = substr(line, RSTART + RLENGTH)

            if (pre != "") {
                result = result "<tspan class=\"" cls "\">" pre "</tspan>"
            }

            # Parse the escape
            if (code ~ /\[0m/) {
                cls = "t"
            } else if (code ~ /\[1;32m/ || code ~ /\[32;1m/) {
                cls = "green-bold"
            } else if (code ~ /\[1m/) {
                cls = "bold"
            } else if (code ~ /\[31m/) {
                cls = "red"
            } else if (code ~ /\[32m/) {
                if (cls == "bold") cls = "green-bold"
                else cls = "green"
            } else if (code ~ /\[33m/) {
                cls = "yellow"
            }
            # ESC[K (clear to EOL) - ignore
        }
        # Remaining text
        if (line != "") {
            result = result "<tspan class=\"" cls "\">" line "</tspan>"
        }
        if (result != "") print result
    }'
}

build_spans=$(ansi_to_svg "$build_out")
noop_spans=$(ansi_to_svg "$noop_out")
clean_spans=$(ansi_to_svg "$clean_out")

# Count lines for height calculation
build_lines=$(printf '%s\n' "$build_spans" | wc -l)
noop_lines=$(printf '%s\n' "$noop_spans" | wc -l)
clean_lines=$(printf '%s\n' "$clean_spans" | wc -l)
# 3 prompts + 2 blank separators + all output lines
total_lines=$((build_lines + noop_lines + clean_lines + 3 + 2))
height=$((40 + total_lines * 22 + 10))

# Generate SVG
{
cat << SVGHEAD
<svg xmlns="http://www.w3.org/2000/svg" width="760" height="$height">
  <style>
    .bg { fill: #1e1e2e; }
    .titlebar { fill: #313244; }
    .title { fill: #cdd6f4; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .t { fill: #cdd6f4; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .bold { fill: #cdd6f4; font-weight: bold; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .green { fill: #a6e3a1; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .green-bold { fill: #a6e3a1; font-weight: bold; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .red { fill: #f38ba8; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .yellow { fill: #f9e2af; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
    .prompt { fill: #89b4fa; font-family: ui-monospace, 'SF Mono', 'Cascadia Code', Menlo, Consolas, monospace; font-size: 13px; }
  </style>
  <rect class="bg" width="760" height="$height" rx="8"/>
  <rect class="titlebar" width="760" height="32" rx="8"/>
  <rect class="bg" x="0" y="24" width="760" height="8"/>
  <circle cx="20" cy="16" r="6" fill="#f38ba8"/>
  <circle cx="40" cy="16" r="6" fill="#f9e2af"/>
  <circle cx="60" cy="16" r="6" fill="#a6e3a1"/>
  <text class="title" x="380" y="20" text-anchor="middle">Terminal</text>
SVGHEAD

y=60

# Helper: emit a prompt line
prompt_line() {
    printf '  <text y="%d" x="16"><tspan class="prompt">$</tspan><tspan class="t"> %s</tspan></text>\n' "$y" "$1"
    y=$((y + 22))
}

# Helper: emit captured output lines
emit_lines() {
    printf '%s\n' "$1" | while IFS= read -r span; do
        [ -z "$span" ] && continue
        printf '  <text y="%d" x="16">%s</text>\n' "$y" "$span"
        y=$((y + 22))
        # Propagate y through subshell via temp file
        printf '%d' "$y" > "$DIR/.y"
    done
    # Read back y from subshell
    if [ -f "$DIR/.y" ]; then
        y=$(cat "$DIR/.y")
    fi
}

# Build
prompt_line "kiln Kilnfile"
emit_lines "$build_spans"

# Blank separator
y=$((y + 10))

# No-op
prompt_line "kiln Kilnfile"
emit_lines "$noop_spans"

# Blank separator
y=$((y + 10))

# Clean
prompt_line "kiln -a clean Kilnfile"
emit_lines "$clean_spans"

echo '</svg>'

} > "$OUT"

echo "Generated $OUT ($total_lines lines, ${height}px tall)"
