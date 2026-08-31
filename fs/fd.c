#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/devices.h"
#include "fs/tty.h"

int fd_getflags(struct fd *fd);

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    fd->mount_flags = 0;
    fd->offset = 0;
    fd->stat.ctime = (dword_t)time(NULL);
    list_init(&fd->poll_fds);
    lock_init(&fd->poll_lock, "fd_create_poll\0");
    lock_init(&fd->lock, "fd_create\0");
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

struct fd *fd_retain_if_live(struct fd *fd) {
    unsigned expected = atomic_load(&fd->refcount);
    do {
        if (expected == 0)
            return NULL;
    } while (!atomic_compare_exchange_weak(&fd->refcount, &expected, expected + 1));
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        // IN_CLOSE_WRITE / IN_CLOSE_NOWRITE, on the last reference to this
        // open file description -- which is exactly what Linux reports on.
        // Emitted before ops->close, while the path can still be resolved,
        // and behind inotify_has_instances() because generic_getpath is a
        // SQLite lookup on fakefs and this runs on every close in the system.
        if (inotify_has_instances() && fd->mount != NULL &&
                fd->mount->fs != &procfs && !S_ISSOCK(fd->type)) {
            char path[MAX_PATH];
            if (generic_getpath(fd, path) == 0) {
                unsigned acc = fd_getflags(fd) & O_ACCMODE_;
                inotify_notify_close(path, acc == O_WRONLY_ || acc == O_RDWR_);
            }
        }
        poll_cleanup_fd(fd);
        if (fd->inode != NULL) {
            flock_remove_owned_by(fd);
            // Open file description locks are owned by this struct fd; release
            // them when the last reference to the open file description goes
            // away (process POSIX locks, owned by the fdtable, are dropped per
            // close() in fdtable_close).
            file_lock_remove_owned_by(fd, fd);
        }
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd);
//        fd = NULL; // KLUGE?
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    lock_init(&fdt->lock, "fdtable_new\0");
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

struct fdtable *fdtable_retain(struct fdtable *table) {
    table->refcount++;
    return table;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// Not the UAF it resembles in isolation: refcount is atomic, and every
// fdtable_retain() runs under the owning task's general_lock -- which do_exit
// also holds while it releases task->files and nulls the slot (kernel/exit.c).
// So a concurrent retain either precedes teardown (its +1 keeps this release
// from reaching 0) or sees task->files == NULL and skips. See the cross-task
// *_task_files_retain wrappers in fs/proc, fs/sock, and kernel/signal.
//
// The decrement must NOT be taken under table->lock, even though it used to
// be. Those cross-task wrappers exist to be callable from the signal-delivery
// path, which runs holding sighand->lock and often pids_lock, and they are
// carefully written to give up rather than block -- signalfd_wakeup_task()
// trylocks files->lock and bails out when it is busy. But the bail-out
// released the table, and a blocking lock() here meant it then waited for the
// very lock whose trylock had just failed, with pids_lock still in hand. That
// closed a three-lock cycle and wedged the whole emulator:
//
//   A exiting        pids_lock          -> waits for files->lock  (here)
//   B in close(2)    files->lock        -> waits for poll->lock   (epoll_close)
//   C in epoll_wait  poll->lock         -> waits for pids_lock    (pidfd_poll)
//
// Reproduced by tests/manual/pidfd_epoll_deadlock.c, which hammers exactly
// this interleaving; every thread that later wants pids_lock (any fork, any
// exit, any /proc/<pid> lookup) then queues behind A for ever.
//
// refcount is already atomic, so the lock was never what made the decrement
// safe. Take it only for the teardown, which by definition runs when no other
// reference -- and therefore no other lock holder -- is left.
void fdtable_release(struct fdtable *table) {
    if (atomic_fetch_sub(&table->refcount, 1) != 1)
        return;
    lock(&table->lock, 0);
    for (fd_t f = 0; (unsigned) f < table->size; f++) {
        fdtable_close(table, f);
    }
    free(table->files);
    free(table->cloexec);
    unlock(&table->lock);
    free(table);
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock, 0);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

int fdtable_unshare_current(void) {
    struct fdtable *table = current->files;
    if (table->refcount == 1)
        return 0;

    struct fdtable *new_table = fdtable_copy(table);
    if (IS_ERR(new_table))
        return (int) PTR_ERR(new_table);
    current->files = new_table;
    fdtable_release(table);
    return 0;
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= table->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock, 0);
    struct fd *fd = fdtable_get(current->files, f); 
    unlock(&current->files->lock);
    return fd;
}

