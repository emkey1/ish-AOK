#include <limits.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/inode.h"
#include "kernel/signal.h"
#include <string.h>

static bool file_locks_overlap(struct file_lock *a, struct file_lock *b) {
    return a->end >= b->start && b->end >= a->start;
}

static bool file_locks_conflict(struct file_lock *a, struct file_lock *b) {
    if (a->owner == b->owner)
        return false;
    if (!file_locks_overlap(a, b))
        return false;
    // write locks are incompatible with other types of locks
    if (a->type == F_WRLCK_ || b->type == F_WRLCK_)
        return true;
    return false;
}

static bool file_locks_adjacent(struct file_lock *a, struct file_lock *b) {
    return a->end == b->start - 1 || b->end == a->start - 1;
}

static bool flock_locks_conflict(struct file_lock *a, struct file_lock *b) {
    if (a->owner == b->owner)
        return false;
    if (a->type == F_WRLCK_ || b->type == F_WRLCK_)
        return true;
    return false;
}

static struct file_lock *file_lock_test(struct inode_data *inode, struct file_lock *request) {
    struct file_lock *lock;
    list_for_each_entry(&inode->posix_locks, lock, locks) {
        if (file_locks_conflict(lock, request))
            return lock;
    }
    return NULL;
}

static struct file_lock *file_lock_copy(struct file_lock *request) {
    struct file_lock *lock = malloc(sizeof(struct file_lock));
    lock->start = request->start;
    lock->end = request->end;
    lock->type = request->type;
    lock->owner = request->owner;
    lock->pid = request->pid;
    strncpy(lock->comm, request->comm, 16);
    list_init(&lock->locks);
    return lock;
}

static void file_lock_delete(struct file_lock *lock) {
    list_remove(&lock->locks);
    free(lock);
}

static int file_lock_acquire(struct inode_data *inode, struct file_lock *request) {
    struct file_lock *lock;

    if (request->type != F_UNLCK_) {
        list_for_each_entry(&inode->posix_locks, lock, locks) {
            if (file_locks_conflict(lock, request))
                return _EAGAIN;
            // TODO check for deadlocks
        }
    }

    // If the loop above succeeded, the lock can be placed. Now we just need to
    // add it into our existing set of locks. This is complicated because it
    // might need to:
    // - merge with an adjacent or overlapping lock of the same type
    // - override an existing overlapping lock of a different type
    // - split an existing lock into two, if it overlaps just the middle
    // - do any or all of the above at the same time

    bool found_our_locks = false;
    struct file_lock *tmp;
    list_for_each_entry_safe(&inode->posix_locks, lock, tmp, locks) {
        // To speed up looping over all of our locks, the locks are grouped by owner.
        if (!found_our_locks) {
            if (lock->owner != request->owner)
                continue;
            found_our_locks = true;
        } else {
            if (lock->owner != request->owner)
                break;
        }
        assert(lock->owner == request->owner);

        if (request->type == lock->type) {
            if (!file_locks_overlap(lock, request) && !file_locks_adjacent(request, lock))
                continue;
            // merge request with lock
            // extend request until it covers lock, then delete lock
            if (lock->start < request->start)
                request->start = lock->start;
            if (lock->end > request->end)
                request->end = lock->end;
            file_lock_delete(lock);
        } else {
            if (!file_locks_overlap(lock, request))
                continue;
            // request must subtract from the lock
            // the main thing to worry about here is if the lock is larger than
            // the request on both ends, in which case it needs to be split
            // into two locks
            //
            // test cases to think about: request on the top, lock on the bottom
            // ..::'' ''::.. ..::.. ''::'' :::... ...::: '''::: :::''' ::::::

            // every case here has the possibility of reducing the locking on some region
            notify(&inode->posix_unlock);

            if (request->start > lock->start && request->end < lock->end) {
                // lock sticks out on both ends, split
                struct file_lock *lock2 = file_lock_copy(lock);
                // see below for why these can't overflow
                lock->end = request->start - 1;
                lock2->start = request->end + 1;
                list_add_after(&lock->locks, &lock2->locks);
            } else if (request->start <= lock->start && request->end >= lock->end) {
                // lock doesn't stick out at all, so just remove it
                file_lock_delete(lock);
            } else if (lock->start < request->start) {
                // lock sticks out on the start, so move the end down
                assert(lock->end >= request->start);
                // subtract can't overflow since the comparision above would fail if request->start is 0
                lock->end = request->start - 1;
            } else if (lock->end > request->end) {
                // lock sticks out on the end, so move the start up
                assert(lock->start <= request->end);
                // add can't overflow since the comparision above would fail if request->start is OFF_T_MAX
                lock->start = request->end + 1;
            }
        }
    }

    if (request->type != F_UNLCK_) {
        struct file_lock *new_lock = file_lock_copy(request);
        list_add_before(&lock->locks, &new_lock->locks);
    }
    return 0;
}

