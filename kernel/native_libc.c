// Implementation of the SmallCLUE libc shim. See kernel/native_libc.h for
// why every one of these exists.
//
// This file must NOT be compiled with the shim force-included -- it is the
// thing the shim redirects *to*, and needs the real libc.

#define NATIVE_LIBC_NO_REDIRECT

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/native_syscall.h"
#include "kernel/task.h"
#include "fs/tty.h"
#include "kernel/uts.h"
#include "util/list.h"
#include "fs/fd.h"
#include "fs/path.h"

// AOK's errno values are negative LINUX errnos, and the numbering is not the
// host's. Flipping the sign and storing that in `errno` is wrong the moment
// the two disagree: SmallCLUE formats its messages with the HOST's strerror,
// so Linux ENOSYS (38) came out as macOS ENOTSOCK -- which is exactly the
// "df: /: Socket operation on non-socket" that surfaced this. ENOENT and a
// handful of others happen to share a number, which is why most errors looked
// fine and only the unusual ones were nonsense.
//
// Anything not listed maps to EINVAL rather than being passed through, so a
// new guest errno produces a wrong-but-plausible message instead of a wildly
// misleading one from an unrelated part of the host's table.
static int nlibc_host_errno(int guest_err) {
    switch (guest_err < 0 ? -guest_err : guest_err) {
        case 1:  return EPERM;
        case 2:  return ENOENT;
        case 3:  return ESRCH;
        case 4:  return EINTR;
        case 5:  return EIO;
        case 6:  return ENXIO;
        case 7:  return E2BIG;
        case 8:  return ENOEXEC;
        case 9:  return EBADF;
        case 10: return ECHILD;
        case 11: return EAGAIN;
        case 12: return ENOMEM;
        case 13: return EACCES;
        case 14: return EFAULT;
        case 16: return EBUSY;
        case 17: return EEXIST;
        case 18: return EXDEV;
        case 19: return ENODEV;
        case 20: return ENOTDIR;
        case 21: return EISDIR;
        case 22: return EINVAL;
        case 23: return ENFILE;
        case 24: return EMFILE;
        case 25: return ENOTTY;
        case 26: return ETXTBSY;
        case 27: return EFBIG;
        case 28: return ENOSPC;
        case 29: return ESPIPE;
        case 30: return EROFS;
        case 31: return EMLINK;
        case 32: return EPIPE;
        case 33: return EDOM;
        case 34: return ERANGE;
        case 36: return ENAMETOOLONG;
        case 38: return ENOSYS;   // Linux 38 is macOS 78; this is the one that bit
        case 39: return ENOTEMPTY;
        case 40: return ELOOP;
        default: return EINVAL;
    }
}

static int nlibc_tty_ioctl(int fd_no, dword_t cmd, void *arg, size_t size, bool out);

// Same reason as native_have_task: refuse rather than dereference NULL.
#define NLIBC_NEED_TASK() do { if (!native_have_task()) return nlibc_fail(_EFAULT); } while (0)

static int nlibc_fail(int guest_err) {
    errno = nlibc_host_errno(guest_err);
    return -1;
}

// The tail nearly every entry point shares: a negative guest errno becomes -1
// with a translated `errno`; anything else is the result. Widened to long so
// the same helper serves lseek's off_t and read's ssize_t.
static long nlibc_ret(sqword_t res) {
    return res < 0 ? nlibc_fail((int) res) : (long) res;
}

// ------------------------------------------------------------ flag translation
//
// Host and guest constants are different numbers for the same ideas, so every
// flag word crossing the shim has to be mapped rather than passed through. An
// unmapped bit is DROPPED, which shows up as "that option did not take"
// instead of the guest acting on some unrelated flag.
struct nlibc_flagmap { unsigned long host; dword_t guest; };

#define NLIBC_MAP_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static dword_t nlibc_flags_to_guest(unsigned long host, const struct nlibc_flagmap *map, size_t n) {
    dword_t out = 0;
    for (size_t i = 0; i < n; i++)
        if (host & map[i].host)
            out |= map[i].guest;
    return out;
}
static unsigned long nlibc_flags_to_host(dword_t guest, const struct nlibc_flagmap *map, size_t n) {
    unsigned long out = 0;
    for (size_t i = 0; i < n; i++)
        if (guest & map[i].guest)
            out |= map[i].host;
    return out;
}

// open(2) flags. Only the two access-mode bits agree; everything else moves.
// SmallCLUE's O_CREAT is Darwin's 0x0200, which in the guest's numbering is
// O_NOCTTY|O_NONBLOCK -- so a create quietly became an open-existing before
// this table, and every applet that writes a new file failed with ENOENT
// against a rootfs that did not already have the target.
//
// The guest column is the asm-generic (arm64) numbering, which is what
// kernel/native_syscall.h speaks; kernel/calls.c's openat case maps it onward
// to the fs layer's internal values.
static const struct nlibc_flagmap nlibc_open_flags[] = {
    { O_CREAT,     0x40 },     { O_EXCL,      0x80 },
    { O_NOCTTY,    0x100 },    { O_TRUNC,     0x200 },
    { O_APPEND,    0x400 },    { O_NONBLOCK,  0x800 },
    { O_DIRECTORY, 0x4000 },   { O_NOFOLLOW,  0x8000 },
    { O_CLOEXEC,   0x80000 },  { O_SYNC,      0x101000 },
};

static dword_t nlibc_open_flags_to_guest(int flags) {
    // O_RDONLY/O_WRONLY/O_RDWR are 0/1/2 on both sides.
    return (dword_t) (flags & O_ACCMODE) | nlibc_flags_to_guest((unsigned long) flags,
            nlibc_open_flags, NLIBC_MAP_COUNT(nlibc_open_flags));
}

// Darwin's AT_FDCWD is -2, the guest's is -100. Any other value is a real
// descriptor, and the shim's descriptors ARE the guest's, so it passes through.
static int nlibc_at_fd(int dirfd) {
    return dirfd == AT_FDCWD ? AT_FDCWD_ : dirfd;
}

// Marshals a path and reports the two ways it can fail apart: a NULL path is
// the caller's EFAULT, an empty scratch is ENOMEM.
#define NLIBC_PATH(name, path) \
    guest_addr_t name = native_scratch_str(path); \
    if (name == 0) \
        return nlibc_fail((path) == NULL ? _EFAULT : _ENOMEM)

// Signal numbers. The two agree through SIGABRT and then diverge completely:
// Linux's SIGUSR1 is 10, which on Darwin is SIGBUS; Linux's SIGCHLD is 17,
// which on Darwin is SIGSTOP. A raw number passed through would have `timeout`
// stopping its child where it meant to reap it, and `kill -USR1` hitting
// something else entirely.
struct nlibc_signalmap { int host; int guest; };
static const struct nlibc_signalmap nlibc_signals[] = {
    { SIGHUP, SIGHUP_ },   { SIGINT, SIGINT_ },     { SIGQUIT, SIGQUIT_ },
    { SIGILL, SIGILL_ },   { SIGTRAP, SIGTRAP_ },   { SIGABRT, SIGABRT_ },
    { SIGBUS, SIGBUS_ },   { SIGFPE, SIGFPE_ },     { SIGKILL, SIGKILL_ },
    { SIGUSR1, SIGUSR1_ }, { SIGSEGV, SIGSEGV_ },   { SIGUSR2, SIGUSR2_ },
    { SIGPIPE, SIGPIPE_ }, { SIGALRM, SIGALRM_ },   { SIGTERM, SIGTERM_ },
    { SIGCHLD, SIGCHLD_ }, { SIGCONT, SIGCONT_ },   { SIGSTOP, SIGSTOP_ },
    { SIGTSTP, SIGTSTP_ }, { SIGTTIN, SIGTTIN_ },   { SIGTTOU, SIGTTOU_ },
    { SIGURG, SIGURG_ },   { SIGXCPU, SIGXCPU_ },   { SIGXFSZ, SIGXFSZ_ },
    { SIGVTALRM, SIGVTALRM_ }, { SIGPROF, SIGPROF_ }, { SIGWINCH, SIGWINCH_ },
    { SIGIO, SIGIO_ },     { SIGSYS, SIGSYS_ },
};

