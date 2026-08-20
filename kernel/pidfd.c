// pidfd support (issue #423): a pidfd is a stable, pollable file descriptor
// referencing a specific task, used by CLONE_PIDFD (kernel/fork.c),
// pidfd_open(2) and pidfd_send_signal(2).
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/poll.h"

struct pidfd_data {
    struct task *task; // pinned via task_ref_cnt_mod
    struct fd *fd;      // back-pointer, for poll_wakeup from pidfd_notify_exit
    struct list pidfd_link; // linked into task->pidfds, locked by pids_lock
};

static struct fd_ops pidfd_ops;

// Takes its own reference on `task` (the caller's own reference, if any, is
// unaffected and must still be released by the caller as usual).
struct fd *pidfd_create(struct task *task) {
    struct pidfd_data *data = malloc(sizeof(struct pidfd_data));
    if (data == NULL)
        return ERR_PTR(_ENOMEM);
    struct fd *fd = adhoc_fd_create(&pidfd_ops);
    if (fd == NULL) {
        free(data);
        return ERR_PTR(_ENOMEM);
    }
    task_ref_cnt_mod(task, 1);
    // Flag our ref as pidfd-held so do_exit's exit_wait_needed() can ignore
    // it -- see the pidfd_ref_count comment in kernel/task.h.
    atomic_fetch_add(&task->pidfd_ref_count, 1);
    data->task = task;
    data->fd = fd;
    fd->data = data;

    complex_lockt(&pids_lock, 0);
    list_add(&task->pidfds, &data->pidfd_link);
    unlock(&pids_lock);
    return fd;
}

static int pidfd_close(struct fd *fd) {
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    list_remove(&data->pidfd_link);
    unlock(&pids_lock);
    // Drop the pidfd-held flag before the ref itself: exit_wait_needed()
    // computes refs minus pidfd refs, and this order only ever makes that
    // difference transiently LARGER (costing at most one extra wait round),
    // never lets the gate run early.
    atomic_fetch_sub(&data->task->pidfd_ref_count, 1);
    task_ref_cnt_mod(data->task, -1);
    free(data);
    return 0;
}

static int pidfd_poll(struct fd *fd) {
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    int types = data->task->zombie ? POLL_READ : 0;
    unlock(&pids_lock);
    return types;
}

static struct fd_ops pidfd_ops = {
    .anon_inode_class = "[pidfd]",
    .poll = pidfd_poll,
    .close = pidfd_close,
};

// Called from do_exit (kernel/exit.c) with pids_lock already held, right
// after a task becomes a zombie: wakes any poll()/epoll_wait() blocked on a
// pidfd referencing it.
//
// Must be the TRYLOCK form, because pids_lock is held here. pidfd_poll()
// takes pids_lock, and poll_scan_ready_locked() calls it holding poll->lock,
// so the two paths acquire the same two locks in opposite orders:
//
//   epoll_wait scan:  poll->lock -> pids_lock   (poll_scan_ready_locked -> pidfd_poll)
//   task exiting:     pids_lock  -> poll->lock  (do_exit -> here -> poll_wakeup)
//
// which is an AB-BA deadlock, and exactly what poll.h's contract forbids:
// do not call poll_wakeup while holding a lock your poll operation acquires.
// This is the same bug the signalfd path had with sighand->lock, and has the
// same shape of fix.
//
// Best-effort is fine here for the same reason it was there: a pidfd's
// readiness is derived state (task->zombie), so the next scan or timeout
// recomputes it, and do_exit goes on to wake waiters through the ordinary
// wait/exit paths regardless. Dropping a poke costs at most one extra poll
// round; taking the lock costs the whole emulator.
void pidfd_notify_exit(struct task *task) {
    struct pidfd_data *data;
    list_for_each_entry(&task->pidfds, data, pidfd_link)
        poll_wakeup_trylock(data->fd, POLL_READ);
}

#define PIDFD_NONBLOCK_ O_NONBLOCK_

