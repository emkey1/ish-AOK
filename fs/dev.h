#ifndef DEV_H
#define DEV_H

#include <sys/types.h>
#if __linux__
#include <sys/sysmacros.h>
#endif
#include "fs/fd.h"

// a dev_t is encoded like this in hex, where M is major and m is minor:
// mmmMMMmm
// (legacy I guess)

typedef uint32_t dev_t_;

static inline dev_t_ dev_make(int major, int minor) {
    return ((minor & 0xfff00) << 12) | (major << 8) | (minor & 0xff);
}
static inline int dev_major(dev_t_ dev) {
    return (dev & 0xfff00) >> 8;
}
static inline int dev_minor(dev_t_ dev) {
    return ((dev & 0xfff00000) >> 12) | (dev & 0xff);
}

static inline dev_t dev_real_from_fake(dev_t_ dev) {
    return makedev(dev_major(dev), dev_minor(dev));
}
static inline dev_t_ dev_fake_from_real(dev_t dev) {
    return dev_make(major(dev), minor(dev));
}

// Anonymous device minors (major 0, Linux 0:xx semantics) for filesystems
// with no real backing device. Static pseudo-mounts get fixed minors below
// FAKE_DEV_MINOR_DYNAMIC; do_mount() hands out minors from there up, one per
// mount. Note dev_make(0, m) == m for m < 256, so the static-mount
// initializers can use these constants directly.
#define FAKE_DEV_MINOR_MEMFD 1
// anon_inodefs: eventfd, epoll, timerfd, signalfd, pidfd, fscontext. Linux
// keeps all of these on ONE internal filesystem, so they share a device --
// verified against Devuan, where every one of them reports the same 0:16.
#define FAKE_DEV_MINOR_ADHOC 2
// The kinds Linux gives a filesystem of their own, and therefore a device of
// their own: pipefs, sockfs and nsfs. Splitting them apart is not cosmetic.
// A program cannot ask "what kind of thing is this descriptor?" directly, so
// it asks which filesystem the descriptor lives on -- lsns reads the device
// number off /proc/self/ns/net and then examines exactly the descriptors that
// match it. With one device shared by everything anonymous, that filter
// selected every pipe and socket on the machine.
#define FAKE_DEV_MINOR_PIPEFS 3
#define FAKE_DEV_MINOR_SOCKFS 4
#define FAKE_DEV_MINOR_NSFS 5
#define FAKE_DEV_MINOR_DYNAMIC 15

#define DEV_BLOCK 0
#define DEV_CHAR 1

struct dev_ops {
    int (*open)(int major, int minor, struct fd *fd);
    struct fd_ops fd;
};

extern struct dev_ops *block_devs[];
extern struct dev_ops *char_devs[];

// The standard /dev node set, in one place because three things build it:
// devtmpfs populates a fresh mount with it (fs/tmp.c), sys_mount_guest
// repairs an existing /dev in place with it (fs/mount.c), and the app and
// command-line startup paths create it on the main root.
struct dev_node_spec {
    const char *name; // relative to the /dev directory, no leading slash
    mode_t_ mode;
    int major, minor;
};
// Always published: these majors are statically dispatched in fs/dev.c.
extern const struct dev_node_spec dev_standard_nodes[];
extern const size_t dev_standard_nodes_count;
// Published only where dyn_dev_is_registered() says the device exists, which
// in practice means the iOS app and not the command-line build.
extern const struct dev_node_spec dev_dynamic_nodes[];
extern const size_t dev_dynamic_nodes_count;

int dev_open(int major, int minor, int type, struct fd *fd);

extern struct dev_ops null_dev;
extern struct dev_ops fuse_dev; // fs/fuse.c

#endif
