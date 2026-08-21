#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"

#define STATX_TYPE_        0x00000001U
#define STATX_MODE_        0x00000002U
#define STATX_NLINK_       0x00000004U
#define STATX_UID_         0x00000008U
#define STATX_GID_         0x00000010U
#define STATX_ATIME_       0x00000020U
#define STATX_MTIME_       0x00000040U
#define STATX_CTIME_       0x00000080U
#define STATX_INO_         0x00000100U
#define STATX_SIZE_        0x00000200U
#define STATX_BLOCKS_      0x00000400U
#define STATX_BASIC_STATS_ 0x000007ffU
#define STATX_MNT_ID_ 0x00001000U

#define AT_STATX_SYNC_TYPE_     0x6000

#define STATX_ATTR_MOUNT_ROOT_  0x00002000U

static bool http_resolver_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_HTTPFS") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && strncmp(current->comm, "http", 4) == 0;
}

static bool dpkg_stat_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_DPKG_STAT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return strncmp(current->comm, "dpkg", 4) == 0 ||
        strcmp(current->comm, "apt") == 0 ||
        strcmp(current->comm, "apt-get") == 0;
}

static bool ldconfig_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_LDCONFIG_STAT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && strcmp(current->comm, "ldconfig") == 0;
}

static bool dpkg_stat_trace_path(const char *path) {
    return path != NULL && strncmp(path, "/var/lib/dpkg/", 14) == 0;
}

static bool ldconfig_trace_path(const char *path) {
    static const char *prefixes[] = {
        "/lib/i386-linux-gnu",
        "/usr/lib/i386-linux-gnu",
        "/etc/ld.so.conf",
        "/etc/ld.so.conf.d/",
        "/usr/local/lib/i386-linux-gnu",
        "/usr/local/lib/i686-linux-gnu",
        "/lib/i686-linux-gnu",
        "/usr/lib/i686-linux-gnu",
    };
    if (path == NULL)
        return false;
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], len) == 0)
            return true;
    }
    return false;
}

static void dpkg_stat_trace_overflow(const char *op, fd_t at_f, const char *path, const struct statbuf *stat) {
    if (!dpkg_stat_trace_enabled() || !dpkg_stat_trace_path(path) || stat == NULL)
        return;
    fprintf(stderr,
            "ish-dpkgstat:%s-overflow pid=%d comm=%s at=%d path=%s dev=%llu ino=%llu mode=%#x nlink=%u uid=%u gid=%u rdev=%llu size=%llu blksize=%u blocks=%llu\n",
            op, current->pid, current->comm, at_f, path,
            (unsigned long long) stat->dev,
            (unsigned long long) stat->inode,
            stat->mode, stat->nlink, stat->uid, stat->gid,
            (unsigned long long) stat->rdev,
            (unsigned long long) stat->size,
            stat->blksize,
            (unsigned long long) stat->blocks);
}

static void dpkg_stat_trace_result(const char *op, fd_t at_f, const char *path, long result,
        const struct statbuf *stat, bool have_stat) {
    if (!dpkg_stat_trace_enabled() || !dpkg_stat_trace_path(path))
        return;
    if (have_stat && stat != NULL) {
        fprintf(stderr,
                "ish-dpkgstat:%s pid=%d comm=%s abi=%d at=%d path=%s result=%ld dev=%llu ino=%llu mode=%#x nlink=%u uid=%u gid=%u rdev=%llu size=%llu blksize=%u blocks=%llu\n",
                op, current->pid, current->comm, current->abi, at_f, path, result,
                (unsigned long long) stat->dev,
                (unsigned long long) stat->inode,
                stat->mode, stat->nlink, stat->uid, stat->gid,
                (unsigned long long) stat->rdev,
                (unsigned long long) stat->size,
                stat->blksize,
                (unsigned long long) stat->blocks);
        return;
    }
    fprintf(stderr,
            "ish-dpkgstat:%s pid=%d comm=%s abi=%d at=%d path=%s result=%ld\n",
            op, current->pid, current->comm, current->abi, at_f, path, result);
}

