// Dispatch and registry for natively-implemented programs. See kernel/native.h
// for the execution model; this file is the table and the plumbing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"

#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/signal.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"
#include "debug.h"

// Everything a native program prints has to go through iSH's fd layer, not the
// host's stdio -- see the libc warning in kernel/native.h.
static void native_write_str(fd_t fd_no, const char *s) {
    if (s == NULL)
        return;
    native_write(fd_no, s, strlen(s));
}

// The applet a multicall binary was invoked as: the basename of argv[0], which
// the caller supplies independently of the path exec resolved. That separation
// is the whole point -- /usr/local/bin/df and /AOK/native/smallclue resolve to
// the same program, and only argv[0] says which behaviour was wanted.
static const char *native_applet_name(int argc, char *const argv[]) {
    if (argc < 1 || argv[0] == NULL)
        return "";
    const char *slash = strrchr(argv[0], '/');
    return slash == NULL ? argv[0] : slash + 1;
}

static size_t native_envp_count(char *const envp[]) {
    size_t n = 0;
    if (envp != NULL)
        while (envp[n] != NULL)
            n++;
    return n;
}

static const char *native_getenv(char *const envp[], const char *name) {
    if (envp == NULL)
        return NULL;
    size_t len = strlen(name);
    for (size_t i = 0; envp[i] != NULL; i++)
        if (strncmp(envp[i], name, len) == 0 && envp[i][len] == '=')
            return envp[i] + len + 1;
    return NULL;
}

// cat: the smallest applet that proves the filesystem seam end to end -- guest
// path resolution against the task's cwd, a real read through iSH's VFS, and
// output onto the guest's own stdout.
static int native_applet_cat(int argc, char *const argv[]) {
    if (argc < 2) {
        native_write_str(2, "cat: reading stdin is not implemented yet\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct fd *fd = NULL;
        int err = native_open(argv[i], O_RDONLY_, &fd);
        if (err < 0) {
            native_printf(2, "cat: %s: cannot open (errno %d)\n", argv[i], -err);
            rc = 1;
            continue;
        }
        char buf[4096];
        for (;;) {
            ssize_t n = native_read(fd, buf, sizeof(buf));
            if (n < 0) {
                native_printf(2, "cat: %s: read error (errno %d)\n", argv[i], (int) -n);
                rc = 1;
                break;
            }
            if (n == 0)
                break;
            native_write(1, buf, (size_t) n);
        }
        native_close(fd);
    }
    return rc;
}

// ls: proves directory iteration, which is the other half of the seam. One
// name per line, unsorted -- this is a seam probe, not a coreutils replacement.
static int native_applet_ls(int argc, char *const argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0) {
        native_printf(2, "ls: %s: cannot open (errno %d)\n", path, -err);
        return 1;
    }
    if (fd->ops->readdir_begin != NULL)
        fd->ops->readdir_begin(fd);
    struct dir_entry entry;
    int rc = 0;
    for (;;) {
        int res = native_readdir(fd, &entry);
        if (res < 0) {
            native_printf(2, "ls: %s: readdir error (errno %d)\n", path, -res);
            rc = 1;
            break;
        }
        if (res == 0)
            break;
        native_printf(1, "%s\n", entry.name);
    }
    if (fd->ops->readdir_end != NULL)
        fd->ops->readdir_end(fd);
    native_close(fd);
    return rc;
}

// pwd/stat: the remaining two seam primitives, so all of them have a caller.
static int native_applet_pwd(void) {
    char buf[MAX_PATH];
    int err = native_getcwd(buf);
    if (err < 0) {
        native_printf(2, "pwd: cannot determine cwd (errno %d)\n", -err);
        return 1;
    }
    native_printf(1, "%s\n", buf[0] != '\0' ? buf : "/");
    return 0;
}

