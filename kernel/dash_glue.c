// dash as a native program: the seam between iSH-AOK and it.
//
// kernel/zsh_glue.c is the model and kernel/bash_glue.c explains the idea --
// that a native program is a C function on a guest task's thread, not a
// process. This file is the smallest of the three, because dash is the
// smallest of the three shells: no modules, no line editor, no completion
// system, and a state that is variables, functions, aliases, options and traps
// and nothing else.
//
// The same WARNING that belongs on the other two belongs here: this file is
// AOK's own code and tools/check-native-libc.py does not run over it. It
// includes kernel/native_libc.h, so the libc names below are routed the way
// dash's are -- but anything added here has to be read twice, because a name
// the header does not rewrite reaches the HOST.
//
// WHY dash IS HERE AT ALL. bash is GPLv3 and disabled by default
// (native_bash meson option; docs/historical/shell_transition_plan.md); zsh
// stays as the interactive shell. Neither
// is /bin/sh. dash is the POSIX shell scripts actually run under, it is
// BSD-3-Clause, and it starts in a fraction of the time either of the others
// does -- which matters when a script forks one per line.
//
// WHAT IS NOT DONE YET, said plainly rather than left to be discovered:
//
//  - FORK. nlibc_fork returns ENOSYS, so every dash forkshell() site fails:
//    pipelines, subshells, command substitution, background jobs, and the
//    fork-then-exec of an ordinary external command. bash and zsh each answer
//    this with a re-launch (deps/bash/aok_fork.c, deps/zsh/Src/aok_fork.c) and
//    dash will need the same. It is deliberately NOT stubbed here: a shell that
//    silently mis-runs a pipeline is worse than one that says it cannot.

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"

// dash's own main(), renamed by -Dmain=dash_main in meson.build for the same
// reason bash's and ktop's are: a native program is called, not executed.
int dash_main(int argc, char **argv);

int native_dash_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // dash reads the routed `environ`, as zsh does

    // Re-entry on the SAME thread. dash's globals are ordinary statics -- it
    // has had no thread-local conversion, because unlike zsh it has no reason
    // to run two of itself in one address space yet. Two dashes on two guest
    // TASKS are two threads and are fine; two on one task share one copy of
    // everything, and the second inherits the first's variables, functions and
    // parser state.
    //
    // __thread rather than a plain static, for the reason zsh_glue.c gives: as
    // a plain static this would fire on the FIRST dash of every task, since
    // each re-launched child is a new task.
    //
    // Said on the guest's own stderr as well as the log, because the person
    // watching a shell behave strangely is not reading AOK's log.
    static __thread bool dash_has_run;
    if (dash_has_run) {
        native_printf(2, "native dash: this is the second dash on this task; "
                "its globals are the first one's and it will misbehave.\n");
    }
    dash_has_run = true;

    int status = dash_main(argc, (char **) argv);
    nlibc_flush_std();
    // dash normally leaves through exit(), which nlibc_exit turns into the
    // guest task's exit, so reaching here means dash_main RETURNED -- unusual
    // enough to be worth a line.
    printk("native dash: returned %d\n", status);
    return status;
}
