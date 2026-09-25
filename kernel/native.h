#ifndef KERNEL_NATIVE_H
#define KERNEL_NATIVE_H

#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>
#include "misc.h"

// Programs whose implementation is compiled into iSH-AOK and executed as HOST
// code, instead of being loaded and translated as guest instructions.
//
// They are reached through /AOK/native/<name> (fs/aok.c). Exec matches on the
// RESOLVED path, so a symlink from anywhere dispatches natively while argv[0]
// stays whatever the caller passed -- which is exactly what lets one multicall
// binary serve `ln -s /AOK/native/smallclue /usr/local/bin/df` the same way it
// works on Linux. Adding a program is a table entry here plus a node in
// fs/aok.c; the dispatcher itself does not change.
//
// Because no guest code runs, a native program is guest-ABI-independent: one
// implementation serves i386, amd64, arm64 and riscv64 guests alike, which
// matters most for the slowest of them.
//
// Execution model, and the three things easiest to get wrong:
//
//  - A native program is NOT a host process. It runs on the calling task's own
//    thread, inside the execve syscall that would otherwise have replaced the
//    process image, and its return value becomes that process's exit status.
//    iSH's existing task is the process the guest sees, so pid, waitpid and
//    exit status all work without new machinery. It also means a long-running
//    native program blocks that guest task for its whole duration.
//
//  - A native program is linked against the HOST libc. A bare open()/write()
//    would therefore hit the host filesystem, not the guest's rootfs. All I/O
//    must go through iSH's kernel instead (fd_write_host_buf and friends in
//    kernel/fs.h). This is the seam that real programs need filling in before
//    they can do anything filesystem-shaped.
//
//  - Any process-global state the program sets MUST be cleaned up when it
//    exits, or keyed per invocation -- because the process outlives the
//    program. A global that caches a file descriptor, a "logger already
//    installed" flag, or a registered signal handler survives into the NEXT
//    run of the program, pointing into a task that no longer exists; the fd
//    NUMBER it holds may even name an unrelated open file of the new task.
//    This is how the second `hx` in one app session failed to start: tokio's
//    once-per-process signal socketpair held the first run's dead fds, and
//    helix's once-per-process logger refused to initialize again. The two
//    correct shapes are (a) reset/teardown on every entry and exit (see
//    nextvi_glue.c), and (b) keying the state by nlibc_invocation_token()
//    below (see the tokio and signal-hook-registry forks under deps/). An fd
//    held in a global is never safe to close from a later run's thread --
//    close routes through the CURRENT task's table -- so replaced state that
//    holds descriptors must be leaked, not dropped, unless it is torn down by
//    the run that owns it.

struct native_program {
    // basename under /AOK/native/
    const char *name;
    // argv[0] is the name the caller invoked, as on any multicall binary.
    // envp is NULL-terminated. The return value becomes the exit status.
    int (*main)(int argc, char *const argv[], char *const envp[]);

    // Runs as root however it was invoked, the way a setuid-root binary does.
    //
    // ONE flag rather than a mode and an owner, because two would be two
    // things to disagree: fs/aok.c derives the mode it reports from this, and
    // kernel/exec.c derives the credential change from this, so a program
    // cannot end up looking setuid to stat(2) while gaining nothing on exec,
    // or the reverse. That mismatch is how a security feature becomes a
    // decoration.
    //
    // Safe here in a way it is NOT for smallclue: these are separate programs,
    // so a setuid one cannot be talked into being `sh`. Setting it on a
    // multicall entry point would hand every applet root. Anything carrying
    // this flag is security-critical host code -- it runs unsandboxed in the
    // app process -- and inherits the caller's environment, so it must
    // sanitise what it trusts (PATH, IFS) itself.
    bool setuid_root;

