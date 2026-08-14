// Dispatch and registry for natively-implemented programs. See kernel/native.h
// for the execution model; this file is the table and the plumbing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"

#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/task.h"
#include "debug.h"

// Everything a native program prints has to go through iSH's fd layer, not the
// host's stdio -- see the libc warning in kernel/native.h.
static void native_write_str(fd_t fd_no, const char *s) {
    if (s == NULL)
        return;
    fd_write_host_buf(fd_no, s, strlen(s));
}

static void native_printf(fd_t fd_no, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if ((size_t) n >= sizeof(buf))
        n = sizeof(buf) - 1;
    fd_write_host_buf(fd_no, buf, (size_t) n);
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

// Placeholder standing in for the real SmallCLUE, which is not yet part of this
// build. It implements only applets that need no filesystem access at all, so
// it exercises the whole dispatch path -- resolved-path matching, argv[0]
// applet selection, env handoff, writes onto the guest's own fds, exit status
// -- without depending on the libc redirection seam that real applets need.
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
        native_write_str(1,
                "smallclue (native placeholder, built into iSH-AOK)\n"
                "\n"
                "Runs as host code -- not translated -- so it costs the same on every\n"
                "guest architecture. Select an applet the usual multicall way:\n"
                "  ln -s /AOK/native/smallclue /usr/local/bin/true\n"
                "\n"
                "Applets in this build: true, false, echo\n"
                "Self-test: smallclue --selftest\n");
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
