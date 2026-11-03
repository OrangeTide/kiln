/* kiln_stdlib.h : embedded standard library */
/* Generated from lib/kiln.tcl by bin/embed.sh. Do not edit. */
#ifndef KILN_STDLIB_H
#define KILN_STDLIB_H

static char kiln_stdlib_tcl[] =
	"# kiln.tcl : kiln standard library\n"
	"#\n"
	"# Loaded automatically before Kilnfiles unless --no-stdlib is given.\n"
	"# These helpers use only the commands kiln exposes today; the set will\n"
	"# grow as the Tcl surface does.\n"
	"#\n"
	"# In a helper body, $out, $in, and $depfile are escaped as \\$out so they\n"
	"# reach the rule unexpanded; kiln substitutes them when the rule runs.\n"
	"# Plain $vars (a helper's own arguments) are substituted immediately.\n"
	"\n"
	"# cp_rule dst src\n"
	"#   Copy src to dst as a build step.\n"
	"proc cp_rule {dst src} {\n"
	"    rule $dst \"inputs $src ; command {cp \\$in \\$out}\"\n"
	"}\n"
	"\n"
	"# cc_object obj src dep flags\n"
	"#   Compile one C source to an object file, writing a gcc/clang depfile\n"
	"#   to dep so header changes are tracked.\n"
	"proc cc_object {obj src dep flags} {\n"
	"    rule $obj \"inputs $src ; depfile $dep ; command {cc $flags -c -o \\$out \\$in -MMD -MF \\$depfile}\"\n"
	"}\n"
	;

#endif /* KILN_STDLIB_H */
