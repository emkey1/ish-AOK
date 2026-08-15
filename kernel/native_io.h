#ifndef KERNEL_NATIVE_IO_H
#define KERNEL_NATIVE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "kernel/fs.h"
#include "kernel/signal.h"   // sigset_t_, for the spawned child's mask below
#include "fs/fd.h"

// Guest-filesystem access for natively-implemented programs (kernel/native.h).
//
// This is the seam that makes native programs useful rather than merely fast.
// A native program is compiled into iSH-AOK and linked against the HOST libc,
// so calling open()/read()/opendir() directly would resolve against the iOS
// filesystem -- silently reading the wrong files rather than failing, which is
// the worst possible failure mode. Everything here goes through iSH's own VFS
// instead, resolving relative to the calling task's cwd and root, so a native
// program sees exactly the filesystem the guest sees.
//
// Every call returns 0 or a negative guest errno, except where noted.

// False when there is no AOK task on this thread -- true of a thread the
// native program created itself before kernel/native_libc.c's pthread_create
// wrapper propagates one. Callers that reach f_get or the VFS directly must
// check, or they dereference NULL and kill the app rather than the applet.
bool native_have_task(void);

// Opens a guest path. flags are guest O_* values. On success *fd_out is a
// reference the caller must release with native_close.
int native_open(const char *path, int flags, struct fd **fd_out);
void native_close(struct fd *fd);

// Bytes read, 0 at EOF, or a negative errno.
ssize_t native_read(struct fd *fd, void *buf, size_t size);

// Reads one directory entry. 1 on success, 0 at end of directory, negative
// errno on failure -- matching the fs_ops readdir convention.
int native_readdir(struct fd *fd, struct dir_entry *entry);

// stat/lstat against a guest path, resolved like native_open.
int native_stat(const char *path, struct statbuf *stat, bool follow_links);

// The calling task's cwd as a guest path, into a buffer of at least MAX_PATH.
int native_getcwd(char *buf);

// Writes a host buffer to one of the calling task's guest fds (1 and 2 being
// the interesting ones). Bytes written, or a negative errno.
ssize_t native_write(fd_t fd_no, const void *buf, size_t size);

// PATH search against the GUEST filesystem, for execvp-style callers.
// 0 and fills `out`, or a negative errno.
int native_path_search(const char *file, char *out, size_t outlen);

// Start a child running `path` as a real AOK task and return its pid. There
// is one host process, so a native program's child cannot be a host process.
int native_spawn(const char *path, char *const argv[], char *const envp[],
        dword_t *pid_out);

// What a fork+exec caller would do in the child between the two -- setpgid,
// then dup2, open and close in whatever order it wrote them. A native program
// has no such window, because there is no fork: the child is created, given its
// image and started, all from the parent. So it is described here instead and
// applied on the child's behalf.
//
// The actions are an ORDERED list rather than a tidier "stdio[3] plus fds to
// close", which is what this was at first. Order is not a detail: closing an
// fd that a later dup2 reads from, or closing one that an earlier dup2 just
// wrote to, are both silent breakage, and only the caller knows which it meant.
// It is also exactly posix_spawn's model, which is what lets the shim's
// posix_spawn be a thin translation rather than an interpretation.
//
// A zeroed struct is the useful default, hence the INHERIT sentinel being -1
// rather than 0: `pgid = 0` has to keep meaning what setpgid(0, 0) means.
#define NATIVE_SPAWN_PGID_INHERIT (-1)

enum native_spawn_action_kind {
    NATIVE_SPAWN_DUP2,    // fd <- from
    NATIVE_SPAWN_CLOSE,   // close fd
    NATIVE_SPAWN_OPEN,    // fd <- open(path, flags, mode)
};

struct native_spawn_action {
    enum native_spawn_action_kind kind;
    int fd;
    int from;             // DUP2
    const char *path;     // OPEN, a GUEST path
    int flags;            // OPEN, guest O_* values
    int mode;             // OPEN
};

struct native_spawn_opts {
    // The child's process group:
    //   NATIVE_SPAWN_PGID_INHERIT  keep the parent's
    //   0                          a group of its own, led by the child
    //   >0                         join this existing group, which must be in
    //                              the same session
    int pgid;
    const struct native_spawn_action *actions;
    size_t action_count;

    // The child's blocked-signal mask, when set_sigmask is true. GUEST bits.
    //
    // Needed because the shim tells the kernel a lie on the program's behalf:
    // a native program cannot hand the kernel a signal handler (it would jump
    // the guest CPU into host code), so every signal the program handles is
    // BLOCKED and drained at syscall checkpoints instead. That blocking is an
    // implementation detail of the shim, and a child must not inherit it --
    // exec preserves the mask, so `sleep 30` under a native bash ran with
    // SIGINT blocked and ^C did nothing at all.
    //
    // nlibc_posix_spawn_common fills this in for every spawn: from the
    // attribute when the caller set POSIX_SPAWN_SETSIGMASK, and otherwise from
    // the task's own mask with the shim-held signals taken back out. Which is
    // the same thing a forked child does when it calls restore_sigmask.
    bool set_sigmask;
    sigset_t_ sigmask;

    // Signals to put back to SIG_DFL in the child. GUEST bits.
    //
    // exec resets handlers but PRESERVES SIG_IGN, so a shell that ignores the
    // job-control signals for itself -- every interactive bash does -- hands
    // that ignoring to everything it runs. A forked child undoes it
    // (default_tty_job_signals, jobs.c); there is no child here to do it, so
    // ^Z did nothing at all: the foreground job ignored the SIGTSTP the
    // terminal sent it, and the shell went on waiting for a job that never
    // stopped.
    bool set_sigdefault;
    sigset_t_ sigdefault;
};

int native_spawn_opts(const char *path, char *const argv[], char *const envp[],
        const struct native_spawn_opts *opts, dword_t *pid_out);

// Wait for a child, blocking where the guest's own wait4 blocks. Returns the
// reaped pid, or a negative errno.
int native_waitpid(dword_t pid, int *status_out, int options);

// printf onto a guest fd. Output longer than the internal buffer is truncated,
// which is fine for the diagnostics this exists to carry.
void native_printf(fd_t fd_no, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