// 0 for a signal the other side has no name for -- Darwin's SIGEMT and SIGINFO
// have no Linux number, and Linux's SIGSTKFLT and SIGPWR have no Darwin one.
// The kernel rejects 0 with EINVAL, which is the honest answer.
static int nlibc_signal_to_guest(int host_sig) {
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_signals); i++)
        if (nlibc_signals[i].host == host_sig)
            return nlibc_signals[i].guest;
    return 0;
}

// ------------------------------------------------------------ descriptors

int nlibc_openat(int dirfd, const char *path, int flags, ...) {
    NATIVE_FRAME;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_openat, nlibc_at_fd(dirfd),
            guest_path, nlibc_open_flags_to_guest(flags), mode));
}

int nlibc_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    return nlibc_openat(AT_FDCWD, path, flags, mode);
}

int nlibc_close(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_close, fd_no));
}

ssize_t nlibc_read(int fd_no, void *buf, size_t n) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_alloc(n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_read, fd_no, guest_buf, n);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return nlibc_ret(res);
}

ssize_t nlibc_write(int fd_no, const void *buf, size_t n) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_put(buf, n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    return nlibc_ret(native_syscall(NATIVE_SYS_write, fd_no, guest_buf, n));
}

off_t nlibc_lseek(int fd_no, off_t off, int whence) {
    // SEEK_SET/SEEK_CUR/SEEK_END are 0/1/2 on both sides.
    return (off_t) nlibc_ret(native_syscall(NATIVE_SYS_lseek, fd_no, off, whence));
}

int nlibc_access(const char *path, int mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    // F_OK/R_OK/W_OK/X_OK are 0/4/2/1 on both sides.
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_faccessat, AT_FDCWD_,
            guest_path, mode, 0));
}

// ------------------------------------------------------------------- stat

// The syscall layer speaks the asm-generic ABI, so what comes back is the
// arm64 struct stat -- fs/stat.h's own definition of it, rather than a second
// copy here that could drift from the one the kernel writes.
static void nlibc_guest_stat_to_host(const struct arm64_stat_ *in, struct stat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = (dev_t) in->dev;
    out->st_ino = (ino_t) in->ino;
    out->st_mode = (mode_t) in->mode;
    out->st_nlink = (nlink_t) in->nlink;
    out->st_uid = (uid_t) in->uid;
    out->st_gid = (gid_t) in->gid;
    out->st_rdev = (dev_t) in->rdev;
    out->st_size = (off_t) in->size;
    out->st_blksize = (blksize_t) in->blksize;
    out->st_blocks = (blkcnt_t) in->blocks;
    out->st_atime = (time_t) in->atime;
    out->st_mtime = (time_t) in->mtime;
    out->st_ctime = (time_t) in->ctime;
}

static int nlibc_statat(int dirfd, const char *path, struct stat *st, dword_t at_flags) {
    NATIVE_FRAME;
    if (st == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_st = native_scratch_alloc(sizeof(struct arm64_stat_));
    if (guest_st == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_newfstatat, nlibc_at_fd(dirfd),
            guest_path, guest_st, at_flags);
    if (res < 0)
        return nlibc_fail((int) res);
    struct arm64_stat_ guest_stat;
    if (native_scratch_get(&guest_stat, guest_st, sizeof(guest_stat)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_stat_to_host(&guest_stat, st);
    return 0;
}

int nlibc_stat(const char *path, struct stat *st) { return nlibc_statat(AT_FDCWD, path, st, 0); }
int nlibc_lstat(const char *path, struct stat *st) {
    return nlibc_statat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW_);
}

int nlibc_fstat(int fd_no, struct stat *st) {
    NATIVE_FRAME;
    if (st == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_st = native_scratch_alloc(sizeof(struct arm64_stat_));
    if (guest_st == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_fstat, fd_no, guest_st);
    if (res < 0)
        return nlibc_fail((int) res);
    struct arm64_stat_ guest_stat;
    if (native_scratch_get(&guest_stat, guest_st, sizeof(guest_stat)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_stat_to_host(&guest_stat, st);
    return 0;
}

ssize_t nlibc_readlink(const char *path, char *buf, size_t bufsize) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_buf = native_scratch_alloc(bufsize);
    if (guest_buf == 0 && bufsize > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_readlinkat, AT_FDCWD_, guest_path,
            guest_buf, bufsize);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return nlibc_ret(res);
}

char *nlibc_realpath(const char *path, char *resolved) {
    // realpath(3)'s contract is that the caller's buffer holds PATH_MAX bytes
    // -- 1024 on Darwin. AOK's MAX_PATH is 4096, and writing that much into a
    // caller's PATH_MAX buffer smashed its stack on EVERY call: df's
    //     char resolved[PATH_MAX]; realpath(query_path, resolved)
    // aborted in __stack_chk_fail before printing a single row. The two
    // constants are not interchangeable and the caller's one governs.
    //
    // Canonicalisation goes through AOK rather than string-joining: opening
    // the path and asking for its path is the same resolution the guest gets,
    // symlinks and mounts included.
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0) {
        nlibc_fail(err);
        return NULL;
    }
    char canonical[MAX_PATH];
    err = generic_getpath(fd, canonical);
    native_close(fd);
    if (err < 0) {
        nlibc_fail(err);
        return NULL;
    }
    if (canonical[0] == '\0')
        strcpy(canonical, "/");

    size_t need = strlen(canonical) + 1;
    if (resolved == NULL) {
        char *out = malloc(need);
        if (out == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        memcpy(out, canonical, need);
        return out;
    }
    if (need > PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, canonical, need);
    return resolved;
}

// ------------------------------------------------------------- directories

// getdents64's record layout, which is syscall ABI rather than an internal
// detail -- fs/dir.c writes exactly this. Mirrored rather than shared because
// the definition there is private to that file; if the ABI ever moved, both
// would have to move together anyway.
struct nlibc_dirent64 {
    uint64_t inode;
    uint64_t offset;
    uint16_t reclen;
    uint8_t type;
    char name[];
} __attribute__((packed));

// DIR is opaque to callers -- they only ever hand it back to readdir/closedir
// -- so a private struct behind the same pointer is safe and avoids trying to
// synthesize a host DIR. The buffer is what makes this one syscall per batch
// of entries rather than one per entry.
struct nlibc_dir {
    int fd;
    size_t filled;
    size_t pos;
    struct dirent ent;
    char buf[8192];
};

DIR *nlibc_opendir(const char *path) {
    int fd = nlibc_open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;
    struct nlibc_dir *dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        nlibc_close(fd);
        errno = ENOMEM;
        return NULL;
    }
    dir->fd = fd;
    return (DIR *) dir;
}

struct dirent *nlibc_readdir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }
    if (dir->pos >= dir->filled) {
        NATIVE_FRAME;
        guest_addr_t guest_buf = native_scratch_alloc(sizeof(dir->buf));
        if (guest_buf == 0) {
            errno = ENOMEM;
            return NULL;
        }
        sqword_t res = native_syscall(NATIVE_SYS_getdents64, dir->fd, guest_buf,
                sizeof(dir->buf));
        if (res <= 0) {
            if (res < 0)
                nlibc_fail((int) res);
            return NULL;   // end of directory, or error
        }
        if (native_scratch_get(dir->buf, guest_buf, (size_t) res) < 0) {
            nlibc_fail(_EFAULT);
            return NULL;
        }
        dir->filled = (size_t) res;
        dir->pos = 0;
    }

    const struct nlibc_dirent64 *entry = (const void *) (dir->buf + dir->pos);
    // A record short enough to overlap its own header, or one claiming to run
    // past what was read, would walk this loop off the buffer.
    if (entry->reclen < sizeof(*entry) + 1 ||
            dir->pos + entry->reclen > dir->filled) {
        nlibc_fail(_EIO);
        return NULL;
    }
    dir->pos += entry->reclen;

    memset(&dir->ent, 0, sizeof(dir->ent));
    dir->ent.d_ino = (ino_t) entry->inode;
    dir->ent.d_type = entry->type;
    size_t name_max = entry->reclen - sizeof(*entry);
    if (name_max > sizeof(dir->ent.d_name) - 1)
        name_max = sizeof(dir->ent.d_name) - 1;
    memcpy(dir->ent.d_name, entry->name, name_max);
    dir->ent.d_name[name_max] = '\0';
    dir->ent.d_reclen = sizeof(dir->ent);
    return &dir->ent;
}

int nlibc_closedir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL)
        return nlibc_fail(_EBADF);
    int fd = dir->fd;
    free(dir);
    return nlibc_close(fd);
}

