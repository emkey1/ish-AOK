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
#include "kernel/task.h"
#include "kernel/abi.h"

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
// The block ioctls a swap tool asks before it will touch a device. Without
// them the node was a block device the way a photograph of a door is a door:
// read, write and lseek all worked, /proc/swaps named it, the swap header was
// there to be read -- and `swapon /dev/aokswap0` still failed with "read swap
// header failed", because busybox asks the size through an ioctl first and got
// ENOTTY. The swapon(2) and swapoff(2) SYSCALLS worked the whole time, which is
// why this survived a reading of the code and only fell out of running the
// command a user would actually type.
//
// Values are Linux's. BLKGETSIZE64 encodes sizeof(size_t) in the _IOR size
// field, so a 32-bit guest sends a different number from a 64-bit one and both
// have to be answered.
#define BLKGETSIZE_       0x1260      // _IO(0x12, 96)   unsigned long, 512b sectors
#define BLKFLSBUF_        0x1261      // _IO(0x12, 97)   flush buffers
#define BLKSSZGET_        0x1268      // _IO(0x12, 104)  int, logical sector size
#define BLKGETSIZE64_32_  0x80041272  // _IOR(0x12, 114, size_t), 32-bit size_t
#define BLKGETSIZE64_64_  0x80081272  // _IOR(0x12, 114, size_t), 64-bit size_t
#define BLKPBSZGET_       0x127b      // _IO(0x12, 123)  int, physical block size

// The sector size the size ioctls are denominated in. 512 regardless of the
// 4096-byte swap page: that is what a block device reports and what the tools
// divide by.
#define AOKSWAP_SECTOR_SIZE 512

// `unsigned long` is as wide as a pointer on every ABI AOK runs, so the ABI's
// pointer size is the honest answer for BLKGETSIZE's argument. Outside a task
// (no `current`), assume the widest rather than truncate.
static size_t aokswap_ulong_size(void) {
    if (current == NULL)
        return sizeof(qword_t);
    return guest_abi_desc(current->abi).pointer_size;
}

static ssize_t aokswap_ioctl_size(int cmd) {
    switch (cmd) {
        case BLKGETSIZE_:
            return (ssize_t) aokswap_ulong_size();
        case BLKGETSIZE64_32_: case BLKGETSIZE64_64_:
            return sizeof(qword_t);
        case BLKSSZGET_: case BLKPBSZGET_:
            return sizeof(dword_t);
        case BLKFLSBUF_:
            return 0;
    }
    return -1;
}

// Everything answers from swap_area_bytes(), the same figure /proc/meminfo's
// SwapTotal and the /proc/swaps row come from, so the device's capacity cannot
// contradict the swap totals -- which is the whole reason this node exists.
static int aokswap_ioctl(struct fd *UNUSED(fd), int cmd, void *arg) {
    uint64_t bytes = swap_area_bytes();
    switch (cmd) {
        case BLKGETSIZE_: {
            uint64_t sectors = bytes / AOKSWAP_SECTOR_SIZE;
            if (aokswap_ulong_size() == sizeof(dword_t))
                *(dword_t *) arg = (dword_t) sectors;
            else
                *(qword_t *) arg = sectors;
            return 0;
        }
        case BLKGETSIZE64_32_:
        case BLKGETSIZE64_64_:
            *(qword_t *) arg = bytes;
            return 0;
        case BLKSSZGET_:
        case BLKPBSZGET_:
            *(dword_t *) arg = AOKSWAP_SECTOR_SIZE;
            return 0;
        case BLKFLSBUF_:
            // Nothing of ours is cached: reads and writes go straight at the
            // area's file descriptor. Succeeding is the honest answer.
            return 0;
    }
    return _ENOTTY;
}

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
    .fd.ioctl_size = aokswap_ioctl_size,
    .fd.ioctl = aokswap_ioctl,
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
