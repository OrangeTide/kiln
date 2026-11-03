# kiln.tcl : kiln standard library
#
# Loaded automatically before Kilnfiles unless --no-stdlib is given.
# These helpers use only the commands kiln exposes today; the set will
# grow as the Tcl surface does.
#
# In a helper body, $out, $in, and $depfile are escaped as \$out so they
# reach the rule unexpanded; kiln substitutes them when the rule runs.
# Plain $vars (a helper's own arguments) are substituted immediately.

# cp_rule dst src
#   Copy src to dst as a build step.
proc cp_rule {dst src} {
    rule $dst "inputs $src ; command {cp \$in \$out}"
}

# cc_object obj src dep flags
#   Compile one C source to an object file, writing a gcc/clang depfile
#   to dep so header changes are tracked.
proc cc_object {obj src dep flags} {
    rule $obj "inputs $src ; depfile $dep ; command {cc $flags -c -o \$out \$in -MMD -MF \$depfile}"
}
