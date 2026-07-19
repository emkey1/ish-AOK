#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "kernel/random.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/real.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/tty.h"

#define MAX_RW_COUNT (16 * 1024 * 1024)

// Per-task I/O accounting (surfaced via /proc/<pid>/io). syscr/syscw count
// read/write-family syscalls that reached an fd; rchar/wchar count bytes
// actually returned. read_bytes/write_bytes only count bytes moved through
// file-backed fds (realfs/fakefs), approximating Linux's "hit the storage
// layer" semantics — tmpfs/pipes/sockets/ttys don't reach storage, same as
// Linux, where these two fields track block-layer I/O. The globals aggregate
// disk bytes system-wide for /proc/vmstat's pgpgin/pgpgout.
_Atomic qword_t io_disk_read_bytes;
_Atomic qword_t io_disk_write_bytes;

static bool fd_is_disk_backed(struct fd *fd) {
    // Regular files only: device nodes (/dev/zero etc.) live on the same
    // fakefs mount but their traffic never reaches storage.
    return S_ISREG(fd->type) && fd->mount != NULL &&
        (fd->mount->fs == &realfs || fd->mount->fs == &fakefs);
}

static void io_account_read(struct fd *fd, ssize_t res) {
    atomic_fetch_add_explicit(&current->io.syscr, 1, memory_order_relaxed);
    if (res <= 0)
        return;
    atomic_fetch_add_explicit(&current->io.rchar, (qword_t) res, memory_order_relaxed);
    if (fd_is_disk_backed(fd)) {
        atomic_fetch_add_explicit(&current->io.read_bytes, (qword_t) res, memory_order_relaxed);
        atomic_fetch_add_explicit(&io_disk_read_bytes, (qword_t) res, memory_order_relaxed);
    }
}

// Block-I/O delay accounting for taskstats (iotop's IO> percentage): wall
// time spent inside a file-backed read/write op. io_delay_start returns 0
// for fds that aren't disk-backed so pipes/ttys don't pay the clock reads.
static uint64_t io_delay_start(struct fd *fd) {
    if (fd == NULL || !fd_is_disk_backed(fd))
        return 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    uint64_t now = (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
    return now != 0 ? now : 1;
}

static void io_delay_end(uint64_t start) {
    if (start == 0)
        return;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return;
    uint64_t now = (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
    atomic_fetch_add_explicit(&current->io.blkio_count, 1, memory_order_relaxed);
    if (now > start)
        atomic_fetch_add_explicit(&current->io.blkio_delay_ns, now - start, memory_order_relaxed);
}

static void io_account_write(struct fd *fd, ssize_t res) {
    atomic_fetch_add_explicit(&current->io.syscw, 1, memory_order_relaxed);
    if (res <= 0)
        return;
    atomic_fetch_add_explicit(&current->io.wchar, (qword_t) res, memory_order_relaxed);
    if (fd_is_disk_backed(fd)) {
        atomic_fetch_add_explicit(&current->io.write_bytes, (qword_t) res, memory_order_relaxed);
        atomic_fetch_add_explicit(&io_disk_write_bytes, (qword_t) res, memory_order_relaxed);
    }
}

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;
extern bool isGlibC;

static struct fd *at_fd(fd_t f) {
    if (f == AT_FDCWD_)
        return AT_PWD;
    return f_get(f);
}

static bool fs_trace_elogind(void) {
    return false;
}

static bool http_resolver_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_HTTPFS") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && strncmp(current->comm, "http", 4) == 0;
}

static bool ldconfig_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_LDCONFIG_FS") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && strcmp(current->comm, "ldconfig") == 0;
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

static void http_resolver_trace_path_result(const char *op, fd_t at_f, const char *path, long result, unsigned long arg) {
    if (!http_resolver_trace_enabled() || path == NULL)
        return;
    fprintf(stderr, "ish-httpfs:%s pid=%d comm=%s at=%d path=%s arg=%#lx result=%ld\n",
            op, current->pid, current->comm, at_f, path, arg, result);
}

static void ldconfig_trace_path_result(const char *op, fd_t at_f, const char *path, long result,
        unsigned long arg, const char *extra) {
    if (!ldconfig_trace_enabled() || !ldconfig_trace_path(path))
        return;
    fprintf(stderr,
            "ish-ldconfig-fs:%s pid=%d at=%d path=%s arg=%#lx result=%ld%s%s\n",
            op, current->pid, at_f, path, arg, result,
            extra != NULL ? " " : "",
            extra != NULL ? extra : "");
}

static bool fs_trace_interesting_path(const char *path) {
    static const char *prefixes[] = {
        "/sys/fs/cgroup",
        "/run/systemd",
        "/run/elogind",
        "/run/dbus",
        "/dev/kmsg",
        "/dev/log",
        "/proc/",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], len) == 0)
            return true;
    }
    return false;
}

static struct tty *amd64_tty_stdio_trace_tty(fd_t fd_no) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_TTY_STDIO") != NULL ? 1 : 0;
    if (!enabled)
        return NULL;
    if (current == NULL || current->abi != GUEST_ABI_AMD64 || fd_no > 2)
        return NULL;
    if (strcmp(current->comm, "sh") != 0)
        return NULL;
    struct fd *fd = f_get(fd_no);
    if (fd == NULL || fd->tty == NULL)
        return NULL;
    if (fd->tty->type != TTY_CONSOLE_MAJOR || fd->tty->num != 2)
        return NULL;
    return fd->tty;
}

static void amd64_tty_stdio_trace(const char *op, fd_t fd_no, guest_addr_t addr,
        dword_t size, ssize_t res, const void *buf, size_t buf_len) {
    enum { AMD64_TTY_STDIO_LOG_BUDGET = 128, AMD64_TTY_STDIO_PREVIEW = 48 };
    static unsigned amd64_tty_stdio_log_count;
    struct tty *tty = amd64_tty_stdio_trace_tty(fd_no);
    if (tty == NULL)
        return;
    if (amd64_tty_stdio_log_count >= AMD64_TTY_STDIO_LOG_BUDGET)
        return;
    amd64_tty_stdio_log_count++;

    char preview[AMD64_TTY_STDIO_PREVIEW + 1];
    size_t preview_len = buf_len > AMD64_TTY_STDIO_PREVIEW ? AMD64_TTY_STDIO_PREVIEW : buf_len;
    for (size_t i = 0; i < preview_len; i++) {
        unsigned char ch = ((const unsigned char *) buf)[i];
        preview[i] = (ch >= 0x20 && ch <= 0x7e) ? (char) ch : '.';
    }
    preview[preview_len] = '\0';

    printk("amd64 tty io: tty=%d pid=%d comm=%s %s fd=%d addr=%#llx size=%u res=%zd data=\"%s\"\n",
           tty->num, current->pid, current->comm, op, fd_no,
           (unsigned long long) addr, size, res, preview);
}

static bool amd64_as_source_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_AS_SOURCE") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        strcmp(current->comm, "as") == 0;
}

static bool amd64_as_source_trace_path(const char *path) {
    size_t len;
    if (path == NULL)
        return false;
    if (strncmp(path, "/tmp/", 5) != 0)
        return false;
    len = strlen(path);
    return len >= 3 && strcmp(path + len - 2, ".s") == 0;
}

static void amd64_as_source_trace_open(fd_t fd_no, const char *path, dword_t flags) {
    enum { AMD64_AS_SOURCE_OPEN_BUDGET = 16 };
    static unsigned amd64_as_source_open_count;

    if (!amd64_as_source_trace_enabled() || !amd64_as_source_trace_path(path))
        return;
    if (amd64_as_source_open_count >= AMD64_AS_SOURCE_OPEN_BUDGET)
        return;
    amd64_as_source_open_count++;
    printk("amd64 as source open: pid=%d tgid=%d fd=%d flags=%#x path=%s\n",
           current->pid, current->tgid, fd_no, flags, path);
}

static void amd64_as_source_trace_read(fd_t fd_no, const char *path, const void *buf, size_t buf_len) {
    enum {
        AMD64_AS_SOURCE_READ_BUDGET = 8,
        AMD64_AS_SOURCE_MAX_BYTES = 4096,
        AMD64_AS_SOURCE_MAX_LINES = 80,
        AMD64_AS_SOURCE_LINE_MAX = 192
    };
    static unsigned amd64_as_source_read_count;
    static pid_t_ amd64_as_source_last_pid;
    static fd_t amd64_as_source_last_fd;
    const unsigned char *src = buf;
    size_t limit, i = 0;
    unsigned line_no = 1;

    if (!amd64_as_source_trace_enabled() || !amd64_as_source_trace_path(path))
        return;
    if (amd64_as_source_read_count >= AMD64_AS_SOURCE_READ_BUDGET)
        return;
    if (amd64_as_source_last_pid == current->pid && amd64_as_source_last_fd == fd_no)
        return;

    amd64_as_source_last_pid = current->pid;
    amd64_as_source_last_fd = fd_no;
    amd64_as_source_read_count++;

    limit = buf_len;
    if (limit > AMD64_AS_SOURCE_MAX_BYTES)
        limit = AMD64_AS_SOURCE_MAX_BYTES;

    printk("amd64 as source read: pid=%d tgid=%d fd=%d path=%s bytes=%zu\n",
           current->pid, current->tgid, fd_no, path, limit);

    while (i < limit && line_no <= AMD64_AS_SOURCE_MAX_LINES) {
        char line[AMD64_AS_SOURCE_LINE_MAX];
        size_t li = 0;

        while (i < limit) {
            unsigned char ch = src[i++];
            if (ch == '\n')
                break;
            if (li + 1 >= sizeof(line))
                continue;
            if (ch == '\t') {
                if (li + 2 < sizeof(line)) {
                    line[li++] = '\\';
                    line[li++] = 't';
                }
                continue;
            }
            line[li++] = (ch >= 0x20 && ch <= 0x7e) ? (char) ch : '.';
        }
        line[li] = '\0';
        printk("amd64 as source:%4u | %s\n", line_no, line);
        line_no++;
    }

    if (i < buf_len)
        printk("amd64 as source: ... truncated after %zu bytes\n", limit);
}

static void apply_umask(mode_t_ *mode) {
    struct fs_info *fs = current->fs;
    lock(&fs->lock, 0);
    *mode &= ~fs->umask;
    unlock(&fs->lock);
}

int access_check(struct statbuf *stat, int check) {
    if (superuser()) return 0;
    if (check == 0) return 0;
    // Align check with the correct bits in mode
    if (current->fsuid == stat->uid) {
        check <<= 6;
    } else if (current->fsgid == stat->gid) {
        check <<= 3;
    }
    // All requested bits must be set, not merely overlap one of them: with a
    // combined check like AC_W|AC_X, "other" permission bits of e.g. 5 (r-x)
    // must be rejected for missing AC_W, but (mode & check) != 0 was true
    // because AC_X alone overlapped -- silently granting write access. This
    // was invisible for the common single-bit checks (AC_R or AC_W or AC_X
    // alone, where "any overlap" and "all bits present" coincide) but wrong
    // for any caller passing a combined mask, e.g. O_RDWR's AC_R|AC_W here
    // and the parent-directory AC_W|AC_X check in fs/generic.c / fs/path.c.
    if (((int) stat->mode & check) != check)
        return _EACCES;
    return 0;
}