    // ---- describing itself to a checkpoint (kernel/checkpoint.c) ----------
    //
    // A native program is a C function on a HOST thread. There is no
    // serialising that stack, so a checkpoint cannot photograph one the way it
    // photographs an emulated task -- and the project's rule is that a native
    // program either knows how to DUMP ITS OWN STATE or the checkpoint refuses
    // while it is running. This pair is the knowing half.
    //
    // ckpt_dump returns a malloc'd blob the same program can be re-launched
    // with, or NULL if it cannot describe itself right now (which is a
    // refusal, not a silent loss). It runs on the PROGRAM'S OWN THREAD, from
    // its parking place in native_checkpoint, because that is the only thread
    // its state exists on.
    //
    // ckpt_state_var names the environment variable through which a restored
    // instance is told the descriptor its state arrives on. The restore
    // creates a pipe, writes the blob, and dispatches -- which for a shell is
    // exactly the channel its fork-by-relaunch child already uses, so nothing
    // new has to be taught to the program itself.
    //
    // Both NULL means "cannot describe itself"; the checkpoint says so, by
    // name.
    char *(*ckpt_dump)(void);
    const char *ckpt_state_var;
};

// NULL when no native program of that name is compiled into this build. Exec
// then falls through to the ordinary path and runs the /AOK/native stub, which
// reports the situation loudly rather than failing with ENOEXEC.
// zsh's self-description, for the table's ckpt_dump slot. In kernel/zsh_glue.c.
char *native_zsh_ckpt_dump(void);

const struct native_program *native_program_lookup(const char *name);

// The native program this task is RUNNING, or NULL. Set when the program is
// dispatched and cleared when it ends; kernel/checkpoint.c asks so it can find
// the ckpt_dump above without re-deriving the program from argv[0].
const struct native_program *native_program_running(struct task *task);

// Enumerating what this build actually has, so /AOK/native (fs/aok.c) is served
// FROM the registry rather than from a second list beside it. Adding a native
// program is then one table entry, not an entry plus five places in the
// filesystem that have to agree with it.
//
// The index is over programs that are present: a program whose implementation
// is not in this build (a submodule nobody populated) is skipped, so it has no
// node and exec finds nothing, rather than a node that dispatches into a hole.
size_t native_program_count(void);
const struct native_program *native_program_at(size_t index);

// Running a native program cannot simply happen where execve would have loaded
// the ELF: that path never returns, so every buffer the execve syscall had
// allocated -- including the argv/envp blocks it frees on the way out -- would
// leak on each invocation. Instead execve records the program with private
// copies of argv/envp and returns success, then unwinds normally, and the
// entry point runs it once its own frees have happened.
//
// Records prog for the current task; returns 0, or _ENOMEM. The copies are
// owned from here on, so the caller's argv/envp may be freed immediately.
int native_exec_set_pending(const struct native_program *prog, int argc,
        char *const argv[], char *const envp[]);

// Runs whatever native_exec_set_pending recorded for this task and terminates
// the task with its return value; returns immediately if nothing is pending.
// Called from each execve entry point AFTER it has freed its own argv/envp
// blocks -- that ordering is the entire reason the pending step exists -- and
// from task_run_current, which covers a task whose very first image is native
// and is therefore reached without any execve syscall returning.
void native_exec_run_pending(void);

// Marks the program just recorded by native_exec_set_pending as one a
// checkpoint restore is bringing back rather than one being exec'd. Two things
// change when it runs. A task whose terminal is held by one of its own jobs
// waits for that job before the program starts, because a re-launched shell
// has no job table and would otherwise read the terminal out from under it.
// And a task that was an exec stand-in (standin_child != 0) resumes as that
// wait instead of running its program at all. Call with current = the task.
void native_exec_mark_restored(dword_t standin_child);

// The exec stand-in's wait, entered directly: waits for `child`, forwarding
// signals, and exits with its status word. For a stand-in coming back from a
// checkpoint. kernel/native_libc.c.
//
// Spelled __noreturn__, not noreturn: kernel/native_libc.h includes
// <stdnoreturn.h>, whose `#define noreturn _Noreturn` turns the plain spelling
// into an attribute clang does not know, and it is then dropped with only a
// warning in every file that includes that header before this one.
void nlibc_exec_standin_resume(dword_t child) __attribute__((__noreturn__));

