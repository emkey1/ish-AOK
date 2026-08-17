/* What zsh needs undone after kernel/native_libc.h, and nothing else.
 *
 * Force-included by meson AFTER native_libc.h, so it can only take things
 * back. Every entry here is a name the shim rewrites that zsh uses as an
 * IDENTIFIER OF ITS OWN, where the rewrite is not merely unnecessary but
 * makes the file fail to compile.
 *
 * The bar for adding a line: zsh must never call the libc function behind the
 * name. If it did, undefining the rewrite would send that call to the HOST --
 * the exact silent wrongness tools/check-native-libc.py exists to catch. That
 * gate still runs over libzsh.a, so a name undefined here and then genuinely
 * called shows up as an unrouted symbol rather than passing quietly.
 */
#ifndef AOK_ZSH_SHIM_H
#define AOK_ZSH_SHIM_H

/* getopt's four globals. native_libc.h turns each into an accessor call --
 * `#define optarg (*nlibc_optargp())` -- so that concurrent native programs
 * do not share one parse state.
 *
 * zsh does not use getopt AT ALL. It has its own: zoptind and zoptarg
 * (Src/params.c, the $OPTIND and $OPTARG the shell exposes), parsed by
 * bin_getopts in Src/builtin.c. What it does have is LOCAL variables named
 * optarg -- Src/builtin.c's `char *eptr, *optarg = OPT_ARG(ops,c);` and
 * Src/Modules/system.c's option loop -- and a local cannot be declared as a
 * function call.
 *
 * Verified rather than assumed: no reference to getopt, getopt_long, optind,
 * opterr or optopt exists anywhere in zsh's sources.
 */
#undef optarg
#undef optind
#undef opterr
#undef optopt

#endif