#define AT_EACCESS_ 0x200
#define FACCESSAT_ALLOWED_FLAGS_ (AT_EACCESS_ | AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_)
#define FCHMODAT2_ALLOWED_FLAGS_ (AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_)

struct open_how_ {
    qword_t flags;
    qword_t mode;
    qword_t resolve;
};

static dword_t sys_faccessat_common(fd_t at_f, guest_addr_t path_addr, mode_t_ mode, dword_t flags) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    STRACE("faccessat(%d, \"%s\", 0x%x, %d)", at_f, path, mode, flags);

    static int trace_dpkg_enabled = -1;
    if (trace_dpkg_enabled < 0)
        trace_dpkg_enabled = getenv("ISH_TRACE_DPKG_STAT") != NULL ? 1 : 0;

    bool trace_dpkg = trace_dpkg_enabled &&
        current != NULL &&
        (strncmp(current->comm, "dpkg", 4) == 0 ||
         strcmp(current->comm, "apt") == 0 ||
         strcmp(current->comm, "apt-get") == 0) &&
        strncmp(path, "/var/lib/dpkg/", 14) == 0;

    if (flags & ~FACCESSAT_ALLOWED_FLAGS_)
        return _EINVAL;

    int stat_flags = 0;
    if (flags & AT_SYMLINK_NOFOLLOW_)
        stat_flags |= AT_SYMLINK_NOFOLLOW_;
    if (flags & AT_EMPTY_PATH_)
        stat_flags |= AT_EMPTY_PATH_;

    if (flags & AT_EACCESS_) {
        if (stat_flags != 0) {
            struct statbuf stat = {};
            int err = generic_statat(at, path, &stat, stat_flags);
            if (trace_dpkg) {
                fprintf(stderr,
                        "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=stat result=%d\n",
                        current->pid, current->comm, current->abi, at_f, path, mode, flags, err);
            }
            if (err < 0)
                return err;
            err = access_check(&stat, mode);
            if (trace_dpkg) {
                fprintf(stderr,
                        "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=check result=%d dev=%llu ino=%llu\n",
                        current->pid, current->comm, current->abi, at_f, path, mode, flags, err,
                        (unsigned long long) stat.dev, (unsigned long long) stat.inode);
            }
            return err;
        }
        int err = generic_accessat(at, path, mode);
        if (trace_dpkg) {
            fprintf(stderr,
                    "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=generic result=%d\n",
                    current->pid, current->comm, current->abi, at_f, path, mode, flags, err);
        }
        return err;
    }

    uid_t_ uid_tmp = current->fsuid;
    uid_t_ gid_tmp = current->fsgid;
    current->fsuid = current->uid;
    current->fsgid = current->gid;
    int err;
    if (stat_flags != 0) {
        struct statbuf stat = {};
        err = generic_statat(at, path, &stat, stat_flags);
        if (trace_dpkg) {
            fprintf(stderr,
                    "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=stat result=%d\n",
                    current->pid, current->comm, current->abi, at_f, path, mode, flags, err);
        }
        if (err >= 0)
            err = access_check(&stat, mode);
        if (trace_dpkg) {
            fprintf(stderr,
                    "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=check result=%d dev=%llu ino=%llu\n",
                    current->pid, current->comm, current->abi, at_f, path, mode, flags, err,
                    (unsigned long long) stat.dev, (unsigned long long) stat.inode);
        }
    } else {
        err = generic_accessat(at, path, mode);
        if (trace_dpkg) {
            fprintf(stderr,
                    "ish-dpkgstat:faccessat pid=%d comm=%s abi=%d at=%d path=%s mode=%#x flags=%#x phase=generic result=%d\n",
                    current->pid, current->comm, current->abi, at_f, path, mode, flags, err);
        }
    }
    current->fsuid = uid_tmp;
    current->fsgid = gid_tmp;
    return err;
}

dword_t sys_access_guest(guest_addr_t path_addr, dword_t mode) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    dword_t err = sys_faccessat_common(AT_FDCWD_, path_addr, mode, 0);
    http_resolver_trace_path_result("access", AT_FDCWD_, path, err, mode);
    return err;
}
dword_t sys_access(addr_t path_addr, dword_t mode) {
    return sys_access_guest(path_addr, mode);
}
dword_t sys_faccessat_guest(fd_t at_f, guest_addr_t path_addr, mode_t_ mode, dword_t flags) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    dword_t err = sys_faccessat_common(at_f, path_addr, mode, flags);
    http_resolver_trace_path_result("faccessat", at_f, path, err, ((unsigned long) flags << 16) | mode);
    return err;
}
dword_t sys_faccessat(fd_t at_f, addr_t path_addr, mode_t_ mode, dword_t flags) {
    return sys_faccessat_guest(at_f, path_addr, mode, flags);
}

fd_t sys_openat_guest(fd_t at_f, guest_addr_t path_addr, dword_t flags, mode_t_ mode) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("openat(%d, \"%s\", 0x%x, 0x%x)", at_f, path, flags, mode);

    if (flags & O_CREAT_)
        apply_umask(&mode);

    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    struct fd *fd;
    TASK_MAY_BLOCK {
        fd = generic_openat(at, path, flags, mode);
    }
    if (IS_ERR(fd))
        goto out;
    fd_t installed = f_install(fd, flags);
    amd64_as_source_trace_open(installed, path, flags);
    http_resolver_trace_path_result("openat", at_f, path, installed, flags);
    ldconfig_trace_path_result("openat", at_f, path, installed, flags, NULL);
    return installed;
out:
    http_resolver_trace_path_result("openat", at_f, path, PTR_ERR(fd), flags);
    ldconfig_trace_path_result("openat", at_f, path, PTR_ERR(fd), flags, NULL);
    return PTR_ERR(fd);
}

fd_t sys_openat(fd_t at_f, addr_t path_addr, dword_t flags, mode_t_ mode) {
    return sys_openat_guest(at_f, path_addr, flags, mode);
}

fd_t sys_open_guest(guest_addr_t path_addr, dword_t flags, mode_t_ mode) {
    return sys_openat_guest(AT_FDCWD_, path_addr, flags, mode);
}

fd_t sys_open(addr_t path_addr, dword_t flags, mode_t_ mode) {
    return sys_open_guest(path_addr, flags, mode);
}

fd_t sys_openat2_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t how_addr, dword_t size) {
    STRACE("openat2(%d, %#x, %#x, %u)", at_f, path_addr, how_addr, size);

    if (size < sizeof(struct open_how_))
        return _EINVAL;

    struct open_how_ how = {};
    if (user_read(how_addr, &how, sizeof(how)))
        return _EFAULT;

    if (size > sizeof(how)) {
        char extra[64];
        size_t offset = sizeof(how);
        while (offset < size) {
            size_t chunk = sizeof(extra);
            if (chunk > size - offset)
                chunk = size - offset;
            if (user_read(how_addr + offset, extra, chunk))
                return _EFAULT;
            for (size_t i = 0; i < chunk; i++) {
                if (extra[i] != 0)
                    return _E2BIG;
            }
            offset += chunk;
        }
    }

    if ((how.flags >> 32) != 0 || (how.mode >> 32) != 0)
        return _EINVAL;
    if (how.resolve != 0)
        return _EINVAL;

    dword_t flags = (dword_t) how.flags;
    // The struct is read here rather than at the dispatch site, so plain
    // openat's arm64 flag translation (calls.c) can't cover openat2: an
    // aarch64 guest's open_how.flags arrive in the aarch64 encoding (e.g.
    // O_LARGEFILE 0x20000 reads as internal O_NOFOLLOW -> spurious ELOOP).
    // amd64 needs no translation: x86-64's O_ constants match the internal
    // (i386-derived) values.
    if (current->abi == GUEST_ABI_ARM64)
        flags = arm64_open_flags_to_internal(flags);

    return sys_openat_guest(at_f, path_addr, flags, (mode_t_) how.mode);
}

fd_t sys_openat2(fd_t at_f, addr_t path_addr, addr_t how_addr, dword_t size) {
    return sys_openat2_guest(at_f, path_addr, how_addr, size);
}

fd_t sys_creat_guest(guest_addr_t path_addr, mode_t_ mode) {
    isGlibC = true; // In theory, musl should never call creat  -mk
    dword_t flags = 0;
    flags |= O_CREAT_;
    flags |= O_WRONLY_;
    flags |= O_TRUNC_;
    return sys_openat_guest(AT_FDCWD_, path_addr, flags, mode);
}

fd_t sys_creat(addr_t path_addr, mode_t_ mode) {
    return sys_creat_guest(path_addr, mode);
}



static dword_t sys_readlinkat_common(fd_t at_f, guest_addr_t path_addr, guest_addr_t buf_addr, dword_t bufsize) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("readlinkat(%d, \"%s\", %#x, %#x)", at_f, path, buf_addr, bufsize);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    if (bufsize > MAX_PATH)
        bufsize = MAX_PATH;
    char buf[bufsize];
    ssize_t size = generic_readlinkat(at, path, buf, bufsize);
    if (size >= 0) {
        STRACE(" \"%.*s\"", size, buf);
        if (user_write(buf_addr, buf, size))
            return _EFAULT;
    }
    http_resolver_trace_path_result("readlinkat", at_f, path, size, bufsize);
    if (size >= 0) {
        char target[MAX_PATH + 32];
        snprintf(target, sizeof(target), "target=\"%.*s\"", (int) size, buf);
        ldconfig_trace_path_result("readlinkat", at_f, path, size, bufsize, target);
    } else {
        ldconfig_trace_path_result("readlinkat", at_f, path, size, bufsize, NULL);
    }
    return (dword_t)size;
}

dword_t sys_readlinkat_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat_common(at_f, path_addr, buf_addr, bufsize);
}
dword_t sys_readlink(addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat_common(AT_FDCWD_, path_addr, buf_addr, bufsize);
}
dword_t sys_readlink_guest(guest_addr_t path_addr, guest_addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat_common(AT_FDCWD_, path_addr, buf_addr, bufsize);
}
dword_t sys_readlinkat(fd_t at_f, addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat_common(at_f, path_addr, buf_addr, bufsize);
}