// exec from a native program, done for real. A native program cannot become
// a guest image in the middle of a C function, so its exec is spawn-then-wait
// (nlibc_exec_standin): the program runs as a CHILD, and the pid changes. For
// a traced task that is not an exec at all to the one watching it. gdb starts
// a program as `$SHELL -c 'exec prog'` under PTRACE_TRACEME and counts one trap
// per exec; with $SHELL /AOK/native/zsh, the default login shell a release
// asks people to use, the traced task never became `prog` -- an untraced child
// ran it -- and gdb reported "During startup program exited normally" for
// every program, while `set startup-with-shell off` worked.
//
// So a traced task's exec replaces its image in place, keeping its pid: the
// exec commits (native_exec_in_place, kernel/native_io.c), and the program's
// C stack is abandoned with a siglongjmp back to the native_exec_run_pending
// that called its main, which returns into the new image -- or runs the next
// program, if the exec was of a native one. What the program had allocated is
// left behind, which is the price and why an untraced exec still stands in.
//
// Wanted when the task is traced and the unwind is safe: this thread is the
// one native_exec_run_pending runs the program on, no shim call below is
// mid-flight, the program has no threads of its own, and no host stdio lock is
// held. Anything else keeps the stand-in.
bool native_exec_in_place_wanted(void);
// Whether this thread has a native_exec_run_pending to come back to, for the
// task it is running.
bool native_exec_landing_available(void);
// The unwind itself; for native_exec_in_place, once the exec has committed.
void native_exec_land(void) __attribute__((__noreturn__));
// The shim's per-thread, per-program state, back to how a thread starts: once
// a program has exec'd in place, the thread runs whatever comes next.
// kernel/native_libc.c.
void nlibc_program_state_reset(void);
// Whether the calling thread is inside a host stdio callback. native_libc.c.
bool nlibc_in_stdio(void);

// Poll for pending signals and group-stops, and act on them.
//
// A native program runs as host code on the guest task's thread, so nothing
// checks for signals the way the instruction dispatcher does for translated
// code: the program is a plain C loop. Without this it is UNINTERRUPTIBLE --
// ^C and ^Z do nothing, which is how `top` became impossible to quit.
//
// Call it from anywhere a native program can block or loop. The libc shim
// calls it on every read and write, which covers anything doing I/O; a
// compute loop with no I/O still needs its own call. It may not return: a
// default-fatal signal exits the task from inside receive_signals.
//
// This lives here rather than in the shim because it is a property of running
// natively, not of any one program -- exsh will need it on the same terms.
void native_checkpoint(void);

// True while this thread is somewhere native_checkpoint() cannot deliver a
// signal -- today, inside a host stdio callback holding a FILE lock (see
// nlibc_stdio_defer_fatal in kernel/native_libc.c). False for a thread running
// translated guest code, which has no such place. Callers use it to avoid
// manufacturing an interruption whose handler is not going to run.
bool native_delivery_deferred(void);

struct task;
// Drops a record that will never run, for a task torn down between exec and
// first execution.
void native_exec_discard_pending(struct task *task);

// The per-invocation token behind the third bullet above: a value that is
// different for every run of every native program, readable from any thread
// the program creates (nlibc_pthread_create hands it down with `current`),
// and async-signal-safe to read (one __thread load). 0 means "this thread is
// not running a native program". Foreign-toolchain code imports it by name --
// the tokio and signal-hook-registry forks key their per-process globals on
// it -- so the symbol name is ABI. Implemented in kernel/native_libc.c;
// assigned by native_exec_run_pending immediately before the program's main.
uint64_t nlibc_invocation_token(void);
void nlibc_invocation_token_assign(void);

// The environment a native program sees.
//
// getenv() in host code reads the HOST process's environment -- the Mac's, or
// the iOS app's. So `env` printed the developer's shell, and worse, the PATH
// search for a native program's children walked the Mac's directories. The
// guest's environment arrives as execve's envp and has to be kept somewhere
// the shim can reach; it lives on the task, so a program's own pthreads (which
// share `current`) share it, and each concurrently-running native program has
// its own.
//
// Seeded from execve's envp; owned by the task from then on.
void native_env_init(char *const envp[]);
void native_env_discard(struct task *task);
void native_cmdline_discard(struct task *task);
void native_sigtable_discard(struct task *task);

// NULL-terminated, and never NULL itself -- an empty environment is an array
// holding just the terminator, which is what a caller iterating it expects.
char **native_env_vector(void);
char ***native_argv_slot(void);
int *native_argc_slot(void);
// The task's environ slot, so `environ` is assignable and not just readable.
char ***native_env_slot(void);
const char *native_env_get(const char *name);
// 0, or a negative errno.
int native_env_set(const char *name, const char *value, bool overwrite);
int native_env_unset(const char *name);

#endif
