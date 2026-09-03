// Running ktop as a native program.
//
// ktop is AOK's process viewer, and its source is one file that ships to the
// guest at /AOK/tools/ktop/ktop.c for anyone who wants to build or read it
// (opt/AOK/tools/ktop). meson.build compiles that same file a second time as
// HOST code with kernel/native_libc.h force-included, so `ktop` also exists at
// /AOK/native/ktop and runs without being translated.
//
// What that buys, measured rather than assumed: 2.7x per refresh on an i386
// guest (0.46ms against 1.23ms, marginal cost of one /proc sweep plus render,
// so process startup is excluded). Worthwhile, not dramatic -- the bulk of a
// refresh is the kernel's own /proc work, which is host code whichever ktop
// asks for it. The larger prize is that it needs no toolchain and no build
// step in the guest at all.
//
// One source, two builds, deliberately: a copy would drift, and the guest-side
// build is what the documentation tells people to use.
//
// The build renames ktop's entry point rather than this file declaring a
// second one, the same arrangement kernel/nextvi_glue.c uses -- so ktop.c
// still has an ordinary main() and still compiles with a bare `cc ktop.c`.
//
// What this has to put back
// -------------------------
// ktop resets its own file-scope state on entry (reset_state in ktop.c). What
// it cannot reset is getopt's, which lives here in the shim: nlibc's getopt
// keeps optind and its scanning position in __thread storage, so a thread that
// runs ktop twice would begin the second parse wherever the first one stopped
// and silently drop the flags. optreset is the documented way to say "new
// argv" to the BSD getopt macOS ships and nlibc reimplements; ktop.c cannot
// set it itself, because a guest build against musl or glibc has no such
// variable.

#include <unistd.h>

#include "kernel/native_libc.h"

// ktop.c's main(), renamed by meson so it does not collide with AOK's.
int ktop_program_main(int argc, char **argv);

int native_ktop_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // ktop reads no environment variable of its own.

    optreset = 1;
    optind = 1;

    // argv is const in the native_program signature because the dispatcher
    // owns it; getopt's prototype takes char *const [] and ktop's main takes
    // char **, which is the ordinary main() shape. Neither writes through it.
    return ktop_program_main(argc, (char **) argv);
}