// ---------------------------------------------------------------- mutation

// AT_REMOVEDIR, which fs/dir.c's unlinkat reads. Named here because kernel/fs.c
// keeps its own private copy rather than exporting one.
#define NLIBC_AT_REMOVEDIR 0x200

int nlibc_unlink(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_unlinkat, AT_FDCWD_, guest_path, 0));
}
int nlibc_rmdir(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_unlinkat, AT_FDCWD_, guest_path,
            NLIBC_AT_REMOVEDIR));
}
int nlibc_mkdir(const char *path, mode_t mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_mkdirat, AT_FDCWD_, guest_path, mode));
}
int nlibc_rename(const char *from, const char *to) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_from, from);
    NLIBC_PATH(guest_to, to);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_renameat, AT_FDCWD_, guest_from,
            AT_FDCWD_, guest_to));
}
int nlibc_symlink(const char *target, const char *linkpath) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_target, target);
    NLIBC_PATH(guest_link, linkpath);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_symlinkat, guest_target,
            AT_FDCWD_, guest_link));
}
int nlibc_link(const char *from, const char *to) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_from, from);
    NLIBC_PATH(guest_to, to);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_linkat, AT_FDCWD_, guest_from,
            AT_FDCWD_, guest_to));
}

int nlibc_chmod(const char *path, mode_t mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchmodat, AT_FDCWD_, guest_path, mode));
}
static int nlibc_chown_common(const char *path, uid_t uid, gid_t gid, dword_t at_flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchownat, AT_FDCWD_, guest_path,
            uid, gid, at_flags));
}
int nlibc_chown(const char *path, uid_t uid, gid_t gid) {
    return nlibc_chown_common(path, uid, gid, 0);
}
int nlibc_lchown(const char *path, uid_t uid, gid_t gid) {
    return nlibc_chown_common(path, uid, gid, AT_SYMLINK_NOFOLLOW_);
}
int nlibc_truncate(const char *path, off_t len) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_truncate, guest_path, len));
}

// --------------------------------------------------------------------- cwd

char *nlibc_getcwd(char *buf, size_t size) {
    NATIVE_FRAME;
    char path[MAX_PATH];
    guest_addr_t guest_path = native_scratch_alloc(sizeof(path));
    if (guest_path == 0) {
        errno = ENOMEM;
        return NULL;
    }
    sqword_t res = native_syscall(NATIVE_SYS_getcwd, guest_path, sizeof(path));
    if (res < 0 || native_scratch_get(path, guest_path, sizeof(path)) < 0) {
        nlibc_fail(res < 0 ? (int) res : _EFAULT);
        return NULL;
    }
    path[sizeof(path) - 1] = '\0';
    if (path[0] == '\0')
        strcpy(path, "/");
    if (buf == NULL) {
        char *out = strdup(path);
        if (out == NULL)
            errno = ENOMEM;
        return out;
    }
    // The caller's buffer governs, and it is usually Darwin's PATH_MAX (1024)
    // against AOK's MAX_PATH (4096) -- see the note in nlibc_realpath, where
    // confusing the two smashed df's stack on every call.
    if (strlen(path) + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, path);
    return buf;
}

int nlibc_chdir(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_chdir, guest_path));
}

// ------------------------------------------------------------------- stdio

