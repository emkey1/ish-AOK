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

static const struct native_program native_programs[] = {
    { "smallclue", smallclue_native_main },
};

const struct native_program *native_program_lookup(const char *name) {
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < sizeof(native_programs) / sizeof(native_programs[0]); i++)
        if (strcmp(native_programs[i].name, name) == 0)
            return &native_programs[i];
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

    int status = prog->main(argc, argv, envp);

    native_free_vector(argv);
    native_free_vector(envp);

    // Same encoding sys_exit_group uses: the wait status carries the exit code
    // in its high byte.
    do_exit_group((status & 0xff) << 8);
}