static dword_t sys_linkat_common(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr) {
    char src[MAX_PATH];
    int path_err = user_read_path(src_addr, src, sizeof(src));
    if (path_err)
        return path_err;
    char dst[MAX_PATH];
    path_err = user_read_path(dst_addr, dst, sizeof(dst));
    if (path_err)
        return path_err;
    STRACE("linkat(%d, \"%s\", %d, \"%s\")", src_at_f, src, dst_at_f, dst);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_linkat(src_at, src, dst_at, dst);
}

dword_t sys_linkat_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr) {
    return sys_linkat_common(src_at_f, src_addr, dst_at_f, dst_addr);
}
dword_t sys_link(addr_t src_addr, addr_t dst_addr) {
    return sys_linkat_common(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr);
}
dword_t sys_link_guest(guest_addr_t src_addr, guest_addr_t dst_addr) {
    return sys_linkat_common(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr);
}
dword_t sys_linkat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    return sys_linkat_common(src_at_f, src_addr, dst_at_f, dst_addr);
}

#define AT_REMOVEDIR_ 0x200
static dword_t sys_unlinkat_common(fd_t at_f, guest_addr_t path_addr, int_t flags) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("unlinkat(%d, \"%s\", %d)", at_f, path, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    if (flags & AT_REMOVEDIR_)
        return generic_rmdirat(at, path);
    else
        return generic_unlinkat(at, path);
}

dword_t sys_unlinkat_guest(fd_t at_f, guest_addr_t path_addr, int_t flags) {
    return sys_unlinkat_common(at_f, path_addr, flags);
}
dword_t sys_unlink(addr_t path_addr) {
    return sys_unlinkat_common(AT_FDCWD_, path_addr, 0);
}
dword_t sys_unlink_guest(guest_addr_t path_addr) {
    return sys_unlinkat_common(AT_FDCWD_, path_addr, 0);
}
dword_t sys_unlinkat(fd_t at_f, addr_t path_addr, int_t flags) {
    return sys_unlinkat_common(at_f, path_addr, flags);
}

static dword_t sys_renameat2_common(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr, int_t flags) {
    char src[MAX_PATH];
    int path_err = user_read_path(src_addr, src, sizeof(src));
    if (path_err)
        return path_err;
    char dst[MAX_PATH];
    path_err = user_read_path(dst_addr, dst, sizeof(dst));
    if (path_err)
        return path_err;
    STRACE("renameat2(%d, \"%s\", %d, \"%s\", %#x)", src_at_f, src, dst_at_f, dst, flags);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_renameat(src_at, src, dst_at, dst, flags);
}

dword_t sys_renameat2_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr, int_t flags) {
    return sys_renameat2_common(src_at_f, src_addr, dst_at_f, dst_addr, flags);
}
dword_t sys_renameat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    return sys_renameat2_common(src_at_f, src_addr, dst_at_f, dst_addr, 0);
}

dword_t sys_rename(addr_t src_addr, addr_t dst_addr) {
    return sys_renameat2_common(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr, 0);
}
dword_t sys_renameat_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr) {
    return sys_renameat2_common(src_at_f, src_addr, dst_at_f, dst_addr, 0);
}
dword_t sys_rename_guest(guest_addr_t src_addr, guest_addr_t dst_addr) {
    return sys_renameat2_common(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr, 0);
}
dword_t sys_renameat2(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr, int_t flags) {
    return sys_renameat2_common(src_at_f, src_addr, dst_at_f, dst_addr, flags);
}

static dword_t sys_symlinkat_common(guest_addr_t target_addr, fd_t at_f, guest_addr_t link_addr) {
    char target[MAX_PATH];
    int path_err = user_read_path(target_addr, target, sizeof(target));
    if (path_err)
        return path_err;
    char link[MAX_PATH];
    path_err = user_read_path(link_addr, link, sizeof(link));
    if (path_err)
        return path_err;
    STRACE("symlinkat(\"%s\", %d, \"%s\")", target, at_f, link);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    return generic_symlinkat(target, at, link);
}

dword_t sys_symlinkat_guest(guest_addr_t target_addr, fd_t at_f, guest_addr_t link_addr) {
    return sys_symlinkat_common(target_addr, at_f, link_addr);
}
dword_t sys_symlink(addr_t target_addr, addr_t link_addr) {
    return sys_symlinkat_common(target_addr, AT_FDCWD_, link_addr);
}
dword_t sys_symlink_guest(guest_addr_t target_addr, guest_addr_t link_addr) {
    return sys_symlinkat_common(target_addr, AT_FDCWD_, link_addr);
}
dword_t sys_symlinkat(addr_t target_addr, fd_t at_f, addr_t link_addr) {
    return sys_symlinkat_common(target_addr, at_f, link_addr);
}

static dword_t sys_mknodat_common(fd_t at_f, guest_addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("mknodat(%d, \"%s\", %#x, %#x)", at_f, path, mode, dev);
    apply_umask(&mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    int err = generic_mknodat(at, path, mode, dev);
    if (fs_trace_elogind()) {
        printk("INFO: elogind mknodat pid=%d comm=%s at=%d path=%s mode=%#o dev=%#x result=%d\n",
               current->pid, current->comm, at_f, path, mode, dev, err);
    }
    return err;
}

dword_t sys_mknodat_guest(fd_t at_f, guest_addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat_common(at_f, path_addr, mode, dev);
}
dword_t sys_mknod(addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat_common(AT_FDCWD_, path_addr, mode, dev);
}
dword_t sys_mknod_guest(guest_addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat_common(AT_FDCWD_, path_addr, mode, dev);
}
dword_t sys_mknodat(fd_t at_f, addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat_common(at_f, path_addr, mode, dev);
}

static ssize_t sys_read_buf(fd_t fd_no, void *buf, size_t size) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    if (S_ISDIR(fd->type))
        return _EISDIR;

    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);
    if (fd->ops->read) {
        res = fd->ops->read(fd, buf, size);
    } else if (fd->ops->pread) {
        res = fd->ops->pread(fd, buf, size, fd->offset);
        if (res > 0) {
            fd->ops->lseek(fd, res, LSEEK_CUR);
        }
    } else {
        return _EBADF;
    }
    io_delay_end(delay_start);

    if (res >= 0) {
        char path[MAX_PATH];
        if (generic_getpath(fd, path) == 0) {
            amd64_as_source_trace_read(fd_no, path, buf, (size_t) res);
            if (fs_trace_elogind() && fs_trace_interesting_path(path)) {
                size_t print_size = res;
                if (print_size > 80)
                    print_size = 80;
                printk("INFO: elogind read pid=%d comm=%s fd=%d path=%s size=%zd data=\"%.*s\"\n",
                       current->pid, current->comm, fd_no, path, res, (int) print_size, (char *) buf);
            }
        }
        STRACE(" -> %zd bytes", res);
    }
    io_account_read(fd, res);
    return res;
}

static dword_t sys_read_common(fd_t fd_no, guest_addr_t buf_addr, dword_t size) {
    STRACE("read(%d, %#llx, %d)", fd_no, (unsigned long long) buf_addr, size);
    if (size > MAX_RW_COUNT)
        size = MAX_RW_COUNT;

    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (size > sizeof(stack_buf)) {
        buf = (char *) malloc(size);
        if (buf == NULL)
            return _ENOMEM;
    }
    
    int_t res = 0;
    
    TASK_MAY_BLOCK {
        res = (int_t)sys_read_buf(fd_no, buf, size);
    }
    if (res == _EINTR && signal_should_restart_syscall())
        res = _ERESTART;
    amd64_tty_stdio_trace("read", fd_no, buf_addr, size, res, buf, res > 0 ? (size_t) res : 0);
    if (res >= 0) {
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
    if (buf != stack_buf) free(buf);
    
    return res;
}

dword_t sys_read(fd_t fd_no, addr_t buf_addr, dword_t size) {
    return sys_read_common(fd_no, buf_addr, size);
}

dword_t sys_read_guest(fd_t fd_no, guest_addr_t buf_addr, dword_t size) {
    return sys_read_common(fd_no, buf_addr, size);
}

static ssize_t sys_write_buf(fd_t fd_no, void *buf, size_t size) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;

    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);
    if (fd->ops->write) {
        res = fd->ops->write(fd, buf, size);
    } else if (fd->ops->pwrite) {
        res = fd->ops->pwrite(fd, buf, size, fd->offset);
        if (res > 0) {
            fd->ops->lseek(fd, res, LSEEK_CUR);
        }
    } else {
        return _EBADF;
    }
    io_delay_end(delay_start);
    io_account_write(fd, res);
    if (res > 0) {
        if (fd->mount != NULL && fd->mount->fs == &procfs)
            return res;
        char path[MAX_PATH];
        if (generic_getpath(fd, path) == 0) {
            inotify_notify_modify(path);
            if (fs_trace_elogind() && fs_trace_interesting_path(path)) {
                size_t print_size = res;
                if (print_size > 80)
                    print_size = 80;
                printk("INFO: elogind write pid=%d comm=%s fd=%d path=%s size=%zd data=\"%.*s\"\n",
                       current->pid, current->comm, fd_no, path, res, (int) print_size, (char *) buf);
            }
        }
    }
    return res;
}

static dword_t sys_write_common(fd_t fd_no, guest_addr_t buf_addr, dword_t size) {
    // Capped below to prevent an unbounded allocation from a guest-supplied
    // size (see 61961b67, which added this cap and the matching ones on
    // read/pread/pwrite/readv/writev specifically to close that DoS).
    if (size > MAX_RW_COUNT)
        size = MAX_RW_COUNT;

    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (size > sizeof(stack_buf)) {
        buf = malloc(size);
        if (buf == NULL)
            return _ENOMEM;
    }

    dword_t res = _EFAULT;
    if (user_read(buf_addr, buf, size))
        goto out;

    STRACE("write(%d, %#llx, %d)", fd_no, (unsigned long long) buf_addr, size);

    TASK_MAY_BLOCK {
        res = sys_write_buf(fd_no, buf, size);
    }
    amd64_tty_stdio_trace("write", fd_no, buf_addr, size, res, buf, res > 0 ? (size_t) res : size);
out:
    if (buf != stack_buf) free(buf);
    return res;
}

dword_t sys_write(fd_t fd_no, addr_t buf_addr, dword_t size) {
    return sys_write_common(fd_no, buf_addr, size);
}

dword_t sys_write_guest(fd_t fd_no, guest_addr_t buf_addr, dword_t size) {
    return sys_write_common(fd_no, buf_addr, size);
}

// The vector operations work by flattening the vector into a malloc buffer.
// This at least isn't much worse than what it was before, which copied each
// element of the vector into a malloc buffer. The perfect solution would be to
// construct a vector with an entry for each page of the buffer. I haven't done
// that yet because it's more work and the efficiency gain from that is dwarfed
// by the inefficiency of the emulator.

static ssize_t iovec_size(struct guest_iovec_ *iovec, unsigned iovec_count) {
    size_t size = 0;
    for (unsigned i = 0; i < iovec_count; i++)
        size += iovec[i].len;
    return size;
}