// funopen() gives a real FILE* driven by our callbacks, which is what lets the
// ~1600 fprintf/fputs/fread/fwrite/fclose call sites keep working untouched:
// only where the handle comes from changes.
static int nlibc_file_read(void *cookie, char *buf, int n) {
    ssize_t res = nlibc_read((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static int nlibc_file_write(void *cookie, const char *buf, int n) {
    ssize_t res = nlibc_write((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static fpos_t nlibc_file_seek(void *cookie, fpos_t off, int whence) {
    return nlibc_lseek((int) (intptr_t) cookie, (off_t) off, whence);
}
static int nlibc_file_close(void *cookie) {
    return nlibc_close((int) (intptr_t) cookie);
}
// The standard streams must not close the guest's fd 0/1/2 when the FILE is
// closed or the program exits.
static int nlibc_file_close_noop(void *cookie) { (void) cookie; return 0; }

static FILE *nlibc_file_wrap(int fd, int closes_fd) {
    FILE *f = funopen((void *) (intptr_t) fd, nlibc_file_read, nlibc_file_write,
            nlibc_file_seek, closes_fd ? nlibc_file_close : nlibc_file_close_noop);
    if (f == NULL)
        errno = ENOMEM;
    return f;
}

static int nlibc_mode_to_flags(const char *mode) {
    if (mode == NULL || mode[0] == '\0')
        return O_RDONLY;
    int flags;
    switch (mode[0]) {
        case 'r': flags = strchr(mode, '+') ? O_RDWR : O_RDONLY; break;
        case 'w': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
        case 'a': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
        default:  flags = O_RDONLY; break;
    }
    return flags;
}

FILE *nlibc_fopen(const char *path, const char *mode) {
    int fd = nlibc_open(path, nlibc_mode_to_flags(mode));
    if (fd < 0)
        return NULL;
    FILE *f = nlibc_file_wrap(fd, 1);
    if (f == NULL)
        nlibc_close(fd);
    return f;
}

FILE *nlibc_freopen(const char *path, const char *mode, FILE *stream) {
    if (stream != NULL)
        fclose(stream);
    return nlibc_fopen(path, mode);
}

FILE *nlibc_fdopen(int fd, const char *mode) {
    (void) mode;
    return nlibc_file_wrap(fd, 1);
}

// One wrapper per standard stream, made on demand and kept, so repeated use
// does not leak a FILE each time and buffering behaves consistently.
static FILE *nlibc_std[3];

static FILE *nlibc_std_stream(int fd) {
    if (nlibc_std[fd] == NULL) {
        nlibc_std[fd] = nlibc_file_wrap(fd, 0);
        if (nlibc_std[fd] != NULL) {
            // A funopen stream is fully buffered by default, and a native
            // program exits by returning rather than through exit(), so
            // nothing would ever flush it -- printf output simply vanished.
            // Match what a terminal program expects instead: stderr
            // unbuffered, stdout line-buffered. nlibc_flush_std() below still
            // catches the tail when stdout is a pipe.
            setvbuf(nlibc_std[fd], NULL, fd == 2 ? _IONBF : _IOLBF, BUFSIZ);
        }
    }
    return nlibc_std[fd];
}

// Called once a native program returns, before its task exits: a return from
// main() is not exit(), so the C runtime never flushes these for us.
void nlibc_flush_std(void) {
    for (int i = 0; i < 3; i++)
        if (nlibc_std[i] != NULL)
            fflush(nlibc_std[i]);
}
FILE *nlibc_stdin(void)  { return nlibc_std_stream(0); }
FILE *nlibc_stdout(void) { return nlibc_std_stream(1); }
FILE *nlibc_stderr(void) { return nlibc_std_stream(2); }

int nlibc_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(nlibc_stdout(), fmt, args);
    va_end(args);
    return n;
}
int nlibc_puts(const char *s) {
    if (fputs(s, nlibc_stdout()) < 0)
        return EOF;
    return fputc('\n', nlibc_stdout()) == EOF ? EOF : 0;
}
int nlibc_putchar(int c) { return fputc(c, nlibc_stdout()); }
void nlibc_perror(const char *s) {
    if (s != NULL && s[0] != '\0')
        fprintf(nlibc_stderr(), "%s: %s\n", s, strerror(errno));
    else
        fprintf(nlibc_stderr(), "%s\n", strerror(errno));
}

// ----------------------------------------------------------------- process
//
// A native program's child is a real AOK task, not a host process -- there is
// only one of those. native_spawn/native_waitpid (kernel/native_io.h) do the
// work; these give it the shapes a C program expects.
//
// exec-in-place is the one that does not fit: execv replaces the CALLER, and a
// native program cannot be replaced by a guest image in the middle of a C
// function. It is implemented as spawn-then-exit, which is observably the same
// to everything except the caller itself -- the pid changes. nohup and chroot
// use it and do not care; a program that depends on keeping its pid across
// exec would notice.

static int nlibc_exec_common(const char *path, char *const argv[], int search_path) {
    if (path == NULL || argv == NULL)
        return nlibc_fail(_EFAULT);

    char resolved[MAX_PATH];
    if (search_path && strchr(path, '/') == NULL) {
        if (native_path_search(path, resolved, sizeof(resolved)) < 0)
            return nlibc_fail(_ENOENT);
        path = resolved;
    }

    dword_t pid = 0;
    int err = native_spawn(path, argv, native_env_vector(), &pid);
    if (err < 0)
        return nlibc_fail(err);

    // Stand in for "this process became that program": wait for it and exit
    // with its status, so the caller's caller sees what it expects.
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        nlibc_exit(127);
    nlibc_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

int nlibc_execv(const char *path, char *const argv[]) {
    return nlibc_exec_common(path, argv, 0);
}
int nlibc_execvp(const char *file, char *const argv[]) {
    return nlibc_exec_common(file, argv, 1);
}
int nlibc_execl(const char *path, const char *arg0, ...) {
    // Collect the varargs into a vector; callers here pass short lists.
    enum { MAX_ARGS = 64 };
    char *argv[MAX_ARGS];
    size_t n = 0;
    va_list ap;
    va_start(ap, arg0);
    argv[n++] = (char *) arg0;
    while (n < MAX_ARGS - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[n++] = next;
    }
    va_end(ap);
    argv[n] = NULL;
    return nlibc_exec_common(path, argv, 0);
}

int nlibc_system(const char *command) {
    if (command == NULL)
        return 1;   // a shell is available
    char *argv[] = { (char *) "/bin/sh", (char *) "-c", (char *) command, NULL };
    dword_t pid = 0;
    int err = native_spawn("/bin/sh", argv, native_env_vector(), &pid);
    if (err < 0)
        return nlibc_fail(err);
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        return -1;
    return status;
}

pid_t nlibc_waitpid(pid_t pid, int *status, int options) {
    int res = native_waitpid((dword_t) pid, status, options);
    return res < 0 ? nlibc_fail(res) : res;
}
pid_t nlibc_wait(int *status) {
    return nlibc_waitpid(-1, status, 0);
}

// popen needs a pipe between parent and child, which needs the guest's own
// pipe and dup2 on the child side -- neither is wired yet. Refusing keeps it
// honest rather than half-working.
FILE *nlibc_popen(const char *command, const char *mode) {
    (void) command; (void) mode;
    errno = ENOSYS;
    return NULL;
}
int nlibc_pclose(FILE *stream) {
    (void) stream;
    return nlibc_fail(_ENOSYS);
}

// ------------------------------------------------- remaining host-libc holes
//
// Everything below exists so that NOTHING in SmallCLUE can reach the host's
// libc for a filesystem- or process-shaped call. That matters most on the
// macOS CLI build, where these would otherwise succeed against the developer's
// own machine: a host fork(), a host chroot(), a host kill(). Failing with
// ENOSYS is always better than silently acting on the wrong system.
//
// Most of these were refusals when the shim reimplemented calls by hand -- a
// dup2 or a poll was more machinery than the applet needing it was worth. Over
// the syscall dispatcher they are a few lines each, which is the point.

int nlibc_dup(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_dup, fd_no));
}

int nlibc_dup2(int oldfd, int newfd) {
    // dup2(fd, fd) is a no-op returning fd, where dup3 rejects it with EINVAL.
    // The check that the descriptor is open still has to happen.
    if (oldfd == newfd) {
        if (native_syscall(NATIVE_SYS_fcntl, oldfd, F_GETFD, 0) < 0)
            return nlibc_fail(_EBADF);
        return newfd;
    }
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_dup3, oldfd, newfd, 0));
}

// fcntl commands 0-4 (DUPFD/GETFD/SETFD/GETFL/SETFL) happen to agree between
// Darwin and Linux; everything above them does not (Darwin's F_GETLK is 7,
// Linux's is 5), and the lock commands would need struct flock translated as
// well. Refusing the rest is better than acting on the wrong command.
int nlibc_fcntl(int fd_no, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);

    switch (cmd) {
        case F_DUPFD:
        case F_GETFD:
        case F_SETFD:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, cmd, arg));
        case F_DUPFD_CLOEXEC:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, 1030, arg));
        case F_GETFL: {
            sqword_t res = native_syscall(NATIVE_SYS_fcntl, fd_no, F_GETFL, 0);
            if (res < 0)
                return nlibc_fail((int) res);
            // Comes back in the guest's numbering, and the caller will test it
            // against Darwin's.
            return (int) (((dword_t) res & 3) | (int) nlibc_flags_to_host((dword_t) res,
                    nlibc_open_flags, NLIBC_MAP_COUNT(nlibc_open_flags)));
        }
        case F_SETFL:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, F_SETFL,
                    nlibc_open_flags_to_guest((int) arg)));
        default:
            return nlibc_fail(_ENOSYS);
    }
}

int nlibc_mkstemp(char *template) {
    // mkstemp's contract is create-exclusively and report the name used. The
    // pattern is the caller's buffer, so the substitution has to happen there.
    if (template == NULL) return nlibc_fail(_EINVAL);
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0)
        return nlibc_fail(_EINVAL);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 128; attempt++) {
        for (int i = 0; i < 6; i++)
            template[len - 6 + i] = alphabet[(unsigned) rand() % (sizeof(alphabet) - 1)];
        int fd = nlibc_open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    return nlibc_fail(_EEXIST);
}

// utimensat's UTIME_NOW/UTIME_OMIT, which the guest reads out of tv_nsec.
#define NLIBC_UTIME_NOW  ((1l << 30) - 1l)
#define NLIBC_UTIME_OMIT ((1l << 30) - 2l)

// The guest's struct timespec is two 64-bit words, which is also Darwin's on
// arm64 -- but writing the pair explicitly keeps this correct if the shim is
// ever built somewhere they differ.
struct nlibc_guest_timespec { sqword_t sec; sqword_t nsec; };

