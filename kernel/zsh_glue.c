// zsh as a native program: the seam between iSH-AOK and it.
//
// The bash counterpart, kernel/bash_glue.c, is the model and is worth reading
// first -- it explains why a native program is a C function rather than a
// process. This file is smaller because zsh needs less coaxing at the entry
// point, and because this build deliberately does LESS than bash's: one live
// zsh at a time, no thread-local conversion, no fork.
//
// The same WARNING that belongs on bash_glue.c belongs here: this file is
// AOK's own code, and tools/check-native-libc.py does not run over it. It does
// include kernel/native_libc.h, so the libc names below are routed the way
// zsh's are -- but anything added here has to be read twice, because a name
// the header does not rewrite reaches the HOST.
//
// WHAT IS NOT DONE, said plainly so nobody has to infer it from behaviour:
//
//  - zsh's globals are process-global, not __thread. Two native zshs at once
//    would share one shell's state. bash solved this with a rewrite pass
//    (tools/bash-tls-rewrite.py); zsh has not had it. One at a time.
//  - fork() is nlibc_fork, which is ENOSYS. So subshells, pipelines and
//    command substitution do not run. Builtins, the parser, parameter
//    expansion, globbing, loops and redirection do -- they are in-process.
//    bash's answer to this is fork-by-re-launch (deps/bash/aok_fork.c); zsh
//    has not had that either.
//  - re-entry. bash needed shell_name reset before its second run in one app
//    session (see bash_glue.c). zsh's equivalent has not been mapped: the
//    second /AOK/native/zsh in one app session inherits the first one's
//    globals. The counter below exists to make that visible in the log rather
//    than to fix it.

#include <stdbool.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"

// zsh's entry point, Src/init.c. NOT Src/main.c, which is a three-line
// wrapper around this and would bring a second main() into the binary; the
// build leaves that file out rather than renaming it the way bash's -Dmain
// does.
//
// The signature differs from bash_main_entry's on purpose: zsh_main takes
// only (argc, argv) and reads the environment through `environ`, which
// kernel/native_libc.h rewrites into a per-task accessor. So the envp the
// dispatcher hands us is already where zsh will look for it, and passing it on
// would be the wrong shape as well as redundant.
int zsh_main(int argc, char **argv);

int native_zsh_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // see zsh_main above: zsh reads the routed `environ`

    // Re-entry, and it is BROKEN rather than merely unproven -- measured, not
    // guessed. The second /AOK/native/zsh in one app session prints
    //
    //     zsh:1: name clash when adding hook `exit'
    //     zsh:1: command not found: print
    //
    // because zsh's module table is process-global and still holds the first
    // shell's registrations, so boot_ re-adds hooks that are already there and
    // the builtin table ends up in a state where `print` is not a builtin any
    // more. bash needed one variable reset here (shell_name) and a
    // shell_reinitialize inside its own sources; zsh has no equivalent
    // function, and finding its one is the next piece of work, not this one.
    //
    // Said on the guest's OWN stderr rather than only through printk. printk
    // goes to AOK's log, which the person watching a terminal fill up with
    // "name clash" messages is not reading. One line naming the cause is the
    // difference between a known limitation and a mystery.
    static bool zsh_has_run;
    if (zsh_has_run) {
        native_printf(2, "native zsh: this is the second zsh in one app "
                "session; its globals are the first one's and it will "
                "misbehave. Restart the app (or use the guest's own zsh).\n");
    }
    zsh_has_run = true;

    int status = zsh_main(argc, (char **) argv);
    nlibc_flush_std();
    // zsh normally leaves through exit(), which nlibc_exit turns into the
    // guest task's exit -- so this is only reached when zsh_main RETURNS,
    // which is unusual enough to be worth a line in the log.
    printk("native zsh: returned %d\n", status);
    return status;
}
