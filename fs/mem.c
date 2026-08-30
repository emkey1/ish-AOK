#include <string.h>
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/random.h"
#include "fs/poll.h"
#include "fs/mem.h"
#include "fs/dev.h"
#include "fs/devices.h"

extern struct dev_ops
    null_dev,
    zero_dev,
    full_dev,
    random_dev,
    kmsg_dev;

// this file handles major device number MEM_MAJOR, minor device numbers are mapped in table below
struct dev_ops *mem_devs[256] = {
    // [1] = &prog_mem_dev,
    // [2] = &kmem_dev, // (not really applicable)
    [DEV_NULL_MINOR] = &null_dev,
    // [4] = &port_dev,
    [DEV_ZERO_MINOR] = &zero_dev,
    [DEV_FULL_MINOR] = &full_dev,
    [DEV_RANDOM_MINOR] = &random_dev,
    [DEV_URANDOM_MINOR] = &random_dev,
    // [10] = &aio_dev,
    [DEV_KMSG_MINOR] = &kmsg_dev,
    // [12] = &oldmem_dev, // replaced by /proc/vmcore
};

// dispatch device for major device 1
static int mem_open(int major, int minor, struct fd *fd) {
    struct dev_ops *dev = mem_devs[minor];
    if (dev == NULL) {
        return _ENXIO;
    }
    fd->ops = &dev->fd;
    if (!dev->open)
        return 0;
    return dev->open(major, minor, fd);
}

struct dev_ops mem_dev = {
    .open = mem_open,
};

static int ready_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
}

// begin inline devices
static int null_open(int UNUSED(major), int UNUSED(minor), struct fd *UNUSED(fd)) {
    return 0;
}
static ssize_t null_read(struct fd *UNUSED(fd), void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return 0;
}
static ssize_t null_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t bufsize) {
    return bufsize;
}
static off_t_ null_lseek(struct fd *UNUSED(fd), off_t_ UNUSED(off), int UNUSED(whence)) {
    return 0;
}
struct dev_ops null_dev = {
    .open = null_open,
    .fd.read = null_read,
    .fd.write = null_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
};

static ssize_t zero_read(struct fd *UNUSED(fd), void *buf, size_t bufsize) {
    memset(buf, 0, bufsize);
    return bufsize;
}
static ssize_t zero_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t bufsize) {
    return bufsize;
}
// mmap of /dev/zero is a plain zero-filled mapping -- exactly MAP_ANONYMOUS,
// which is what Linux does with it (mmap_zero in drivers/char/mem.c). Without
// an .mmap op the generic path returned ENODEV, so the oldest portable idiom
// for getting anonymous memory failed outright.
//
// Only /dev/zero. /dev/null and /dev/full have no mmap in Linux either and
// keep returning ENODEV; that was measured, not assumed.
static int zero_mmap(struct fd *UNUSED(fd), struct mem *mem, page_t start,
                     pages_t pages, off_t UNUSED(offset), int prot, int UNUSED(flags)) {
    // prot already carries P_SHARED when the caller asked for MAP_SHARED, so a
    // shared mapping of /dev/zero is shared anonymous memory, as on Linux.
    return pt_map_nothing(mem, start, pages, prot);
}

struct dev_ops zero_dev = {
    .open = null_open,
    .fd.read = zero_read,
    .fd.write = zero_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
    .fd.mmap = zero_mmap,
};

static ssize_t full_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _ENOSPC;
}
struct dev_ops full_dev = {
    .open = null_open,
    .fd.read = zero_read,
    .fd.write = full_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
};

static ssize_t random_read(struct fd *UNUSED(fd), void *buf, size_t bufsize) {
    get_random(buf, bufsize);
    return bufsize;
}

static ssize_t random_ioctl_size(int cmd) {
    switch (cmd) {
        case RNDGETENTCNT_: case RNDADDTOENTCNT_:
            return sizeof(dword_t);
        case RNDADDENTROPY_:
            // struct rand_pool_info header { int entropy_count; int buf_size; };
            // the variable-length entropy payload that follows is ignored.
            return 2 * sizeof(dword_t);
        case RNDZAPENTCNT_: case RNDCLEARPOOL_: case RNDRESEEDCRNG_:
            return 0;
    }
    return -1;
}

// iSH has no real entropy pool — randomness comes from the host CSPRNG, so the
// pool is always full and crediting/reseeding are no-ops. We only answer the
// "how much entropy is available" query so callers (seedrng's RNDADDENTROPY,
// rng-tools) succeed instead of getting ENOTTY and reporting a seeding error.
static int random_ioctl(struct fd *UNUSED(fd), int cmd, void *arg) {
    switch (cmd) {
        case RNDGETENTCNT_:
            *(dword_t *) arg = RANDOM_POOL_BITS;
            return 0;
        case RNDADDTOENTCNT_:
        case RNDADDENTROPY_:
        case RNDZAPENTCNT_:
        case RNDCLEARPOOL_:
        case RNDRESEEDCRNG_:
            return 0;
    }
    return _ENOTTY;
}

struct dev_ops random_dev = {
    .open = null_open,
    .fd.read = random_read,
    .fd.write = null_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
    .fd.ioctl_size = random_ioctl_size,
    .fd.ioctl = random_ioctl,
};

static ssize_t kmsg_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res = ish_log_read_bytes(fd->offset, buf, bufsize);
    if (res > 0)
        fd->offset += (unsigned long) res;
    return res;
}

static ssize_t kmsg_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _EPERM;
}

static off_t_ kmsg_lseek(struct fd *fd, off_t_ off, int whence) {
    size_t size = ish_log_size();
    off_t_ target;
    switch (whence) {
        case LSEEK_SET:
            target = off;
            break;
        case LSEEK_CUR:
            target = (off_t_) fd->offset + off;
            break;
        case LSEEK_END:
            target = (off_t_) size + off;
            break;
        default:
            return _EINVAL;
    }
    if (target < 0)
        return _EINVAL;
    fd->offset = (unsigned long) target;
    return target;
}

struct dev_ops kmsg_dev = {
    .open = null_open,
    .fd.read = kmsg_read,
    .fd.write = kmsg_write,
    .fd.lseek = kmsg_lseek,
    .fd.poll = ready_poll,
};
