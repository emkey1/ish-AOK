#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "kernel/anonfd_ckpt.h"

static struct fd_ops eventfd_ops;

#define EFD_SEMAPHORE_ 1

int_t sys_eventfd2(uint_t initval, int_t flags) {
    STRACE("eventfd(%d, %#x)", initval, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_|EFD_SEMAPHORE_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&eventfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    fd->eventfd.val = initval;
    fd->eventfd.semaphore = (flags & EFD_SEMAPHORE_) != 0;
    // EFD_SEMAPHORE is not an fd status flag; don't leak it into fd->flags.
    return f_install(fd, flags & ~EFD_SEMAPHORE_);
}
int_t sys_eventfd(uint_t initval) {
    return sys_eventfd2(initval, 0);
}

static ssize_t eventfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;

    lock(&fd->lock, 0);
    while (fd->eventfd.val == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        if (wait_for_blocked(&fd->cond, &fd->lock, NULL)) {
            unlock(&fd->lock);
            return _EINTR;
        }
    }

    // EFD_SEMAPHORE: each read returns 1 and decrements the counter by 1.
    // Default: the read returns the whole counter and resets it to 0.
    if (fd->eventfd.semaphore) {
        *(uint64_t *) buf = 1;
        fd->eventfd.val -= 1;
    } else {
        *(uint64_t *) buf = fd->eventfd.val;
        fd->eventfd.val = 0;
    }
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_WRITE);
    return sizeof(uint64_t);
}

static ssize_t eventfd_write(struct fd *fd, const void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;
    uint64_t increment = *(uint64_t *) buf;
    if (increment == UINT64_MAX)
        return _EINVAL;

    lock(&fd->lock, 0);
    while (fd->eventfd.val >= UINT64_MAX - increment) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        if (wait_for_blocked(&fd->cond, &fd->lock, NULL)) {
            unlock(&fd->lock);
            return _EINTR;
        }
    }

    fd->eventfd.val += increment;
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_READ);
    return sizeof(uint64_t);
}

static int eventfd_poll(struct fd *fd) {
    lock(&fd->lock, 0);
    int types = 0;
    if (fd->eventfd.val > 0)
        types |= POLL_READ;
    if (fd->eventfd.val < UINT64_MAX - 1)
        types |= POLL_WRITE;
    unlock(&fd->lock);
    return types;
}

static struct fd_ops eventfd_ops = {
    .name = "eventfd",
    .anon_inode_class = "eventfd",
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
};

// ---- checkpoint (kernel/anonfd_ckpt.h) ------------------------------------

bool eventfd_fd_is(struct fd *fd) {
    return fd != NULL && fd->ops == &eventfd_ops;
}

struct fd *eventfd_ckpt_new(uint64_t val, bool semaphore) {
    struct fd *fd = adhoc_fd_create(&eventfd_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    fd->eventfd.val = val;
    fd->eventfd.semaphore = semaphore;
    return fd;
}
