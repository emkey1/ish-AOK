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

#include <stdatomic.h>
#include <stdbool.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"

// bash's main(), renamed by the build (-Dmain=bash_main_entry) because AOK has
// one of its own. The same trick SmallCLUE's CMake build uses on Nextvi.
int bash_main_entry(int argc, char **argv, char **envp);

// The guest's own bash, for the second and later SIMULTANEOUS invocations. The
// same choice deps/bash/aok_fork.c makes for a subshell, for the same reason,
// and named the same way there.
#define AOK_GUEST_BASH "/bin/bash"

// Is a native bash live in this address space right now?
//
// bash is a program written to run once and exit; inside AOK it is a C function
// on a thread of the app's process (see kernel/nextvi_glue.c, the same problem
// in a much smaller program). Two questions follow from that, and they are NOT
// the same question:
//
//   Sequentially -- a second bash after the first has returned -- every global
//   still holds what the last one left. bash has a function for exactly this,
//   shell_reinitialize(), which main() already calls when shell_initialized is
//   set; the iSH-AOK additions to it are in deps/bash/shell.c and the audit
//   behind them is docs/bash_native_reentry.md.
//
//   SIMULTANEOUSLY -- `bash` from a bash prompt, a second terminal, or any of
//   bash's own tests that re-run $THIS_SH -- no amount of reinitialising helps,
//   because the two shells would be sharing one set of globals while BOTH are
//   live. The second one's shell_reinitialize is then not a fix but the injury:
//   it resets the first shell's variables, jobs and traps out from under it.
//   Observed as `bash -c 'echo outer; bash -c "echo inner"; echo back'` printing
//   outer and inner and then stopping, with no error of any kind.
//
// What a second live shell actually needs is a separate address space, and AOK
// has one mechanism for that: a guest process. So this hands over to the
// emulated bash, which is a real bash 5.2 of the same version, merely slower.
// exec rather than spawn-and-wait, so the task becomes that shell instead of
// supervising it -- $$ changes (docs/bash_native_plan.md section 2), which the
// shell being replaced cannot observe.
static atomic_flag bash_live = ATOMIC_FLAG_INIT;

int native_bash_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // bash reads the environment through getenv, which is routed
    if (atomic_flag_test_and_set(&bash_live)) {
        nlibc_execv(AOK_GUEST_BASH, argv);
        // Only reached if the guest has no bash at all, which is worth saying
        // out loud rather than failing as a shell that does nothing.
        nlibc_perror(AOK_GUEST_BASH);
        return 127;
    }
    int status = bash_main_entry(argc, (char **) argv, native_env_vector());
    nlibc_flush_std();
    atomic_flag_clear(&bash_live);
    return status;
}
