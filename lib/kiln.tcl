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

# cc_objects sources flags
#   Define a compile rule for each C source in the list and return the
#   list of object paths. The object and depfile names are the source
#   base name with .o and .d; combine with the builddir command to place
#   them under a build directory. This is the one-rule-per-source pattern
#   as a single call:
#       set objs [cc_objects [glob *.c] "-Wall -O2"]
proc cc_objects {sources flags} {
    set objs {}
    foreach src $sources {
        set base [file rootname [file tail $src]]
        cc_object $base.o $src $base.d $flags
        lappend objs $base.o
    }
    return $objs
}

# cc_program out sources flags
#   Compile each source and link the resulting objects into out.
proc cc_program {out sources flags} {
    set objs [cc_objects $sources $flags]
    rule $out "inputs $objs ; command {cc -o \$out \$in}"
}