static void ldconfig_stat_trace_result(const char *op, fd_t at_f, const char *path, long result,
        const struct statbuf *stat, bool have_stat) {
    if (!ldconfig_trace_enabled() || !ldconfig_trace_path(path))
        return;
    if (have_stat && stat != NULL) {
        fprintf(stderr,
                "ish-ldconfig-stat:%s pid=%d at=%d path=%s result=%ld dev=%llu ino=%llu mode=%#x nlink=%u uid=%u gid=%u rdev=%llu size=%llu blksize=%u blocks=%llu\n",
                op, current->pid, at_f, path, result,
                (unsigned long long) stat->dev,
                (unsigned long long) stat->inode,
                stat->mode, stat->nlink, stat->uid, stat->gid,
                (unsigned long long) stat->rdev,
                (unsigned long long) stat->size,
                stat->blksize,
                (unsigned long long) stat->blocks);
        return;
    }
    fprintf(stderr,
            "ish-ldconfig-stat:%s pid=%d at=%d path=%s result=%ld\n",
            op, current->pid, at_f, path, result);
}

static void http_resolver_trace_path_result(const char *op, fd_t at_f, const char *path, long result, unsigned long flags) {
    if (!http_resolver_trace_enabled() || path == NULL)
        return;
    fprintf(stderr, "ish-httpfs:%s pid=%d comm=%s at=%d path=%s flags=%#lx result=%ld\n",
            op, current->pid, current->comm, at_f, path, flags, result);
}

struct newstat64 stat_convert_newstat64(struct statbuf stat) {
    struct newstat64 newstat = {};
    newstat.dev = stat.dev;
    newstat.__st_ino = stat.inode;
    newstat.ino = stat.inode;
    newstat.mode = stat.mode;
    newstat.nlink = stat.nlink;
    newstat.uid = stat.uid;
    newstat.gid = stat.gid;
    newstat.rdev = stat.rdev;
    newstat.size = stat.size;
    newstat.blksize = stat.blksize;
    newstat.blocks = stat.blocks;
    newstat.atime = stat.atime;
    newstat.atime_nsec = stat.atime_nsec;
    newstat.mtime = stat.mtime;
    newstat.mtime_nsec = stat.mtime_nsec;
    newstat.ctime = stat.ctime;
    newstat.ctime_nsec = stat.ctime_nsec;
    return newstat;
}

static struct amd64_stat_ stat_convert_amd64(struct statbuf stat) {
    struct amd64_stat_ out = {};
    out.dev = stat.dev;
    out.ino = stat.inode;
    out.nlink = stat.nlink;
    out.mode = stat.mode;
    out.uid = stat.uid;
    out.gid = stat.gid;
    out.rdev = stat.rdev;
    out.size = stat.size;
    out.blksize = stat.blksize;
    out.blocks = stat.blocks;
    out.atime = stat.atime;
    out.atime_nsec = stat.atime_nsec;
    out.mtime = stat.mtime;
    out.mtime_nsec = stat.mtime_nsec;
    out.ctime = stat.ctime;
    out.ctime_nsec = stat.ctime_nsec;
    return out;
}

static struct arm64_stat_ stat_convert_arm64(struct statbuf stat) {
    struct arm64_stat_ out = {};
    out.dev = stat.dev;
    out.ino = stat.inode;
    out.mode = stat.mode;
    out.nlink = stat.nlink;
    out.uid = stat.uid;
    out.gid = stat.gid;
    out.rdev = stat.rdev;
    out.size = stat.size;
    out.blksize = (sdword_t) stat.blksize;
    out.blocks = stat.blocks;
    out.atime = stat.atime;
    out.atime_nsec = stat.atime_nsec;
    out.mtime = stat.mtime;
    out.mtime_nsec = stat.mtime_nsec;
    out.ctime = stat.ctime;
    out.ctime_nsec = stat.ctime_nsec;
    return out;
}

