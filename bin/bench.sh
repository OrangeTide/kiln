#!/bin/sh
# bench.sh : generate synthetic Kilnfiles and time kiln's phases at scale.
#
# This measures kiln's own overhead (DAG construction, topological sort,
# staleness checking, CDB I/O), not the compiler. Rules use trivial cp/cat
# commands so the numbers reflect kiln and not cc.
#
# Usage:
#   sh bin/bench.sh [kiln-binary] [size...]
#
# Default binary is ./kiln; default sizes are 5000 10000 20000. Each size
# is run in both shapes:
#   flat  - N independent object rules (many parallel targets)
#   chain - N rules in a single dependency chain (depth stress)
set -e

KILN=${1:-./kiln}
[ "$#" -gt 0 ] && shift
SIZES=${*:-5000 10000 20000}

case "$KILN" in
    /*) ;;
    *) KILN="$PWD/$KILN" ;;
esac

if [ ! -x "$KILN" ]; then
    echo "bench: no kiln binary at $KILN" >&2
    echo "build one first (sh bin/bootstrap.sh) or pass a path" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# time_phase LABEL CMD...  -- print wall time of CMD, output discarded.
time_phase() {
    label=$1
    shift
    printf '  %-18s' "$label"
    if command -v /usr/bin/time >/dev/null 2>&1; then
        /usr/bin/time -f '%e s  %M KB' "$@" >/dev/null
    else
        start=$(date +%s)
        "$@" >/dev/null
        echo "$(( $(date +%s) - start )) s"
    fi
}

gen_flat() {
    n=$1
    dir=$2
    mkdir -p "$dir/src"
    : > "$dir/Kilnfile"
    i=0
    while [ "$i" -lt "$n" ]; do
        echo "src$i" > "$dir/src/f$i.in"
        printf 'rule out/f%d.o {\n    inputs src/f%d.in\n    command {cp $in $out}\n}\n' \
            "$i" "$i" >> "$dir/Kilnfile"
        i=$((i + 1))
    done
}

# The chain is generated with a Tcl loop so the Kilnfile stays small.
# expr is avoided: this Picol does not substitute variables inside expr
# braces, so the previous index is tracked in its own variable.
gen_chain() {
    n=$1
    dir=$2
    mkdir -p "$dir"
    echo seed > "$dir/f0.in"
    cat > "$dir/Kilnfile" <<EOF
rule f1.out {
    inputs f0.in
    command {cp \$in \$out}
}
set prev 1
for {set i 2} {\$i <= $n} {incr i} {
    rule f\$i.out "inputs f\$prev.out\ncommand {cp \\\$in \\\$out}"
    set prev \$i
}
EOF
}

for n in $SIZES; do
    echo "=== N=$n ==="

    flat="$WORK/flat$n"
    gen_flat "$n" "$flat"
    ( cd "$flat"
      echo "flat ($n independent rules):"
      time_phase "parse+plan (-n)" "$KILN" --triplet t -n
      time_phase "first build" "$KILN" --triplet t
      time_phase "incremental no-op" "$KILN" --triplet t )

    chain="$WORK/chain$n"
    gen_chain "$n" "$chain"
    ( cd "$chain"
      echo "chain ($n deep):"
      time_phase "parse+plan (-n)" "$KILN" --triplet t -n "f$n.out" )
done
