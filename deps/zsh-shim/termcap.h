/* termcap.h for a native-zsh build.
 *
 * WHY THIS FILE EXISTS. zsh needs tgetent/tgetstr/tgetnum/tgetflag/tgoto/tputs
 * and, unlike bash, ships no termcap of its own. macOS has <termcap.h>; the iOS
 * SDK does not -- but it DOES ship the library:
 *
 *     iPhoneOS.sdk/usr/lib/libtermcap.tbd     present
 *     iPhoneOS.sdk/usr/lib/libncurses.tbd     present, exports _tgetent
 *     iPhoneOS.sdk/usr/include/termcap.h      ABSENT
 *
 * So the symbols link and only the declarations are missing, which is a header
 * problem rather than a capability one. The iOS build failed on
 * `zsh_system.h:909: fatal error: 'termcap.h' file not found` -- reported as
 * Xcode's unhelpful "never received target ended message for target ID".
 *
 * deps/zsh-shim is on zsh's include path ahead of the system directories, so
 * this is what `#include <termcap.h>` finds on BOTH platforms. Deliberately
 * both: one declaration set means macOS cannot quietly compile against a
 * different prototype than iOS does, which is the kind of divergence that
 * shows up only on device.
 *
 * The prototypes are the historical termcap ones. `char *` rather than
 * `const char *` throughout, and that is not a style choice: zsh's own
 * Src/prototypes.h declares this same set, unqualified, for every translation
 * unit that does no terminal handling. A declaration we cannot edit is the one
 * everything else has to agree with -- kernel/native_libc.h matches it too,
 * which is what these names resolve to now (kernel/native_termcap.c).
 */

#ifndef AOK_ZSH_TERMCAP_H
#define AOK_ZSH_TERMCAP_H

#ifdef __cplusplus
extern "C" {
#endif

int   tgetent(char *bp, char *name);
int   tgetflag(char *id);
int   tgetnum(char *id);
char *tgetstr(char *id, char **area);
char *tgoto(char *cap, int col, int row);
int   tputs(char *str, int affcnt, int (*putc_fn)(int));

/* The padding globals a termcap library traditionally exports. zsh only ever
 * DECLARES ospeed (Src/zsh_system.h) and never reads or writes any of the
 * four, so these are declarations with nothing behind them -- and nothing
 * references them, so nothing needs there to be. kernel/native_termcap.c's
 * tputs drops padding outright; see the note there for why that is right for a
 * pty rather than a serial line. */
extern char PC;
extern char *BC;
extern char *UP;
extern short ospeed;

/* The capability-code tables, in terminfo index order and NULL-terminated,
 * exactly as ncurses exports them from <term.h>. meson defines HAVE_BOOLCODES,
 * HAVE_NUMCODES and HAVE_STRCODES for the zsh build so that Src/Modules/
 * termcap.c walks THESE to enumerate $termcap. Its own fallback lists are
 * ncurses-5-era and shorter, and using them made ${(k)termcap} 13 entries
 * shorter than the guest's own zsh reports -- for capabilities the reader
 * resolves perfectly well when asked by name. Defined in
 * kernel/native_termcap.c, which is also what indexes a compiled entry with
 * them, so the two can never disagree. */
extern const char *const boolcodes[];
extern const char *const numcodes[];
extern const char *const strcodes[];

#ifdef __cplusplus
}
#endif

#endif /* AOK_ZSH_TERMCAP_H */
