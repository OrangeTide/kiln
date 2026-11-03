#!/bin/sh
# embed.sh : generate src/kiln_stdlib.h from lib/kiln.tcl
#
# Run after editing lib/kiln.tcl, then commit both files together.
set -e

cd "$(dirname "$0")/.."

SRC=lib/kiln.tcl
OUT=src/kiln_stdlib.h

{
    printf '/* kiln_stdlib.h : embedded standard library */\n'
    printf '/* Generated from lib/kiln.tcl by bin/embed.sh. Do not edit. */\n'
    printf '#ifndef KILN_STDLIB_H\n'
    printf '#define KILN_STDLIB_H\n\n'
    printf 'static char kiln_stdlib_tcl[] =\n'
    # Escape backslashes, then double quotes, then wrap each line as a C
    # string literal with a trailing newline.
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/\t"/' -e 's/$/\\n"/' "$SRC"
    printf '\t;\n\n'
    printf '#endif /* KILN_STDLIB_H */\n'
} > "$OUT"

echo "Generated $OUT from $SRC"