static int native_applet_stat(int argc, char *const argv[]) {
    if (argc < 2) {
        native_write_str(2, "stat: missing operand\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct statbuf st = {};
        int err = native_stat(argv[i], &st, true);
        if (err < 0) {
            native_printf(2, "stat: %s: cannot stat (errno %d)\n", argv[i], -err);
            rc = 1;
            continue;
        }
        native_printf(1, "%s size=%llu mode=%o uid=%u gid=%u\n", argv[i],
                (unsigned long long) st.size, st.mode & 07777, st.uid, st.gid);
    }
    return rc;
}

// Placeholder standing in for the real SmallCLUE, which is not yet part of this
// build. The applets here are deliberately a seam probe rather than a coreutils
// implementation: between them they exercise every part of the path SmallCLUE
// will need -- resolved-path matching, argv[0] applet selection, env handoff,
// exit status, and now guest filesystem access through kernel/native_io.h.
// Replacing this with SmallCLUE's own multicall entry point is a change to this
// function and nothing else.
static int smallclue_native_main(int argc, char *const argv[], char *const envp[]) {
    const char *applet = native_applet_name(argc, argv);

    if (strcmp(applet, "true") == 0)
        return 0;
    if (strcmp(applet, "false") == 0)
        return 1;

    if (strcmp(applet, "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1)
                native_write_str(1, " ");
            native_write_str(1, argv[i]);
        }
        native_write_str(1, "\n");
        return 0;
    }

    if (strcmp(applet, "cat") == 0)
        return native_applet_cat(argc, argv);
    if (strcmp(applet, "ls") == 0)
        return native_applet_ls(argc, argv);
    if (strcmp(applet, "pwd") == 0)
        return native_applet_pwd();
    if (strcmp(applet, "stat") == 0)
        return native_applet_stat(argc, argv);

    // Invoked by its own name: report what this build can do. Also the
    // verification surface for the dispatch path, hence the argv/env dump.
    if (strcmp(applet, "smallclue") == 0) {
        if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
            native_printf(1, "native: ok\n");
            native_printf(1, "argc: %d\n", argc);
            for (int i = 0; i < argc; i++)
                native_printf(1, "argv[%d]: %s\n", i, argv[i]);
            native_printf(1, "envp count: %zu\n", native_envp_count(envp));
            const char *path = native_getenv(envp, "PATH");
            native_printf(1, "envp PATH: %s\n", path != NULL ? path : "<unset>");
            native_write_str(2, "native: this line went to fd 2\n");
            return 0;
        }
        // Explicit applet form: `smallclue ls -l` means the same as `ls -l`,
        // the way busybox works. Without this, the only way to reach an applet
        // is through a symlink, which is a surprise for anyone who invokes the
        // multicall binary by name to try one out. Shifting argv rather than
        // special-casing keeps argv[0] meaning the same thing to the applet.
        if (argc > 1 && argv[1][0] != '-')
            return smallclue_native_main(argc - 1, argv + 1, envp);

        native_write_str(1,
                "smallclue (native placeholder, built into iSH-AOK)\n"
                "\n"
                "Runs as host code -- not translated -- so it costs the same on every\n"
                "guest architecture.\n"
                "\n"
                "Applets in this build: true, false, echo, cat, ls, pwd, stat\n"
                "\n"
                "Run one directly:      smallclue ls /etc\n"
                "or via a symlink:      ln -s /AOK/native/smallclue /usr/local/bin/ls\n"
                "Self-test:             smallclue --selftest\n");
        return 0;
    }

    native_printf(2, "smallclue: applet '%s' not built into this iSH-AOK\n", applet);
    return 127;
}

// SmallCLUE proper, compiled in from deps/smallclue (meson.build). Its
// src/main.c -- a four-line shim around this -- is excluded, so this is the
// same entry point the standalone binary uses.
extern int smallclueMain(int argc, char **argv);

static int smallclue_real_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;  // SmallCLUE reads the environment through getenv, not argv3
    int status = smallclueMain(argc, (char **) argv);
    nlibc_flush_std();
    return status;
}