static int stat_convert_newstat(struct statbuf stat, struct newstat *out) {
    if (stat.dev > UINT32_MAX ||
            stat.mode > UINT16_MAX ||
            stat.nlink > UINT16_MAX ||
            stat.uid > UINT16_MAX ||
            stat.gid > UINT16_MAX ||
            stat.rdev > UINT32_MAX ||
            stat.size > UINT32_MAX ||
            stat.blksize > UINT32_MAX ||
            stat.blocks > UINT32_MAX) {
        return _EOVERFLOW;
    }

    struct newstat newstat = {};
    newstat.dev = stat.dev;
    // Legacy i386 stat has only a 32-bit inode field. Returning EOVERFLOW
    // for large host inode numbers breaks common existence checks on filesystems
    // like APFS, so preserve the low 32 bits instead.
    newstat.ino = (dword_t) stat.inode;
    newstat.mode = stat.mode;
    newstat.nlink = stat.nlink;
    newstat.uid = stat.uid;
    newstat.gid = stat.gid;
    newstat.rdev = stat.rdev;
    newstat.size = stat.size;
    newstat.blksize = stat.blksize;
    newstat.blocks = stat.blocks;
    newstat.atime = stat.atime;
    newstat.atime_nsec = stat.atime_nsec;
    newstat.mtime = stat.mtime;
    newstat.mtime_nsec = stat.mtime_nsec;
    newstat.ctime = stat.ctime;
    newstat.ctime_nsec = stat.ctime_nsec;
    *out = newstat;
    return 0;
}

static void statx_timestamp_convert(struct statx_timestamp_ *timestamp, dword_t sec, dword_t nsec) {
    timestamp->tv_sec = sec;
    timestamp->tv_nsec = nsec;
    timestamp->__reserved = 0;
}

static struct statx_ stat_convert_statx(struct statbuf stat) {
    struct statx_ statx = {};
    statx.stx_mask = STATX_BASIC_STATS_;
    statx.stx_blksize = stat.blksize;
    statx.stx_nlink = stat.nlink;
    statx.stx_uid = stat.uid;
    statx.stx_gid = stat.gid;
    statx.stx_mode = stat.mode;
    statx.stx_ino = stat.inode;
    statx.stx_size = stat.size;
    statx.stx_blocks = stat.blocks;
    statx.stx_rdev_major = dev_major(stat.rdev);
    statx.stx_rdev_minor = dev_minor(stat.rdev);
    statx.stx_dev_major = dev_major(stat.dev);
    statx.stx_dev_minor = dev_minor(stat.dev);
    statx_timestamp_convert(&statx.stx_atime, stat.atime, stat.atime_nsec);
    statx_timestamp_convert(&statx.stx_btime, 0, 0);
    statx_timestamp_convert(&statx.stx_ctime, stat.ctime, stat.ctime_nsec);
    statx_timestamp_convert(&statx.stx_mtime, stat.mtime, stat.mtime_nsec);
    return statx;
}

// Filesystems with no real backing device (tmpfs, proc, devpts...) leave
// stat->dev at 0; stamp the mount's anonymous device so each mount is
// distinguishable by st_dev, as on Linux.
static void stat_stamp_fake_dev(struct mount *mount, struct statbuf *stat) {
    if (stat->dev == 0)
        stat->dev = mount->fake_dev;
}

int generic_fstat(struct fd *fd, struct statbuf *stat) {
    // O_PATH symlink fds have no backend open file; stat the link by path.
    if (fd_is_opath_link(fd))
        return opath_link_fstat(fd, stat);
    memset(stat, 0, sizeof(*stat));
    int err = fd->mount->fs->fstat(fd, stat);
    if (err >= 0)
        stat_stamp_fake_dev(fd->mount, stat);
    return err;
}