static int nlibc_utimens(int dirfd, const char *path, int fd_no,
        const struct timeval times[2]) {
    NATIVE_FRAME;
    struct nlibc_guest_timespec ts[2];
    for (int i = 0; i < 2; i++) {
        if (times == NULL) {
            ts[i].sec = 0;
            ts[i].nsec = NLIBC_UTIME_NOW;
        } else {
            ts[i].sec = times[i].tv_sec;
            ts[i].nsec = (sqword_t) times[i].tv_usec * 1000;
        }
    }
    guest_addr_t guest_ts = native_scratch_put(ts, sizeof(ts));
    if (guest_ts == 0)
        return nlibc_fail(_ENOMEM);
    if (path == NULL) {
        // futimes: utimensat with a NULL path acts on the descriptor itself.
        return (int) nlibc_ret(native_syscall(NATIVE_SYS_utimensat, fd_no, 0, guest_ts, 0));
    }
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_utimensat, nlibc_at_fd(dirfd),
            guest_path, guest_ts, 0));
}

int nlibc_utimes(const char *path, const struct timeval times[2]) {
    return nlibc_utimens(AT_FDCWD, path, -1, times);
}
int nlibc_futimes(int fd_no, const struct timeval times[2]) {
    return nlibc_utimens(AT_FDCWD, NULL, fd_no, times);
}

int nlibc_chroot(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_chroot, guest_path));
}

int nlibc_kill(pid_t pid, int sig) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_kill, pid,
            nlibc_signal_to_guest(sig)));
}

int nlibc_mknod(const char *path, mode_t mode, dev_t dev) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    // Darwin packs a dev_t as major:minor = 24:8, the guest as 8:8 in the low
    // 16 bits (fs/dev.h). Repacking rather than passing the raw number through.
    dword_t guest_dev = (dword_t) (((major(dev) & 0xff) << 8) | (minor(dev) & 0xff));
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_mknodat, AT_FDCWD_, guest_path,
            mode, guest_dev));
}

// The guest writes the asm-generic statfs64 (fs/stat.h's amd64_statfs_, shared
// by every 64-bit ABI here); Darwin's struct statfs is a different shape with
// different field widths, so the fields are copied across by name.
static void nlibc_guest_statfs_to_host(const struct amd64_statfs_ *in, struct statfs *out) {
    memset(out, 0, sizeof(*out));
    out->f_bsize = (uint32_t) (in->bsize > 0 ? in->bsize : 4096);
    out->f_blocks = in->blocks;
    out->f_bfree = in->bfree;
    out->f_bavail = in->bavail;
    out->f_files = in->files;
    out->f_ffree = in->ffree;
}

int nlibc_statfs(const char *path, void *buf) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_buf = native_scratch_alloc(sizeof(struct amd64_statfs_));
    if (guest_buf == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_statfs, guest_path, guest_buf);
    if (res < 0)
        return nlibc_fail((int) res);
    struct amd64_statfs_ guest_statfs;
    if (native_scratch_get(&guest_statfs, guest_buf, sizeof(guest_statfs)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_statfs_to_host(&guest_statfs, buf);
    return 0;
}

// fork() has no guest syscall that would help: the caller is host code running
// on a task's thread, and a forked task would resume in the middle of a C
// function with no image to execute. nlibc_execv's spawn-then-exit is the
// shape that does work (see the process section above).
int nlibc_fork(void)                        { return nlibc_fail(_ENOSYS); } // init, runit, micro
int nlibc_glob(const char *p, int f, void *e, void *g) { (void) p; (void) f; (void) e; (void) g; return nlibc_fail(_ENOSYS); } // shell globbing

// Darwin and Linux number their ioctls differently, so the request has to be
// mapped, not forwarded. Only the terminal ones are handled; anything else
// still refuses rather than being passed through with a number that means
// something unrelated on the other side.
int nlibc_ioctl(int fd_no, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
        case TIOCGWINSZ: {
            // struct winsize and AOK's winsize_ are both four u16 in the same
            // order, so this one is a straight copy after the request maps.
            struct winsize_ w = {};
            if (nlibc_tty_ioctl(fd_no, TIOCGWINSZ_, &w, sizeof(w), true) < 0)
                return -1;
            struct winsize *out = arg;
            if (out == NULL)
                return nlibc_fail(_EFAULT);
            out->ws_row = w.row;
            out->ws_col = w.col;
            out->ws_xpixel = w.xpixel;
            out->ws_ypixel = w.ypixel;
            return 0;
        }
        case TIOCSWINSZ: {
            const struct winsize *in = arg;
            if (in == NULL)
                return nlibc_fail(_EFAULT);
            struct winsize_ w = {
                .row = in->ws_row, .col = in->ws_col,
                .xpixel = in->ws_xpixel, .ypixel = in->ws_ypixel,
            };
            return nlibc_tty_ioctl(fd_no, TIOCSWINSZ_, &w, sizeof(w), false);
        }
        default:
            return nlibc_fail(_ENOSYS);
    }
}

// struct pollfd is {int, short, short} on both sides and the POLL* bits share
// their values, so this one really is a straight copy.
int nlibc_poll(void *fds, unsigned nfds, int timeout) {
    NATIVE_FRAME;
    size_t size = (size_t) nfds * sizeof(struct pollfd);
    guest_addr_t guest_fds = native_scratch_put(fds, size);
    if (guest_fds == 0 && size > 0)
        return nlibc_fail(_ENOMEM);

    // ppoll takes a timespec rather than poll's milliseconds; a negative
    // timeout is "wait forever", which is a NULL pointer here.
    guest_addr_t guest_ts = 0;
    if (timeout >= 0) {
        struct nlibc_guest_timespec ts = {
            .sec = timeout / 1000,
            .nsec = (sqword_t) (timeout % 1000) * 1000000,
        };
        guest_ts = native_scratch_put(&ts, sizeof(ts));
        if (guest_ts == 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_ppoll, guest_fds, nfds, guest_ts, 0, 0);
    if (res >= 0 && size > 0 && native_scratch_get(fds, guest_fds, size) < 0)
        return nlibc_fail(_EFAULT);
    return (int) nlibc_ret(res);
}

// An fd_set is a bit array in both libcs, and on a little-endian machine the
// byte-level layout is the same despite Darwin's 32-bit words against Linux's
// 64-bit ones. Only the number of bytes the guest reads has to be worked out
// from nfds, since the two disagree on FD_SETSIZE.
int nlibc_select(int nfds, void *readfds, void *writefds, void *errorfds, void *timeout) {
    NATIVE_FRAME;
    if (nfds < 0)
        return nlibc_fail(_EINVAL);
    size_t size = ((size_t) nfds + 7) / 8;
    size = (size + 7) & ~(size_t) 7;   // the guest reads whole 64-bit words

    guest_addr_t guest_sets[3] = {0, 0, 0};
    void *host_sets[3] = { readfds, writefds, errorfds };
    for (int i = 0; i < 3; i++) {
        if (host_sets[i] == NULL)
            continue;
        guest_sets[i] = native_scratch_put(host_sets[i], size);
        if (guest_sets[i] == 0)
            return nlibc_fail(_ENOMEM);
    }

    guest_addr_t guest_ts = 0;
    if (timeout != NULL) {
        const struct timeval *tv = timeout;
        struct nlibc_guest_timespec ts = {
            .sec = tv->tv_sec,
            .nsec = (sqword_t) tv->tv_usec * 1000,
        };
        guest_ts = native_scratch_put(&ts, sizeof(ts));
        if (guest_ts == 0)
            return nlibc_fail(_ENOMEM);
    }

    sqword_t res = native_syscall(NATIVE_SYS_pselect6, nfds, guest_sets[0],
            guest_sets[1], guest_sets[2], guest_ts, 0);
    if (res < 0)
        return nlibc_fail((int) res);
    for (int i = 0; i < 3; i++)
        if (guest_sets[i] != 0 && native_scratch_get(host_sets[i], guest_sets[i], size) < 0)
            return nlibc_fail(_EFAULT);
    return (int) res;
}

// ------------------------------------------------------ host-global refusals
//
// These change state belonging to the whole machine, not to a guest process.
// An applet must never be able to reach the host's clock, hostname, mount
// table or power state -- on the macOS CLI build those calls would land on the
// developer's own system, and `reboot` in particular is not a mistake worth
// making once. iOS marks several of them unavailable outright, which is how
// clock_settime surfaced: the CLI build accepted it and only the iOS SDK
// refused.
//
// A guest-side implementation would go through AOK's own syscall handlers, not
// these. Until then, refusing is the only safe answer.
// A guest cannot move the host's clock, and should not be told it failed
// either -- the guest's own clock is the host's. Succeed and do nothing,
// matching what a VM does when the hypervisor owns the timebase.
int nlibc_clock_settime(int clk, const struct timespec *ts) {
    (void) clk; (void) ts;
    return 0;
}

// This genuinely works: AOK namespaces the hostname per task (kernel/uts.h),
// so setting it changes what the GUEST reports, exactly as it would in a VM,
// without touching the host's.
int nlibc_sethostname(const char *name, size_t len) {
    NATIVE_FRAME;
    if (name == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_name = native_scratch_put(name, len);
    if (guest_name == 0)
        return nlibc_fail(_ENOMEM);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_sethostname, guest_name, len));
}

