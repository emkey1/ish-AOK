#include <string.h>
#include <sys/stat.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "kernel/errno.h"

static struct mount adhoc_mount;
static unsigned adhoc_inode_seq;

struct fd *adhoc_fd_create(const struct fd_ops *ops) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    mount_retain(&adhoc_mount);
    fd->mount = &adhoc_mount;
    fd->stat = (struct statbuf) {};
    // Give every adhoc fd a unique nonzero inode so /proc/<pid>/fd/<n> and lsof
    // can tell sockets/pipes/eventfds apart. st_dev is 0 for these, so they
    // never collide with real-fs inodes.
    fd->stat.inode = __atomic_add_fetch(&adhoc_inode_seq, 1, __ATOMIC_RELAXED);
    // An anon_inode fd is a REGULAR file to stat(2). Linux makes these with
    // alloc_anon_inode, which sets S_IFREG|S_IRUSR|S_IWUSR; leaving the mode 0
    // reports a file with NO type bits at all, which is not a thing a file can
    // be. coreutils calls it "weird file"; lsof 4.99.4 SEGVs on it -- statting
    // dbus-daemon's epoll fd killed `lsof' outright, on every device, and took
    // the whole listing with it because one bad fd ends the run:
    //
    //     newfstatat("/proc/683/fd/3", {st_mode=000, ...}, 0) = 0
    //     --- SIGSEGV {si_code=SEGV_MAPERR, si_addr=0xfa000c98} ---
    //
    // Only for fds that declare a class, which is exactly the anon_inode family
    // (eventpoll, inotify, timerfd, signalfd, eventfd, pidfd, nsfs, fscontext).
    // Sockets and pipes set S_IFSOCK/S_IFIFO themselves and are left alone --
    // the namer below keys off those bits, so defaulting them would rename
    // every socket to anon_inode.
    if (ops != NULL && ops->anon_inode_class != NULL)
        fd->stat.mode = S_IFREG | 0600;
    return fd;
}

// Which anonymous filesystem this descriptor belongs on.
//
// Linux has no single "anonymous" filesystem: pipes live on pipefs, sockets
// on sockfs, namespaces on nsfs, and the eventfd/epoll/timerfd/signalfd
// family on anon_inodefs. Each has its own anonymous device number, and that
// number is how a program tells the kinds apart without opening anything.
//
// Measured on Devuan: nsfs 0:4, sockfs 0:9, pipefs 0:15, anon_inodefs 0:16 --
// four devices, with the last shared by all four of its members. AOK reported
// one device for all of them, which is what sent lsns interrogating every
// pipe and socket it could find: it takes the device number from
// /proc/self/ns/net and probes everything that matches, so everything matched.
static dev_t_ adhoc_anon_dev(struct fd *fd) {
    if (S_ISSOCK(fd->stat.mode))
        return FAKE_DEV_MINOR_SOCKFS;
    if (S_ISFIFO(fd->stat.mode))
        return FAKE_DEV_MINOR_PIPEFS;
    if (fd->ops != NULL && fd->ops->anon_inode_class != NULL &&
            strcmp(fd->ops->anon_inode_class, "nsfs") == 0)
        return FAKE_DEV_MINOR_NSFS;
    return FAKE_DEV_MINOR_ADHOC;
}

static int adhoc_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = fd->stat;
    // Left at 0 by everything that creates one of these, so the choice is
    // made here, where the descriptor's kind is finally known -- sockets and
    // pipes set their own type bits after adhoc_fd_create returns.
    if (stat->dev == 0)
        stat->dev = adhoc_anon_dev(fd);
    return 0;
}

static int adhoc_fsetattr(struct fd *fd, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            break;
        case attr_gid:
            fd->stat.gid = attr.gid;
            break;
        case attr_mode:
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            return _EINVAL;
    }
    return 0;
}

static int adhoc_getpath(struct fd *fd, char *buf) {
    // Render the way Linux does in /proc/<pid>/fd: sockets and pipes show their
    // type plus inode; everything else is anon_inode:[class], where the class
    // (eventfd, eventpoll, signalfd, ...) comes from the fd's ops. Previously
    // every adhoc fd reported "anon_inode:[unknown]" with a zero inode, so lsof
    // could not classify sockets/eventfds (the listener of a daemon like sshd
    // showed up as an unstattable anon_inode).
    unsigned long ino = (unsigned long) fd->stat.inode;
    if (S_ISSOCK(fd->stat.mode))
        snprintf(buf, MAX_PATH, "socket:[%lu]", ino);
    else if (S_ISFIFO(fd->stat.mode))
        snprintf(buf, MAX_PATH, "pipe:[%lu]", ino);
    else {
        const char *cls = (fd->ops != NULL && fd->ops->anon_inode_class != NULL)
            ? fd->ops->anon_inode_class : "anon_inode";
        // Two classes spell their own brackets -- "[pidfd]" and "[fscontext]" --
        // and wrapping those again produced `anon_inode:[[pidfd]]', which is not
        // what any reader expects to parse. Seen on elogind, which holds three.
        if (cls[0] == '[')
            snprintf(buf, MAX_PATH, "anon_inode:%s", cls);
        else
            snprintf(buf, MAX_PATH, "anon_inode:[%s]", cls);
    }
    return 0;
}

bool is_adhoc_fd(struct fd *fd) {
    return fd->mount == &adhoc_mount;
}

// futimens() on a socket/pipe/anon fd sets the times on its (anonymous) inode
// on Linux; mirror that on the fd's fake stat so fstat reflects the update.
// (stress-ng --sockabuse futimens()es a socket fd in a loop.)
static int adhoc_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    fd->stat.atime = (dword_t) atime.tv_sec;
    fd->stat.atime_nsec = (dword_t) atime.tv_nsec;
    fd->stat.mtime = (dword_t) mtime.tv_sec;
    fd->stat.mtime_nsec = (dword_t) mtime.tv_nsec;
    return 0;
}

static const struct fs_ops adhoc_fs = {
    .magic = 0x09041934, // FIXME wrong for pipes and sockets
    .fstat = adhoc_fstat,
    .fsetattr = adhoc_fsetattr,
    .futime = adhoc_futime,
    .getpath = adhoc_getpath,
};

static struct mount adhoc_mount = {
    .fs = &adhoc_fs,
    .point = "",
    // The fallback for anything reaching stat_stamp_fake_dev without having
    // gone through adhoc_fstat; the per-kind devices are chosen there.
    .fake_dev = FAKE_DEV_MINOR_ADHOC,
};