int generic_statat_full(struct fd *at, const char *path_raw, struct statbuf *stat, int flags,
        bool *is_mount_root, int *mnt_id) {
    int err;

    bool empty_path = flags & AT_EMPTY_PATH_;
    bool follow_links = !(flags & AT_SYMLINK_NOFOLLOW_);

    if (is_mount_root)
        *is_mount_root = false;
    if (mnt_id)
        *mnt_id = 1;

    char path[MAX_PATH];
    // AT_PWD (fs/path.h) is a non-dereferenceable sentinel meaning "current
    // directory", not a real struct fd* -- `at` lands here whenever a
    // caller passes AT_FDCWD together with AT_EMPTY_PATH and an empty path
    // (systemd's chase() internals do exactly this; it should behave like
    // stat(".")). Route that combination through the normal path-based
    // resolution below (which already knows how to resolve AT_PWD) instead
    // of treating the sentinel as a real fd to fstat -- fd->mount or
    // fd->ops on AT_PWD wraps (mod 2^64) to a low, easily-reached address
    // and previously crashed with SIGSEGV during Arch aarch64 boot.
    if (empty_path && (path_raw[0] == '\0') && at != AT_PWD) {
        if (mnt_id) {
            struct mount *at_mount = fd_is_opath_link(at) ? opath_link_get_mount(at) : at->mount;
            if (at_mount != NULL)
                *mnt_id = mount_id(at_mount);
        }
        return generic_fstat(at, stat);
    } else {
        const char *resolve_path =
            (empty_path && at == AT_PWD && path_raw[0] == '\0') ? "." : path_raw;
        // A plain stat() (not lstat()) following the final symlink of a
        // /proc/PID/fd/N entry needs procfd_statat's special handling, not
        // the generic symlink chase -- see its doc comment. lstat()
        // (!follow_links) reports the symlink itself and is unaffected.
        // mnt_id (rarely requested together with this path shape) stays at
        // its default here rather than threading a mount through the lookup.
        if (follow_links) {
            int procfd_err;
            if (procfd_statat(at, resolve_path, stat, &procfd_err))
                return procfd_err;
            if (procns_statat(at, resolve_path, stat, &procfd_err))
                return procfd_err;
        }
        err = path_normalize(at, resolve_path, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
        if (err < 0)
            return err;
    }

    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // find_mount_and_trim_path trims the mount point prefix off path in
    // place; an empty remainder means path was exactly the mount's root.
    if (is_mount_root)
        *is_mount_root = path[0] == '\0';
    memset(stat, 0, sizeof(*stat));
    // fakefs_stat reads its SQLite ish_stat metadata and the real host stat()
    // as two separate, unlocked steps; without inodes_lock here, a concurrent
    // mkdir/unlink/open(O_CREAT)/etc. (which serialize their own real-op +
    // metadata-update pair under inodes_lock -- see generic_openat) can land
    // in between those two steps and produce a torn combination: metadata
    // describing one entry type together with a live host entry of a
    // different type. mount->fs->stat never blocks, so it's safe to hold
    // the lock across it unconditionally.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = mount->fs->stat(mount, path, stat);
    unlock(&inodes_lock);
    if (err >= 0)
        stat_stamp_fake_dev(mount, stat);
    if (mnt_id)
        *mnt_id = mount_id(mount);
    mount_release(mount);
    return err;
}

int generic_statat_ext(struct fd *at, const char *path_raw, struct statbuf *stat, int flags, bool *is_mount_root) {
    return generic_statat_full(at, path_raw, stat, flags, is_mount_root, NULL);
}

int generic_statat(struct fd *at, const char *path_raw, struct statbuf *stat, int flags) {
    return generic_statat_full(at, path_raw, stat, flags, NULL, NULL);
}

// The `flags` parameter accepts AT_ flags
static dword_t sys_stat_path(fd_t at_f, addr_t path_addr, addr_t statbuf_addr, int flags) {
    int err;
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("stat(at=%d, path=\"%s\", statbuf=0x%x, flags=0x%x)", at_f, path, statbuf_addr, flags);
    struct fd *at = at_fd_for_path(at_f, path);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, flags)) < 0) {
        dpkg_stat_trace_result("stat64", at_f, path, err, NULL, false);
        ldconfig_stat_trace_result("stat64", at_f, path, err, NULL, false);
        http_resolver_trace_path_result("stat", at_f, path, err, flags);
        return err;
    }
    struct newstat64 newstat = stat_convert_newstat64(stat);
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    dpkg_stat_trace_result("stat64", at_f, path, 0, &stat, true);
    ldconfig_stat_trace_result("stat64", at_f, path, 0, &stat, true);
    http_resolver_trace_path_result("stat", at_f, path, 0, flags);
    return 0;
}