static dword_t sys_readv_common(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, enum guest_abi abi) {
    STRACE("readv(%d, %#llx, %d)", fd_no, (unsigned long long) iovec_addr, iovec_count);
    struct guest_iovec_ *iovec = user_read_iovecs_abi(current, abi, iovec_addr, iovec_count);
    
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    if (io_size > MAX_RW_COUNT)
        io_size = MAX_RW_COUNT;
    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (io_size > sizeof(stack_buf)) {
        buf = malloc(io_size);
        if (buf == NULL) {
            free(iovec);
            return _ENOMEM;
        }
    }
    ssize_t res = 0;
    TASK_MAY_BLOCK {
        res = sys_read_buf(fd_no, buf, io_size);
    }
    if (res < 0)
        goto error;

    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        size_t len = iovec[i].len;
        size_t total = (size_t) res;
        if (offset >= total)
            break;
        if (offset + len > total)
            len = total - offset;

        STRACE(" {base=%#llx, len=%zu}", (unsigned long long) iovec[i].base, iovec[i].len);

        if (user_write(iovec[i].base, buf + offset, len)) {
            res = _EFAULT;
            goto error;
        }
        offset += len;
    }

error:
    if (buf != stack_buf) free(buf);
    free(iovec);
    return res;
}

dword_t sys_readv(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    return sys_readv_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_I386);
}

dword_t sys_readv_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count) {
    return sys_readv_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_I386);
}

dword_t sys_readv_amd64(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    return sys_readv_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
}

dword_t sys_readv_amd64_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count) {
    return sys_readv_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
}

static dword_t sys_writev_common(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, enum guest_abi abi) {
    STRACE("writev(%d, %#llx, %d)", fd_no, (unsigned long long) iovec_addr, iovec_count);
    struct guest_iovec_ *iovec = user_read_iovecs_abi(current, abi, iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    if (io_size > MAX_RW_COUNT)
        io_size = MAX_RW_COUNT;
    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (io_size > sizeof(stack_buf)) {
        buf = malloc(io_size);
        if (buf == NULL) {
            free(iovec);
            return _ENOMEM;
        }
    }

    ssize_t res = 0;
    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (offset >= io_size)
            break;
        size_t copy_len = iovec[i].len;
        if (offset + copy_len > io_size)
            copy_len = io_size - offset;

        if (user_read(iovec[i].base, buf + offset, copy_len)) {
            res = _EFAULT;
            goto error;
        }

        STRACE(" {base=%#llx, len=%zu}", (unsigned long long) iovec[i].base, iovec[i].len);
        offset += copy_len;
    }
    TASK_MAY_BLOCK {
        res = sys_write_buf(fd_no, buf, offset);
    }
    amd64_tty_stdio_trace("writev", fd_no, iovec_addr, (dword_t) offset, res, buf,
            res > 0 ? (size_t) res : offset);
error:
    if (buf != stack_buf) free(buf);
    free(iovec);
    return res;
}

dword_t sys_writev(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    return sys_writev_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_I386);
}

dword_t sys_writev_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count) {
    return sys_writev_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_I386);
}

dword_t sys_writev_amd64(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    return sys_writev_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
}

dword_t sys_writev_amd64_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count) {
    return sys_writev_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
}

// preadv/pwritev: positioned scatter-gather, built from the same pieces
// as readv/writev above (single bounce buffer) with the positioned I/O
// core of sys_pread/sys_pwrite. Every 64-bit ABI's libc still splits the
// offset into (lo, hi) words per the kernel ABI; the caller recombines.
static dword_t sys_preadv_common(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count,
        off_t_ off, enum guest_abi abi) {
    STRACE("preadv(%d, %#llx, %d, %lld)", fd_no, (unsigned long long) iovec_addr,
           iovec_count, (long long) off);
    struct guest_iovec_ *iovec = user_read_iovecs_abi(current, abi, iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    if (io_size > MAX_RW_COUNT)
        io_size = MAX_RW_COUNT;
    char *buf = malloc(io_size + 1);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }
    struct fd *fd = f_get(fd_no);
    ssize_t res;
    if (fd == NULL) {
        res = _EBADF;
        goto out;
    }
    task_may_block_start();
    lock(&fd->lock, 0);
    uint64_t delay_start = io_delay_start(fd);
    if (fd->ops->pread) {
        res = fd->ops->pread(fd, buf, io_size, off);
    } else if (fd->ops->lseek && fd->ops->read) {
        off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) >= 0) {
            res = fd->ops->read(fd, buf, io_size);
            fd->ops->lseek(fd, saved_off, LSEEK_SET);
        }
    } else {
        res = _ESPIPE;
    }
    io_delay_end(delay_start);
    unlock(&fd->lock);
    task_may_block_end();
    io_account_read(fd, res);
    if (res > 0) {
        size_t remaining = (size_t) res;
        size_t offset = 0;
        for (unsigned i = 0; i < iovec_count && remaining > 0; i++) {
            size_t copy_len = iovec[i].len;
            if (copy_len > remaining)
                copy_len = remaining;
            if (user_write(iovec[i].base, buf + offset, copy_len)) {
                res = _EFAULT;
                goto out;
            }
            offset += copy_len;
            remaining -= copy_len;
        }
    }
out:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys_preadv_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off) {
    return sys_preadv_common(fd_no, iovec_addr, iovec_count, off, GUEST_ABI_AMD64);
}

static dword_t sys_pwritev_common(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count,
        off_t_ off, enum guest_abi abi) {
    STRACE("pwritev(%d, %#llx, %d, %lld)", fd_no, (unsigned long long) iovec_addr,
           iovec_count, (long long) off);
    struct guest_iovec_ *iovec = user_read_iovecs_abi(current, abi, iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    if (io_size > MAX_RW_COUNT)
        io_size = MAX_RW_COUNT;
    char *buf = malloc(io_size + 1);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }
    ssize_t res = 0;
    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (offset >= io_size)
            break;
        size_t copy_len = iovec[i].len;
        if (offset + copy_len > io_size)
            copy_len = io_size - offset;
        if (user_read(iovec[i].base, buf + offset, copy_len)) {
            res = _EFAULT;
            goto out;
        }
        offset += copy_len;
    }
    struct fd *fd = f_get(fd_no);
    if (fd == NULL) {
        res = _EBADF;
        goto out;
    }
    task_may_block_start();
    lock(&fd->lock, 0);
    uint64_t delay_start = io_delay_start(fd);
    if (fd->ops->pwrite) {
        res = fd->ops->pwrite(fd, buf, offset, off);
    } else if (fd->ops->lseek && fd->ops->write) {
        off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) >= 0) {
            res = fd->ops->write(fd, buf, offset);
            fd->ops->lseek(fd, saved_off, LSEEK_SET);
        }
    } else {
        res = _ESPIPE;
    }
    io_delay_end(delay_start);
    unlock(&fd->lock);
    task_may_block_end();
    io_account_write(fd, res);
out:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys_pwritev_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off) {
    return sys_pwritev_common(fd_no, iovec_addr, iovec_count, off, GUEST_ABI_AMD64);
}

// preadv2/pwritev2: same as preadv/pwritev, but an offset of -1 means "use
// and advance the current file position" (like plain readv/writev) rather
// than a positioned access, and there's a trailing RWF_* flags word. The
// flags (RWF_HIPRI, RWF_DSYNC, RWF_SYNC, RWF_NOWAIT, RWF_APPEND) are
// performance/durability/append hints with no analog in our fd_ops
// backends, so they're accepted but ignored, like fadvise64.
dword_t sys_preadv2_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count,
        off_t_ off, uint_t UNUSED(flags)) {
    if (off == (off_t_) -1)
        return sys_readv_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
    return sys_preadv_common(fd_no, iovec_addr, iovec_count, off, GUEST_ABI_AMD64);
}

dword_t sys_pwritev2_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count,
        off_t_ off, uint_t UNUSED(flags)) {
    if (off == (off_t_) -1)
        return sys_writev_common(fd_no, iovec_addr, iovec_count, GUEST_ABI_AMD64);
    return sys_pwritev_common(fd_no, iovec_addr, iovec_count, off, GUEST_ABI_AMD64);
}

dword_t sys__llseek(fd_t f, dword_t off_high, dword_t off_low, addr_t res_addr, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock, 0);
    off_t_ off = ((qword_t) off_high << 32) | off_low;
    STRACE("llseek(%d, %lu, %#x, %d)", f, off, res_addr, whence);
    off_t_ res = fd->ops->lseek(fd, off, whence);
    STRACE(" -> %lu", res);
    unlock(&fd->lock);
    if (res < 0)
        return res;
    if (user_put(res_addr, res))
        return _EFAULT;
    return 0;
}

dword_t sys_lseek(fd_t f, dword_t off, dword_t whence) {
    off_t_ res = sys_lseek_guest(f, off, whence);
    if ((dword_t) res != res)
        return _EOVERFLOW;
    return (dword_t) res;
}

dword_t sys_lseek_amd64(fd_t f, dword_t off, dword_t whence) {
    off_t_ res = sys_lseek_amd64_guest(f, off, whence);
    if ((dword_t) res != res)
        return _EOVERFLOW;
    return (dword_t) res;
}

off_t_ sys_lseek_guest(fd_t f, off_t_ off, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock, 0);
    off_t_ res = fd->ops->lseek(fd, off, whence);
    unlock(&fd->lock);
    return res;
}

off_t_ sys_lseek_amd64_guest(fd_t f, off_t_ off, dword_t whence) {
    return sys_lseek_guest(f, off, whence);
}

dword_t sys_pread_guest(fd_t f, guest_addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pread(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    if (size > MAX_RW_COUNT)
        size = MAX_RW_COUNT;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (size >= sizeof(stack_buf)) {
        buf = malloc(size+1);
        if (buf == NULL)
            return _ENOMEM;
    }

    task_may_block_start();
    lock(&fd->lock, 0);
    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);
    if (fd->ops->pread) {
        res = fd->ops->pread(fd, buf, size, off);
    } else {
        off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) < 0) {
            io_delay_end(delay_start);
            goto out;
        }
        res = fd->ops->read(fd, buf, size);
        // This really shouldn't fail. The lseek man page lists these reasons:
        // EBADF, ESPIPE: can't happen because the last lseek wouldn't have succeeded.
        // EOVERFLOW: can't happen for LSEEK_SET.
        // EINVAL: can't happen other than typoing LSEEK_SET, because we know saved_off is not negative.
        off_t_ lseek_res = fd->ops->lseek(fd, saved_off, LSEEK_SET);
        assert(lseek_res >= 0);
    }
    io_delay_end(delay_start);
    io_account_read(fd, res);
    if (res >= 0) {
        char path[MAX_PATH];
        if (fs_trace_elogind() && generic_getpath(fd, path) == 0 && fs_trace_interesting_path(path)) {
            size_t print_size = res;
            if (print_size > 80)
                print_size = 80;
            printk("INFO: elogind pread pid=%d comm=%s fd=%d path=%s off=%d size=%zd data=\"%.*s\"\n",
                   current->pid, current->comm, f, path, off, res, (int) print_size, buf);
        }
        buf[res] = '\0';
        STRACE(" \"%.99s\"", buf);
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
out:
    unlock(&fd->lock);
    task_may_block_end();
    if (buf != stack_buf) free(buf);
    return res;
}

