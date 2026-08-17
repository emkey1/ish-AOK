#!/bin/sh
# Configure zsh the way iSH-AOK's native build needs it. Run from the zsh tree.
#
# TERMCAP ONLY. The iOS SDK ships libcurses/libncurses .tbd stubs but NO
# curses.h and NO term.h, so anything that needs terminfo cannot even compile
# for device. Forcing the header checks off is not enough on macOS: zsh's
# "Solaris 8 curses.h mistake" fallback compiles `#include <curses.h>` and
# defines HAVE_CURSES_H anyway, and setupterm/tigetstr/resize_term are found in
# libncurses regardless of headers. Hence the cache overrides below.
set -e
ac_cv_header_curses_h=no \
ac_cv_header_term_h=no \
ac_cv_header_ncurses_h=no \
ac_cv_header_ncursesw_ncurses_h=no \
ac_cv_header_ncurses_ncurses_h=no \
ac_cv_header_ncursesw_term_h=no \
ac_cv_header_ncurses_term_h=no \
zsh_cv_header_curses_solaris=no \
ac_cv_func_setupterm=no \
ac_cv_func_resize_term=no \
ac_cv_func_tigetstr=no \
ac_cv_func_tigetnum=no \
ac_cv_func_tigetflag=no \
./configure --disable-dynamic --enable-multibyte \
            --disable-gdbm --disable-pcre --disable-cap \
            --with-term-lib="termcap ncurses curses" "$@"