static dword_t sys_stat_path_amd64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t statbuf_addr, int flags) {
    int err;
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("stat64_amd64(at=%d, path=\"%s\", statbuf=0x%x, flags=0x%x)", at_f, path, statbuf_addr, flags);
    struct fd *at = at_fd_for_path(at_f, path);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, flags)) < 0)
        return err;
    struct amd64_stat_ guest_stat = stat_convert_amd64(stat);
    if (user_put(statbuf_addr, guest_stat))
        return _EFAULT;
    return 0;
}

dword_t sys_stat_amd64_guest(guest_addr_t path_addr, guest_addr_t statbuf_addr) {
    return sys_stat_path_amd64_guest(AT_FDCWD_, path_addr, statbuf_addr, 0);
}
dword_t sys_stat_amd64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_amd64_guest(path_addr, statbuf_addr);
}

dword_t sys_lstat_amd64_guest(guest_addr_t path_addr, guest_addr_t statbuf_addr) {
    return sys_stat_path_amd64_guest(AT_FDCWD_, path_addr, statbuf_addr, AT_SYMLINK_NOFOLLOW_);
}
dword_t sys_lstat_amd64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_lstat_amd64_guest(path_addr, statbuf_addr);
}

dword_t sys_newfstatat_amd64_guest(fd_t at, guest_addr_t path_addr, guest_addr_t statbuf_addr, dword_t flags) {
    return sys_stat_path_amd64_guest(at, path_addr, statbuf_addr, flags);
}
dword_t sys_newfstatat_amd64(fd_t at, addr_t path_addr, addr_t statbuf_addr, dword_t flags) {
    return sys_newfstatat_amd64_guest(at, path_addr, statbuf_addr, flags);
}

// AArch64 stat family: same 64-bit statbuf sourcing as the amd64 pair
// above, marshalled through struct arm64_stat_ (fs/stat.h) — the
// asm-generic layout, which genuinely differs from amd64's.
dword_t sys_newfstatat_arm64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t statbuf_addr, dword_t flags) {
    int err;
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("newfstatat_arm64(at=%d, path=\"%s\", statbuf=0x%llx, flags=0x%x)", at_f, path,
           (unsigned long long) statbuf_addr, flags);
    struct fd *at = at_fd_for_path(at_f, path);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, (int) flags)) < 0)
        return err;
    struct arm64_stat_ guest_stat = stat_convert_arm64(stat);
    if (user_put(statbuf_addr, guest_stat))
        return _EFAULT;
    return 0;
}

dword_t sys_fstat_arm64_guest(fd_t fd_no, guest_addr_t statbuf_addr) {
    STRACE("fstat_arm64(%d, 0x%llx)", fd_no, (unsigned long long) statbuf_addr);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    struct statbuf stat = {};
    int err = generic_fstat(fd, &stat);
    if (err < 0)
        return err;
    struct arm64_stat_ guest_stat = stat_convert_arm64(stat);
    if (user_put(statbuf_addr, guest_stat))
        return _EFAULT;
    return 0;
}

