#ifndef FS_H
#define FS_H

#include "misc.h"
#include "util/list.h"
#include "fs/stat.h"
#include "fs/dev.h"
#include "fs/fake-db.h"
#include "fs/fix_path.h"
#include "emu/memory.h"
#include <dirent.h>
#include <sqlite3.h>

struct fs_info {
    atomic_uint refcount;
    mode_t_ umask;
    struct fd *pwd;
    struct fd *root;
    lock_t lock;
};
struct fs_info *fs_info_new(void);
struct fs_info *fs_info_retain(struct fs_info *fs);
struct fs_info *fs_info_copy(struct fs_info *fs);
void fs_info_release(struct fs_info *fs);

void fs_chdir(struct fs_info *fs, struct fd *pwd);

#define MAX_PATH 4096
#define MAX_NAME 256

struct attr {
    enum attr_type {
        attr_uid,
        attr_gid,
        attr_mode,
        attr_size,
    } type;
    union {
        uid_t_ uid;
        uid_t_ gid;
        mode_t_ mode;
        off_t_ size;
    };
};
#define make_attr(_type, thing) \
    ((struct attr) {.type = attr_##_type, ._type = thing})

#define AT_EMPTY_PATH_ 0x1000
#define AT_SYMLINK_NOFOLLOW_ 0x100
#define AT_NO_AUTOMOUNT_ 0x800

// renameat2 flags
#define RENAME_NOREPLACE_ (1 << 0)
#define RENAME_EXCHANGE_ (1 << 1)
#define RENAME_WHITEOUT_ (1 << 2)

struct fd *generic_open(const char *path, int flags, int mode);
struct fd *generic_openat(struct fd *at, const char *path, int flags, int mode);
// For stored, already-normalized paths (chroot prefix included): anchors at
// the real root instead of the caller's chroot. See fs/generic.c.
struct fd *generic_open_realroot(const char *path, int flags, int mode);
int generic_getpath(struct fd *fd, char *buf);
int fs_rebase_path_to_root(struct fs_info *fs, char *path);
int fs_rebase_readlink_path(struct fs_info *fs, char *path);
int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw);
int generic_unlinkat(struct fd *at, const char *path);
int generic_rmdirat(struct fd *at, const char *path);
int generic_renameat(struct fd *src_at, const char *src, struct fd *dst_at, const char *dst, int flags);
int generic_symlinkat(const char *target, struct fd *at, const char *link);
int generic_mknodat(struct fd *at, const char *path, mode_t_ mode, dev_t_ dev);
int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size);
#define AC_R 4
#define AC_W 2
#define AC_X 1
#define AC_F 0
int generic_accessat(struct fd *dirfd, const char *path, int mode);
int generic_statat(struct fd *at, const char *path, struct statbuf *stat, int flags);
// Same as generic_statat, but also reports (via is_mount_root, when non-NULL)
// whether path resolved to exactly the root of its mount.
int generic_statat_ext(struct fd *at, const char *path, struct statbuf *stat, int flags, bool *is_mount_root);
// Also reports (via mnt_id, when non-NULL) the mountinfo/statx mount ID the
// path (or fd, for AT_EMPTY_PATH) resolved into.
int generic_statat_full(struct fd *at, const char *path, struct statbuf *stat, int flags, bool *is_mount_root, int *mnt_id);
// fstat with the mount's anonymous st_dev stamped when the fs leaves dev 0
int generic_fstat(struct fd *fd, struct statbuf *stat);
// stat(2)/lstat(2)-following a /proc/PID/fd/N magic symlink: reports the
// pointee's real attributes via generic_fstat rather than chasing its
// descriptive (non-path) readlink target. False if path isn't such an
// entry (result untouched, caller falls through to normal resolution).
bool procfd_statat(struct fd *at, const char *path, struct statbuf *stat, int *err_out);
// Same, for a /proc/PID/ns/* nsfs magic link. systemd's namespace-support
// probe is access() on one of these, so a miss here disables namespace setup
// for the whole unit.
bool procns_statat(struct fd *at, const char *path, struct statbuf *stat, int *err_out);
int generic_setattrat(struct fd *at, const char *path, struct attr attr, bool follow_links);
int generic_utime(struct fd *at, const char *path, struct timespec atime, struct timespec mtime, bool follow_links);
ssize_t generic_readlinkat(struct fd *at, const char *path, char *buf, size_t bufsize);
int generic_mkdirat(struct fd *at, const char *path, mode_t_ mode);

int access_check(struct statbuf *stat, int check);
int setattr_check(struct statbuf *stat, struct attr attr);

