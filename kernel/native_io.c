// Guest-filesystem access for natively-implemented programs. See
// kernel/native_io.h for why native code must not use the host libc for any of
// this.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"

// Every entry point checks for a task first. A native program can be on a
// thread AOK never created (one the program spawned itself), where `current`
// is NULL -- and f_get/generic_openat dereference it. That took the whole app
// down with EXC_BAD_ACCESS when SmallCLUE ran its editor on its own pthread.
// kernel/native_libc.c now propagates the task into such threads, so this
// should not trigger; it is here because the failure mode is a dead app rather
// than a failed call, and that is not a thing to leave to one mechanism.
bool native_have_task(void) {
    return current != NULL && current->files != NULL;
}

int native_open(const char *path, int flags, struct fd **fd_out) {
    if (!native_have_task())
        return _EFAULT;
    if (path == NULL || fd_out == NULL)
        return _EINVAL;
    // AT_PWD, not an absolute-only open: relative paths have to resolve
    // against the calling task's cwd, exactly as they would for the guest.
    struct fd *fd = generic_openat(AT_PWD, path, flags, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    *fd_out = fd;
    return 0;
}

void native_close(struct fd *fd) {
    if (fd != NULL)
        fd_close(fd);
}

ssize_t native_read(struct fd *fd, void *buf, size_t size) {
    if (!native_have_task())
        return _EFAULT;

    if (fd == NULL || buf == NULL)
        return _EINVAL;
    if (S_ISDIR(fd->type))
        return _EISDIR;

    ssize_t res;
    // Same blocking discipline the read(2) path uses: a native program runs on
    // the guest task's own thread, so a blocking read has to be visible to the
    // scheduler as that task blocking.
    TASK_MAY_BLOCK {
        if (fd->ops->read != NULL) {
            res = fd->ops->read(fd, buf, size);
        } else if (fd->ops->pread != NULL) {
            res = fd->ops->pread(fd, buf, size, fd->offset);
            if (res > 0)
                fd->ops->lseek(fd, res, LSEEK_CUR);
        } else {
            res = _EBADF;
        }
    }
    return res;
}

int native_readdir(struct fd *fd, struct dir_entry *entry) {
    if (fd == NULL || entry == NULL)
        return _EINVAL;
    if (fd->ops->readdir == NULL)
        return _ENOTDIR;
    return fd->ops->readdir(fd, entry);
}

int native_stat(const char *path, struct statbuf *stat, bool follow_links) {
    if (!native_have_task())
        return _EFAULT;

    if (path == NULL || stat == NULL)
        return _EINVAL;
    return generic_statat(AT_PWD, path, stat,
            follow_links ? 0 : AT_SYMLINK_NOFOLLOW_);
}

int native_getcwd(char *buf) {
    if (!native_have_task())
        return _EFAULT;

    if (buf == NULL)
        return _EINVAL;
    lock(&current->fs->lock, 0);
    struct fd *pwd = current->fs->pwd;
    int err = pwd != NULL ? generic_getpath(pwd, buf) : _ENOENT;
    unlock(&current->fs->lock);
    return err;
}

ssize_t native_write(fd_t fd_no, const void *buf, size_t size) {
    if (!native_have_task())
        return _EFAULT;

    return fd_write_host_buf(fd_no, buf, size);
}

void native_printf(fd_t fd_no, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if ((size_t) n >= sizeof(buf))
        n = (int) sizeof(buf) - 1;
    native_write(fd_no, buf, (size_t) n);
}

// execvp semantics: walk PATH from the guest's environment and test each
// candidate through the VFS, not the host's -- searching the host's PATH would
// find the Mac's binaries. Shared by the libc shim and SmallCLUE's spawn hook
// so there is one copy of the rule.
int native_path_search(const char *file, char *out, size_t outlen) {
    if (file == NULL || out == NULL)
        return _EINVAL;
    // The GUEST's PATH. getenv() here reads the host process's environment,
    // so this walked the Mac's directories -- it only ever found anything
    // because each candidate is then tested through the VFS.
    const char *path = native_env_get("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin";
    while (*path != '\0') {
        const char *sep = strchr(path, ':');
        size_t len = sep != NULL ? (size_t) (sep - path) : strlen(path);
        const char *dir = len == 0 ? "." : path;
        if (len == 0)
            len = 1;
        int n = snprintf(out, outlen, "%.*s/%s", (int) len, dir, file);
        if (n > 0 && (size_t) n < outlen) {
            struct statbuf sb = {};
            if (native_stat(out, &sb, true) == 0 && (sb.mode & 0111) != 0)
                return 0;
        }
        if (sep == NULL)
            break;
        path = sep + 1;
    }
    return _ENOENT;
}

// ------------------------------------------------------------------- spawn
//
// Starting a child from a native program. There is one host process, so the
// child cannot be one: it becomes a real AOK task, exactly as a guest's own
// fork+exec would produce. kernel/init.c's boot-command launcher is the same
// shape -- impersonate the child, exec, then hand it its own thread.
//
// This is the piece exsh cannot do without; SmallCLUE's env/xargs/timeout want
// it too.

// do_execve wants its arguments packed as "a\0b\0c\0" with ONE MORE trailing
// NUL after the last -- args_size() walks `count` strings and then asserts the
// next byte is '\0' (kernel/exec.c). Getting that terminator wrong trips an
// assert rather than returning an error.
static char *native_pack_args(char *const vec[], size_t *count_out) {
    size_t count = 0, bytes = 1;   // the extra terminator
    if (vec != NULL)
        for (; vec[count] != NULL; count++)
            bytes += strlen(vec[count]) + 1;
    char *packed = malloc(bytes);
    if (packed == NULL)
        return NULL;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(vec[i]) + 1;
        memcpy(packed + off, vec[i], n);
        off += n;
    }
    packed[off] = '\0';
    *count_out = count;
    return packed;
}

// Everything a forked child would do to itself before exec. Runs while the
// caller is impersonating the child, so these are the guest's own syscalls
// acting on the guest's own state -- no second implementation of setpgid's
// session rules or of dup2's cloexec handling to keep in step with the first.
//
// Before exec, not after, for the one case where the order is observable:
// do_execve closes cloexec descriptors, and dup2 clears the cloexec bit on
// what it writes to. Doing this first therefore keeps a redirection whose
// source was cloexec (and closes that source, which is what fork+exec does
// too); doing it afterwards would find the descriptor already gone.
static int native_spawn_setup_child(const struct native_spawn_opts *opts) {
    if (opts == NULL)
        return 0;

    // The mask first: it is what a forked child restores before it does
    // anything else, and nothing below depends on the child's own signals.
    if (opts->set_sigmask)
        sigmask_set_blocked(opts->sigmask);

    // Dispositions next, straight into the sighand rather than through
    // sys_rt_sigaction: this is the guest's own state and there is no guest
    // memory to marshal a struct through.
    if (opts->set_sigdefault) {
        lock(&current->sighand->lock, 0);
        for (int sig = 1; sig < NUM_SIGS; sig++)
            if (opts->sigdefault & ((sigset_t_) 1 << (sig - 1)))
                current->sighand->action[sig].handler = SIG_DFL_;
        unlock(&current->sighand->lock);
    }

    if (opts->pgid != NATIVE_SPAWN_PGID_INHERIT) {
        // setpgid on a task that has not exec'd, which is precisely the case
        // the guest's own rules allow -- and why this happens here rather than
        // in the parent afterwards, where sys_setpgid refuses with EACCES once
        // the child has exec'd.
        int err = (int) sys_setpgid(0, opts->pgid);
        if (err < 0)
            return err;
    }

    // In the caller's order, exactly as a forked child would have run them.
    for (size_t i = 0; i < opts->action_count; i++) {
        const struct native_spawn_action *a = &opts->actions[i];
        int err = 0;
        switch (a->kind) {
            case NATIVE_SPAWN_DUP2:
                // sys_dup2 already gets dup2(fd, fd) right -- it checks the
                // descriptor is open and returns it without clearing cloexec,
                // where dup3 would reject it with EINVAL.
                err = (int) sys_dup2(a->from, a->fd);
                break;
            case NATIVE_SPAWN_CLOSE:
                // A descriptor already gone is not an error: closing what the
                // caller does not want kept is idempotent by intent.
                (void) sys_close(a->fd);
                break;
            case NATIVE_SPAWN_OPEN: {
                struct fd *fd = generic_openat(AT_PWD, a->path, a->flags, a->mode);
                if (IS_ERR(fd)) {
                    err = (int) PTR_ERR(fd);
                    break;
                }
                fd_t got = f_install(fd, 0);
                if (got < 0) {
                    err = got;
                    break;
                }
                if (got != a->fd) {
                    err = (int) sys_dup2(got, a->fd);
                    sys_close(got);
                }
                break;
            }
        }
        if (err < 0)
            return err;
    }
    return 0;
}

int native_spawn(const char *path, char *const argv[], char *const envp[],
        dword_t *pid_out) {
    return native_spawn_opts(path, argv, envp, NULL, pid_out);
}

int native_spawn_opts(const char *path, char *const argv[], char *const envp[],
        const struct native_spawn_opts *opts, dword_t *pid_out) {
    if (!native_have_task())
        return _EFAULT;
    if (path == NULL || argv == NULL)
        return _EINVAL;

    size_t argc = 0, envc = 0;
    char *argv_packed = native_pack_args(argv, &argc);
    char *envp_packed = native_pack_args(envp, &envc);
    if (argv_packed == NULL || envp_packed == NULL) {
        free(argv_packed);
        free(envp_packed);
        return _ENOMEM;
    }

    struct task *saved = current;
    struct task *child = task_fork_for_exec();
    if (child == NULL) {
        free(argv_packed);
        free(envp_packed);
        return _ENOMEM;
    }

    // do_execve acts on `current`, so impersonate the child across it and put
    // it back immediately after -- the same trick kernel/init.c uses. The
    // pre-exec setup runs inside the same impersonation for the same reason.
    current = child;
    int err = native_spawn_setup_child(opts);
    if (err >= 0)
        err = do_execve(path, argc, argv_packed, envp_packed);
    current = saved;

    free(argv_packed);
    free(envp_packed);

    if (err < 0) {
        // The child never ran, so its fds and mm are released by this rather
        // than by an exit.
        task_never_ran_destroy(child);
        return err;
    }

    dword_t pid = child->pid;
    if (task_start(child) < 0) {
        task_never_ran_destroy(child);
        return _EAGAIN;
    }
    if (pid_out != NULL)
        *pid_out = pid;
    return 0;
}

int native_waitpid(dword_t pid, int *status_out, int options) {
    if (!native_have_task())
        return _EFAULT;
    return task_wait_child(pid, status_out, options);
}