dword_t sys_pread(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    return sys_pread_guest(f, buf_addr, size, off);
}

dword_t sys_pwrite_guest(fd_t f, guest_addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pwrite(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    if (size > MAX_RW_COUNT)
        size = MAX_RW_COUNT;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    char stack_buf[256] __attribute__((aligned(16)));
    char *buf = stack_buf;
    if (size >= sizeof(stack_buf)) {
        buf = malloc(size+1);
        if (buf == NULL)
            return _ENOMEM;
    }

    if (user_read(buf_addr, buf, size)) {
        if (buf != stack_buf) free(buf);
        return _EFAULT;
    }
    
    lock(&fd->lock, 0);
    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);

    TASK_MAY_BLOCK {
        if (fd->ops->pwrite) {
            res = fd->ops->pwrite(fd, buf, size, off);
        } else {
            off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
            if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) >= 0) {
                res = fd->ops->write(fd, buf, size);
                // This really shouldn't fail. The lseek man page lists these reasons:
                // EBADF, ESPIPE: can't happen because the last lseek wouldn't have succeeded.
                // EOVERFLOW: can't happen for LSEEK_SET.
                // EINVAL: can't happen other than typoing LSEEK_SET, because we know saved_off is not negative.
                off_t_ lseek_res = fd->ops->lseek(fd, saved_off, LSEEK_SET);
                assert(lseek_res >= 0);
            }
        }
    }
    unlock(&fd->lock);
    io_delay_end(delay_start);
    io_account_write(fd, res);
    if (buf != stack_buf) free(buf);
    return res;
}

dword_t sys_pwrite(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    return sys_pwrite_guest(f, buf_addr, size, off);
}

static bool ioctl_user_arg_needs_read(dword_t cmd) {
    switch (cmd) {
        case TCGETS_:
        case TCGETS2_:
        case TIOCGPGRP_:
        case TIOCGWINSZ_:
        case TIOCGPTN_:
        case TIOCGPKT_:
        case FIONREAD_:
        case RNDGETENTCNT_:
            return false;
        default:
            return true;
    }
}

static bool ioctl_user_arg_needs_write(dword_t cmd) {
    switch (cmd) {
        case TCSETS_:
        case TCSETSF_:
        case TCSETSW_:
        case TCSETS2_:
        case TCSETSF2_:
        case TCSETSW2_:
        case TIOCSPGRP_:
        case TIOCSPTLCK_:
        case TIOCPKT_:
            return false;
        default:
            return true;
    }
}

static int fd_ioctl(struct fd *fd, dword_t cmd, guest_addr_t arg) {
    ssize_t size = -1;
    if (fd->ops->ioctl_size)
        size = fd->ops->ioctl_size(cmd);
    if (size < 0)
        return _ENOTTY;
    if (size == 0)
        return fd->ops->ioctl(fd, cmd, (void *) (unsigned long) arg);

    // Some ioctls are pure output and must not fault on unreadable scratch
    // buffers. Others are pure input and do not need a copy-back.
    char buf[size];
    if (ioctl_user_arg_needs_read(cmd)) {
        if (user_read(arg, buf, size))
            return _EFAULT;
    } else {
        memset(buf, 0, size);
    }
    int res = fd->ops->ioctl(fd, cmd, buf);
    if (res < 0)
        return res;
    if (ioctl_user_arg_needs_write(cmd) && user_write(arg, buf, size))
        return _EFAULT;
    return res;
}

static int set_nonblock(struct fd *fd, guest_addr_t nb_addr) {
    dword_t nonblock;
    if (user_get(nb_addr, nonblock))
        return _EFAULT;
    int flags = fd_getflags(fd);
    if (nonblock)
        flags |= O_NONBLOCK_;
    else
        flags &= ~O_NONBLOCK_;
    return fd_setflags(fd, flags);
}

static dword_t sys_ioctl_common(fd_t f, dword_t cmd, guest_addr_t arg) {
    STRACE("ioctl(%d, 0x%x, %#llx)", f, cmd, (unsigned long long) arg);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    switch (cmd) {
        case FIONBIO_:
            return set_nonblock(fd, arg);
        case FIOCLEX_:
            bit_set(f, current->files->cloexec);
            return 0;
        case FIONCLEX_:
            bit_clear(f, current->files->cloexec);
            return 0;
    }
    
    dword_t res = 0;
    TASK_MAY_BLOCK {
        res = fd_ioctl(fd, cmd, arg);
    }
    return res;
}

dword_t sys_ioctl(fd_t f, dword_t cmd, dword_t arg) {
    return sys_ioctl_common(f, cmd, arg);
}

dword_t sys_ioctl_guest(fd_t f, dword_t cmd, guest_addr_t arg) {
    return sys_ioctl_common(f, cmd, arg);
}

// Shared prefix-matching step for fs_rebase_path_to_root and
// fs_rebase_readlink_path: computes fs's chroot root path and, if `path`
// lies under it, rewrites `path` in place to the root-relative form (root
// itself -> "/"). Returns 1 if rebased, 0 if `path` is outside the root (or
// the caller isn't chrooted at all, in which case `path` is left untouched
// and the two callers' "outside the root" handling never triggers), or a
// negative errno on failure to resolve the root's own path.
static int fs_try_rebase_path(struct fs_info *fs, char *path) {
    lock(&fs->lock, 0);
    struct fd *root = fs->root;
    char root_path[MAX_PATH + 1];
    int root_err = root != NULL ? generic_getpath(root, root_path) : 0;
    unlock(&fs->lock);
    if (root_err < 0)
        return root_err;
    if (root == NULL || strcmp(root_path, "/") == 0)
        return 1; // un-chrooted: nothing to rebase, path is already correct

    size_t root_len = strlen(root_path);
    if (strcmp(path, root_path) == 0) {
        strcpy(path, "/");
        return 1;
    }
    if (strncmp(path, root_path, root_len) == 0 && path[root_len] == '/') {
        memmove(path, path + root_len, strlen(path + root_len) + 1);
        return 1;
    }
    return 0;
}

// Rebase a mount-absolute path (as returned by generic_getpath) to be
// relative to `fs`'s chroot root, matching Linux getcwd(2) semantics: the
// root's own path becomes "/", a path under the root has the root prefix
// stripped, and a path outside the root entirely (unreachable, e.g. chroot
// without a following chdir) is ENOENT. Mutates `path` in place; the result
// is always <= the input length.
int fs_rebase_path_to_root(struct fs_info *fs, char *path) {
    int rebased = fs_try_rebase_path(fs, path);
    if (rebased < 0)
        return rebased;
    return rebased ? 0 : _ENOENT;
}

// Like fs_rebase_path_to_root, but for readlink() of the magic-symlink
// targets under /proc/*/{cwd,root,exe,fd/N} -- those are computed via
// d_path() against the CALLING process's root on real Linux (so callers pass
// current->fs here even when reading another task's /proc entries), and
// unlike getcwd(2), d_path() does NOT fail when the target is outside the
// caller's root: it prefixes the un-rebased absolute path with
// "(unreachable)" and the readlink still succeeds. Getting this wrong as an
// outright ENOENT (as fs_rebase_path_to_root does) broke opening another
// process's /proc/<pid>/exe from inside a chroot entirely -- iSH's open()
// resolves a followed symlink's target by re-walking the string returned by
// .readlink() (see __path_normalize in fs/path.c), so a failing readlink()
// here made the subsequent open() fail too, not just the informational
// readlink(2) syscall.
int fs_rebase_readlink_path(struct fs_info *fs, char *path) {
    int rebased = fs_try_rebase_path(fs, path);
    if (rebased < 0)
        return rebased;
    if (rebased)
        return 0;
    static const char prefix[] = "(unreachable)";
    size_t path_len = strlen(path);
    if (sizeof(prefix) - 1 + path_len >= MAX_PATH)
        return _ENAMETOOLONG;
    memmove(path + sizeof(prefix) - 1, path, path_len + 1);
    memcpy(path, prefix, sizeof(prefix) - 1);
    return 0;
}

static dword_t sys_getcwd_common(guest_addr_t buf_addr, dword_t size) {
    STRACE("getcwd(%#x, %#x)", buf_addr, size);
    lock(&current->fs->lock, 0);
    struct fd *wd = current->fs->pwd;
    char pwd[MAX_PATH + 1];
    int err = generic_getpath(wd, pwd);
    unlock(&current->fs->lock);
    if (err < 0)
        return err;

    int rebase_err = fs_rebase_path_to_root(current->fs, pwd);
    if (rebase_err < 0)
        return rebase_err;

    size_t pwd_len = strlen(pwd);
    if (pwd_len + 1 > size)
        return _ERANGE;
    size = pwd_len + 1;
    STRACE(" \"%.*s\"", size, pwd);
    dword_t res = size;

    // Bolt: We can pass the stack-allocated buffer directly to user_write
    // instead of allocating, copying, and freeing a temporary heap buffer.
    if (user_write(buf_addr, pwd, size))
        res = _EFAULT;
    return res;
}
dword_t sys_getcwd_guest(guest_addr_t buf_addr, dword_t size) {
    return sys_getcwd_common(buf_addr, size);
}
dword_t sys_getcwd(addr_t buf_addr, dword_t size) {
    return sys_getcwd_common(buf_addr, size);
}

static struct fd *open_dir(const char *path) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, 0);
    if (err < 0)
        return ERR_PTR(err);
    if (!(stat.mode & S_IFDIR))
        return ERR_PTR(_ENOTDIR);

    return generic_open(path, O_RDONLY_, 0);
}

void fs_chdir(struct fs_info *fs, struct fd *fd) {
    lock(&fs->lock, 0);
    fd_close(fs->pwd);
    fs->pwd = fd;
    unlock(&fs->lock);
}

static dword_t sys_chdir_common(guest_addr_t path_addr) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("chdir(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    fs_chdir(current->fs, dir);
    return 0;
}
dword_t sys_chdir_guest(guest_addr_t path_addr) {
    return sys_chdir_common(path_addr);
}
dword_t sys_chdir(addr_t path_addr) {
    return sys_chdir_common(path_addr);
}