#define OFF_T_MAX ~(1l << (sizeof(off_t) * 8 - 1))

static int file_lock_from_flock(struct fd *fd, struct flock_ *flock, struct file_lock *lock, bool ofd) {
    off_t_ offset;
    switch (flock->whence) {
        case LSEEK_SET:
            offset = 0;
            break;
        case LSEEK_CUR: {
            // Snapshot the fn ptr so the NULL-check and the call observe the
            // same value across the intervening mylock() (an opaque call that
            // forces a reload). Prevents the file_lock_from_flock NULL-call
            // crash seen on lockf()/SEEK_CUR locks.
            typeof(fd->ops->lseek) lseek_fn = fd->ops->lseek;
            if (lseek_fn == NULL)
                return _ESPIPE;
            mylock(&fd->lock, 0);
            offset = lseek_fn(fd, 0, LSEEK_CUR);
            unlock(&fd->lock);
            if (offset < 0)
                return (int)offset;
            break;
        }
        case LSEEK_END: {
            struct statbuf stat;
            if (fd->mount == NULL || fd->mount->fs == NULL || fd->mount->fs->fstat == NULL)
                return _ESPIPE;
            int err = fd->mount->fs->fstat(fd, &stat);
            if (err < 0)
                return err;
            offset = stat.size;
            break;
        }
        default:
            return _EINVAL;
    }

    lock->start = flock->start + offset;
    if (flock->len > 0) {
        lock->end = lock->start + flock->len - 1;
    } else if (flock->len < 0) {
        lock->end = lock->start - 1;
        lock->start = lock->end + flock->len + 1;
    } else {
        lock->end = OFF_T_MAX;
    }
    lock->type = flock->type;
    if (ofd) {
        // Open file description lock (F_OFD_*): owned by the open file
        // description (the struct fd), not the process. dup()/fork() share the
        // same struct fd, so the lock is shared across them -- matching OFD
        // semantics -- while a separate open() gets a distinct owner and thus
        // conflicts. The owner is a struct fd* here vs a struct fdtable* for
        // process POSIX locks, so the two never alias and correctly conflict
        // with each other even within one process. OFD locks are not owned by
        // any process, so l_pid is reported as -1 (per fcntl(2)).
        lock->owner = fd;
        lock->pid = -1;
    } else {
        lock->owner = current->files;
        lock->pid = current->pid;
    }
    strncpy(lock->comm, current->comm, 16);
    return 0;
}

static int flock_from_file_lock(struct file_lock *lock, struct flock_ *flock) {
    flock->type = lock->type;
    flock->whence = LSEEK_SET;
    flock->start = lock->start;
    if (lock->end != OFF_T_MAX)
        flock->len = lock->end - lock->start + 1;
    else
        flock->len = 0;
    flock->pid = lock->pid;
    strncpy(flock->comm, lock->comm, sizeof(flock->comm));
    return 0;
}

int fcntl_getlk(struct fd *fd, struct flock_ *flock, bool ofd) {
    if (flock->type != F_RDLCK_ && flock->type != F_WRLCK_)
        return _EINVAL;
    struct inode_data *inode = fd->inode;
    if (inode == NULL) {
        // fds without a backing inode (e.g. the pty fds synthesized for a
        // session's initial controlling terminal, see pty_open_fake) have
        // nowhere to store lock state. Real Linux permits fcntl locks on a
        // tty, so report "unlocked" rather than crash on inode->lock.
        flock->type = F_UNLCK_;
        return 0;
    }
    lock(&inode->lock, 0);

    struct file_lock request;
    int err = file_lock_from_flock(fd, flock, &request, ofd);
    if (err < 0)
        goto out;
    struct file_lock *lock = file_lock_test(inode, &request);
    err = 0;
    if (lock != NULL)
        err = flock_from_file_lock(lock, flock);
    else
        flock->type = F_UNLCK_;
out:
    unlock(&inode->lock);
    return err;
}

