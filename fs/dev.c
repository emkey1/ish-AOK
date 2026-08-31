#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/mem.h"
#include "fs/tty.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "app/RTCDevice.h"

struct dev_ops *block_devs[256] = {
    // no block devices yet
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