dword_t sys_fchdir(fd_t f) {
    STRACE("fchdir(%d)", f);
    struct fd *dir = f_get(f);
    if (dir == NULL)
        return _EBADF;
    // Linux fchdir() requires the fd to be a directory; a non-directory (e.g. a
    // socket, as stress-ng --sockabuse's fd-abuse sweep does) returns ENOTDIR
    // without touching the cwd. The path-based chdir already enforces this via
    // open_dir(); fchdir must match. Skipping the check pinned the fd as
    // current->fs->pwd (fs_chdir retains it), so closing a listening socket
    // never freed the real host socket -- the port stayed EADDRINUSE and the
    // sockabuse retry loop spun for minutes.
    if (!S_ISDIR(dir->type))
        return _ENOTDIR;
    dir->refcount++;
    fs_chdir(current->fs, dir);
    return 0;
}

static dword_t sys_chroot_common(guest_addr_t path_addr) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("chroot(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    lock(&current->fs->lock, 0);
    fd_close(current->fs->root);
    current->fs->root = dir;
    unlock(&current->fs->lock);
    return 0;
}
dword_t sys_chroot_guest(guest_addr_t path_addr) {
    return sys_chroot_common(path_addr);
}
dword_t sys_chroot(addr_t path_addr) {
    return sys_chroot_common(path_addr);
}

dword_t sys_umask(dword_t mask) {
    STRACE("umask(0%o)", mask);
    struct fs_info *fs = current->fs;
    lock(&fs->lock, 0);
    mode_t_ old_umask = fs->umask;
    fs->umask = ((mode_t_) mask) & 0777;
    unlock(&fs->lock);
    return old_umask;
}

static int mount_statfs(struct mount *mount, struct statfsbuf *stat) {
    int err = 0;
    if (mount->fs->statfs)
        err = mount->fs->statfs(mount, stat);
    if (stat->type == 0)
        stat->type = mount->fs->magic;
    return err;
}

static int_t statfs_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

static int_t statfs_mount_amd64(struct mount *mount, guest_addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct amd64_statfs_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

static int_t statfs64_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs64_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

dword_t sys_statfs(addr_t path_addr, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    int path_err = user_read_path(path_addr, path_raw, sizeof(path_raw));
    if (path_err)
        return path_err;
    STRACE("statfs(\"%s\", %#x)", path_raw, buf_addr);
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return _ENOENT;
    err = statfs_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

static dword_t sys_statfs_amd64_common(guest_addr_t path_addr, guest_addr_t buf_addr) {
    char path_raw[MAX_PATH];
    int path_err = user_read_path(path_addr, path_raw, sizeof(path_raw));
    if (path_err)
        return path_err;
    STRACE("statfs_amd64(\"%s\", %#x)", path_raw, buf_addr);
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return _ENOENT;
    err = statfs_mount_amd64(mount, buf_addr);
    mount_release(mount);
    return err;
}
dword_t sys_statfs_amd64_guest(guest_addr_t path_addr, guest_addr_t buf_addr) {
    return sys_statfs_amd64_common(path_addr, buf_addr);
}
dword_t sys_statfs_amd64(addr_t path_addr, addr_t buf_addr) {
    return sys_statfs_amd64_common(path_addr, buf_addr);
}

dword_t sys_statfs64(addr_t path_addr, dword_t buf_size, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    int path_err = user_read_path(path_addr, path_raw, sizeof(path_raw));
    if (path_err)
        return path_err;
    STRACE("statfs64(\"%s\", %d, %#x)", path_raw, buf_size, buf_addr);
    if (buf_size != sizeof(struct statfs64_))
        return _EINVAL;
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return _ENOENT;
    err = statfs64_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

dword_t sys_fstatfs(fd_t f, addr_t buf_addr) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return statfs_mount(fd->mount, buf_addr);
}

dword_t sys_fstatfs_amd64_guest(fd_t f, guest_addr_t buf_addr) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return statfs_mount_amd64(fd->mount, buf_addr);
}
dword_t sys_fstatfs_amd64(fd_t f, addr_t buf_addr) {
    return sys_fstatfs_amd64_guest(f, buf_addr);
}

dword_t sys_fstatfs64(fd_t f, dword_t buf_size, addr_t buf_addr) {
    if (buf_size != sizeof(struct statfs64_))
        return _EINVAL;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return statfs64_mount(fd->mount, buf_addr);
}

dword_t sys_flock(fd_t f, dword_t operation) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL)
        return flock_lock(fd, operation);
    // TODO: POSIX doesn't allow flock to fail in this way. The check is here
    // because a segfault is worse.
    if (fd->mount->fs->flock == NULL)
        return _EBADF;
    return fd->mount->fs->flock(fd, operation);
}

static dword_t sys_utime_common(fd_t at_f, guest_addr_t path_addr, struct timespec atime, struct timespec mtime, dword_t flags) {
    char path[MAX_PATH];
    if (path_addr != 0) {
        int path_err = user_read_path(path_addr, path, sizeof(path));
        if (path_err)
            return path_err;
    } else {
        path[0] = '\0';
    }
    STRACE("utimensat(%d, %s, {{%d, %d}, {%d, %d}}, %d)", at_f, path,
            atime.tv_sec, atime.tv_nsec, mtime.tv_sec, mtime.tv_nsec, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;

    if (path_addr == 0) {
        // The futimens(fd) form: utimensat(fd, NULL, times, 0). Linux does no
        // path resolution here -- it operates on the open fd itself -- so it
        // works on unlinked-but-open files and on fds with no path at all
        // (sockets, pipes; stress-ng --sockabuse futimens()es a socket).
        // Linux's do_utimes() rejects any flags in this form, and a NULL path
        // with AT_FDCWD faults in getname().
        if (at == AT_PWD)
            return _EFAULT;
        if (flags != 0)
            return _EINVAL;
        if (at->mount != NULL && at->mount->fs->futime != NULL)
            return at->mount->fs->futime(at, atime, mtime);
        // No fd-direct support on this fs: fall back to resolving the fd's
        // own path (the historical behavior).
        return generic_utime(at, ".", atime, mtime, true);
    }

    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;
    return generic_utime(at, path, atime, mtime, follow_links);
}

dword_t sys_utimensat64(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = sizeof(struct timespec64_);
        if (read_guest_timespec_abi(GUEST_ABI_AMD64, times_addr, &atime) ||
                read_guest_timespec_abi(GUEST_ABI_AMD64, times_addr + stride, &mtime))
            return _EFAULT;
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, flags);
}

dword_t sys_utimensat_amd64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr, dword_t flags) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timespec_size(GUEST_ABI_AMD64);
        if (read_guest_timespec_abi(GUEST_ABI_AMD64, times_addr, &atime) ||
                read_guest_timespec_abi(GUEST_ABI_AMD64, times_addr + stride, &mtime))
            return _EFAULT;
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, flags);
}

dword_t sys_utimensat_amd64(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags) {
    return sys_utimensat_amd64_guest(at_f, path_addr, times_addr, flags);
}

dword_t sys_utimensat_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr, dword_t flags) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timespec_size(GUEST_ABI_I386);
        if (read_guest_timespec_abi(GUEST_ABI_I386, times_addr, &atime) ||
                read_guest_timespec_abi(GUEST_ABI_I386, times_addr + stride, &mtime))
            return _EFAULT;
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, flags);
}
dword_t sys_utimensat(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags) {
    return sys_utimensat_guest(at_f, path_addr, times_addr, flags);
}

dword_t sys_utimes_amd64_guest(guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timeval_size(GUEST_ABI_AMD64);
        struct timeval time_a;
        struct timeval time_m;
        if (read_guest_timeval_abi(GUEST_ABI_AMD64, times_addr, &time_a) ||
                read_guest_timeval_abi(GUEST_ABI_AMD64, times_addr + stride, &time_m))
            return _EFAULT;
        atime.tv_sec = time_a.tv_sec;
        atime.tv_nsec = time_a.tv_usec * 1000;
        mtime.tv_sec = time_m.tv_sec;
        mtime.tv_nsec = time_m.tv_usec * 1000;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

dword_t sys_utimes_amd64(addr_t path_addr, addr_t times_addr) {
    return sys_utimes_amd64_guest(path_addr, times_addr);
}

dword_t sys_utimes_guest(guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timeval_size(GUEST_ABI_I386);
        struct timeval time_a;
        struct timeval time_m;
        if (read_guest_timeval_abi(GUEST_ABI_I386, times_addr, &time_a) ||
                read_guest_timeval_abi(GUEST_ABI_I386, times_addr + stride, &time_m))
            return _EFAULT;
        atime.tv_sec = time_a.tv_sec;
        atime.tv_nsec = time_a.tv_usec * 1000;
        mtime.tv_sec = time_m.tv_sec;
        mtime.tv_nsec = time_m.tv_usec * 1000;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}
dword_t sys_utimes(addr_t path_addr, addr_t times_addr) {
    return sys_utimes_guest(path_addr, times_addr);
}

dword_t sys_futimesat_amd64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timeval_size(GUEST_ABI_AMD64);
        struct timeval time_a;
        struct timeval time_m;
        if (read_guest_timeval_abi(GUEST_ABI_AMD64, times_addr, &time_a) ||
                read_guest_timeval_abi(GUEST_ABI_AMD64, times_addr + stride, &time_m))
            return _EFAULT;
        atime.tv_sec = time_a.tv_sec;
        atime.tv_nsec = time_a.tv_usec * 1000;
        mtime.tv_sec = time_m.tv_sec;
        mtime.tv_nsec = time_m.tv_usec * 1000;
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, 0);
}

dword_t sys_futimesat_amd64(fd_t at_f, addr_t path_addr, addr_t times_addr) {
    return sys_futimesat_amd64_guest(at_f, path_addr, times_addr);
}

dword_t sys_futimesat_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        size_t stride = guest_timeval_size(GUEST_ABI_I386);
        struct timeval time_a;
        struct timeval time_m;
        if (read_guest_timeval_abi(GUEST_ABI_I386, times_addr, &time_a) ||
                read_guest_timeval_abi(GUEST_ABI_I386, times_addr + stride, &time_m))
            return _EFAULT;
        atime.tv_sec = time_a.tv_sec;
        atime.tv_nsec = time_a.tv_usec * 1000;
        mtime.tv_sec = time_m.tv_sec;
        mtime.tv_nsec = time_m.tv_usec * 1000;
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, 0);
}
dword_t sys_futimesat(fd_t at_f, addr_t path_addr, addr_t times_addr) {
    return sys_futimesat_guest(at_f, path_addr, times_addr);
}