int fcntl_setlk(struct fd *fd, struct flock_ *flock, bool blocking, bool ofd) {
    if (flock->type != F_RDLCK_ && flock->type != F_WRLCK_ && flock->type != F_UNLCK_)
        return _EINVAL;
    int fd_mode = fd_getflags(fd) & O_ACCMODE_;
    if (flock->type == F_RDLCK_ && fd_mode == O_WRONLY_)
        return _EBADF;
    if (flock->type == F_WRLCK_ && fd_mode == O_RDONLY_)
        return _EBADF;

    struct inode_data *inode = fd->inode;
    if (inode == NULL) {
        // See the matching comment in fcntl_getlk: no inode means no place to
        // record the lock. Grant it unconditionally instead of crashing --
        // this fd is never shared with an unrelated open of the same file,
        // so there is no lock state to actually contend over.
        return 0;
    }
    lock(&inode->lock, 0);

    struct file_lock request;
    int err = file_lock_from_flock(fd, flock, &request, ofd);
    if (err < 0)
        goto out;
    TASK_MAY_BLOCK {
        while ((err = file_lock_acquire(inode, &request)) == _EAGAIN) {
            if (!blocking)
                break;
            err = wait_for(&inode->posix_unlock, &inode->lock, NULL);
            if (err < 0)
                break;
        }
    }
out:
    unlock(&inode->lock);
    // SA_RESTART: F_SETLKW is restartable (signal(7)). The non-blocking
    // commands never wait, so there is nothing here for this to convert.
    return signal_restart_or_eintr(err);
}

int flock_lock(struct fd *fd, int operation) {
    int op = operation & (LOCK_SH_ | LOCK_EX_ | LOCK_UN_);
    if (op != LOCK_SH_ && op != LOCK_EX_ && op != LOCK_UN_)
        return _EINVAL;
    if (fd->inode == NULL)
        return _EBADF;

    struct inode_data *inode = fd->inode;
    lock(&inode->lock, 0);

    struct file_lock *lock, *tmp;
    bool removed = false;
    list_for_each_entry_safe(&inode->flock_locks, lock, tmp, locks) {
        if (lock->owner != fd)
            continue;
        file_lock_delete(lock);
        removed = true;
    }
    if (removed)
        notify(&inode->flock_unlock);

    if (op == LOCK_UN_) {
        unlock(&inode->lock);
        return 0;
    }

    struct file_lock request = {
        .start = 0,
        .end = OFF_T_MAX,
        .type = op == LOCK_EX_ ? F_WRLCK_ : F_RDLCK_,
        .owner = fd,
        .pid = current->pid,
    };
    strncpy(request.comm, current->comm, sizeof(request.comm));

    TASK_MAY_BLOCK {
        while (true) {
            bool conflict = false;
            list_for_each_entry(&inode->flock_locks, lock, locks) {
                if (flock_locks_conflict(lock, &request)) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict)
                break;
            if (operation & LOCK_NB_) {
                unlock(&inode->lock);
                return _EAGAIN;
            }
            int err = wait_for(&inode->flock_unlock, &inode->lock, NULL);
            if (err < 0) {
                unlock(&inode->lock);
                return err;
            }
        }
    }

    struct file_lock *new_lock = file_lock_copy(&request);
    list_add_tail(&inode->flock_locks, &new_lock->locks);
    unlock(&inode->lock);
    return 0;
}

void file_lock_remove_owned_by(struct fd *fd, void *owner) {
    struct inode_data *inode = fd->inode;
    lock(&inode->lock, 0);
    struct file_lock *lock, *tmp;
    bool removed = false;
    list_for_each_entry_safe(&inode->posix_locks, lock, tmp, locks) {
        if (lock->owner == owner) {
            file_lock_delete(lock);
            removed = true;
        }
    }
    // Wake any F_SETLKW waiter blocked on inode->posix_unlock: releasing a
    // lock by closing the fd / exiting the process must unblock contenders,
    // just as the explicit F_UNLCK path (file_lock_acquire) does. Without
    // this, a waiter could sleep forever on a lock that is already gone.
    if (removed)
        notify(&inode->posix_unlock);
    unlock(&inode->lock);
}

void flock_remove_owned_by(struct fd *fd) {
    struct inode_data *inode = fd->inode;
    lock(&inode->lock, 0);
    struct file_lock *lock, *tmp;
    bool removed = false;
    list_for_each_entry_safe(&inode->flock_locks, lock, tmp, locks) {
        if (lock->owner != fd)
            continue;
        file_lock_delete(lock);
        removed = true;
    }
    if (removed)
        notify(&inode->flock_unlock);
    unlock(&inode->lock);
}
