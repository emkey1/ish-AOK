#ifndef FS_REAL_H
#define FS_REAL_H

#include <stdatomic.h>
#include "kernel/fs.h"

extern const struct fd_ops realfs_fdops;
extern const struct fs_ops realfs;

// The guest's one block device.
//
// "sda", not the host's "disk1" it used to be called: AOK aggregates every real
// read and write into a single device and has always printed major 8, minor 0
// for it, which IS sda in Linux's numbering. Naming it after an Apple disk both
// leaked a host detail into the guest and contradicted the numbers beside it.
//
// This name is the one thing /proc/mounts, /proc/diskstats, /sys/block and
// /sys/class/block must agree on -- a mount and a device that share no name is
// why btop's disk and io panels listed nothing. Change it in one place.
#define GUEST_DISK_NAME "sda"
#define GUEST_DISK_MAJOR 8
#define GUEST_DISK_MINOR 0

// Aggregate byte-level I/O counters for every real-host read/write/pread/pwrite
// (this is the single choke point fakefs, realfs, and iosfs data all flow
// through), used to back /proc/diskstats and /sys/block so vmstat/iostat have
// real numbers instead of an all-zero stub. One global device, not per-mount.
struct realfs_io_stats {
    _Atomic uint64_t read_ops, read_bytes;
    _Atomic uint64_t write_ops, write_bytes;
};
extern struct realfs_io_stats realfs_io_stats;

struct fd *realfs_open(struct mount *mount, const char *path, int flags, int mode);

ssize_t realfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize);
int realfs_link(struct mount *mount, const char *src, const char *dst);
int realfs_unlink(struct mount *mount, const char *path);
int realfs_rmdir(struct mount *mount, const char *path);
int realfs_rename(struct mount *mount, const char *src, const char *dst);
int realfs_symlink(struct mount *mount, const char *target, const char *link);
int realfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ UNUSED(dev));

int realfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat);
int realfs_statfs(struct mount *mount, struct statfsbuf *stat);
int realfs_fstat(struct fd *fd, struct statbuf *fake_stat);
int realfs_setattr(struct mount *mount, const char *path, struct attr attr);
int realfs_fsetattr(struct fd *fd, struct attr attr);

int realfs_mkdir(struct mount *mount, const char *path, mode_t_ mode);

int realfs_truncate(struct mount *mount, const char *path, off_t_ size);
int realfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links);
int realfs_futime(struct fd *fd, struct timespec atime, struct timespec mtime);

int realfs_statfs(struct mount *mount, struct statfsbuf *stat);
int realfs_flock(struct fd *fd, int operation);
int realfs_getpath(struct fd *fd, char *buf);
ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize);
ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize);

int realfs_readdir(struct fd *fd, struct dir_entry *entry);
unsigned long realfs_telldir(struct fd *fd);
void realfs_seekdir(struct fd *fd, unsigned long ptr);

off_t realfs_lseek(struct fd *fd, off_t offset, int whence);

int realfs_poll(struct fd *fd);
int realfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);
// realfs_mmap's core on a bare host fd; shared with tmpfs (fs/tmp.c) and memfd (kernel/memfd.c)
int host_fd_mmap(int host_fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);
// unlinked CLOEXEC host temp file (or negative errno); mmapable backing for tmpfs files and memfds
int host_unlinked_tmpfd(void);
int realfs_fsync(struct fd *fd);
int realfs_getflags(struct fd *fd);
int realfs_setflags(struct fd *fd, dword_t arg);
ssize_t realfs_ioctl_size(int cmd);
int realfs_ioctl(struct fd *fd, int cmd, void *arg);
int realfs_close(struct fd *fd);

#endif