// bash (kernel/bash_glue.c), which is only in the build when the deps/bash
// submodule is populated. WEAK rather than a build-wide define: the address is
// simply NULL when nothing defines it, so the table below says what programs
// COULD exist and native_program_lookup answers for what actually does. That
// keeps the condition in one place instead of in a -D that every translation
// unit would have to agree about.
#ifdef ISH_NATIVE_BASH
int native_bash_main(int argc, char *const argv[], char *const envp[]);
#endif

// zsh (kernel/zsh_glue.c), on the same terms as bash: the define and the glue
// file are both decided by meson's `have_zsh`, so the table and the archive
// cannot disagree. Off by default -- see meson_options.txt.
#ifdef ISH_NATIVE_ZSH
int native_zsh_main(int argc, char *const argv[], char *const envp[]);
// zsh's MULTIOS byte pump, also kernel/zsh_glue.c. A program of its own rather
// than an applet of the one above because it is spawned as a separate guest
// TASK: `echo hi > a > b` needs something holding the target descriptors that
// is NOT the shell, and the shell is about to close its own copies. See the
// long note in zsh_glue.c, and closemn() in deps/zsh/Src/exec.c which spawns
// it. Nobody is expected to run it by hand -- it exists at /AOK/native only
// because that is how exec reaches a native program.
int native_zsh_multio_main(int argc, char *const argv[], char *const envp[]);
#endif

#ifdef ISH_NATIVE_RUST
int rust_native_probe_main(int argc, char *const argv[], char *const envp[]);
#endif

#ifdef ISH_NATIVE_HELIX
// Reached the same way as the probe -- a staticlib whose libc imports are
// rewritten onto the shim -- but its own crate, deps/helix-native, so that the
// probe stays small enough to be a diagnostic. helix itself is deps/helix.
int helix_native_main(int argc, char *const argv[], char *const envp[]);
#endif

// The terminal half of the Workspace MotePad editor (kernel/native_motepad.c).
// Unconditional: it is one C file with no dependency beyond the shim, so there
// is nothing to gate it on -- unlike helix or bash, which bring a toolchain and
// a licence question with them.
int native_motepad_main(int argc, char *const argv[], char *const envp[]);

static const struct native_program native_programs[] = {
    { "smallclue", smallclue_real_main },
    { "motepad", native_motepad_main },
#ifdef ISH_NATIVE_RUST
    // Rust, reached by rewriting its libc imports onto the shim rather than by
    // the #define redirection that only covers what AOK compiles. See
    // tools/gen-nlibc-renames.py and deps/rust-native-probe.
    { "rust-probe", rust_native_probe_main },
#endif
#ifdef ISH_NATIVE_HELIX
    // Registered as `hx`, which is what helix calls itself and what a user
    // types. deps/helix-native is the staticlib wrapper; deps/helix is the
    // editor.
    { "hx", helix_native_main },
#endif
#ifdef ISH_NATIVE_BASH
    { "bash", native_bash_main },
#endif
#ifdef ISH_NATIVE_ZSH
    { "zsh", native_zsh_main },
    { "zsh-multio", native_zsh_multio_main },
#endif
};

size_t native_program_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(native_programs) / sizeof(native_programs[0]); i++)
        if (native_programs[i].main != NULL)
            n++;
    return n;
}

const struct native_program *native_program_at(size_t index) {
    for (size_t i = 0; i < sizeof(native_programs) / sizeof(native_programs[0]); i++) {
        if (native_programs[i].main == NULL)
            continue;
        if (index-- == 0)
            return &native_programs[i];
    }
    return NULL;
}

