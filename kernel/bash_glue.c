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

// bash's own, not renamed by the build. See the use below.
//
// __thread because bash's globals are: this file is compiled WITHOUT bash's
// headers, so nothing here would have caught the disagreement, and a
// non-thread-local declaration of a thread-local variable compiles, links, and
// then reads the descriptor as if it were the value. tools/check-bash-tls.py
// found this one.
extern __thread char *shell_name;
void aok_tls_fixups(void);

// bash used to be limited to ONE live instance in the app, because a native
// program is a C function in the emulator's process and two shells would have
// shared bash's globals -- the same wall fork hits. Every other session
// silently got the guest's emulated bash instead, which for anyone using this
// as a login shell meant the first terminal was fast and the rest were not.
//
// That restriction is gone: bash's mutable globals are __thread now, so each
// guest task has its own copy of the 50 KB that has to differ per shell. The
// conversion is tools/bash-tls-rewrite.py and the four tools beside it, and
// tools/check-bash-tls.py is what makes it safe to believe -- a translation
// unit that declares one of those variables WITHOUT __thread compiles clean,
// links clean, and then reads the wrong memory. This file was one of them.
//
// What is still SHARED between concurrent shells, deliberately: the builtin
// table, which is the set bash was compiled with and so identical everywhere,
// and readline's keymaps, which cross-reference each other in static
// initialisers and cannot be made per-thread without a fixup pass over all of
// them. So `bind` in one shell can be seen by another, and so can
// `enable`/`disable`. The full list, with reasons, is ALLOW in
// tools/bash-tls-fix-externs.py.

int native_bash_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // bash reads the environment through getenv, which is routed
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
    //
    // Per THREAD, not per app: "has a shell run before" is now asking whose
    // shell_variables is already populated, and that is thread-local like the
    // rest of them. As a plain static it made the second concurrent shell
    // reinitialise a world its own thread had not built, and
    // delete_all_contexts took a null shell_variables.
    static __thread bool bash_has_run;
    if (bash_has_run)
        shell_name = (char *) "bash";
    bash_has_run = true;

    // bash's globals are thread-local, so this task has its own copy of all of
    // them -- which is what lets more than one native bash be live at once.
    // The option tables that used to hold `&some_option` cannot be initialised
    // statically any more; this fills them in for this thread.
    aok_tls_fixups();
    int status = bash_main_entry(argc, (char **) argv, native_env_vector());
    nlibc_flush_std();
    // Only reached when bash RETURNS rather than exiting, which is unusual
    // enough to be worth a line -- and the interesting case is a shell that
    // came back immediately without having done anything.
    printk("native bash: returned %d\n", status);
    return status;
}
