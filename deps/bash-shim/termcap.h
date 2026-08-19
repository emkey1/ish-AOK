/* termcap.h for a native-bash build.
 *
 * WHY THIS FILE EXISTS. readline includes <termcap.h> (lib/readline/tcap.h,
 * because bash's generated config.h defines HAVE_TERMCAP_H) and that used to
 * find bash's OWN bundled GNU termcap header, next to the bundled
 * implementation in lib/termcap. That implementation reads /etc/termcap and
 * nothing else. Debian, Devuan and Alpine all ship terminfo and no
 * /etc/termcap, so every capability readline asked for came back empty -- and
 * a line editor with no `le`, `nd` or `ce` erases with a bare space, which
 * ADVANCES the cursor. Same bug zsh had, one program over; see
 * kernel/native_termcap.c for the long version.
 *
 * The bundled implementation is gone from the build now (meson.build) and
 * these six names route to kernel/native_termcap.c, which answers them from
 * the GUEST's terminfo database through AOK's own I/O. deps/bash-shim is the
 * FIRST entry on bash's include path, so this is what <termcap.h> resolves to
 * on macOS and iOS alike -- deliberately both, so the Mac build cannot quietly
 * compile against the host SDK's declarations while the device gets ours.
 *
 * The prototypes are the historical termcap ones, `char *` throughout rather
 * than `const char *`. Not a style choice: kernel/native_libc.h declares the
 * nlibc_* functions that way because zsh's Src/prototypes.h does, and the
 * shim rewrites the names in this file to those -- so a `const` here would be
 * a conflicting redeclaration rather than a stricter one. deps/zsh-shim/
 * termcap.h says the same thing for the same reason.
 */

#ifndef AOK_BASH_TERMCAP_H
#define AOK_BASH_TERMCAP_H

#ifdef __cplusplus
extern "C" {
#endif

int   tgetent(char *bp, char *name);
int   tgetflag(char *id);
int   tgetnum(char *id);
char *tgetstr(char *id, char **area);
char *tgoto(char *cap, int col, int row);
int   tputs(char *str, int affcnt, int (*putc_fn)(int));

/* The padding globals a termcap library traditionally exports, and __thread
 * because everything of bash's is: two native bash instances are two threads
 * of one process, so a shared PC would be one shell's terminal describing
 * another's (docs/bash_native_plan.md, tools/check-bash-tls.py).
 *
 * They stay bash's, not kernel/native_termcap.c's. PC is DEFINED by
 * lib/readline/terminal.c, which is also the only code that writes it; the
 * shim's tputs drops padding outright (see there for why that is right for a
 * pty rather than a serial line), so nothing reads it. Putting the definition
 * in native_termcap.c instead would force one thread-locality on both native
 * programs, and zsh's view of these -- deps/zsh-shim/termcap.h, declared and
 * never referenced -- is not bash's. Each program keeping its own is what
 * makes each self-consistent.
 *
 * ospeed is declared and deliberately not defined anywhere: nothing in bash
 * references it once the bundled termcap is out of the build (jobs.c's
 * `ospeed` is a parameter of draino, not this). If something starts to, the
 * failure is a link error naming it, which is the loud kind. */
extern __thread char PC;
extern __thread short ospeed;
extern __thread char *UP;
extern __thread char *BC;

#ifdef __cplusplus
}
#endif

#endif /* AOK_BASH_TERMCAP_H */