const struct native_program *native_program_lookup(const char *name) {
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < sizeof(native_programs) / sizeof(native_programs[0]); i++) {
        // A NULL main is a program this build does not have -- see the weak
        // declaration above. Skipping it here means /AOK/native serves no node
        // for it and exec finds nothing, rather than dispatching into a hole.
        if (native_programs[i].main == NULL)
            continue;
        if (strcmp(native_programs[i].name, name) == 0)
            return &native_programs[i];
    }
    return NULL;
}

// Deep copy: the vectors handed to set_pending alias buffers the execve
// syscall is about to free, so the strings have to be copied too, not just the
// pointer array.
static char **native_dup_vector(char *const vec[], size_t count) {
    char **copy = calloc(count + 1, sizeof(*copy));
    if (copy == NULL)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        copy[i] = strdup(vec[i] != NULL ? vec[i] : "");
        if (copy[i] == NULL) {
            for (size_t j = 0; j < i; j++)
                free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    return copy;
}

static void native_free_vector(char **vec) {
    if (vec == NULL)
        return;
    for (size_t i = 0; vec[i] != NULL; i++)
        free(vec[i]);
    free(vec);
}

struct native_exec_pending {
    const struct native_program *prog;
    int argc;
    char **argv;
    char **envp;
};

static void native_pending_free(struct native_exec_pending *pending) {
    if (pending == NULL)
        return;
    native_free_vector(pending->argv);
    native_free_vector(pending->envp);
    free(pending);
}

void native_exec_discard_pending(struct task *task) {
    if (task == NULL || task->native_exec == NULL)
        return;
    native_pending_free(task->native_exec);
    task->native_exec = NULL;
}

int native_exec_set_pending(const struct native_program *prog, int argc,
        char *const argv[], char *const envp[]) {
    size_t envc = 0;
    while (envp != NULL && envp[envc] != NULL)
        envc++;

    struct native_exec_pending *pending = calloc(1, sizeof(*pending));
    char **argv_copy = native_dup_vector(argv, argc < 0 ? 0 : (size_t) argc);
    char **envp_copy = native_dup_vector(envp, envc);
    if (pending == NULL || argv_copy == NULL || envp_copy == NULL) {
        free(pending);
        native_free_vector(argv_copy);
        native_free_vector(envp_copy);
        return _ENOMEM;
    }

    pending->prog = prog;
    pending->argc = argc;
    pending->argv = argv_copy;
    pending->envp = envp_copy;

    // An exec over an exec would otherwise strand the earlier record.
    native_exec_discard_pending(current);
    current->native_exec = pending;
    return 0;
}

void native_exec_run_pending(void) {
    struct native_exec_pending *pending = current != NULL ? current->native_exec : NULL;
    if (pending == NULL)
        return;

    const struct native_program *prog = pending->prog;
    int argc = pending->argc;
    char **argv = pending->argv;
    char **envp = pending->envp;
    // Detached before running: the program must not see a stale record, and
    // task teardown must not double-free what is about to run.
    current->native_exec = NULL;
    free(pending);

    // comm is what /proc and ps report, and the guest should see the name it
    // actually invoked rather than the program backing it.
    const char *applet = native_applet_name(argc, argv);
    lock(&current->general_lock, 0);
    strncpy(current->comm, applet, sizeof(current->comm));
    current->comm[sizeof(current->comm) - 1] = '\0';
    unlock(&current->general_lock);

    // Before the program runs: getenv() in host code would otherwise answer
    // about the Mac (kernel/native.h).
    native_env_init(envp);
    // Published for the same reason and for exactly the call's lifetime:
    // argv is freed below, so a slot left pointing at it would dangle.
    current->native_argv = argv;
    current->native_argc = argc;

    // A fresh token for this run, so per-invocation state keyed on it (see the
    // third bullet in kernel/native.h) can never be mistaken for a previous
    // run's.
    nlibc_invocation_token_assign();

    int status = prog->main(argc, argv, envp);

    current->native_argv = NULL;
    current->native_argc = 0;
    native_free_vector(argv);
    native_free_vector(envp);

    // A fatal signal deferred while the program was inside host stdio (see
    // nlibc_stdio_defer_fatal) is still pending. The program has unwound and
    // its glue flushed its streams, so this is the safe point to take it:
    // the task then reports died-by-signal -- what ^C on a blocked native
    // reader should look like to its parent -- instead of whatever exit code
    // the error path it was detoured into produced.
    native_checkpoint();

    // Same encoding sys_exit_group uses: the wait status carries the exit code
    // in its high byte.
    do_exit_group((status & 0xff) << 8);
}

// -------------------------------------------------------------- environment
//
// See kernel/native.h. Kept here rather than in the libc shim because the shim
// is force-included with its own macros, and this has to call the real
// getenv-free libc; kernel/native_io.c's PATH search needs it too, and that
// file is not shim-included either.

static char **native_env_empty(void) {
    static char *empty[] = { NULL };
    return empty;
}

char **native_env_vector(void) {
    return *native_env_slot();
}

// The argv equivalents of native_env_slot. Same no-task fallback: a native
// program can be entered from a context with no current task, and answering
// with an empty vector is better than a crash in someone else's runtime.
char ***native_argv_slot(void) {
    static char **no_task_argv;
    static char *empty[] = { NULL };
    if (current == NULL || current->native_argv == NULL)
        return (no_task_argv = empty, &no_task_argv);
    return &current->native_argv;
}

int *native_argc_slot(void) {
    static int no_task_argc;
    if (current == NULL || current->native_argv == NULL)
        return (no_task_argc = 0, &no_task_argc);
    return &current->native_argc;
}

// The task's environ SLOT, not its value, so that `environ` can be assigned to
// and not merely read. SmallCLUE only ever read it and a function returning the
// vector was enough; bash writes it -- variables.c keeps the C-level environment
// in step with the shell's export list via `environ = export_env` -- and a call
// is not an lvalue, so that would not compile.
//
// With no task, or a task that has no environment yet, this hands back a slot
// pointing at a shared empty vector. A write through it is then per-PROCESS
// rather than per-task, which is wrong but unreachable: native code runs with a
// task, and native_env_init gives every one its own storage.
char ***native_env_slot(void) {
    static char **no_task_env;
    if (current == NULL)
        return (no_task_env = native_env_empty(), &no_task_env);
    if (current->native_env == NULL)
        current->native_env = native_dup_vector((char *const[]) { NULL }, 0);
    if (current->native_env == NULL)
        return (no_task_env = native_env_empty(), &no_task_env);
    return &current->native_env;
}

void native_env_init(char *const envp[]) {
    if (current == NULL)
        return;
    native_env_discard(current);
    size_t count = native_envp_count(envp);
    current->native_env = native_dup_vector(envp, count);
}

void native_env_discard(struct task *task) {
    if (task == NULL || task->native_env == NULL)
        return;
    native_free_vector(task->native_env);
    task->native_env = NULL;
}

// The signal table goes the same way and at the same time: it is the same kind
// of per-task native state, and a task that is going away has no dispositions.
void native_sigtable_discard(struct task *task) {
    if (task == NULL)
        return;
    void *t = task->native_sigtable;
    task->native_sigtable = NULL;
    free(t);
}

// The index of `name` in the vector, or -1. Matches on the whole name up to
// the '=', so PATH does not match PATH_TO_SOMETHING.
static ssize_t native_env_find(const char *name) {
    if (name == NULL || current == NULL || current->native_env == NULL)
        return -1;
    size_t len = strlen(name);
    for (size_t i = 0; current->native_env[i] != NULL; i++)
        if (strncmp(current->native_env[i], name, len) == 0 &&
                current->native_env[i][len] == '=')
            return (ssize_t) i;
    return -1;
}

const char *native_env_get(const char *name) {
    ssize_t at = native_env_find(name);
    if (at < 0)
        return NULL;
    return current->native_env[at] + strlen(name) + 1;
}

int native_env_set(const char *name, const char *value, bool overwrite) {
    if (current == NULL || name == NULL || value == NULL || name[0] == '\0' ||
            strchr(name, '=') != NULL)
        return _EINVAL;

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *entry = malloc(name_len + value_len + 2);
    if (entry == NULL)
        return _ENOMEM;
    snprintf(entry, name_len + value_len + 2, "%s=%s", name, value);

    ssize_t at = native_env_find(name);
    if (at >= 0) {
        if (!overwrite) {
            free(entry);
            return 0;
        }
        free(current->native_env[at]);
        current->native_env[at] = entry;
        return 0;
    }

    size_t count = native_envp_count(current->native_env);
    char **grown = realloc(current->native_env, (count + 2) * sizeof(*grown));
    if (grown == NULL) {
        free(entry);
        return _ENOMEM;
    }
    grown[count] = entry;
    grown[count + 1] = NULL;
    current->native_env = grown;
    return 0;
}

int native_env_unset(const char *name) {
    ssize_t at = native_env_find(name);
    if (at < 0)
        return 0;   // unsetenv succeeds on a name that was not set
    free(current->native_env[at]);
    size_t count = native_envp_count(current->native_env);
    memmove(&current->native_env[at], &current->native_env[at + 1],
            (count - (size_t) at) * sizeof(*current->native_env));
    return 0;
}

// ---------------------------------------------------------------- checkpoint

void native_checkpoint(void) {
    if (current == NULL)
        return;

    // Same cheap pre-check the syscall return path uses (kernel/calls.c): read
    // the pending/blocked sets locklessly and only take the slow path when
    // something is actually deliverable. This runs on every read and write a
    // native program makes, so the common case has to cost almost nothing.
    sigset_t_ pending = __atomic_load_n(&current->pending, __ATOMIC_ACQUIRE);
    sigset_t_ group_pending = __atomic_load_n(&current->sighand->pending, __ATOMIC_ACQUIRE);
    sigset_t_ blocked = __atomic_load_n(&current->blocked, __ATOMIC_ACQUIRE);
    bool has_saved_mask = __atomic_load_n(&current->has_saved_mask, __ATOMIC_ACQUIRE);

    // Inside a host stdio callback the FILE's lock is held by this thread, and
    // neither receive_signals (fatal default action exits without returning)
    // nor nlibc_deliver_signals (a handler may longjmp -- bash's SIGINT does)
    // may abandon it: Darwin never releases a dead owner's mutex, and one
    // orphaned stream lock wedges every later _fwalk in the process. Defer
    // both; the interrupted callback fails back through stdio's own unlock and
    // the signal is taken at the next checkpoint outside stdio. See the
    // callbacks in kernel/native_libc.c for the whole story.
    bool defer = nlibc_stdio_defer_fatal();

    if (has_saved_mask || ((pending | group_pending) & ~blocked) != 0) {
        // receive_signals runs the default action, which for SIGINT means
        // do_exit_group -- so this call may not return, and that is the point:
        // ^C on a native program has to end it the way it ends any other.
        if (!defer)
            receive_signals();
    }

    // ^Z. A stopped group parks its threads here until SIGCONT -- the SAME
    // function handle_interrupt uses for translated code (kernel/signal.c),
    // rather than a second copy of it. The copy this replaces claimed in its
    // comment to mirror handle_interrupt and did not: it had no ptrace handling
    // at all, so a traced native program that group-stopped never reported to
    // its tracer and the tracer's wait4 hung forever. Parking WHILE holding a
    // stdio lock is fine -- the owner is alive and will release it on SIGCONT.
    group_stop_wait();

    // Signals the program installed a handler for. Those are kept blocked in
    // the kernel -- it cannot jump host code -- so receive_signals above skips
    // them and the shim runs them here instead (kernel/native_libc.c).
    if (!defer)
        nlibc_deliver_signals();
}