dword_t sys_utime_amd64_guest(guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct amd64_utimbuf_ {
            qword_t actime;
            qword_t modtime;
        } times;
        if (user_get(times_addr, times))
            return _EFAULT;
        atime.tv_sec = times.actime;
        atime.tv_nsec = 0;
        mtime.tv_sec = times.modtime;
        mtime.tv_nsec = 0;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

dword_t sys_utime_amd64(addr_t path_addr, addr_t times_addr) {
    return sys_utime_amd64_guest(path_addr, times_addr);
}

dword_t sys_utime_guest(guest_addr_t path_addr, guest_addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct utimbuf_ {
            time_t_ actime;
            time_t_ modtime;
        } times;
        if (user_get(times_addr, times))
            return _EFAULT;
        atime.tv_sec = times.actime;
        atime.tv_nsec = 0;
        mtime.tv_sec = times.modtime;
        mtime.tv_nsec = 0;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}
dword_t sys_utime(addr_t path_addr, addr_t times_addr) {
    return sys_utime_guest(path_addr, times_addr);
}

static int generic_fsetattr(struct fd *fd, struct attr attr) {
    if (fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    int err = fd->mount->fs->fsetattr(fd, attr);
    if (err >= 0) {
        char path[MAX_PATH];
        if (generic_getpath(fd, path) == 0) {
            if (attr.type == attr_size)
                inotify_notify_modify(path);
            else
                inotify_notify_attrib(path);
        }
    }
    return err;
}

dword_t sys_fchmod(fd_t f, dword_t mode) {
    STRACE("fchmod(%d, %o)", f, mode);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    mode &= ~S_IFMT;
    return generic_fsetattr(fd, make_attr(mode, mode));
}

static dword_t sys_fchmodat_common(fd_t at_f, guest_addr_t path_addr, dword_t mode, dword_t flags, bool is_fchmodat2) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    if (is_fchmodat2) {
        STRACE("fchmodat2(%d, \"%s\", %o, 0x%x)", at_f, path, mode, flags);
        if (flags & ~FCHMODAT2_ALLOWED_FLAGS_)
            return _EINVAL;
    } else {
        STRACE("fchmodat(%d, \"%s\", %o)", at_f, path, mode);
    }
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    mode &= ~S_IFMT;

    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        if (at_f == AT_FDCWD_)
            return generic_setattrat(AT_PWD, ".", make_attr(mode, mode), true);
        return generic_fsetattr(at, make_attr(mode, mode));
    }

    bool follow_links = !(flags & AT_SYMLINK_NOFOLLOW_);
    if (!follow_links) {
        struct statbuf statbuf = {};
        int err = generic_statat(at, path, &statbuf, AT_SYMLINK_NOFOLLOW_);
        if (err < 0)
            return err;
        if (S_ISLNK(statbuf.mode))
            return _EOPNOTSUPP;
    }
    return generic_setattrat(at, path, make_attr(mode, mode), follow_links);
}

dword_t sys_fchmodat(fd_t at_f, addr_t path_addr, dword_t mode) {
    return sys_fchmodat_common(at_f, path_addr, mode, 0, false);
}

dword_t sys_fchmodat_guest(fd_t at_f, guest_addr_t path_addr, dword_t mode) {
    return sys_fchmodat_common(at_f, path_addr, mode, 0, false);
}
dword_t sys_fchmodat2(fd_t at_f, addr_t path_addr, dword_t mode, dword_t flags) {
    return sys_fchmodat_common(at_f, path_addr, mode, flags, true);
}
dword_t sys_fchmodat2_guest(fd_t at_f, guest_addr_t path_addr, dword_t mode, dword_t flags) {
    return sys_fchmodat_common(at_f, path_addr, mode, flags, true);
}

dword_t sys_chmod(addr_t path_addr, dword_t mode) {
    return sys_fchmodat(AT_FDCWD_, path_addr, mode);
}
dword_t sys_chmod_guest(guest_addr_t path_addr, dword_t mode) {
    return sys_fchmodat_guest(AT_FDCWD_, path_addr, mode);
}

static dword_t sys_fchown_common(fd_t f, uid_t_ owner, uid_t_ group) {
    STRACE("fchown(%d, %d, %d)", f, owner, group);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err;
    if (owner != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(uid, owner));
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(gid, group));
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_fchown32(fd_t f, uid_t_ owner, uid_t_ group) {
    return sys_fchown_common(f, owner, group);
}

dword_t sys_fchown_amd64(fd_t f, uid_t_ owner, uid_t_ group) {
    return sys_fchown_common(f, owner, group);
}

static dword_t sys_fchownat_common(fd_t at_f, guest_addr_t path_addr, dword_t owner, dword_t group, int flags) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("fchownat(%d, \"%s\", %d, %d, %d)", at_f, path, owner, group, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    int err;
    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;

    // AT_EMPTY_PATH: an empty pathname operates on the at fd itself -- the
    // fchownat(fd, "", ..., AT_EMPTY_PATH) idiom glibc/systemd use as
    // fchown-by-fd (AT_FDCWD means the current directory). Without this the
    // empty path is resolved relative to `at` and returns ENOENT, which broke
    // systemd-sysusers copying /etc/gshadow's permissions onto its temp file
    // (openssh-server / polkitd user creation). Mirrors sys_fchmodat_common.
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        if (owner != (uid_t) -1) {
            err = at_f == AT_FDCWD_ ? generic_setattrat(AT_PWD, ".", make_attr(uid, owner), true)
                                    : generic_fsetattr(at, make_attr(uid, owner));
            if (err < 0)
                return err;
        }
        if (group != (uid_t) -1) {
            err = at_f == AT_FDCWD_ ? generic_setattrat(AT_PWD, ".", make_attr(gid, group), true)
                                    : generic_fsetattr(at, make_attr(gid, group));
            if (err < 0)
                return err;
        }
        return 0;
    }

    if (owner != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(uid, owner), follow_links);
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(gid, group), follow_links);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_fchownat_guest(fd_t at_f, guest_addr_t path_addr, dword_t owner, dword_t group, int flags) {
    return sys_fchownat_common(at_f, path_addr, owner, group, flags);
}
dword_t sys_fchownat(fd_t at_f, addr_t path_addr, dword_t owner, dword_t group, int flags) {
    return sys_fchownat_common(at_f, path_addr, owner, group, flags);
}
dword_t sys_chown32(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, 0);
}
dword_t sys_chown32_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, 0);
}

dword_t sys_chown_amd64(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, 0);
}
dword_t sys_chown_amd64_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, 0);
}

dword_t sys_lchown(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}
dword_t sys_lchown_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_lchown_amd64(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}
dword_t sys_lchown_amd64_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat_common(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_truncate64_guest(guest_addr_t path_addr, dword_t size_low, dword_t size_high) {
    off_t_ size = ((qword_t) size_high << 32) | size_low;
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    return generic_setattrat(NULL, path, make_attr(size, size), true);
}
dword_t sys_truncate64(addr_t path_addr, dword_t size_low, dword_t size_high) {
    return sys_truncate64_guest(path_addr, size_low, size_high);
}

dword_t sys_ftruncate64(fd_t f, dword_t size_low, dword_t size_high) {
    off_t_ size = ((qword_t) size_high << 32) | size_low;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return generic_fsetattr(fd, make_attr(size, size));
}

dword_t sys_ftruncate(fd_t f, dword_t size) { 
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return generic_fsetattr(fd, make_attr(size, size));
}

dword_t sys_fallocate(fd_t f, dword_t UNUSED(mode), dword_t offset_low, dword_t offset_high, dword_t len_low, dword_t len_high) {
    off_t_ offset = ((qword_t) offset_high << 32) | offset_low;
    off_t_ len = ((qword_t) len_high << 32) | len_low;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct statbuf statbuf;
    int err = fd->mount->fs->fstat(fd, &statbuf);
    if (err < 0)
        return err;
    if ((uint64_t) offset + (uint64_t) len > statbuf.size)
        return generic_fsetattr(fd, make_attr(size, offset + len));
    return 0;
}

static dword_t sys_mkdirat_common(fd_t at_f, guest_addr_t path_addr, mode_t_ mode) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("mkdirat(%d, %s, 0%o)", at_f, path, mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    apply_umask(&mode);
    mode &= 0777;
    int err = generic_mkdirat(at, path, mode);
    if (fs_trace_elogind())
        printk("INFO: elogind mkdirat pid=%d comm=%s at=%d path=%s mode=%#o result=%d\n",
               current->pid, current->comm, at_f, path, mode, err);
    return err;
}

dword_t sys_mkdirat_guest(fd_t at_f, guest_addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat_common(at_f, path_addr, mode);
}
dword_t sys_mkdir(addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat_common(AT_FDCWD_, path_addr, mode);
}
dword_t sys_mkdir_guest(guest_addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat_common(AT_FDCWD_, path_addr, mode);
}
dword_t sys_mkdirat(fd_t at_f, addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat_common(at_f, path_addr, mode);
}

dword_t sys_rmdir_guest(guest_addr_t path_addr) {
    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("rmdir(%s)", path);
    return generic_rmdirat(AT_PWD, path);
}
dword_t sys_rmdir(addr_t path_addr) {
    return sys_rmdir_guest(path_addr);
}

dword_t sys_fsync(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err = 0;
    TASK_MAY_BLOCK {
        if (fd->ops->fsync)
            err = fd->ops->fsync(fd);
    }
    return err;
}

dword_t sys_syncfs(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return 0;
}

// a few stubs
// Read one chunk into a host buffer for the sendfile()/copy_file_range() engine.
// If off != NULL the read happens at *off without disturbing the fd's own file
// position and *off is advanced; otherwise the fd's current position is used and
// advanced (mirrors sys_read_buf). Returns bytes read, 0 at EOF, or -errno.
static ssize_t copy_read_chunk(struct fd *fd, void *buf, size_t size, off_t_ *off) {
    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);
    lock(&fd->lock, 0);
    if (off == NULL) {
        if (fd->ops->read)
            res = fd->ops->read(fd, buf, size);
        else if (fd->ops->pread) {
            res = fd->ops->pread(fd, buf, size, fd->offset);
            if (res > 0)
                fd->ops->lseek(fd, res, LSEEK_CUR);
        } else
            res = _EINVAL;
    } else {
        if (fd->ops->pread)
            res = fd->ops->pread(fd, buf, size, *off);
        else if (fd->ops->read && fd->ops->lseek) {
            off_t_ saved = fd->ops->lseek(fd, 0, LSEEK_CUR);
            if ((res = fd->ops->lseek(fd, *off, LSEEK_SET)) >= 0) {
                res = fd->ops->read(fd, buf, size);
                fd->ops->lseek(fd, saved, LSEEK_SET);
            }
        } else
            res = _ESPIPE;
        if (res > 0)
            *off += res;
    }
    unlock(&fd->lock);
    io_delay_end(delay_start);
    return res;
}