// Not reached today -- SmallCLUE's reboot applet prints and exits without
// calling this -- but wired rather than left to find the host's reboot(2) if
// that ever changes. Rebooting a guest has no meaning here; the app is the
// machine.
int nlibc_reboot(int howto) {
    (void) howto;
    return nlibc_fail(_EPERM);
}

// Routed to AOK's own mount, so `mount` behaves for a native applet exactly as
// it does for the guest. do_mount requires mounts_lock to be HELD (kernel/fs.h
// groups it with the other "must hold mounts_lock while calling these"
// entries) -- this called it without, which is the kind of thing that only
// shows up under contention.
int nlibc_mount(const char *src, const char *tgt, const char *type, unsigned long f, const void *d) {
    (void) d;
    if (src == NULL || tgt == NULL || type == NULL)
        return nlibc_fail(_EFAULT);
    const struct fs_ops *fs = fs_lookup(type);
    if (fs == NULL)
        return nlibc_fail(_ENODEV);
    lock(&mounts_lock, 0);
    int err = do_mount(fs, src, tgt, "", (int) f);
    unlock(&mounts_lock);
    return err < 0 ? nlibc_fail(err) : 0;
}

// sysctl answers about the HOST -- hw.memsize is the Mac's RAM, KERN_BOOTTIME
// the Mac's boot. Failing is the right answer rather than the easy one:
// SmallCLUE already handles the failure, falling back to /proc (which AOK
// serves for the guest) and to a monotonic clock for uptime, so the guest gets
// guest numbers. Failing here also short-circuits the mach_host_self /
// host_statistics64 path in smallclueReadMemStats, which runs only after
// sysctlbyname succeeds and would otherwise report host memory.
int nlibc_sysctl(int *name, unsigned namelen, void *old, size_t *oldlen,
              const void *new, size_t newlen) {
    (void) name; (void) namelen; (void) old; (void) oldlen;
    (void) new; (void) newlen;
    errno = ENOTSUP;
    return -1;
}
int nlibc_sysctlbyname(const char *name, void *old, size_t *oldlen,
                    const void *new, size_t newlen) {
    (void) name; (void) old; (void) oldlen; (void) new; (void) newlen;
    errno = ENOTSUP;
    return -1;
}

// uname reported the HOST: Darwin, the Mac's kernel version, the Mac's
// hostname. A guest must see the guest. do_uname is the same source of truth
// the guest's own uname(2) uses, so a native applet and a translated one now
// agree.
int nlibc_uname(struct utsname *buf) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_uts = native_scratch_alloc(sizeof(struct uname));
    if (guest_uts == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_uname, guest_uts);
    if (res < 0)
        return nlibc_fail((int) res);
    struct uname u;
    if (native_scratch_get(&u, guest_uts, sizeof(u)) < 0)
        return nlibc_fail(_EFAULT);
    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, u.system, sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, u.hostname, sizeof(buf->nodename) - 1);
    strncpy(buf->release, u.release, sizeof(buf->release) - 1);
    strncpy(buf->version, u.version, sizeof(buf->version) - 1);
    strncpy(buf->machine, u.arch, sizeof(buf->machine) - 1);
    return 0;
}

// df walks the mount table. On macOS SmallCLUE reaches for getmntinfo, which
// answers about the HOST -- df inside the guest listed the Mac's volumes. The
// traversal itself belongs in fs/mount.c, which owns the locking; doing it
// here held mounts_lock across mount_statfs and hung.
int nlibc_getmntinfo(struct statfs **mntbufp, int flags) {
    (void) flags;
    if (mntbufp == NULL)
        return nlibc_fail(_EFAULT);

    struct mount_info *info = NULL;
    size_t count = 0;
    int err = mount_snapshot(&info, &count);
    if (err < 0)
        return nlibc_fail(err);

    struct statfs *out = calloc(count > 0 ? count : 1, sizeof(*out));
    if (out == NULL) {
        free(info);
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        out[i].f_bsize = (uint32_t) (info[i].statfs.bsize > 0 ? info[i].statfs.bsize : 4096);
        out[i].f_blocks = info[i].statfs.blocks;
        out[i].f_bfree = info[i].statfs.bfree;
        out[i].f_bavail = info[i].statfs.bavail;
        out[i].f_files = info[i].statfs.files;
        out[i].f_ffree = info[i].statfs.ffree;
        strncpy(out[i].f_mntfromname, info[i].source, sizeof(out[i].f_mntfromname) - 1);
        strncpy(out[i].f_mntonname, info[i].point, sizeof(out[i].f_mntonname) - 1);
        strncpy(out[i].f_fstypename, info[i].type, sizeof(out[i].f_fstypename) - 1);
    }
    free(info);

    // Freed on the next call: getmntinfo's contract is that the caller never
    // owns the buffer.
    static struct statfs *previous;
    free(previous);
    previous = out;
    *mntbufp = out;
    return (int) count;
}

// ------------------------------------------------------- interruptible waits
//
// A host sleep parks the thread with nothing watching for signals, which is
// the main reason `top` could not be quit: it spends almost all its time in a
// one-second sleep between refreshes, so ^C had nowhere to land. Sleeping in
// short slices with a checkpoint between them makes the wait interruptible
// without changing what the program observes -- it still sleeps the requested
// time, just in pieces.
#define SC_SLEEP_SLICE_US 50000  // 50ms: responsive to a keypress, negligible overhead

static int nlibc_sleep_us(uint64_t total_us) {
    struct timespec slice;
    while (total_us > 0) {
        native_checkpoint();     // may not return, if the signal is fatal
        uint64_t chunk = total_us < SC_SLEEP_SLICE_US ? total_us : SC_SLEEP_SLICE_US;
        slice.tv_sec = (time_t) (chunk / 1000000u);
        slice.tv_nsec = (long) ((chunk % 1000000u) * 1000u);
        nanosleep(&slice, NULL);
        total_us -= chunk;
    }
    native_checkpoint();
    return 0;
}

unsigned int nlibc_sleep(unsigned int seconds) {
    nlibc_sleep_us((uint64_t) seconds * 1000000u);
    return 0;
}

int nlibc_usleep(unsigned int usec) {
    return nlibc_sleep_us(usec);
}

int nlibc_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req == NULL) {
        errno = EINVAL;
        return -1;
    }
    nlibc_sleep_us((uint64_t) req->tv_sec * 1000000u + (uint64_t) (req->tv_nsec / 1000));
    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