// iSH-internal mount flag, NOT part of the guest mount(2) ABI. fs/mount.c's
// sys_mount masks incoming guest flags to MS_FLAGS before calling do_mount(),
// so a guest process can never set or clear this bit; only native do_mount()
// callers (see app/AppDelegate.m) do. Marks a realfs mount whose backing
// directory is a single app-owned host area shared by every guest uid (e.g.
// /AOK/persist, /AOK/roots) rather than a real multi-user filesystem: every
// file in it is always owned, at the host layer, by the app's own real uid
// regardless of which guest uid nominally created it, so iSH's normal
// owner-vs-other permission model (kernel/fs.c:access_check) can never see
// the guest uid as "owner" and falls back to the (usually non-writable)
// "other" bits. realfs_open/realfs_mkdir/realfs_mknod force newly created
// nodes world-writable under this flag so every guest uid keeps working.
#define MOUNT_ISH_SHARED_ (1 << 30)

struct mount {
    const char *point;
    size_t point_len;
    const char *source;
    // Name reported for this mount in /proc/mounts and /proc/self/mountinfo
    // (df's "Filesystem" column), when the real `source` is not worth showing
    // the guest. The root's source is a host path -- on device one containing
    // the app group UUID -- which df wraps onto its own line and which the
    // guest can do nothing with, so the root reports its own name instead
    // ("Devuan6-arm64"). NULL means "report source", which is every mount the
    // guest itself made: mount(2) shows the string the guest passed, like
    // Linux. Deliberately not a copy of source: realfs (fs/real.c) and iosfs
    // (app/iOSFS.m) replace mount->source after the mount completes, and a
    // snapshot taken here would go stale.
    const char *display_source;
    const char *info;
    int flags;
    const struct fs_ops *fs;
    unsigned refcount;
    struct list mounts;

    // Anonymous device number for this mount (Linux 0:xx semantics), stamped
    // into stat results when the filesystem doesn't provide one (stat->dev
    // left 0: tmpfs, proc, sys, devpts, cgroup...). Filesystems that derive a
    // real device (realfs/fakefs via the host st_dev) are left alone. Unique
    // per mount so st_dev comparisons (df mount matching, find -xdev, du -x)
    // can tell virtual mounts apart. Binds copy their origin's, matching
    // Linux (a bind shares the origin superblock).
    dev_t_ fake_dev;

    int root_fd;
    union {
        void *data;
        struct fakefs_db fakefs;
    };

    // Bind mounts (MS_BIND): a bind has no backing of its own. bind_origin is the
    // real mount whose storage it aliases (held with a reference for the bind's
    // lifetime), and bind_prefix is the source path relative to bind_origin->point.
    // find_mount_and_trim_path() rewrites a bound path to bind_prefix + remainder
    // and resolves it against bind_origin. NULL for ordinary mounts.
    struct mount *bind_origin;
    const char *bind_prefix;
};
extern lock_t mounts_lock;

// returns a reference, which must be released
struct mount *mount_find(char *path);
void mount_retain(struct mount *mount);
void mount_release(struct mount *mount);
// statfs for a mount, filling in the filesystem's magic and following a bind
// to the mount that actually backs it. Every statfs(2)/fstatfs(2) variant goes
// through this; call it rather than mount->fs->statfs, which is wrong for a
// bind (see kernel/fs.c).
int mount_statfs(struct mount *mount, struct statfsbuf *stat);
// mountinfo/statx mount ID (1-based list position); see fs/mount.c
int mount_id(struct mount *mount);
// The st_dev files on this mount report, i.e. mountinfo's device field; asks
// the filesystem rather than assuming, since only backing-less filesystems use
// mount->fake_dev. Follows a bind to its origin. See fs/mount.c.
dev_t_ mount_dev(struct mount *mount);

// O_PATH|O_NOFOLLOW fd referring to a symlink itself; see fs/generic.c
bool fd_is_opath_link(struct fd *fd);
struct fd *opath_link_fd_create(struct mount *mount, const char *path);
int opath_link_fstat(struct fd *fd, struct statbuf *stat);
struct mount *opath_link_get_mount(struct fd *fd);
ssize_t opath_link_readlink(struct fd *fd, char *buf, size_t bufsize);

// The name to report for a mount in /proc/mounts and /proc/self/mountinfo.
static inline const char *mount_display_source(struct mount *mount) {
    return mount->display_source != NULL ? mount->display_source : mount->source;
}

// Set the name the mount at `point` reports in /proc/mounts and
// /proc/self/mountinfo, leaving mount->source (the host path the filesystem
// code opens) alone. Takes mounts_lock, so call it without the lock held.
int mount_set_display_source(const char *point, const char *display_source);

// True if a filesystem is mounted at exactly `point` ("/" accepted for the
// root). Takes mounts_lock, so call it without the lock held.
bool mount_exists_at_point(const char *point);

// must hold mounts_lock while calling these, or traversing mounts
int do_mount(const struct fs_ops *fs, const char *source, const char *point, const char *info, int flags);
int do_umount(const char *point);
int mount_remove(struct mount *mount);
extern struct list mounts;

bool mount_param_flag(const char *info, const char *flag);