// Symmetric write side for the copy engine.
static ssize_t copy_write_chunk(struct fd *fd, const void *buf, size_t size, off_t_ *off) {
    ssize_t res;
    uint64_t delay_start = io_delay_start(fd);
    lock(&fd->lock, 0);
    if (off == NULL) {
        if (fd->ops->write)
            res = fd->ops->write(fd, buf, size);
        else if (fd->ops->pwrite) {
            res = fd->ops->pwrite(fd, buf, size, fd->offset);
            if (res > 0)
                fd->ops->lseek(fd, res, LSEEK_CUR);
        } else
            res = _EINVAL;
    } else {
        if (fd->ops->pwrite)
            res = fd->ops->pwrite(fd, buf, size, *off);
        else if (fd->ops->write && fd->ops->lseek) {
            off_t_ saved = fd->ops->lseek(fd, 0, LSEEK_CUR);
            if ((res = fd->ops->lseek(fd, *off, LSEEK_SET)) >= 0) {
                res = fd->ops->write(fd, buf, size);
                fd->ops->lseek(fd, saved, LSEEK_SET);
            }
        } else
            res = _ESPIPE;
        if (res > 0)
            *off += res;
    }
    unlock(&fd->lock);
    io_delay_end(delay_start);
    return res;
}

// Shared engine for sendfile() and copy_file_range(): move up to count bytes
// from in_fd to out_fd through a host bounce buffer. in_off/out_off are host
// copies of the caller's offsets (NULL = use the fd's current position). Returns
// bytes copied (possibly short, e.g. at EOF or on a signal), or -errno if none.
static dword_t fd_copy_range(fd_t in_no, off_t_ *in_off, fd_t out_no, off_t_ *out_off, uint64_t count) {
    struct fd *in_fd = f_get(in_no);
    struct fd *out_fd = f_get(out_no);
    if (in_fd == NULL || out_fd == NULL)
        return _EBADF;
    if (S_ISDIR(in_fd->type) || S_ISDIR(out_fd->type))
        return _EISDIR;
    if ((fd_getflags(in_fd) & O_ACCMODE_) == O_WRONLY_)
        return _EBADF;
    if ((fd_getflags(out_fd) & O_ACCMODE_) == O_RDONLY_)
        return _EBADF;
    if (count > MAX_RW_COUNT)
        count = MAX_RW_COUNT;
    if (count == 0)
        return 0;

    size_t bufsize = count < 65536 ? (size_t) count : 65536;
    char *buf = malloc(bufsize);
    if (buf == NULL)
        return _ENOMEM;

    dword_t total = 0;
    int err = 0;
    TASK_MAY_BLOCK {
        while (total < count) {
            size_t want = count - total < bufsize ? (size_t) (count - total) : bufsize;
            ssize_t nr = copy_read_chunk(in_fd, buf, want, in_off);
            if (nr < 0) { err = (int) nr; break; }
            if (nr == 0) break; // EOF on the input
            io_account_read(in_fd, nr);
            ssize_t nw = copy_write_chunk(out_fd, buf, (size_t) nr, out_off);
            if (nw < 0) { err = (int) nw; break; }
            io_account_write(out_fd, nw);
            total += (dword_t) nw;
            if (nw < nr) break; // short write: report progress, stop
        }
    }

    free(buf);
    if (total > 0)
        return total;
    return err < 0 ? (dword_t) err : 0;
}

// sendfile(out_fd, in_fd, offset, count): the offset, if present, applies to
// in_fd (read side); out_fd always uses its current position. off64 selects the
// width of the guest *offset (i386 sendfile is 32-bit, sendfile64/amd64 64-bit).
static dword_t do_sendfile(fd_t out_fd, fd_t in_fd, guest_addr_t offset_addr, uint64_t count, bool off64) {
    off_t_ off = 0;
    off_t_ *off_ptr = NULL;
    if (offset_addr != 0) {
        if (off64) {
            if (user_get(offset_addr, off))
                return _EFAULT;
        } else {
            dword_t off32;
            if (user_get(offset_addr, off32))
                return _EFAULT;
            off = (off_t_) (sdword_t) off32;
        }
        off_ptr = &off;
    }
    dword_t res = fd_copy_range(in_fd, off_ptr, out_fd, NULL, count);
    if ((sdword_t) res >= 0 && off_ptr != NULL) {
        if (off64) {
            if (user_put(offset_addr, off))
                return _EFAULT;
        } else {
            dword_t off32 = (dword_t) off;
            if (user_put(offset_addr, off32))
                return _EFAULT;
        }
    }
    return res;
}

dword_t sys_sendfile(fd_t out_fd, fd_t in_fd, addr_t offset_addr, dword_t count) {
    STRACE("sendfile(%d, %d, 0x%x, %d)", out_fd, in_fd, offset_addr, count);
    return do_sendfile(out_fd, in_fd, offset_addr, count, false);
}
dword_t sys_sendfile64(fd_t out_fd, fd_t in_fd, addr_t offset_addr, dword_t count) {
    STRACE("sendfile64(%d, %d, 0x%x, %d)", out_fd, in_fd, offset_addr, count);
    return do_sendfile(out_fd, in_fd, offset_addr, count, true);
}
dword_t sys_sendfile_guest(fd_t out_fd, fd_t in_fd, guest_addr_t offset_addr, uint64_t count) {
    STRACE("sendfile(%d, %d, 0x%llx, %llu)", out_fd, in_fd, (unsigned long long) offset_addr,
           (unsigned long long) count);
    return do_sendfile(out_fd, in_fd, offset_addr, count, true); // amd64 off_t is 64-bit
}
dword_t sys_splice(fd_t UNUSED(in_fd), addr_t UNUSED(in_off_addr), fd_t UNUSED(out_fd), addr_t UNUSED(out_off_addr), dword_t UNUSED(count), dword_t UNUSED(flags)) {
    return _EINVAL;
}
// copy_file_range(fd_in, off_in, fd_out, off_out, len, flags). off_in/off_out
// are loff_t* (64-bit on both ABIs) or NULL. flags must be 0.
static dword_t do_copy_file_range(fd_t in_fd, guest_addr_t in_off_addr, fd_t out_fd,
        guest_addr_t out_off_addr, uint64_t len, uint_t flags) {
    if (flags != 0)
        return _EINVAL;
    // Unlike sendfile (which legitimately targets a socket as out_fd, and
    // shares fd_copy_range below), Linux's copy_file_range requires both
    // ends to be regular files and rejects anything else -- including
    // sockets and pipes -- with EINVAL before touching either fd, confirmed
    // against real Linux. fd_copy_range has no such check, so falling
    // through here for a socket would reach a genuine blocking read() with
    // none of sys_recvfrom's signal-interruptible wrapping. stress-ng
    // --sockabuse calls copy_file_range on a connected socket pair
    // expecting exactly this EINVAL; getting past this check instead hung
    // that thread forever (nothing else ever writes to the socket) in a
    // read() no signal could interrupt, wedging the whole stress-ng run.
    struct fd *in_f = f_get(in_fd);
    struct fd *out_f = f_get(out_fd);
    if (in_f == NULL || out_f == NULL)
        return _EBADF;
    if (!S_ISREG(in_f->type) || !S_ISREG(out_f->type))
        return _EINVAL;
    off_t_ in_off = 0, out_off = 0;
    off_t_ *in_ptr = NULL, *out_ptr = NULL;
    if (in_off_addr != 0) {
        if (user_get(in_off_addr, in_off))
            return _EFAULT;
        in_ptr = &in_off;
    }
    if (out_off_addr != 0) {
        if (user_get(out_off_addr, out_off))
            return _EFAULT;
        out_ptr = &out_off;
    }
    dword_t res = fd_copy_range(in_fd, in_ptr, out_fd, out_ptr, len);
    if ((sdword_t) res >= 0) {
        if (in_ptr != NULL && user_put(in_off_addr, in_off))
            return _EFAULT;
        if (out_ptr != NULL && user_put(out_off_addr, out_off))
            return _EFAULT;
    }
    return res;
}

dword_t sys_copy_file_range(fd_t in_fd, addr_t in_off, fd_t out_fd, addr_t out_off,
        dword_t len, uint_t flags) {
    STRACE("copy_file_range(%d, 0x%x, %d, 0x%x, %d, %#x)", in_fd, in_off, out_fd, out_off, len, flags);
    return do_copy_file_range(in_fd, in_off, out_fd, out_off, len, flags);
}
dword_t sys_copy_file_range_guest(fd_t in_fd, guest_addr_t in_off, fd_t out_fd, guest_addr_t out_off,
        uint64_t len, uint_t flags) {
    STRACE("copy_file_range(%d, 0x%llx, %d, 0x%llx, %llu, %#x)", in_fd,
           (unsigned long long) in_off, out_fd, (unsigned long long) out_off,
           (unsigned long long) len, flags);
    return do_copy_file_range(in_fd, in_off, out_fd, out_off, len, flags);
}

dword_t sys_xattr_stub(addr_t UNUSED(path_addr), addr_t UNUSED(name_addr),
        addr_t UNUSED(value_addr), dword_t UNUSED(size), dword_t UNUSED(flags)) {
    return _ENOTSUP;
}

static dword_t sys_xattr_stub_guest_impl(guest_addr_t UNUSED(path_addr), guest_addr_t UNUSED(name_addr),
        guest_addr_t UNUSED(value_addr), dword_t UNUSED(size), dword_t UNUSED(flags)) {
    return _ENOTSUP;
}

dword_t sys_setxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, dword_t size, dword_t flags) {
    return sys_xattr_stub_guest_impl(path_addr, name_addr, value_addr, size, flags);
}

dword_t sys_fsetxattr_guest(fd_t UNUSED(fd), guest_addr_t name_addr,
        guest_addr_t value_addr, dword_t size, dword_t flags) {
    return sys_xattr_stub_guest_impl(0, name_addr, value_addr, size, flags);
}

dword_t sys_getxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, dword_t size) {
    return sys_xattr_stub_guest_impl(path_addr, name_addr, value_addr, size, 0);
}

dword_t sys_fgetxattr_guest(fd_t UNUSED(fd), guest_addr_t name_addr,
        guest_addr_t value_addr, dword_t size) {
    return sys_xattr_stub_guest_impl(0, name_addr, value_addr, size, 0);
}

dword_t sys_listxattr_guest(guest_addr_t path_addr, guest_addr_t list_addr, dword_t size) {
    return sys_xattr_stub_guest_impl(path_addr, 0, list_addr, size, 0);
}

dword_t sys_flistxattr_guest(fd_t UNUSED(fd), guest_addr_t list_addr, dword_t size) {
    return sys_xattr_stub_guest_impl(0, 0, list_addr, size, 0);
}

dword_t sys_removexattr_guest(guest_addr_t path_addr, guest_addr_t name_addr) {
    return sys_xattr_stub_guest_impl(path_addr, name_addr, 0, 0, 0);
}

dword_t sys_fremovexattr_guest(fd_t UNUSED(fd), guest_addr_t name_addr) {
    return sys_xattr_stub_guest_impl(0, name_addr, 0, 0, 0);
}
