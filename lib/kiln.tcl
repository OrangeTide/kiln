# kiln.tcl : kiln standard library
#
# Loaded automatically before Kilnfiles unless --no-stdlib is given.
# These helpers use only the commands kiln exposes today; the set will
# grow as the Tcl surface does.
#
# In a helper body, $out, $in, and $depfile are escaped as \$out so they
# reach the rule unexpanded; kiln substitutes them when the rule runs.
# Plain $vars (a helper's own arguments) are substituted immediately.

# Toolchain defaults. Each may be overridden from the environment, or by
# setting the global in a Kilnfile before declaring rules. CC is the C
# compiler and link driver used by the cc_* helpers; set it per target
# for cross-compilation (e.g. set CC arm-linux-gnueabihf-gcc).
set CC [getenv CC cc]

# Flag globals shared by the cc_* helpers and the variant selector. CFLAGS
# is passed to compilation; LDFLAGS is passed to linking by cc_program. A
# variant appends to them. They start empty so a build depends only on the
# Kilnfile, not the ambient environment; set common flags before declaring
# variants, and use [getenv CFLAGS ""] explicitly if environment flags are
# wanted.
set CFLAGS ""
set LDFLAGS ""

# variant name flags
#   Register a build variant. When name is the active $Variant (chosen
#   with --variant, default release), append flags to both CFLAGS and
#   LDFLAGS and place the build under build/$Triplet/name, so each variant
#   keeps its own outputs and staleness state. Flags reach both the
#   compile and the link step, so a sanitizer variant works with a single
#   entry. The same rules serve every variant; pick one per run. Example:
#       set CFLAGS "-Wall"
#       variant release {-O2 -DNDEBUG}
#       variant debug   {-O0 -g}
#       variant asan    {-O1 -g -fsanitize=address}
proc variant {name flags} {
    if {[string equal $name $Variant]} {
        if {[string length $CFLAGS]} { append CFLAGS " " }
        append CFLAGS $flags
        if {[string length $LDFLAGS]} { append LDFLAGS " " }
        append LDFLAGS $flags
        builddir build/$Triplet/$name
    }
}

# cp_rule dst src
#   Copy src to dst as a build step.
proc cp_rule {dst src} {
    rule $dst "inputs $src ; command {cp \$in \$out}"
}

# cc_object obj src dep flags
#   Compile one C source to an object file, writing a gcc/clang depfile
#   to dep so header changes are tracked.
proc cc_object {obj src dep flags} {
    rule $obj "inputs $src ; depfile $dep ; command {$CC $flags -c -o \$out \$in -MMD -MF \$depfile}"
}

# cc_objects sources flags
#   Define a compile rule for each C source in the list and return the
#   list of object paths. Each object and depfile keeps the source's own
#   relative path with the extension changed to .o and .d, so a source
#   tree is mirrored rather than flattened (src/a/foo.c -> src/a/foo.o);
#   this keeps two sources with the same base name from colliding. Combine
#   with the builddir command to place the tree under a build directory;
#   kiln creates each output's directory as needed. One rule per source in
#   a single call:
#       set objs [cc_objects [glob *.c] "-Wall -O2"]
proc cc_objects {sources flags} {
    set objs {}
    foreach src $sources {
        set base [file rootname $src]
        cc_object $base.o $src $base.d $flags
        lappend objs $base.o
    }
    return $objs
}

# cc_program out sources flags
#   Compile each source and link the resulting objects into out.
proc cc_program {out sources flags} {
    set objs [cc_objects $sources $flags]
    set link $CC
    if {[string length $LDFLAGS]} {
        set link "$CC $LDFLAGS"
    }
    rule $out "inputs $objs ; command {$link -o \$out \$in}"
}

# RUN wraps a program launch, for running a cross-compiled binary under an
# emulator such as qemu. Empty runs the program natively. Override it from
# the environment (RUN=qemu-arm) or per target in a Kilnfile.
set RUN [getenv RUN ""]