struct fd *f_get_retain(fd_t f) {
    lock(&current->files->lock, 0);
    struct fd *fd = fdtable_get(current->files, f);
    if (fd != NULL)
        fd_retain(fd);
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start) {
    assert(start >= 0);
    struct fdtable *table = current->files;
    unsigned size = (unsigned)rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    lock(&current->files->lock, 0);
    fd_t f = f_install_start(fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock, 0);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock, 0);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec)) {
            fdtable_close(table, f);
        }
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_SETOWN_ 8
#define F_GETOWN_ 9
#define F_SETOWN_EX_ 15
#define F_GETOWN_EX_ 16

// struct f_owner_ex.type values (arch-independent; int fields).
#define F_OWNER_TID_ 0
#define F_OWNER_PID_ 1
#define F_OWNER_PGRP_ 2

// struct f_owner_ex { int type; pid_t pid; } -- identical 8-byte layout on
// every guest ABI (i386/amd64/arm64/riscv64 all use int for both fields).
struct f_owner_ex_ {
    int_t type;
    pid_t_ pid;
};

// F_SETOWN validation, shared by F_SETOWN and F_SETOWN_EX. Linux validates the
// target exists: who > 0 must be a live pid, who < 0 must be a live process
// group (kill_group uses the same pid_get() check for -pgid). who == 0 clears
// the owner and is always accepted. Returns 0 on success, _ESRCH otherwise.
static int fd_setown(struct fd *fd, pid_t_ who) {
    if (who == 0) {
        fd->owner = 0;
        return 0;
    }
    complex_lockt(&pids_lock, 0);
    bool target_exists = who > 0 ?
        pid_get_task_zombie(who) != NULL :
        pid_get(-who) != NULL;
    unlock(&pids_lock);
    if (!target_exists)
        return _ESRCH;
    fd->owner = who;
    return 0;
}

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

// Open file description locks. Same flock argument layout as the matching
// F_*LK / F_*LK64 commands on each ABI, but owned by the open file description
// rather than the process. glibc (LFS) and musl both route these through the
// 64-bit struct flock, so the i386 path reads a struct flock_ (like F_*LK64).
#define F_OFD_GETLK_ 36
#define F_OFD_SETLK_ 37
#define F_OFD_SETLKW_ 38

#define F_DUPFD_CLOEXEC_ 1030
#define F_ADD_SEALS_ 1033
#define F_GET_SEALS_ 1034
#define CLOSE_RANGE_UNSHARE_ (1U << 1)
#define CLOSE_RANGE_CLOEXEC_ (1U << 2)

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    fd->refcount++;
    fd_t new_f = f_install(fd, 0);
    return new_f;
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    if (flags & ~O_CLOEXEC_)
        return _EINVAL;
    if (new_f < 0)
        return _EBADF;
    if (f == new_f)
        return _EINVAL;
    struct fdtable *table = current->files;
    lock(&table->lock, 0);
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    int err = fdtable_expand(table, new_f);
    if (err < 0) {
        unlock(&table->lock);
        return err;
    }
    fd_retain(fd);
    if (table->files[new_f] != NULL)
        fdtable_close(table, new_f);
    table->files[new_f] = fd;
    bit_clear(new_f, table->cloexec);
    if (flags & O_CLOEXEC_)
        bit_set(new_f, table->cloexec);
    unlock(&table->lock);
    return new_f;
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    if (new_f < 0)
        return _EBADF;
    if (f == new_f) {
        struct fd *fd = f_get(f);
        if (fd == NULL)
            return _EBADF;
        return new_f;
    }
    return sys_dup3(f, new_f, 0);
}