int_t sys_pidfd_open(pid_t_ pid, dword_t flags) {
    STRACE("pidfd_open(%d, %#x)", pid, flags);
    if (flags & ~PIDFD_NONBLOCK_)
        return _EINVAL;
    // A zombie is a valid target, and the interesting one: pidfd_open on a
    // child that has exited but not been reaped is how a process learns it
    // exited. Linux answers ESRCH only for a pid that does not exist, meaning
    // after reaping. pid_get_task_ref filters zombies out by design, so it is
    // the wrong accessor here -- using it made pidfd_open fail whenever the
    // child won the race to exit, which is exactly what pidfd_epoll_deadlock
    // provokes on purpose and why it failed about one run in three.
    //
    // Safe against the exit path: pidfd_create flags its reference in
    // pidfd_ref_count so do_exit's exit_wait_needed() ignores it, and
    // pidfd_poll already reports POLL_READ for a zombie. Both were written for
    // a pidfd whose task exits while it is open; a task that is already a
    // zombie is the same situation, further along.
    struct task *task = pid_get_task_zombie_ref(pid);
    if (task == NULL)
        return _ESRCH;
    struct fd *fd = pidfd_create(task);
    task_ref_cnt_mod(task, -1); // drop pid_get_task_ref's ref; pidfd_create took its own
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    // Linux unconditionally sets close-on-exec on every pidfd regardless of
    // the flags argument (only PIDFD_NONBLOCK is a valid input bit). Without
    // this, a self-pidfd opened just before exec (e.g. systemd's
    // exec-invoke helper watching its own PID) survives into the exec'd
    // program and keeps the extra reference alive -- do_exit's very first
    // step busy-waits for exactly that reference to drop before it can reach
    // the fd-table teardown that would close it, so the task can never
    // finish exiting and everything waiting on it (waitid up the parent
    // chain) wedges forever.
    return f_install(fd, O_CLOEXEC_ | ((flags & PIDFD_NONBLOCK_) ? O_NONBLOCK_ : 0));
}

// Resolve a pidfd fd number to its task's pid, for waitid(P_PIDFD, fd, ...)
// (how systemd >= 260 waits for every process it forks, e.g. generators).
int_t pidfd_get_pid(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    // A valid fd that isn't a pidfd is EBADF too (Linux pidfd_pid()).
    if (fd->ops != &pidfd_ops)
        return _EBADF;
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    pid_t_ pid = data->task->pid;
    unlock(&pids_lock);
    return pid;
}

// Linux's fdinfo for a pidfd (fs/proc/fd.c's pidfd_show_fdinfo) appends a
// "Pid:" line beyond the generic pos/flags/mnt_id fields. glibc's
// pidfd_get_pid() (systemd's posix_spawn_wrapper, used for every clone3
// service spawn) reads exactly this line as its fallback when the newer
// PIDFD_GET_INFO ioctl isn't supported -- if the line is absent it treats
// the fd as "not a pidfd" and returns ENOTTY, which is exactly what
// surfaced as "Failed to spawn executor: Inappropriate ioctl for device"
// for every clone3-spawned service before this fix. Returns -1 if `fd`
// isn't a pidfd.
pid_t_ fd_pidfd_pid(struct fd *fd) {
    if (fd->ops != &pidfd_ops)
        return -1;
    struct pidfd_data *data = fd->data;
    complex_lockt(&pids_lock, 0);
    pid_t_ pid = data->task->pid;
    unlock(&pids_lock);
    return pid;
}

// A self-referencing pidfd (pidfd_open(getpid())) that a task never gets
// around to closing before its OWN exit would otherwise deadlock every
// exit_wait_needed() gate in do_exit forever: the extra task reference it
// holds can only be dropped by closing this very fd, which normally only
// happens once fdtable_release runs in do_exit -- but that's itself gated
// behind those same checks. Real-world trigger: systemd >= 260's
// exec-invoke helper opens a self-pidfd, then (depending on the code path)
// exits without ever closing it. Called once, at the very top of do_exit,
// before any wait loop.
//
// Only touches fds when this task solely owns its fd table (refcount == 1):
// a CLONE_FILES-shared table might still be legitimately used by a live
// sibling thread, so leave those alone -- whichever thread exits last (and
// by then solely owns the table) will clean it up.
void pidfd_close_self_refs(struct task *task) {
    struct fdtable *files = task->files;
    if (files == NULL || files->refcount != 1)
        return;
    for (fd_t f = 0; (unsigned) f < files->size; f++) {
        struct fd *fd = files->files[f];
        if (fd != NULL && fd->ops == &pidfd_ops) {
            struct pidfd_data *data = fd->data;
            if (data->task == task)
                f_close(f);
        }
    }
}

int_t sys_pidfd_send_signal(fd_t pidfd, dword_t sig, addr_t UNUSED(info_addr), dword_t flags) {
    STRACE("pidfd_send_signal(%d, %d, flags=%#x)", pidfd, sig, flags);
    if (flags != 0)
        return _EINVAL;
    if (sig >= NUM_SIGS)
        return _EINVAL;
    struct fd *fd = f_get(pidfd);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &pidfd_ops)
        return _EINVAL;
    struct pidfd_data *data = fd->data;
    return signal_kill_task(data->task, sig, SI_USER_);
}