dword_t sys_stat64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path(AT_FDCWD_, path_addr, statbuf_addr, 0);
}

dword_t sys_lstat64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path(AT_FDCWD_, path_addr, statbuf_addr, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_fstatat64(fd_t at, addr_t path_addr, addr_t statbuf_addr, dword_t flags) {
    return sys_stat_path(at, path_addr, statbuf_addr, flags);
}

dword_t sys_fstat64(fd_t fd_no, addr_t statbuf_addr) {
    STRACE("fstat64(%d, 0x%x)", fd_no, statbuf_addr);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    struct statbuf stat = {};
    int err = generic_fstat(fd, &stat);
    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    if (err < 0) {
        dpkg_stat_trace_result("fstat64", AT_FDCWD_, path, err, NULL, false);
        ldconfig_stat_trace_result("fstat64", AT_FDCWD_, path, err, NULL, false);
        return err;
    }
    struct newstat64 newstat = stat_convert_newstat64(stat);
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    dpkg_stat_trace_result("fstat64", AT_FDCWD_, path, 0, &stat, true);
    ldconfig_stat_trace_result("fstat64", AT_FDCWD_, path, 0, &stat, true);
    return 0;
}

dword_t sys_fstat_amd64_guest(fd_t fd_no, guest_addr_t statbuf_addr) {
    STRACE("fstat_amd64(%d, 0x%x)", fd_no, statbuf_addr);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    struct statbuf stat = {};
    int err = generic_fstat(fd, &stat);
    if (err < 0)
        return err;
    struct amd64_stat_ guest_stat = stat_convert_amd64(stat);
    if (user_put(statbuf_addr, guest_stat))
        return _EFAULT;
    return 0;
}
dword_t sys_fstat_amd64(fd_t fd_no, addr_t statbuf_addr) {
    return sys_fstat_amd64_guest(fd_no, statbuf_addr);
}

// Legacy i386 stat ABI.
static dword_t sys_stat_path_legacy(fd_t at_f, addr_t path_addr, addr_t statbuf_addr, int flags) {
    int err;
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("stat32(at=%d, path=\"%s\", statbuf=0x%x, flags=0x%x)", at_f, path, statbuf_addr, flags);
    struct fd *at = at_fd_for_path(at_f, path);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, flags)) < 0) {
        dpkg_stat_trace_result("stat32", at_f, path, err, NULL, false);
        return err;
    }
    struct newstat newstat;
    err = stat_convert_newstat(stat, &newstat);
    if (err < 0) {
        dpkg_stat_trace_overflow("stat32", at_f, path, &stat);
        return err;
    }
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    dpkg_stat_trace_result("stat32", at_f, path, 0, &stat, true);
    return 0;
}

dword_t sys_stat(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path_legacy(AT_FDCWD_, path_addr, statbuf_addr, 0);
}

