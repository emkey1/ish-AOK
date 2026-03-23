#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/dev.h"
#include "fs/fd.h"
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

#define AT_STATX_SYNC_TYPE_     0x6000

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

static int stat_convert_newstat(struct statbuf stat, struct newstat *out) {
    if (stat.dev > UINT32_MAX ||
            stat.inode > UINT32_MAX ||
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

int generic_statat(struct fd *at, const char *path_raw, struct statbuf *stat, int flags) {
    int err;

    bool empty_path = flags & AT_EMPTY_PATH_;
    bool follow_links = !(flags & AT_SYMLINK_NOFOLLOW_);

    char path[MAX_PATH];
    if (empty_path && (strcmp(path_raw, "") == 0)) {
        memset(stat, 0, sizeof(*stat));
        return at->mount->fs->fstat(at, stat);
    } else {
        err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
        if (err < 0)
            return err;
    }

    struct mount *mount = find_mount_and_trim_path(path);
    memset(stat, 0, sizeof(*stat));
    err = mount->fs->stat(mount, path, stat);
    mount_release(mount);
    return err;
}

// TODO get rid of this and maybe everything else in the file
static struct fd *at_fd(fd_t f) {
    if (f == AT_FDCWD_)
        return AT_PWD;
    return f_get(f);
}

// The `flags` parameter accepts AT_ flags
static dword_t sys_stat_path(fd_t at_f, addr_t path_addr, addr_t statbuf_addr, int flags) {
    int err;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("stat(at=%d, path=\"%s\", statbuf=0x%x, flags=0x%x)", at_f, path, statbuf_addr, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, flags)) < 0)
        return err;
    struct newstat64 newstat = stat_convert_newstat64(stat);
    if (user_put(statbuf_addr, newstat))
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
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0)
        return err;
    if (strcmp(current->comm, "http") == 0 && (fd_no == 0 || fd_no == 1 || fd_no == 2)) {
        printk("APTTRACE fstat64 pid=%d fd=%d mode=%#x dev=%#llx ino=%#llx size=%#llx blksize=%u blocks=%#llx\n",
            current->pid, fd_no, stat.mode,
            (unsigned long long) stat.dev, (unsigned long long) stat.inode,
            (unsigned long long) stat.size, stat.blksize,
            (unsigned long long) stat.blocks);
    }
    struct newstat64 newstat = stat_convert_newstat64(stat);
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    return 0;
}

// Legacy i386 stat ABI.
static dword_t sys_stat_path_legacy(fd_t at_f, addr_t path_addr, addr_t statbuf_addr, int flags) {
    int err;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("stat32(at=%d, path=\"%s\", statbuf=0x%x, flags=0x%x)", at_f, path, statbuf_addr, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    struct statbuf stat = {};
    if ((err = generic_statat(at, path, &stat, flags)) < 0)
        return err;
    struct newstat newstat;
    err = stat_convert_newstat(stat, &newstat);
    if (err < 0)
        return err;
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
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
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0)
        return err;
    if (strcmp(current->comm, "http") == 0 && (fd_no == 0 || fd_no == 1 || fd_no == 2)) {
        printk("APTTRACE fstat pid=%d fd=%d mode=%#x dev=%#llx ino=%#llx size=%#llx blksize=%u blocks=%#llx\n",
            current->pid, fd_no, stat.mode,
            (unsigned long long) stat.dev, (unsigned long long) stat.inode,
            (unsigned long long) stat.size, stat.blksize,
            (unsigned long long) stat.blocks);
    }
    struct newstat newstat;
    err = stat_convert_newstat(stat, &newstat);
    if (err < 0)
        return err;
    if (user_put(statbuf_addr, newstat))
        return _EFAULT;
    return 0;
}

dword_t sys_statx(fd_t at_f, addr_t path_addr, dword_t flags, dword_t mask, addr_t statxbuf_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;

    STRACE("statx(%d, \"%s\", %#x, %#x, %#x)", at_f, path, flags, mask, statxbuf_addr);

    dword_t supported_flags = AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_ |
        AT_NO_AUTOMOUNT_ | AT_STATX_SYNC_TYPE_;
    if (flags & ~supported_flags)
        return _EINVAL;

    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;

    // Be conservative until the full i386 time64/statx ABI is verified.
    // The tar extraction path needs empty-path metadata on stdin; broader
    // statx use in glibc 2.36+ is better served by libc's fstatat64 fallback
    // than by a half-correct statx payload.
    bool empty_path = (flags & AT_EMPTY_PATH_) && strcmp(path, "") == 0;
    if (!(empty_path && at_f == 0))
        return _ENOSYS;

    struct statbuf stat = {};
    int err = generic_statat(at, path, &stat, flags);
    if (err < 0)
        return err;

    struct statx_ statx = stat_convert_statx(stat);
    if (user_write(statxbuf_addr, &statx, sizeof(statx)))
        return _EFAULT;
    return 0;
}
