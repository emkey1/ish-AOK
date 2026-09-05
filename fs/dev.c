#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/mem.h"
#include "fs/tty.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "app/RTCDevice.h"
#include "kernel/swap.h"
#include "fs/poll.h"

// ---- /dev/aokswap0 -------------------------------------------------------
//
// The simulated swap area, as a block device. It exists so /proc/swaps can name
// something real: Linux prints a path there for every area, AOK's backing file
// is created and unlinked immediately (kernel/swap.c, so it can never be
// visible in the container or backed up), and a row naming a path the guest
// cannot stat would be a worse lie than the empty file it replaces.
//
// Everything about it answers from swap_area_bytes(), the same figure
// /proc/meminfo's SwapTotal comes from, so the capacity and the swap totals
// cannot disagree.
//
// With swap off the area is zero bytes and this behaves as an UNBOUND Linux
// block device does -- open succeeds, read returns 0 (EOF, not ENXIO), writes
// fail with ENOSPC -- which is a real state Linux has (an idle /dev/loopN,
// verified) rather than one invented for the occasion.
// A block device is always ready in both directions; fs/mem.c has an identical
// helper but keeps it static.
static int aokswap_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
}

static int aokswap_open(int UNUSED(major), int UNUSED(minor), struct fd *fd) {
    fd->offset = 0;
    return 0;
}

static ssize_t aokswap_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t n = swap_area_pread(buf, bufsize, (off_t) fd->offset);
    if (n > 0)
        fd->offset += n;
    return n;
}

static ssize_t aokswap_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t n = swap_area_pwrite(buf, bufsize, (off_t) fd->offset);
    if (n > 0)
        fd->offset += n;
    return n;
}

// SEEK_END is against the CURRENT area size, so it follows the user resizing
// swap in Settings, exactly as a loop device follows losetup.
static off_t_ aokswap_lseek(struct fd *fd, off_t_ off, int whence) {
    off_t_ base;
    switch (whence) {
        case LSEEK_SET: base = 0; break;
        case LSEEK_CUR: base = fd->offset; break;
        case LSEEK_END: base = (off_t_) swap_area_bytes(); break;
        default: return _EINVAL;
    }
    off_t_ target = base + off;
    if (target < 0)
        return _EINVAL;
    fd->offset = target;
    return target;
}

struct dev_ops aokswap_dev = {
    .open = aokswap_open,
    .fd.read = aokswap_read,
    .fd.write = aokswap_write,
    .fd.lseek = aokswap_lseek,
    .fd.poll = aokswap_poll,
};

struct dev_ops *block_devs[256] = {
    [AOKSWAP_MAJOR] = &aokswap_dev,
};
struct dev_ops *char_devs[256] = {
    [MEM_MAJOR] = &mem_dev,
    [MISC_MAJOR] = &fuse_dev,
    [TTY_CONSOLE_MAJOR] = &tty_dev,
    [TTY_ALTERNATE_MAJOR] = &tty_dev,
    [TTY_PSEUDO_MASTER_MAJOR] = &tty_dev,
    [TTY_PSEUDO_SLAVE_MAJOR] = &tty_dev,
    [DEV_RTC_MAJOR] = &rtc_dev,
    [DYN_DEV_MAJOR] = &dyn_dev_char,
};

const struct dev_node_spec dev_standard_nodes[] = {
    {"null",    0666, MEM_MAJOR, DEV_NULL_MINOR},
    {"zero",    0666, MEM_MAJOR, DEV_ZERO_MINOR},
    {"full",    0666, MEM_MAJOR, DEV_FULL_MINOR},
    {"random",  0666, MEM_MAJOR, DEV_RANDOM_MINOR},
    {"urandom", 0666, MEM_MAJOR, DEV_URANDOM_MINOR},
    {"kmsg",    0644, MEM_MAJOR, DEV_KMSG_MINOR},
    {"tty",     0666, TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR},
    {"console", 0666, TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR},
    {"ptmx",    0666, TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR},
    // systemd's getty@tty1.service carries ConditionPathExists=/dev/tty0 (the
    // Linux "current VT" alias) and silently skips without it, while agetty
    // itself opens /dev/tty1; the rest round out the usual VT set.
    {"tty0",    0666, TTY_CONSOLE_MAJOR, 0},
    {"tty1",    0666, TTY_CONSOLE_MAJOR, 1},
    {"tty2",    0666, TTY_CONSOLE_MAJOR, 2},
    {"tty3",    0666, TTY_CONSOLE_MAJOR, 3},
    {"tty4",    0666, TTY_CONSOLE_MAJOR, 4},
    {"tty5",    0666, TTY_CONSOLE_MAJOR, 5},
    {"tty6",    0666, TTY_CONSOLE_MAJOR, 6},
    {"tty7",    0666, TTY_CONSOLE_MAJOR, 7},
    {"rtc0",    0666, DEV_RTC_MAJOR, DEV_RTC_MINOR},
    {"fuse",    0666, MISC_MAJOR, DEV_FUSE_MINOR},
    // 0660 and not 0666: a Linux swap block device is brw-rw---- root:disk
    // (verified against /dev/loop1). dev_node_spec carries no uid/gid so this
    // lands root:root, which is as close as AOK can say -- there is no disk
    // group here.
    {"aokswap0", 0660, AOKSWAP_MAJOR, DEV_AOKSWAP_MINOR, .is_block = true},
};
const size_t dev_standard_nodes_count = sizeof(dev_standard_nodes)/sizeof(dev_standard_nodes[0]);

const struct dev_node_spec dev_dynamic_nodes[] = {
    {"clipboard", 0666, DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR},
    {"location",  0666, DYN_DEV_MAJOR, DEV_LOCATION_MINOR},
    {"dsp",       0666, DYN_DEV_MAJOR, DEV_DSP_MINOR},
    {"url",       0666, DYN_DEV_MAJOR, DEV_URL_MINOR},
};
const size_t dev_dynamic_nodes_count = sizeof(dev_dynamic_nodes)/sizeof(dev_dynamic_nodes[0]);

int dev_open(int major, int minor, int type, struct fd *fd) {
    struct dev_ops *dev = (type == DEV_BLOCK ? block_devs : char_devs)[major];
    if (dev == NULL)
        return _ENXIO;
    fd->ops = &dev->fd;
    if (!dev->open)
        return 0;
    return dev->open(major, minor, fd);
}
