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
 * The prototypes are the historical termcap ones, matching what ncurses
 * exports. `char *` rather than `const char *` throughout: that is what the
 * library declares, and tightening it here would break zsh's call sites.
 */

#ifndef AOK_ZSH_TERMCAP_H
#define AOK_ZSH_TERMCAP_H

#ifdef __cplusplus
extern "C" {
#endif

int   tgetent(char *bp, const char *name);
int   tgetflag(const char *id);
int   tgetnum(const char *id);
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);
int   tputs(const char *str, int affcnt, int (*putc_fn)(int));

/* Set by tgetent and read by tputs for padding. zsh assigns PC and ospeed
 * itself (Src/init.c), so they must be declared, not merely available. */
extern char PC;
extern char *BC;
extern char *UP;
extern short ospeed;

#ifdef __cplusplus
}
#endif

#endif /* AOK_ZSH_TERMCAP_H */