dword_t sys_lstat(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path_legacy(AT_FDCWD_, path_addr, statbuf_addr, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_fstat(fd_t fd_no, addr_t statbuf_addr) {
    STRACE("fstat(%d, 0x%x)", fd_no, statbuf_addr);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    struct statbuf stat = {};
    int err = generic_fstat(fd, &stat);
    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    if (err < 0) {
        dpkg_stat_trace_result("fstat32", AT_FDCWD_, path, err, NULL, false);
        return err;
    }
    struct newstat newstat;
    err = stat_convert_newstat(stat, &newstat);
    if (err < 0) {
        dpkg_stat_trace_overflow("fstat32", AT_FDCWD_, path, &stat);
        return err;
    }
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    dpkg_stat_trace_result("fstat32", AT_FDCWD_, path, 0, &stat, true);
    return 0;
}

static dword_t sys_statx_guest_abi(fd_t at_f, guest_addr_t path_addr, dword_t flags, dword_t mask,
        guest_addr_t statxbuf_addr, enum guest_abi abi) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;

    STRACE("statx(%d, \"%s\", %#x, %#x, %#x)", at_f, path, flags, mask, statxbuf_addr);

    dword_t supported_flags = AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_ |
        AT_NO_AUTOMOUNT_ | AT_STATX_SYNC_TYPE_;
    if (flags & ~supported_flags)
        return _EINVAL;

    struct fd *at = at_fd_for_path(at_f, path);
    if (at == NULL)
        return _EBADF;

    // Keep i386 on the old fallback path for now. `working` relied on glibc
    // seeing ENOSYS here and using fstatat64 instead; enabling broad i386
    // statx support regressed dpkg existence checks on APFS-backed roots.
    if (abi == GUEST_ABI_I386) {
        bool empty_path = (flags & AT_EMPTY_PATH_) && path[0] == '\0';
        if (!(empty_path && at_f == 0))
            return _ENOSYS;
    }

    struct statbuf stat = {};
    bool is_mount_root = false;
    int mnt_id = 1;
    int err = generic_statat_full(at, path, &stat, flags, &is_mount_root, &mnt_id);
    if (err < 0) {
        dpkg_stat_trace_result("statx", at_f, path, err, NULL, false);
        return err;
    }

    struct statx_ statx = stat_convert_statx(stat);
    // systemd >= 260 statx()s with STATX_MNT_ID mandatory (xstatx_full's
    // XSTATX_MNT_ID_BEST): a success whose stx_mask lacks a mount-id bit is
    // turned into -EUNATCH ("Protocol driver not attached"), which killed
    // every chase()-based open (system.conf, os-release, machine-id) during
    // Arch aarch64 boot. Report the same id mountinfo shows.
    statx.stx_mask |= STATX_MNT_ID_;
    statx.stx_mnt_id = (qword_t) mnt_id;
    // Modern userspace (systemd's fd_is_mount_point()/path_is_mount_point())
    // determines mount-root-ness from this attribute bit instead of walking
    // /proc/self/mountinfo; leaving it unset made statx() on /proc, /sys, and
    // /dev look like ordinary directories to systemd, which treated that as
    // a hard failure ("Failed to determine whether ... is a mount point")
    // and froze boot on Arch Linux ARM's aarch64 guest.
    statx.stx_attributes_mask |= STATX_ATTR_MOUNT_ROOT_;
    if (is_mount_root)
        statx.stx_attributes |= STATX_ATTR_MOUNT_ROOT_;
    if (user_write(statxbuf_addr, &statx, sizeof(statx)))
        return _EFAULT;
    dpkg_stat_trace_result("statx", at_f, path, 0, &stat, true);
    return 0;
}

dword_t sys_statx_guest(fd_t at_f, guest_addr_t path_addr, dword_t flags, dword_t mask, guest_addr_t statxbuf_addr) {
    return sys_statx_guest_abi(at_f, path_addr, flags, mask, statxbuf_addr, GUEST_ABI_I386);
}

dword_t sys_statx_amd64(fd_t at_f, addr_t path_addr, dword_t flags, dword_t mask, addr_t statxbuf_addr) {
    return sys_statx_guest_abi(at_f, path_addr, flags, mask, statxbuf_addr, GUEST_ABI_AMD64);
}

dword_t sys_statx_amd64_guest(fd_t at_f, guest_addr_t path_addr, dword_t flags, dword_t mask, guest_addr_t statxbuf_addr) {
    return sys_statx_guest_abi(at_f, path_addr, flags, mask, statxbuf_addr, GUEST_ABI_AMD64);
}

dword_t sys_statx(fd_t at_f, addr_t path_addr, dword_t flags, dword_t mask, addr_t statxbuf_addr) {
    return sys_statx_guest(at_f, path_addr, flags, mask, statxbuf_addr);
}