# run name program opts
#   Define a phony target that launches program, a build output, through
#   $RUN with opts appended. The program is an input, so it is built first;
#   a non-zero exit fails the build and prints the command and its output.
#   A relative program with no directory part is run as ./program so the
#   shell does not search $PATH. Pass {} for opts when there are none.
proc run {name program opts} {
    # Resolve the program to a runnable path. A name with a directory part
    # is used as is. A bare name is taken from the build directory when one
    # is set (builddir prefixes rule outputs the same way), and otherwise
    # run as ./name so the shell does not search $PATH.
    set bd ""
    catch {set bd $BuildDir}
    if {[string match */* $program]} {
        set exe $program
    } elseif {[string length $bd]} {
        set exe $bd/$program
    } else {
        set exe ./$program
    }
    if {[string length $RUN]} {
        set exe "$RUN $exe"
    }
    if {[string length $opts]} {
        set exe "$exe $opts"
    }
    phony $name "inputs $program ; command {$exe}"
}

# test name program opts
#   Run a test program the same way as run. A test is a program whose exit
#   status is the verdict: a non-zero exit fails the build. Reads as intent
#   at the call site (test parser-check ./test_parser {}).
proc test {name program opts} {
    run $name $program $opts
}

# generate out inputs command
#   Define a rule that produces out by running command, with inputs as its
#   dependencies ($in and $out expand as in any rule). A thin wrapper over
#   rule that names the intent for generated sources such as a parser
#   table. The generator reruns only when its inputs or command change
#   (content and command hashing), so an unchanged build does not
#   regenerate the file and an input touched without a content change is
#   skipped. There is no need to force-run it and guard its mtime. Example:
#       generate parser.c {parser.y} {bison -o $out $in}
proc generate {out inputs command} {
    rule $out "inputs $inputs ; command {$command}"
}

# define_header out macro value
#   Generate a guarded C header at out that defines macro to value. The
#   value is embedded in the generator command, so command hashing rewrites
#   the header only when the value changes and nothing that includes it
#   recompiles while the value holds. This is the make FORCE-plus-cmp dance
#   made automatic: no forced rule, no mtime guard. Pair with git_version
#   to bake a version into a build.
proc define_header {out macro value} {
    set guard $macro
    append guard _H
    set fmt "#ifndef $guard\\n#define $guard\\n#define $macro \"%s\"\\n#endif\\n"
    rule $out "command {printf '$fmt' '$value' > \$out}"
}

# git_version base
#   Refine an authoritative base version (such as the contents of a tracked
#   VERSION file) with git. Returns base unchanged for a clean release
#   tagged v<base>; base plus git describe's suffix (commits since the tag,
#   short sha, -dirty) when the tree has moved past the tag; base-dev-g<sha>
#   when no matching tag exists; and base alone when git is unavailable.
#   Matching v<base> specifically keeps a vendored copy from describing
#   itself against a host project's tags, and the tracked base keeps a
#   tarball or tagless checkout producing a sane version. Example:
#       define_header build/$Triplet/version.h APP_VERSION \
#           [git_version [exec cat VERSION]]
proc git_version {base} {
    set tag v
    append tag $base
    set desc ""
    catch {set desc [exec git describe --tags --dirty --match $tag 2>/dev/null]}
    if {[string length $desc]} {
        return $base[string range $desc [string length $tag] end]
    }
    set sha ""
    catch {set sha [exec git describe --always --dirty 2>/dev/null]}
    if {[string length $sha]} {
        return $base-dev-g$sha
    }
    return $base
}

# version_header out macro
#   Generate a version header the simple way: define macro to the output of
#   git describe, or "unknown" when git is unavailable. For a version that
#   also works from a tarball or a vendored copy, prefer an authoritative
#   base refined by git_version:
#       define_header build/$Triplet/version.h APP_VERSION \
#           [git_version [exec cat VERSION]]
proc version_header {out macro} {
    set v unknown
    catch {set v [exec git describe --tags --always --dirty 2>/dev/null]}
    define_header $out $macro $v
}