// ---------------------------------------------------------------- terminal
//
// Why this is not a pass-through: a native program is host C, so it speaks
// Darwin's struct termios, while AOK's tty speaks Linux's (fs/tty.h). The two
// disagree on layout AND on the flag values -- ICANON is 1<<1 on Linux and
// 0x100 on Darwin, and the c_cc indices differ (VMIN is 6 vs 16). Copying the
// struct across would set the wrong bits, which is worse than not implementing
// it: the terminal would end up in some other mode rather than raw.
//
// Only the flags a program plausibly changes are translated. An untranslated
// bit is dropped rather than passed through, so a mistake shows up as "that
// mode did not take" instead of a terminal in an undefined state.
//
// This is what `q` in `top` needed: without raw mode the applet never sees a
// keystroke, which is why it could not be quit.

// `out` says which way the argument travels: true when the kernel fills it in
// (TCGETS, TIOCGWINSZ), false when the caller supplies it (TCSETS).
static int nlibc_tty_ioctl(int fd_no, dword_t cmd, void *arg, size_t size, bool out) {
    NATIVE_FRAME;
    guest_addr_t guest_arg = out ? native_scratch_alloc(size)
                                 : native_scratch_put(arg, size);
    if (guest_arg == 0)
        return nlibc_fail(arg == NULL ? _EFAULT : _ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_ioctl, fd_no, cmd, guest_arg);
    if (res < 0)
        return nlibc_fail((int) res);
    if (out && native_scratch_get(arg, guest_arg, size) < 0)
        return nlibc_fail(_EFAULT);
    return (int) res;
}

// Only the bits worth honouring; see the note above about dropping the rest.
static const struct nlibc_flagmap nlibc_lflags[] = {
    { ISIG,   ISIG_ }, { ICANON, ICANON_ }, { ECHO, ECHO_ }, { ECHOE, ECHOE_ },
};
static const struct nlibc_flagmap nlibc_iflags[] = {
    { ICRNL,  ICRNL_ }, { IXON, IXON_ },
};
static const struct nlibc_flagmap nlibc_oflags[] = {
    { OPOST,  OPOST_ },
};

int nlibc_tcgetattr(int fd_no, struct termios *out) {
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->c_lflag = nlibc_flags_to_host(t.lflags, nlibc_lflags, NLIBC_MAP_COUNT(nlibc_lflags));
    out->c_iflag = nlibc_flags_to_host(t.iflags, nlibc_iflags, NLIBC_MAP_COUNT(nlibc_iflags));
    out->c_oflag = nlibc_flags_to_host(t.oflags, nlibc_oflags, NLIBC_MAP_COUNT(nlibc_oflags));
    out->c_cc[VMIN] = t.cc[VMIN_];
    out->c_cc[VTIME] = t.cc[VTIME_];
    out->c_cc[VINTR] = t.cc[VINTR_];
    out->c_cc[VQUIT] = t.cc[VQUIT_];
    out->c_cc[VSUSP] = t.cc[VSUSP_];
    out->c_cc[VEOF] = t.cc[VEOF_];
    return 0;
}

int nlibc_tcsetattr(int fd_no, int action, const struct termios *in) {
    (void) action;  // TCSANOW/DRAIN/FLUSH all map to the same guest ioctl here
    if (in == NULL)
        return nlibc_fail(_EFAULT);
    // Read-modify-write: preserve the bits this translation does not cover
    // rather than zeroing them.
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0)
        return -1;
    dword_t lmask = 0, imask = 0, omask = 0;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_lflags); i++) lmask |= nlibc_lflags[i].guest;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_iflags); i++) imask |= nlibc_iflags[i].guest;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_oflags); i++) omask |= nlibc_oflags[i].guest;
    t.lflags = (t.lflags & ~lmask) | nlibc_flags_to_guest(in->c_lflag, nlibc_lflags, NLIBC_MAP_COUNT(nlibc_lflags));
    t.iflags = (t.iflags & ~imask) | nlibc_flags_to_guest(in->c_iflag, nlibc_iflags, NLIBC_MAP_COUNT(nlibc_iflags));
    t.oflags = (t.oflags & ~omask) | nlibc_flags_to_guest(in->c_oflag, nlibc_oflags, NLIBC_MAP_COUNT(nlibc_oflags));
    t.cc[VMIN_] = in->c_cc[VMIN];
    t.cc[VTIME_] = in->c_cc[VTIME];
    return nlibc_tty_ioctl(fd_no, TCSETS_, &t, sizeof(t), false);
}

int nlibc_isatty(int fd_no) {
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

// --------------------------------------------------- threads a program makes
//
// A native program may create its own pthreads -- SmallCLUE runs its editor on
// one (nextvi_app.c). Those are raw host threads, so AOK's `current` is NULL on
// them, and the first shim call that touches the fd table dereferenced it and
// took the whole app down with EXC_BAD_ACCESS. That is the worst possible
// outcome: not the applet failing, the process dying.
//
// pthread_create is wrapped so a thread inherits the task of whoever created
// it, which is what the program already assumes -- it expects its threads to
// share the process's descriptors and cwd.
//
// The two host threads then share one struct task. That is right for the
// pattern in use here, where the creator blocks in pthread_join and only one
// runs at a time, and it is what exsh will want for its own threads. Genuinely
// concurrent use of the same task from two threads is not made safe by this;
// it would need the task's own locking to be audited for it.

struct nlibc_thread_start {
    void *(*fn)(void *);
    void *arg;
    struct task *task;
};

static void *nlibc_thread_trampoline(void *opaque) {
    struct nlibc_thread_start *start = opaque;
    current = start->task;   // inherit, so the shim has a task to work against
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    return fn(arg);
}

int nlibc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*fn)(void *), void *arg) {
    if (fn == NULL)
        return EINVAL;
    struct nlibc_thread_start *start = malloc(sizeof(*start));
    if (start == NULL)
        return EAGAIN;
    start->fn = fn;
    start->arg = arg;
    start->task = current;
    int err = pthread_create(thread, attr, nlibc_thread_trampoline, start);
    if (err != 0)
        free(start);
    return err;
}

// ------------------------------------------------------------------- exit
//
// The host's exit() ends the PROCESS, and a native program shares the app's
// process -- so a plain exit() in an applet terminated iSH-AOK itself. That is
// how `env` came to take the whole emulator down after its child finished, and
// SmallCLUE calls exit()/_exit() in nine places besides.
//
// A native program is a guest TASK, so ending it means do_exit_group, exactly
// as returning from its main does (kernel/native.c).
noreturn void nlibc_exit(int status) {
    nlibc_flush_std();
    do_exit_group((status & 0xff) << 8);
}

// --------------------------------------------------------------- environment
//
// getenv() in host code answers about the HOST process -- the developer's
// shell on the macOS build, the app's on iOS. `env` printed the Mac's
// environment, and every PATH search a native program made walked the Mac's
// directories. The storage lives on the task (kernel/native.h); these are the
// libc shapes over it.

char **nlibc_environ(void) { return native_env_vector(); }

char *nlibc_getenv(const char *name) {
    // getenv's contract is that the pointer stays valid until the environment
    // is next changed, which is exactly the lifetime of the entry it points
    // into -- so this hands back the storage rather than a copy.
    return (char *) native_env_get(name);
}

int nlibc_setenv(const char *name, const char *value, int overwrite) {
    int err = native_env_set(name, value, overwrite != 0);
    return err < 0 ? nlibc_fail(err) : 0;
}

int nlibc_unsetenv(const char *name) {
    int err = native_env_unset(name);
    return err < 0 ? nlibc_fail(err) : 0;
}