// open flags
#define O_ACCMODE_ 3
#define O_RDONLY_ 0
#define O_WRONLY_ (1 << 0)
#define O_RDWR_ (1 << 1)
#define O_CREAT_ (1 << 6)
#define O_EXCL_ (1 << 7)
#define O_NOCTTY_ (1 << 8)
#define O_TRUNC_ (1 << 9)
#define O_APPEND_ (1 << 10)
#define O_NONBLOCK_ (1 << 11)
#define O_DIRECTORY_ (1 << 16)
#define O_NOFOLLOW_ (1 << 17)
#define O_CLOEXEC_ (1 << 19)
// O_PATH: same value (010000000) on i386, amd64, and asm-generic (arm64/
// riscv64), so no per-ABI translation is needed. Combined with O_NOFOLLOW on
// a final symlink it opens the symlink ITSELF (fstat sees S_IFLNK,
// readlinkat(fd, "") returns the target, read/write give EBADF) -- the
// primitive systemd's chase() uses for every path component. Without it,
// any chase() ending on a symlink got ELOOP ("Too many levels of symbolic
// links" for /etc/os-release and, fatally, the /etc/localtime timezone
// watch during Arch aarch64 boot).
#define O_PATH_ (1 << 21)

// generic ioctls
// On sockets 0x5411 is SIOCOUTQ (bytes queued in the send buffer); it shares
// its value with the tty TIOCOUTQ ioctl.
#define SIOCOUTQ_ 0x5411
#define FIONREAD_ 0x541b
#define FIONBIO_ 0x5421
#define FIONCLEX_ 0x5450
#define FIOCLEX_ 0x5451

// All operations are optional unless otherwise specified
struct fs_ops {
    const char *name;
    int magic;

    int (*mount)(struct mount *mount);
    int (*umount)(struct mount *mount);
    int (*statfs)(struct mount *mount, struct statfsbuf *stat);

    struct fd *(*open)(struct mount *mount, const char *path, int flags, int mode); // required
    ssize_t (*readlink)(struct mount *mount, const char *path, char *buf, size_t bufsize);

    // These return _EPERM if not present
    int (*link)(struct mount *mount, const char *src, const char *dst);
    int (*unlink)(struct mount *mount, const char *path);
    int (*rmdir)(struct mount *mount, const char *path);
    int (*rename)(struct mount *mount, const char *src, const char *dst);
    int (*symlink)(struct mount *mount, const char *target, const char *link);
    int (*mknod)(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev);
    int (*mkdir)(struct mount *mount, const char *path, mode_t_ mode);

    // There's a close function in both the fs and fd to handle device files
    // where, for instance, there's a real_fd needed for getpath and also a tty
    // reference, and both need to be released when the fd is closed.
    // If they are the same function, it will only be called once.
    int (*close)(struct fd *fd);

    int (*stat)(struct mount *mount, const char *path, struct statbuf *stat); // required
    int (*fstat)(struct fd *fd, struct statbuf *stat); // required
    int (*setattr)(struct mount *mount, const char *path, struct attr attr);
    int (*fsetattr)(struct fd *fd, struct attr attr);
    int (*utime)(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links);
    // Set atime/mtime directly on an open fd (the futimens/utimensat(fd, NULL)
    // form). No path resolution happens, so it must work on unlinked files and
    // non-path fds (sockets, pipes). Optional; sys_utime_common falls back to
    // resolving the fd's own path when absent.
    int (*futime)(struct fd *fd, struct timespec atime, struct timespec mtime);
    // Returns the path of the file descriptor, null terminated, buf must be at least MAX_PATH+1
    int (*getpath)(struct fd *fd, char *buf); // required

    int (*flock)(struct fd *fd, int operation);

    // If present, called when all references to an inode_data for this
    // filesystem go away.
    void (*inode_orphaned)(struct mount *mount, ino_t inode);
};

struct mount *find_mount_and_trim_path(char *path);

// adhoc fs
struct fd *adhoc_fd_create(const struct fd_ops *ops);
// this is for the "wtf is apple smoking" section
bool is_adhoc_fd(struct fd *fd);

// filesystems
extern const struct fs_ops procfs;
extern const struct fs_ops aokfs;
extern const struct fs_ops fakefs;
extern const struct fs_ops devptsfs;
extern const struct fs_ops tmpfs;
extern const struct fs_ops devtmpfs;
extern const struct fs_ops sysfs;
extern const struct fs_ops cgroupfs;
extern const struct fs_ops cgroup2fs;
void fs_register(const struct fs_ops *fs);
char* get_filesystems(void); // For /proc/filesystems

// System-wide bytes moved through file-backed fds (kernel/fs.c), feeding
// /proc/vmstat's pgpgin/pgpgout (which are in KiB).
extern _Atomic qword_t io_disk_read_bytes;
extern _Atomic qword_t io_disk_write_bytes;

#endif