dword_t sys_close_range(dword_t first, dword_t last, dword_t flags) {
    STRACE("close_range(%u, %u, %#x)", first, last, flags);
    if (flags & ~(CLOSE_RANGE_UNSHARE_ | CLOSE_RANGE_CLOEXEC_))
        return _EINVAL;
    if (last < first)
        return _EINVAL;

    if (flags & CLOSE_RANGE_UNSHARE_) {
        int err = fdtable_unshare_current();
        if (err < 0)
            return err;
    }

    struct fdtable *table = current->files;
    lock(&table->lock, 0);
    if (table->size == 0 || first >= table->size) {
        unlock(&table->lock);
        return 0;
    }

    unsigned end = last;
    if (end >= table->size)
        end = table->size - 1;

    for (fd_t f = (fd_t) first; (unsigned) f <= end; f++) {
        if (table->files[f] == NULL)
            continue;
        if (flags & CLOSE_RANGE_CLOEXEC_)
            bit_set(f, table->cloexec);
        else
            fdtable_close(table, f);
    }
    unlock(&table->lock);
    return 0;
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

static dword_t sys_fcntl_common(fd_t f, dword_t cmd, guest_addr_t arg, bool guest64_locks) {
    struct fdtable *table = current->files;
    struct fd *fd = f_get_retain(f);
    if (fd == NULL)
        return _EBADF;
    struct flock32_ flock32;
    struct flock_ flock;
    // struct flock_/flock32_ append an iSH-internal comm[16] after the guest-
    // visible fields; only the leading guest-ABI bytes (through l_pid) may cross
    // the user boundary. Copying sizeof would over-read and -- on F_GETLK
    // write-back -- overflow the guest's on-stack struct flock and smash its
    // return address (the i386 fcntl_lock/fcntl_ofd crash). The amd64 path uses
    // struct flock_amd64, which has no comm, so it was unaffected.
    const size_t flock_guest_size = offsetof(struct flock_, comm);
    const size_t flock32_guest_size = offsetof(struct flock32_, comm);
    fd_t new_f;
    dword_t ret;
    switch (cmd) {
        case F_DUPFD_:
            STRACE("fcntl(%d, F_DUPFD, %d)", f, arg);
            // Linux: arg < 0 or arg >= RLIMIT_NOFILE is EINVAL. arg is unsigned,
            // so a negative guest value is already a huge value caught here --
            // this also avoids tripping f_install_start's assert(start >= 0).
            if ((uint64_t) arg >= (uint64_t) rlimit(RLIMIT_NOFILE_)) {
                ret = _EINVAL;
                break;
            }
            fd->refcount++;
            new_f = f_install_start(fd, arg);
            ret = new_f;
            break;

        case F_DUPFD_CLOEXEC_:
            STRACE("fcntl(%d, F_DUPFD_CLOEXEC, %d)", f, arg);
            if ((uint64_t) arg >= (uint64_t) rlimit(RLIMIT_NOFILE_)) {
                ret = _EINVAL;
                break;
            }
            fd->refcount++;
            new_f = f_install_start(fd, arg);
            if (new_f >= 0)
                bit_set(new_f, table->cloexec);
            ret = new_f;
            break;

        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            ret = bit_test(f, table->cloexec);
            break;
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            ret = 0;
            break;

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            ret = fd_getflags(fd);
            break;
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            ret = fd_setflags(fd, arg);
            break;

        case F_GETOWN_:
            STRACE("fcntl(%d, F_GETOWN)", f);
            ret = (dword_t) fd->owner;
            break;
        case F_SETOWN_: {
            pid_t_ who = (pid_t_) arg;
            STRACE("fcntl(%d, F_SETOWN, %d)", f, who);
            ret = fd_setown(fd, who);
            break;
        }

        // glibc implements fcntl(fd, F_GETOWN) by issuing F_GETOWN_EX with a
        // pointer to struct f_owner_ex (to avoid the signed-pid ambiguity of
        // the plain return value), so a program calling F_GETOWN never reaches
        // the F_GETOWN_ case above -- F_GETOWN_EX must be handled or every
        // F_GETOWN returns EINVAL.
        case F_GETOWN_EX_: {
            STRACE("fcntl(%d, F_GETOWN_EX)", f);
            struct f_owner_ex_ owner_ex;
            pid_t_ who = fd->owner;
            if (who < 0) {
                owner_ex.type = F_OWNER_PGRP_;
                owner_ex.pid = -who;
            } else {
                // who == 0 (no owner) reports as F_OWNER_TID with pid 0, like
                // Linux; who > 0 is a pid owner.
                owner_ex.type = who > 0 ? F_OWNER_PID_ : F_OWNER_TID_;
                owner_ex.pid = who;
            }
            if (user_write(arg, &owner_ex, sizeof(owner_ex))) {
                ret = _EFAULT;
                break;
            }
            ret = 0;
            break;
        }
        case F_SETOWN_EX_: {
            STRACE("fcntl(%d, F_SETOWN_EX)", f);
            struct f_owner_ex_ owner_ex;
            if (user_read(arg, &owner_ex, sizeof(owner_ex))) {
                ret = _EFAULT;
                break;
            }
            pid_t_ who;
            switch (owner_ex.type) {
                case F_OWNER_TID_:
                case F_OWNER_PID_:
                    who = owner_ex.pid;
                    break;
                case F_OWNER_PGRP_:
                    who = -owner_ex.pid;
                    break;
                default:
                    ret = _EINVAL;
                    goto owner_ex_done;
            }
            ret = fd_setown(fd, who);
        owner_ex_done:
            break;
        }

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (guest64_locks) {
                // amd64 native struct flock (l_start at offset 8); translate
                // field-by-field -- it is NOT layout-compatible with flock_.
                struct flock_amd64 flock64;
                if (user_read(arg, &flock64, sizeof(flock64))) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock64.type;
                flock.whence = flock64.whence;
                flock.start = flock64.start;
                flock.len = flock64.len;
                flock.pid = flock64.pid;
                ret = fcntl_getlk(fd, &flock, false);
                // ret is unsigned (dword_t); fcntl_getlk returns a signed int
                // that is negative on error, so test the sign rather than
                // ret >= 0 (always true for an unsigned).
                if ((sdword_t) ret >= 0) {
                    flock64.type = flock.type;
                    flock64.whence = flock.whence;
                    flock64.start = flock.start;
                    flock64.len = flock.len;
                    flock64.pid = flock.pid;
                    if (user_write(arg, &flock64, sizeof(flock64)))
                        ret = _EFAULT;
                }
            } else {
                if (user_read(arg, &flock32, flock32_guest_size)) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock32.type;
                flock.whence = flock32.whence;
                flock.start = flock32.start;
                flock.len = flock32.len;
                flock.pid = flock32.pid;
                ret = fcntl_getlk(fd, &flock, false);
                if ((sdword_t) ret >= 0) {
                    flock32.type = flock.type;
                    flock32.whence = flock.whence;
                    flock32.start = (unsigned)flock.start;
                    flock32.len = (unsigned)flock.len;
                    flock32.pid = flock.pid;
                    if (user_write(arg, &flock32, flock32_guest_size))
                        ret = _EFAULT;
                }
            }
            break;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, flock_guest_size))
                ret = _EFAULT;
            else {
                ret = fcntl_getlk(fd, &flock, false);
                if ((sdword_t) ret >= 0)
                    if (user_write(arg, &flock, flock_guest_size))
                        ret = _EFAULT;
            }
            break;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (guest64_locks) {
                // amd64 native struct flock (l_start at offset 8); translate
                // field-by-field -- it is NOT layout-compatible with flock_.
                struct flock_amd64 flock64;
                if (user_read(arg, &flock64, sizeof(flock64))) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock64.type;
                flock.whence = flock64.whence;
                flock.start = flock64.start;
                flock.len = flock64.len;
                flock.pid = flock64.pid;
                ret = fcntl_setlk(fd, &flock, cmd == F_SETLKW_, false);
            } else {
                if (user_read(arg, &flock32, flock32_guest_size)) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock32.type;
                flock.whence = flock32.whence;
                flock.start = flock32.start;
                flock.len = flock32.len;
                flock.pid = flock32.pid;
                ret = fcntl_setlk(fd, &flock, cmd == F_SETLKW_, false);
            }
            break;

        case F_SETLK64_:
        case F_SETLKW64_:
            // NB: the blocking flag is cmd == F_SETLKW64_, not F_SETLKW_ -- this
            // is the 64-bit command (14), and musl on i386 always routes its
            // F_SETLKW through here (64-bit off_t). Comparing against F_SETLKW_
            // (7) made every F_SETLKW64 non-blocking, so blocking locks returned
            // EAGAIN instead of waiting.
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW64_, "W", arg);
            if (user_read(arg, &flock, flock_guest_size))
                ret = _EFAULT;
            else
                ret = fcntl_setlk(fd, &flock, cmd == F_SETLKW64_, false);
            break;

        case F_OFD_GETLK_:
            STRACE("fcntl(%d, F_OFD_GETLK, %#x)", f, arg);
            if (guest64_locks) {
                struct flock_amd64 flock64;
                if (user_read(arg, &flock64, sizeof(flock64))) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock64.type;
                flock.whence = flock64.whence;
                flock.start = flock64.start;
                flock.len = flock64.len;
                flock.pid = flock64.pid;
                ret = fcntl_getlk(fd, &flock, true);
                if ((sdword_t) ret >= 0) {
                    flock64.type = flock.type;
                    flock64.whence = flock.whence;
                    flock64.start = flock.start;
                    flock64.len = flock.len;
                    flock64.pid = flock.pid;
                    if (user_write(arg, &flock64, sizeof(flock64)))
                        ret = _EFAULT;
                }
            } else {
                // i386: OFD locks arrive via fcntl64 with the 64-bit struct flock.
                if (user_read(arg, &flock, flock_guest_size)) {
                    ret = _EFAULT;
                    break;
                }
                ret = fcntl_getlk(fd, &flock, true);
                if ((sdword_t) ret >= 0)
                    if (user_write(arg, &flock, flock_guest_size))
                        ret = _EFAULT;
            }
            break;

        case F_OFD_SETLK_:
        case F_OFD_SETLKW_:
            STRACE("fcntl(%d, F_OFD_SETLK%*s, %#x)", f, cmd == F_OFD_SETLKW_, "W", arg);
            if (guest64_locks) {
                struct flock_amd64 flock64;
                if (user_read(arg, &flock64, sizeof(flock64))) {
                    ret = _EFAULT;
                    break;
                }
                flock.type = flock64.type;
                flock.whence = flock64.whence;
                flock.start = flock64.start;
                flock.len = flock64.len;
                flock.pid = flock64.pid;
                ret = fcntl_setlk(fd, &flock, cmd == F_OFD_SETLKW_, true);
            } else {
                if (user_read(arg, &flock, flock_guest_size)) {
                    ret = _EFAULT;
                    break;
                }
                ret = fcntl_setlk(fd, &flock, cmd == F_OFD_SETLKW_, true);
            }
            break;

        case F_ADD_SEALS_:
            STRACE("fcntl(%d, F_ADD_SEALS, %#x)", f, arg);
            ret = memfd_add_seals(fd, arg);
            break;
        case F_GET_SEALS_:
            STRACE("fcntl(%d, F_GET_SEALS)", f);
            ret = memfd_get_seals(fd);
            break;

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            ret = _EINVAL;
            break;
    }
    fd_close(fd);
    return ret;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, dword_t arg) {
    return sys_fcntl_common(f, cmd, arg, false);
}

dword_t sys_fcntl_guest(fd_t f, dword_t cmd, guest_addr_t arg) {
    return sys_fcntl_common(f, cmd, arg, false);
}

dword_t sys_fcntl_amd64(fd_t f, dword_t cmd, dword_t arg) {
    return sys_fcntl_common(f, cmd, arg, true);
}

dword_t sys_fcntl_amd64_guest(fd_t f, dword_t cmd, guest_addr_t arg) {
    return sys_fcntl_common(f, cmd, arg, true);
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, dword_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
        // OFD locks use the 64-bit struct flock here; on i386 glibc/musl always
        // route them through fcntl64, never the legacy 32-bit fcntl syscall.
        case F_OFD_GETLK_:
        case F_OFD_SETLK_:
        case F_OFD_SETLKW_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}
