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

#include <errno.h>
#include <stdbool.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"
#include "util/sync.h"

// bash's main(), renamed by the build (-Dmain=bash_main_entry) because AOK has
// one of its own. The same trick SmallCLUE's CMake build uses on Nextvi.
int bash_main_entry(int argc, char **argv, char **envp);

// bash's own, not renamed by the build. See the use below.
extern char *shell_name;

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
//
// Recorded as the OWNING TASK rather than as a flag, and that is the whole
// lesson of the first attempt. A flag set on entry and cleared on return
// latches ON forever, because a shell does not return: bash leaves through
// exit() -- nlibc_exit, then do_exit_group -- and the line that would have
// cleared the flag is never reached. The symptom was silent and expensive.
// After the first native bash in an app session, every later one quietly
// became the emulated shell: the fast path was gone, and the whole of the
// re-entry work was never being exercised, so it tested clean while doing
// nothing at all.
//
// Asking "is the task that owns this still alive?" instead stays true through
// every way of dying. A pid recycled onto an unrelated task in the window
// between the owner exiting and the next shell starting would read as
// still-owned; the cost is one shell running emulated, which is the safe
// direction to be wrong in.
static lock_t bash_owner_lock = LOCK_INITIALIZER;
static dword_t bash_owner_pid;          // 0: no native bash is live

static bool native_bash_claim(void) {
    bool claimed = false;
    lock(&bash_owner_lock, 0);
    if (bash_owner_pid != 0) {
        struct task *owner = pid_get_task_ref(bash_owner_pid);
        if (owner != NULL)
            task_ref_cnt_mod(owner, -1);   // alive, which is all that was asked
        else
            bash_owner_pid = 0;            // gone; the seat is free
    }
    if (bash_owner_pid == 0 && current != NULL) {
        bash_owner_pid = current->pid;
        claimed = true;
    }
    unlock(&bash_owner_lock);
    return claimed;
}

static void native_bash_release(void) {
    lock(&bash_owner_lock, 0);
    if (current != NULL && bash_owner_pid == current->pid)
        bash_owner_pid = 0;
    unlock(&bash_owner_lock);
}

int native_bash_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // bash reads the environment through getenv, which is routed
    if (!native_bash_claim()) {
        // Say so in the kernel log as well as trying to run it. A shell that
        // hands over is invisible from the outside -- the user sees a working
        // but slower bash, or, if this fails, a session that ends before it
        // prints anything -- and neither tells anyone why. printk reaches the
        // app's own log, which is the only channel a login shell over ssh has
        // not already lost.
        printk("native bash: another is live (pid %d); handing this one to %s\n",
               bash_owner_pid, AOK_GUEST_BASH);
        nlibc_execv(AOK_GUEST_BASH, argv);
        printk("native bash: %s did not start (errno %d); this shell exits 127\n",
               AOK_GUEST_BASH, errno);
        nlibc_perror(AOK_GUEST_BASH);
        return 127;
    }
    // The one piece of re-entry state that has to be handled from OUT here,
    // because bash's main() reads shell_name BEFORE it calls
    // shell_reinitialize: that read is how it decides whether it is a login
    // shell, and the pointer the previous shell left behind is into memory that
    // has since gone. Everything else belongs in shell_reinitialize and is
    // there (deps/bash/shell.c).
    //
    // Pointed at a string literal rather than nulled, and the difference is not
    // cosmetic. `shell_initialized || shell_name` is the condition that decides
    // whether to reinitialise at all, and for `bash -c` shell_initialized is
    // NEVER set -- that path runs exit_shell (shell.c:776) long before
    // shell.c:834 sets it. So shell_name is the only trigger there, and nulling
    // it silently turned re-entry OFF for every non-interactive shell: the
    // second `bash -c` in an app session then found the FIRST one's variable
    // table still populated, and died in bind_variable("COMP_WORDBREAKS")
    // calling assign_comp_wordbreaks on a variable that already had an assign
    // function, with a null value. A literal keeps the trigger and cannot
    // dangle; set_shell_name replaces it from argv[0] a few lines later.
    //
    // From the SECOND shell onwards only. On the first, shell_variables is
    // still null, and shell_reinitialize would walk straight through
    // delete_all_contexts into global_variables->table -- reinitialising a
    // world that has not been built yet.
    static bool bash_has_run;
    if (bash_has_run)
        shell_name = (char *) "bash";
    bash_has_run = true;
    int status = bash_main_entry(argc, (char **) argv, native_env_vector());
    nlibc_flush_std();
    native_bash_release();
    // Only reached when bash RETURNS rather than exiting, which is unusual
    // enough to be worth a line -- and the interesting case is a shell that
    // came back immediately without having done anything.
    printk("native bash: returned %d\n", status);
    return status;
}
