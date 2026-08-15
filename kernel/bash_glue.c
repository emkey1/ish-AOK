// bash as a native program: the seam between AOK and it.
//
// See docs/bash_native_plan.md for why bash is here at all. This file is the
// bash counterpart of kernel/smallclue_glue.c, and it is deliberately small:
// everything bash needs from the kernel goes through kernel/native_libc.h like
// any other native program, so what is left is the entry point and the two
// renames the build applies.
//
// A WARNING that belongs here rather than anywhere else: this file is AOK's own
// code and is compiled WITHOUT native_libc.h force-included, so a plain
// getenv() or `environ` here reaches the HOST. tools/check-native-libc.py
// cannot help -- it runs over the archives of native code, and must not run
// over glue, which is meant to call the host libc. That is exactly how
// SmallCLUE's children came to be handed the Mac's environment for months. Use
// native_env_vector() and the native_* helpers, and read every host-libc name
// here twice.

#include <stdbool.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"

// bash's main(), renamed by the build (-Dmain=bash_main_entry) because AOK has
// one of its own. The same trick SmallCLUE's CMake build uses on Nextvi.
int bash_main_entry(int argc, char **argv, char **envp);

// bash is a program written to run once and exit -- and inside AOK it does not,
// because `bash` is a function called on a thread of the app's process (see
// the note in kernel/nextvi_glue.c, which is the same problem in a smaller
// program). Unlike Nextvi it has thousands of globals rather than ninety-odd,
// so resetting them individually is not a route that ends well.
//
// What saves it is that bash already has to leave a clean shell behind for
// `exec`: shell_reinitialize() exists for exactly this and is what the
// -c path uses. It is not yet wired up here, so the FIRST bash in a session is
// the trustworthy one; a second is on notice until this is finished. That is
// recorded rather than papered over because a stale global in a shell is a
// wrong answer, not a crash.
int native_bash_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // bash reads the environment through getenv, which is routed
    int status = bash_main_entry(argc, (char **) argv, native_env_vector());
    nlibc_flush_std();
    return status;
}
