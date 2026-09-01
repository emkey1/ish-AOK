#include <stdbool.h>
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

// Open /dev/kmsg fds. A log line arrives from anywhere in the emulator and
// has no idea who is watching, so the watchers are kept here -- the same shape
// as fs/proc.c's mountinfo watch list, for the same reason.
static struct list kmsg_fds = LIST_INITIALIZER(kmsg_fds);
static lock_t kmsg_fds_lock = LOCK_INITIALIZER;

static int kmsg_open(int UNUSED(major), int UNUSED(minor), struct fd *fd) {
    // Start at the oldest line still buffered rather than at 0: a fresh open
    // of /dev/kmsg reads the buffer from its start on Linux too, and
    // ish_log_read_at clamps a position that has fallen off the back anyway.
    fd->offset = 0;
    lock(&kmsg_fds_lock, 0);
    list_add(&kmsg_fds, &fd->kmsg.link);
    unlock(&kmsg_fds_lock);
    return 0;
}

static int kmsg_close(struct fd *fd) {
    lock(&kmsg_fds_lock, 0);
    if (!list_null(&fd->kmsg.link))
        list_remove(&fd->kmsg.link);
    unlock(&kmsg_fds_lock);
    return 0;
}

void kmsg_notify_readers(void) {
    // ish_vprintk calls this with log_lock released, so nothing here can be
    // waiting on it. poll_wakeup's FIXME path logs, though, which would come
    // straight back in and try to take a poll_lock this thread already holds.
    static __thread bool notifying = false;
    if (notifying)
        return;
    notifying = true;
    lock(&kmsg_fds_lock, 0);
    struct fd *fd;
    list_for_each_entry(&kmsg_fds, fd, kmsg.link)
        poll_wakeup(fd, POLL_READ);
    unlock(&kmsg_fds_lock);
    notifying = false;
}

// A stream of the kernel log, shared with /proc/kmsg (fs/proc/root.c).
// Positions are absolute -- see ish_log_read_at.
ssize_t kmsg_stream_read(unsigned long *pos, void *buf, size_t bufsize, bool nonblock) {
    // A zero-length read returns 0 at once. POSIX says so, every Linux driver
    // implements it, and here it is load-bearing rather than pedantic: the
    // loop below cannot terminate without it. ish_log_read_at can only ever
    // copy zero bytes for a zero-length buffer, which this loop reads as
    // "nothing new", so it waits -- and wakes immediately, because there IS
    // something new -- and asks again, forever.
    //
    // rsyslogd probes /proc/kmsg with exactly this call at startup, and the
    // spin sits inside kernel code with no syscall boundary in it, so no
    // guest signal can land and the task cannot even be killed. Boot stopped
    // there: rsyslogd never answered, and init gave up on it sixty seconds
    // later having pinned a CPU the whole time.
    if (bufsize == 0)
        return 0;

    for (;;) {
        uint64_t at = *pos;
        uint64_t started_at = at;
        ssize_t res = ish_log_read_at(&at, buf, bufsize);
        if (res != 0) {
            if (res > 0) {
                // Linux hands back exactly one record per read from
                // /dev/kmsg. This is a byte stream, so a reader whose buffer
                // did not land on a record boundary got a partial line and
                // printed it as one -- busybox's klogd logged the tail of a
                // timestamp as its own syslog entry. Stop at the first
                // newline so a record is never split across two reads.
                const char *nl = memchr(buf, '\n', (size_t) res);
                if (nl != NULL) {
                    size_t upto = (size_t) (nl - (const char *) buf) + 1;
                    if (upto < (size_t) res) {
                        res = (ssize_t) upto;
                        at = started_at + upto;
                    }
                }
                *pos = (unsigned long) at;
            }
            return res;
        }
        // Nothing new. Linux blocks here, and a log daemon's entire main loop
        // is this read: answering 0 turned that loop into a spin, which is
        // why the device node was never created in the first place.
        if (nonblock)
            return _EAGAIN;
        int err = ish_log_wait_past(*pos);
        if (err < 0)
            return err;
    }
}

int kmsg_stream_poll(unsigned long pos) {
    return ish_log_total_written() > pos ? POLL_READ : 0;
}

static ssize_t kmsg_read(struct fd *fd, void *buf, size_t bufsize) {
    // /dev/kmsg hands back one whole RECORD per read, so a buffer too small to
    // hold one is a bad argument rather than an empty answer -- Linux answers
    // EINVAL, and a zero-length buffer can never hold a record. Measured on
    // Devuan, where the same read of /proc/kmsg returns 0 instead: /proc/kmsg
    // is a byte stream and has nothing to complain about. The two share an
    // implementation here, so the distinction lives at this end of it.
    if (bufsize == 0)
        return _EINVAL;
    return kmsg_stream_read(&fd->offset, buf, bufsize,
                            (fd->flags & O_NONBLOCK_) != 0);
}

static int kmsg_poll(struct fd *fd) {
    return kmsg_stream_poll(fd->offset);
}

// Linux injects a write into the ring buffer, which is how `echo x >
// /dev/kmsg` and `logger --kernel` put a line in dmesg -- boot scripts and
// initramfs hooks use it to say where they got to. The node's 0644 keeps it
// to root, as there.
//
// A leading "<N>" is the syslog priority/facility, which Linux strips from
// the stored text. There is nowhere to route the level here, so honour the
// syntax -- a line beginning "<6>" must not appear with the marker still on
// it -- and drop the value.
#define KMSG_WRITE_MAX 4096
static ssize_t kmsg_write(struct fd *UNUSED(fd), const void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;
    const char *msg = buf;
    size_t len = bufsize > KMSG_WRITE_MAX ? KMSG_WRITE_MAX : bufsize;
    size_t skip = 0;
    if (len > 2 && msg[0] == '<') {
        size_t i = 1;
        while (i < len && msg[i] >= '0' && msg[i] <= '9')
            i++;
        if (i > 1 && i < len && msg[i] == '>')
            skip = i + 1;
    }
    size_t n = len - skip;
    // printk stores one line per call; a trailing newline of our own would
    // leave a blank line between every injected message.
    while (n > 0 && msg[skip + n - 1] == '\n')
        n--;
    if (n > 0)
        // Never as the format string itself: the text is the guest's.
        ish_printk("%.*s\n", (int) n, msg + skip);
    // Linux reports the whole write consumed even where it truncated.
    return (ssize_t) bufsize;
}

static off_t_ kmsg_lseek(struct fd *fd, off_t_ off, int whence) {
    // Offsets the guest names are relative to the oldest line still buffered,
    // which is what Linux's SEEK_SET on /dev/kmsg means; the position we keep
    // is absolute so a wrap cannot strand it.
    uint64_t total = ish_log_total_written();
    size_t size = ish_log_size();
    uint64_t oldest = total - size;
    off_t_ target;
    switch (whence) {
        case LSEEK_SET:
            target = off;
            break;
        case LSEEK_CUR:
            target = (off_t_) ((uint64_t) fd->offset > oldest
                               ? (uint64_t) fd->offset - oldest : 0) + off;
            break;
        case LSEEK_END:
            target = (off_t_) size + off;
            break;
        default:
            return _EINVAL;
    }
    if (target < 0)
        return _EINVAL;
    fd->offset = (unsigned long) (oldest + (uint64_t) target);
    return target;
}

struct dev_ops kmsg_dev = {
    .open = kmsg_open,
    .fd.read = kmsg_read,
    .fd.write = kmsg_write,
    .fd.lseek = kmsg_lseek,
    .fd.poll = kmsg_poll,
    .fd.close = kmsg_close,
};