int nlibc_putenv(char *entry) {
    // putenv's contract is that the caller's string BECOMES the environment
    // entry. Copying instead is the safe divergence: the alternative is
    // holding a pointer into a caller's buffer that may be a local.
    if (entry == NULL)
        return nlibc_fail(_EINVAL);
    const char *eq = strchr(entry, '=');
    if (eq == NULL)
        return nlibc_unsetenv(entry);
    char name[256];
    size_t len = (size_t) (eq - entry);
    if (len >= sizeof(name))
        return nlibc_fail(_EINVAL);
    memcpy(name, entry, len);
    name[len] = '\0';
    return nlibc_setenv(name, eq + 1, 1);
}

// ------------------------------------------------------------------ identity
//
// `whoami` answered "mobile" -- the iOS user account. Every one of these was
// reaching the host: getuid returned the app's uid, getpid the app's pid, and
// getpwuid read the DEVICE's passwd database rather than the guest's. Same
// class as uname and getmntinfo, and the guard's denylist had no idea it
// existed either.
//
// The task carries the guest's credentials (kernel/task.h), and the guest's
// /etc/passwd and /etc/group are ordinary files reachable through the VFS.

uid_t nlibc_getuid(void)  { return (uid_t) native_syscall(NATIVE_SYS_getuid); }
uid_t nlibc_geteuid(void) { return (uid_t) native_syscall(NATIVE_SYS_geteuid); }
gid_t nlibc_getgid(void)  { return (gid_t) native_syscall(NATIVE_SYS_getgid); }
gid_t nlibc_getegid(void) { return (gid_t) native_syscall(NATIVE_SYS_getegid); }
pid_t nlibc_getpid(void)  { return (pid_t) native_syscall(NATIVE_SYS_getpid); }
pid_t nlibc_getppid(void) { return (pid_t) native_syscall(NATIVE_SYS_getppid); }

int nlibc_getgroups(int size, gid_t list[]) {
    NATIVE_FRAME;
    if (size == 0)
        return (int) nlibc_ret(native_syscall(NATIVE_SYS_getgroups, 0, 0));
    if (size < 0 || list == NULL)
        return nlibc_fail(_EINVAL);
    // The guest's gid_t is 32 bits, as Darwin's is.
    size_t bytes = (size_t) size * sizeof(uint32_t);
    guest_addr_t guest_list = native_scratch_alloc(bytes);
    if (guest_list == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getgroups, size, guest_list);
    if (res < 0)
        return nlibc_fail((int) res);
    if (res > 0 && native_scratch_get(list, guest_list, (size_t) res * sizeof(uint32_t)) < 0)
        return nlibc_fail(_EFAULT);
    return (int) res;
}

int nlibc_setuid(uid_t uid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setuid, uid));
}
int nlibc_setgid(gid_t gid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setgid, gid));
}
int nlibc_initgroups(const char *user, gid_t group) {
    (void) user; (void) group;
    return 0;   // no supplementary groups to set; succeeding is the honest no-op
}

// /etc/passwd and /etc/group, read from the GUEST. One entry is cached at a
// time, which is what getpwuid's contract allows -- the returned pointer is
// only valid until the next call.
static char nlibc_pw_line[512];
static struct passwd nlibc_pw;
static char nlibc_gr_line[512];
static struct group nlibc_gr;
static char *nlibc_gr_members[1];

static char *nlibc_next_field(char **cursor) {
    char *start = *cursor;
    if (start == NULL)
        return NULL;
    char *colon = strchr(start, ':');
    if (colon != NULL) {
        *colon = '\0';
        *cursor = colon + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

// Walks a colon-separated database, handing each line to `match`.
static bool nlibc_scan_db(const char *path, char *linebuf, size_t linelen,
        bool (*match)(char **fields, size_t n, const void *key), const void *key) {
    struct fd *fd = NULL;
    if (native_open(path, O_RDONLY_, &fd) < 0)
        return false;
    char buf[4096];
    size_t held = 0;
    bool found = false;
    for (;;) {
        ssize_t n = native_read(fd, buf + held, sizeof(buf) - held - 1);
        if (n < 0)
            break;
        size_t avail = held + (size_t) n;
        buf[avail] = '\0';
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (line[0] != '\0' && line[0] != '#') {
                snprintf(linebuf, linelen, "%s", line);
                char *cursor = linebuf;
                char *fields[8] = {0};
                size_t nf = 0;
                while (nf < 8) {
                    char *f = nlibc_next_field(&cursor);
                    if (f == NULL) break;
                    fields[nf++] = f;
                }
                if (match(fields, nf, key)) {
                    found = true;
                    goto done;
                }
            }
            line = nl + 1;
        }
        if (n == 0)
            break;
        held = strlen(line);
        memmove(buf, line, held);
    }
done:
    native_close(fd);
    return found;
}

struct nlibc_pw_key { const char *name; uid_t uid; bool by_name; };

static bool nlibc_pw_match(char **f, size_t n, const void *keyv) {
    const struct nlibc_pw_key *key = keyv;
    if (n < 7)
        return false;
    if (key->by_name ? strcmp(f[0], key->name) != 0
                     : (uid_t) strtoul(f[2], NULL, 10) != key->uid)
        return false;
    nlibc_pw.pw_name = f[0];
    nlibc_pw.pw_passwd = f[1];
    nlibc_pw.pw_uid = (uid_t) strtoul(f[2], NULL, 10);
    nlibc_pw.pw_gid = (gid_t) strtoul(f[3], NULL, 10);
    nlibc_pw.pw_gecos = f[4];
    nlibc_pw.pw_dir = f[5];
    nlibc_pw.pw_shell = f[6];
    return true;
}

struct passwd *nlibc_getpwuid(uid_t uid) {
    struct nlibc_pw_key key = { NULL, uid, false };
    memset(&nlibc_pw, 0, sizeof(nlibc_pw));
    if (!nlibc_scan_db("/etc/passwd", nlibc_pw_line, sizeof(nlibc_pw_line),
                       nlibc_pw_match, &key))
        return NULL;
    return &nlibc_pw;
}

struct passwd *nlibc_getpwnam(const char *name) {
    if (name == NULL)
        return NULL;
    struct nlibc_pw_key key = { name, 0, true };
    memset(&nlibc_pw, 0, sizeof(nlibc_pw));
    if (!nlibc_scan_db("/etc/passwd", nlibc_pw_line, sizeof(nlibc_pw_line),
                       nlibc_pw_match, &key))
        return NULL;
    return &nlibc_pw;
}

struct nlibc_gr_key { const char *name; gid_t gid; bool by_name; };

static bool nlibc_gr_match(char **f, size_t n, const void *keyv) {
    const struct nlibc_gr_key *key = keyv;
    if (n < 3)
        return false;
    if (key->by_name ? strcmp(f[0], key->name) != 0
                     : (gid_t) strtoul(f[2], NULL, 10) != key->gid)
        return false;
    nlibc_gr_members[0] = NULL;
    nlibc_gr.gr_name = f[0];
    nlibc_gr.gr_passwd = f[1];
    nlibc_gr.gr_gid = (gid_t) strtoul(f[2], NULL, 10);
    nlibc_gr.gr_mem = nlibc_gr_members;
    return true;
}

struct group *nlibc_getgrgid(gid_t gid) {
    struct nlibc_gr_key key = { NULL, gid, false };
    memset(&nlibc_gr, 0, sizeof(nlibc_gr));
    if (!nlibc_scan_db("/etc/group", nlibc_gr_line, sizeof(nlibc_gr_line),
                       nlibc_gr_match, &key))
        return NULL;
    return &nlibc_gr;
}

struct group *nlibc_getgrnam(const char *name) {
    if (name == NULL)
        return NULL;
    struct nlibc_gr_key key = { name, 0, true };
    memset(&nlibc_gr, 0, sizeof(nlibc_gr));
    if (!nlibc_scan_db("/etc/group", nlibc_gr_line, sizeof(nlibc_gr_line),
                       nlibc_gr_match, &key))
        return NULL;
    return &nlibc_gr;
}
