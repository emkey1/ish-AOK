#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/dev.h"
#include "fs/real.h"
#include "fs/mmap_cache.h"
#include "fs/tty.h"
#include "util/sync.h"
#include "emu/memory.h"

struct realfs_io_stats realfs_io_stats;

static inline void realfs_count_read(ssize_t res) {
    if (res > 0) {
        atomic_fetch_add_explicit(&realfs_io_stats.read_ops, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&realfs_io_stats.read_bytes, (uint64_t) res, memory_order_relaxed);
    }
}
static inline void realfs_count_write(ssize_t res) {
    if (res > 0) {
        atomic_fetch_add_explicit(&realfs_io_stats.write_ops, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&realfs_io_stats.write_bytes, (uint64_t) res, memory_order_relaxed);
    }
}

static bool realfs_guest_signal_pending(void) {
    lock(&current->sighand->lock, 0);
    bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
    unlock(&current->sighand->lock);
    return signal_pending;
}

// Diagnostic for realfs_wait_readable/writable's EINTR paths, added while
// root-causing the foot slave.c:551 crash (2026-07-24): every occurrence
// there was a spuriously-EINTR'd sigunwind_start() wake with the signal
// actually BLOCKED (see the fix in each caller below). strace never once
// caught this live -- ptrace's own scheduling perturbation was apparently
// enough to dodge whatever narrow window is involved -- so this is a plain
// dprintf straight to the pty's stderr (a single direct write(2), no stdio
// buffering/locking) rather than a tracer. Gated behind an env var so it
// costs nothing normally; kept as a standing tool (matches
// ISH_TRACE_REALFS_IO/ISH_TRACE_DPKG_REALFS below) for the next time a
// blocking realfs read/write needs to be checked for a bogus EINTR.
static bool realfs_trace_signal_eintr(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_SIGNAL_EINTR") != NULL ? 1 : 0;
    return enabled == 1;
}

static void realfs_log_signal_eintr(const char *where) {
    if (!realfs_trace_signal_eintr() || current == NULL)
        return;
    lock(&current->sighand->lock, 0);
    sigset_t_ pending = (current->pending | current->sighand->pending) & ~task_wake_blocked(current);
    sigset_t_ task_pending = current->pending;
    sigset_t_ group_pending = current->sighand->pending;
    sigset_t_ blocked = current->blocked;
    unlock(&current->sighand->lock);
    dprintf(2, "[signal-eintr] %s comm=%s pid=%d unblocked_pending=%#llx task_pending=%#llx group_pending=%#llx blocked=%#llx\n",
            where, current->comm, current->pid,
            (unsigned long long) pending, (unsigned long long) task_pending,
            (unsigned long long) group_pending, (unsigned long long) blocked);
}

static bool realfs_trace_comm(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_REALFS_IO") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL &&
        (strcmp(current->comm, "apt") == 0 ||
         strcmp(current->comm, "apt-get") == 0 ||
         strncmp(current->comm, "http", 4) == 0);
}

static bool realfs_dpkg_trace_task(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_DPKG_REALFS") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL &&
        (strncmp(current->comm, "dpkg", 4) == 0 ||
         strcmp(current->comm, "tar") == 0);
}

static bool realfs_dpkg_trace_path(const char *path) {
    if (current == NULL || path == NULL)
        return false;
    if (!realfs_dpkg_trace_task())
        return false;
    return strstr(path, ".dpkg-") != NULL ||
        strncmp(path, "/var/lib/dpkg/tmp.ci", strlen("/var/lib/dpkg/tmp.ci")) == 0;
}

static void realfs_trace_path_event(const char *op, const char *path, int result, int extra) {
    if (!realfs_dpkg_trace_path(path))
        return;
    fprintf(stderr,
            "ish-dpkg-realfs:%s pid=%d comm=%s path=%s fix=%s result=%d errno=%d extra=%#x\n",
            op, current->pid, current->comm, path, fix_path(path), result, errno, extra);
}

static void realfs_trace_path_stat(const char *op, struct mount *mount, const char *path) {
    if (!realfs_dpkg_trace_path(path))
        return;

    struct stat st;
    int err = fstatat(mount->root_fd, fix_path(path), &st, AT_SYMLINK_NOFOLLOW);
    if (err < 0) {
        fprintf(stderr,
                "ish-dpkg-realfs:%s-stat pid=%d comm=%s path=%s fix=%s result=%d errno=%d\n",
                op, current->pid, current->comm, path, fix_path(path), err, errno);
        return;
    }

    fprintf(stderr,
            "ish-dpkg-realfs:%s-stat pid=%d comm=%s path=%s fix=%s mode=%#o size=%lld ino=%llu nlink=%llu\n",
            op, current->pid, current->comm, path, fix_path(path),
            st.st_mode, (long long) st.st_size,
            (unsigned long long) st.st_ino,
            (unsigned long long) st.st_nlink);
}

static void realfs_trace_task_path_event(const char *op, const char *path, int result, int extra) {
    if (!realfs_dpkg_trace_task() || path == NULL)
        return;
    fprintf(stderr,
            "ish-dpkg-realfs:%s pid=%d comm=%s path=%s fix=%s result=%d errno=%d extra=%#x\n",
            op, current->pid, current->comm, path, fix_path(path), result, errno, extra);
}

static void realfs_trace_task_path_stat(const char *op, struct mount *mount, const char *path) {
    if (!realfs_dpkg_trace_task() || path == NULL)
        return;

    struct stat st;
    int err = fstatat(mount->root_fd, fix_path(path), &st, AT_SYMLINK_NOFOLLOW);
    if (err < 0) {
        fprintf(stderr,
                "ish-dpkg-realfs:%s-stat pid=%d comm=%s path=%s fix=%s result=%d errno=%d\n",
                op, current->pid, current->comm, path, fix_path(path), err, errno);
        return;
    }

    fprintf(stderr,
            "ish-dpkg-realfs:%s-stat pid=%d comm=%s path=%s fix=%s mode=%#o size=%lld ino=%llu nlink=%llu\n",
            op, current->pid, current->comm, path, fix_path(path),
            st.st_mode, (long long) st.st_size,
            (unsigned long long) st.st_ino,
            (unsigned long long) st.st_nlink);
}

static void realfs_trace_symlink_event(struct mount *mount, const char *target, const char *link, int result) {
    if (!realfs_dpkg_trace_task())
        return;
    fprintf(stderr,
            "ish-dpkg-realfs:symlink pid=%d comm=%s target=%s link=%s fix=%s result=%d errno=%d\n",
            current->pid, current->comm,
            target != NULL ? target : "<null>",
            link != NULL ? link : "<null>",
            link != NULL ? fix_path(link) : "<null>",
            result, errno);
    realfs_trace_task_path_stat("symlink-link", mount, link);
}

static void realfs_trace_io(const char *op, struct fd *fd, size_t size, ssize_t res, const void *buf) {
    if (!realfs_trace_comm() || fd == NULL || !is_adhoc_fd(fd))
        return;
    char preview[81];
    size_t preview_len = 0;
    if (res > 0 && buf != NULL) {
        size_t limit = (size_t) res < 24 ? (size_t) res : 24;
        const unsigned char *bytes = buf;
        for (size_t i = 0; i < limit && preview_len + 4 < sizeof(preview); i++) {
            unsigned char ch = bytes[i];
            if (ch >= 32 && ch < 127 && ch != '\\') {
                preview[preview_len++] = (char) ch;
            } else {
                int wrote = snprintf(preview + preview_len, sizeof(preview) - preview_len, "\\x%02x", ch);
                if (wrote < 0)
                    break;
                preview_len += (size_t) wrote;
            }
        }
    }
    preview[preview_len] = '\0';
    bool has_config = false;
    bool has_uri_request = false;
    bool has_uri_key = false;
    bool ends_blank = false;
    int blank_lines = 0;
    ptrdiff_t uri_offset = -1;
    char tail[17];
    char around_uri[97];
    tail[0] = '\0';
    around_uri[0] = '\0';
    if (res > 0 && buf != NULL) {
        const char *text = buf;
        size_t text_len = (size_t) res;
        has_config = memmem(text, text_len, "601 Configuration", strlen("601 Configuration")) != NULL;
        const char *uri_ptr = memmem(text, text_len, "600 URI Acquire", strlen("600 URI Acquire"));
        has_uri_request = uri_ptr != NULL;
        if (uri_ptr != NULL)
            uri_offset = uri_ptr - text;
        has_uri_key = memmem(text, text_len, "URI:", strlen("URI:")) != NULL;
        ends_blank = text_len >= 2 && text[text_len - 1] == '\n' && text[text_len - 2] == '\n';
        for (size_t i = 1; i < text_len; i++) {
            if (text[i - 1] == '\n' && text[i] == '\n')
                blank_lines++;
        }
        size_t tail_len = text_len < 16 ? text_len : 16;
        size_t start = text_len - tail_len;
        for (size_t i = 0; i < tail_len; i++) {
            unsigned char ch = (unsigned char) text[start + i];
            tail[i] = (ch >= 32 && ch < 127) ? (char) ch : '.';
        }
        tail[tail_len] = '\0';
        if (uri_ptr != NULL) {
            size_t begin = uri_offset > 16 ? (size_t) uri_offset - 16 : 0;
            size_t end = (size_t) uri_offset + 64;
            if (end > text_len)
                end = text_len;
            size_t pos = 0;
            for (size_t i = begin; i < end && pos + 1 < sizeof(around_uri); i++) {
                unsigned char ch = (unsigned char) text[i];
                around_uri[pos++] = (ch >= 32 && ch < 127) ? (char) ch : '.';
            }
            around_uri[pos] = '\0';
        }
    }
    fprintf(stderr, "ish-realfs-%s: pid=%d comm=%s real=%d mode=%#x req=%zu res=%zd config=%d uri_req=%d uri_key=%d uri_off=%td blanks=%d ends_blank=%d tail=%s uri_ctx=%s preview=%s\n",
            op, current != NULL ? current->pid : -1,
            current != NULL ? current->comm : "?",
            fd->real_fd, fd->stat.mode, size, res,
            has_config, has_uri_request, has_uri_key, uri_offset, blank_lines, ends_blank, tail,
            around_uri,
            preview_len != 0 ? preview : "\"\"");
}

static void realfs_trace_io_enter(const char *op, struct fd *fd, size_t size) {
    if (!realfs_trace_comm() || fd == NULL || !is_adhoc_fd(fd))
        return;
    fprintf(stderr, "ish-realfs-%s-enter: pid=%d comm=%s real=%d mode=%#x req=%zu\n",
            op, current != NULL ? current->pid : -1,
            current != NULL ? current->comm : "?",
            fd->real_fd, fd->stat.mode, size);
}

static ssize_t realfs_write_host(int real_fd, const void *buf, size_t size) {
    return write(real_fd, buf, size);
}

static void realfs_maybe_dump_apt_http_request(struct fd *fd, const void *buf, size_t size) {
    static int enabled = -1;
    static bool dumped = false;
    if (enabled < 0)
        enabled = getenv("ISH_DUMP_APT_HTTP_REQUEST") != NULL ? 1 : 0;
    if (!enabled)
        return;
    if (dumped || current == NULL || fd == NULL || buf == NULL)
        return;
    if (strcmp(current->comm, "apt-get") != 0)
        return;
    if (!is_adhoc_fd(fd) || !S_ISFIFO(fd->stat.mode))
        return;
    const char *text = buf;
    if (memmem(text, size, "600 URI Acquire", strlen("600 URI Acquire")) == NULL)
        return;
    FILE *f = fopen("/tmp/ish-apt-http-request.bin", "wb");
    if (f == NULL)
        return;
    if (fwrite(buf, 1, size, f) == size)
        dumped = true;
    fclose(f);
}

static int realfs_wait_readable(int real_fd) {
    const int poll_timeout_ms = 100;
    for (;;) {
        struct pollfd pfd = {
            .fd = real_fd,
            .events = POLLIN | POLLERR | POLLHUP,
        };
        sigset_t sigusr1, oldmask;
        sigemptyset(&sigusr1);
        sigaddset(&sigusr1, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &sigusr1, &oldmask);

        if (sigunwind_start()) {
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            // sigunwind_start() returning true only means a host SIGUSR1 poke
            // arrived, not that the guest has an actually-observable signal --
            // unlike the other two EINTR sites in this same loop (a few lines
            // below), this branch never rechecked realfs_guest_signal_pending()
            // before declaring EINTR. Confirmed via ISH_TRACE_SIGNAL_EINTR=1 on
            // m4pt (2026-07-24): every single foot slave.c:551 crash landed
            // here with a BLOCKED SIGCHLD sitting in the group's pending queue
            // (group_pending == blocked == 0x10000, so unblocked pending is
            // genuinely 0) -- the exact "spurious SIGUSR1 leaking as guest
            // EINTR" bug class already fixed once for fs/poll.c's poll_wait,
            // just never applied to this second, independent occurrence of the
            // same sigunwind_start() pattern. A blocked signal must never
            // interrupt a syscall on real Linux; retry instead of manufacturing
            // an EINTR the guest was never entitled to see.
            if (!realfs_guest_signal_pending())
                continue;
            realfs_log_signal_eintr("read/sigunwind");
            errno = EINTR;
            return errno_map();
        }

        if (realfs_guest_signal_pending()) {
            sigunwind_end();
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            realfs_log_signal_eintr("read/presleep");
            errno = EINTR;
            return errno_map();
        }

        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        int res = poll(&pfd, 1, poll_timeout_ms);
        sigunwind_end();
        if (res > 0)
            return res;
        if (res == 0) {
            if (realfs_guest_signal_pending()) {
                realfs_log_signal_eintr("read/timeout");
                errno = EINTR;
                return errno_map();
            }
            // Darwin's poll() does not report POLLHUP on a FIFO/pipe read end
            // after the last writer closes, so a reader blocked here would spin
            // on poll timeouts forever and never observe EOF. Return on the
            // timeout so realfs_read() attempts a read(): on a writer-closed,
            // empty FIFO that read returns 0 (EOF); if a writer is still present
            // it returns EAGAIN and realfs_read loops back here. This unwedges
            // GNU make's jobserver_acquire_all(), which closes the FIFO write
            // end and then blocking-reads until EOF to reclaim every token (a
            // parallel `make -jN` on a fakefs/realfs /tmp hung forever otherwise).
            return 0;
        }
        if (errno == EINTR && !realfs_guest_signal_pending())
            continue;
        if (errno == EINTR)
            realfs_log_signal_eintr("read/hostpoll");
        return errno_map();
    }
}

// Write-side counterpart to realfs_wait_readable(): realfs_write() used to
// call the host write(2) directly with no wait loop at all, unlike
// realfs_read(). A write to a full pipe/FIFO then blocks inside the raw
// host syscall with none of the sigunwind_start()/SIGUSR1-unblocking
// machinery below, so it cannot be interrupted by anything -- not the
// SIGUSR1 poke iSH uses to wake blocked tasks, not even SIGKILL, since the
// guest's signals are delivered through iSH's own translation layer, which
// never gets a chance to run. Observed: stress-ng's pipeherd stressor
// deadlocks a ring of processes this way, each stuck writing to a full pipe
// toward a peer that's itself stuck the same way, unrecoverable short of
// killing the whole iSH process.
static int realfs_wait_writable(int real_fd) {
    const int poll_timeout_ms = 100;
    for (;;) {
        struct pollfd pfd = {
            .fd = real_fd,
            .events = POLLOUT | POLLERR | POLLHUP,
        };
        sigset_t sigusr1, oldmask;
        sigemptyset(&sigusr1);
        sigaddset(&sigusr1, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &sigusr1, &oldmask);

        if (sigunwind_start()) {
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            // Same missing recheck as realfs_wait_readable's identical branch
            // (see its comment): a host SIGUSR1 poke alone doesn't mean the
            // guest has an observable signal -- a blocked/not-yet-deliverable
            // one must not interrupt the write either.
            if (!realfs_guest_signal_pending())
                continue;
            errno = EINTR;
            return errno_map();
        }

        if (realfs_guest_signal_pending()) {
            sigunwind_end();
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            errno = EINTR;
            return errno_map();
        }

        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        int res = poll(&pfd, 1, poll_timeout_ms);
        sigunwind_end();
        if (res > 0)
            return res;
        if (res == 0) {
            if (realfs_guest_signal_pending()) {
                errno = EINTR;
                return errno_map();
            }
            // Same periodic-retry rationale as realfs_wait_readable(): don't
            // trust poll() timeout/POLLOUT/POLLERR reporting alone forever
            // (Darwin's pipe/FIFO edge-case reporting is already known to be
            // unreliable on the read side). Wake up every poll_timeout_ms and
            // let realfs_write() attempt an actual non-blocking write() --
            // it gets EAGAIN and loops back here if the pipe is still
            // genuinely full, or succeeds/EPIPEs otherwise.
            return 0;
        }
        if (errno == EINTR && !realfs_guest_signal_pending())
            continue;
        return errno_map();
    }
}

static int getpath(int fd, char *buf) {
#if defined(__linux__)
    char proc_fd[32];
    snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(proc_fd, buf, MAX_PATH - 1);
    if (size >= 0)
        buf[size] = '\0';
    return size;
#elif defined(__APPLE__)
    return fcntl(fd, F_GETPATH, buf);
#endif
}

// temporarily change directory and block other threads from doing so
// useful for simulating mknodat on ios, dealing with long unix socket paths, etc
lock_t fchdir_lock;
static void lock_fchdir(int dirfd) {
    lock(&fchdir_lock, 0);
    fchdir(dirfd);
}
static void unlock_fchdir() {
    unlock(&fchdir_lock);
}

static int open_flags_real_from_fake(int flags) {
    int real_flags = 0;
    if (flags & O_RDONLY_) real_flags |= O_RDONLY;
    if (flags & O_WRONLY_) real_flags |= O_WRONLY;
    if (flags & O_RDWR_) real_flags |= O_RDWR;
    if (flags & O_CREAT_) real_flags |= O_CREAT;
    if (flags & O_EXCL_) real_flags |= O_EXCL;
    if (flags & O_TRUNC_) real_flags |= O_TRUNC;
    if (flags & O_APPEND_) real_flags |= O_APPEND;
    if (flags & O_NONBLOCK_) real_flags |= O_NONBLOCK;
    return real_flags;
}

static int open_flags_fake_from_real(int flags) {
    int fake_flags = 0;
    if (flags & O_RDONLY) fake_flags |= O_RDONLY_;
    if (flags & O_WRONLY) fake_flags |= O_WRONLY_;
    if (flags & O_RDWR) fake_flags |= O_RDWR_;
    if (flags & O_CREAT) fake_flags |= O_CREAT_;
    if (flags & O_EXCL) fake_flags |= O_EXCL_;
    if (flags & O_TRUNC) fake_flags |= O_TRUNC_;
    if (flags & O_APPEND) fake_flags |= O_APPEND_;
    if (flags & O_NONBLOCK) fake_flags |= O_NONBLOCK_;
    return fake_flags;
}

struct fd *realfs_open(struct mount *mount, const char *path, int flags, int mode) {
    int real_flags = open_flags_real_from_fake(flags);
    if ((flags & O_CREAT_) && (mount->flags & MOUNT_ISH_SHARED_))
        mode |= 0666;
    int fd_no = openat(mount->root_fd, fix_path(path), real_flags, mode);
    if (fd_no < 0) {
        realfs_trace_path_event("open", path, -1, flags);
        return ERR_PTR(errno_map());
    }
    realfs_trace_path_event("open", path, fd_no, flags);
    realfs_trace_path_stat("open", mount, path);
    struct fd *fd = fd_create(&realfs_fdops);
    fd->real_fd = fd_no;
    fd->dir = NULL;
    return fd;
}

int realfs_close(struct fd *fd) {
    if (fd->dir != NULL)
        closedir(fd->dir);
    int err = close(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

static void copy_stat(struct statbuf *fake_stat, struct stat *real_stat) {
    fake_stat->dev = dev_fake_from_real(real_stat->st_dev);
    fake_stat->inode = real_stat->st_ino;
    fake_stat->mode = real_stat->st_mode;
    fake_stat->nlink = real_stat->st_nlink;
    fake_stat->uid = real_stat->st_uid;
    fake_stat->gid = real_stat->st_gid;
    fake_stat->rdev = dev_fake_from_real(real_stat->st_rdev);
    fake_stat->size = real_stat->st_size;
    fake_stat->blksize = real_stat->st_blksize;
    fake_stat->blocks = real_stat->st_blocks;
    fake_stat->atime = real_stat->st_atime;
    fake_stat->mtime = real_stat->st_mtime;
    fake_stat->ctime = real_stat->st_ctime;
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
    fake_stat->atime_nsec = real_stat->TIMESPEC(a).tv_nsec;
    fake_stat->mtime_nsec = real_stat->TIMESPEC(m).tv_nsec;
    fake_stat->ctime_nsec = real_stat->TIMESPEC(c).tv_nsec;
#undef TIMESPEC
}

int realfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstatat(mount->root_fd, fix_path(path), &real_stat, AT_SYMLINK_NOFOLLOW) < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);
    return 0;
}

int realfs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);
    return 0;
}

ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;
    size_t read_size = bufsize;

    if (fd->flags & O_NONBLOCK_) {
        realfs_trace_io_enter("read", fd, read_size);
        ssize_t res = read(fd->real_fd, buf, read_size);
        if (res < 0)
            return errno_map();
        if (res > 0 && is_adhoc_fd(fd) && S_ISFIFO(fd->stat.mode))
            fd->realfs_fifo_had_data = true;
        realfs_trace_io("read", fd, read_size, res, buf);
        realfs_count_read(res);
        return res;
    }

    // Force the host fd non-blocking (idempotent -- if it's already set,
    // this is a harmless no-op) rather than restoring it afterward. The
    // host file status flags belong to the open file description, which
    // fork() shares across every task holding this fd (this is exactly
    // pipeherd's topology: many forked children all reading the same
    // inherited pipe end, all racing through this same struct fd). If we
    // ever restored the flags to blocking, one task's cleanup could flip
    // the fd back to blocking a moment before a sibling task -- having
    // already confirmed the fd was non-blocking and skipped re-forcing it
    // -- makes its own read() call, which then blocks in the raw host
    // syscall with no signal-interrupt protection at all: unkillable, even
    // by SIGKILL, since the guest's own signal delivery never gets to run.
    // Observed as the exact cause of a real, SIGKILL-proof stress-ng
    // pipeherd hang (every stuck thread's backtrace stopped right here).
    (void) fcntl(fd->real_fd, F_SETFL, fcntl(fd->real_fd, F_GETFL, 0) | O_NONBLOCK);

    for (;;) {
        realfs_trace_io_enter("read", fd, read_size);
        int wait_res = realfs_wait_readable(fd->real_fd);
        if (wait_res < 0)
            return wait_res;

        ssize_t res = read(fd->real_fd, buf, read_size);
        if (res >= 0) {
            if (res > 0 && is_adhoc_fd(fd) && S_ISFIFO(fd->stat.mode))
                fd->realfs_fifo_had_data = true;
            realfs_trace_io("read", fd, read_size, res, buf);
            realfs_count_read(res);
            return res;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) ||
                (errno == EINTR && !realfs_guest_signal_pending())) {
            continue;
        }
        return errno_map();
    }
}

ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;

    if (fd->flags & O_NONBLOCK_) {
        ssize_t res = realfs_write_host(fd->real_fd, buf, bufsize);
        if (res < 0)
            return errno_map();
        if (res > 0)
            realfs_maybe_dump_apt_http_request(fd, buf, (size_t) res);
        realfs_trace_io("write", fd, bufsize, res, buf);
        realfs_count_write(res);
        return res;
    }

    // See the matching comment in realfs_read(): force non-blocking
    // idempotently and never restore it, since restoring races against
    // sibling tasks sharing this same open file description (fork()).
    (void) fcntl(fd->real_fd, F_SETFL, fcntl(fd->real_fd, F_GETFL, 0) | O_NONBLOCK);

    for (;;) {
        int wait_res = realfs_wait_writable(fd->real_fd);
        if (wait_res < 0)
            return wait_res;

        ssize_t res = realfs_write_host(fd->real_fd, buf, bufsize);
        if (res >= 0) {
            if (res > 0)
                realfs_maybe_dump_apt_http_request(fd, buf, (size_t) res);
            realfs_trace_io("write", fd, bufsize, res, buf);
            realfs_count_write(res);
            return res;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) ||
                (errno == EINTR && !realfs_guest_signal_pending())) {
            continue;
        }
        return errno_map();
    }
}

ssize_t realfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    ssize_t res = pread(fd->real_fd, buf, bufsize, off);
    if (res < 0)
        return errno_map();
    realfs_count_read(res);
    return res;
}

ssize_t realfs_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    // On an O_APPEND fd Linux ignores the offset it was handed and writes at
    // end of file, while leaving the file position alone (both halves verified
    // against a real kernel; the offset half is documented under BUGS in
    // man 2 pwrite). POSIX says the opposite and Darwin follows POSIX, so the
    // host will not do this for us even though the description was opened
    // O_APPEND -- realfs_write() gets it for free only because a plain write(2)
    // goes through the host's own append path. Look the size up and write
    // there, matching what fs/tmp.c and kernel/memfd.c do for their backing.
    //
    // A host write(2) would append atomically, but it also advances the file
    // position that Linux keeps still here, so the size lookup is the closer
    // match; the lost atomicity only affects concurrent pwrites to one
    // O_APPEND description, which is an exotic combination of an already
    // exotic quirk.
    if (fd->flags & O_APPEND_) {
        struct stat real_stat;
        if (fstat(fd->real_fd, &real_stat) < 0)
            return errno_map();
        off = real_stat.st_size;
    }
    ssize_t res = pwrite(fd->real_fd, buf, bufsize, off);
    if (res < 0)
        return errno_map();
    realfs_count_write(res);
    return res;
}

// Returns 0 on success, or a negative ish errno on failure (fd->dir stays NULL).
// A host directory entry that doesn't actually open as a directory (e.g. a
// filesystem left in an inconsistent state by a racing writer) is reported
// to the guest as an error instead of taking down the whole process.
int realfs_opendir(struct fd *fd) {
    if (fd->dir == NULL) {
        int dirfd = dup(fd->real_fd);
        if (dirfd < 0)
            return errno_map();
        fd->dir = fdopendir(dirfd);
        if (fd->dir == NULL) {
            int err = errno_map();
            close(dirfd);
            return err;
        }
    }
    return 0;
}

int realfs_readdir(struct fd *fd, struct dir_entry *entry) {
    int err = realfs_opendir(fd);
    if (err < 0)
        return err;
    errno = 0;
    struct dirent *dirent = readdir(fd->dir);
    if (dirent == NULL) {
        if (errno != 0)
            return errno_map();
        else
            return 0;
    }
    entry->inode = dirent->d_ino;
    entry->type = dirent->d_type;
    strcpy(entry->name, dirent->d_name);
    return 1;
}

unsigned long realfs_telldir(struct fd *fd) {
    if (realfs_opendir(fd) < 0)
        return 0;
    return telldir(fd->dir);
}

void realfs_seekdir(struct fd *fd, unsigned long ptr) {
    if (realfs_opendir(fd) < 0)
        return;
    seekdir(fd->dir, ptr);
}

off_t realfs_lseek(struct fd *fd, off_t offset, int whence) {
    if (fd->dir != NULL && whence == LSEEK_SET) {
        realfs_seekdir(fd, offset);
        return offset;
    }

    if (whence == LSEEK_SET)
        whence = SEEK_SET;
    else if (whence == LSEEK_CUR)
        whence = SEEK_CUR;
    else if (whence == LSEEK_END)
        whence = SEEK_END;
    else
        return _EINVAL;
    off_t res = lseek(fd->real_fd, offset, whence);
    if (res < 0)
        return errno_map();
    return res;
}

int realfs_poll(struct fd *fd) {
    // Linux has no ->poll operation for a regular file or a directory, so both
    // are polled through DEFAULT_POLLMASK: always readable and always
    // writable, never POLLPRI, whatever the open access mode (measured on
    // Linux 6.12 -- an O_RDONLY file still reports POLLOUT and an O_WRONLY one
    // still reports POLLIN). Asking the host gets a different answer to a
    // different question: Darwin reports a spurious POLLPRI on a regular file,
    // and poll(2) only ever returns bits that were requested, so the
    // access-mode gating below turned an O_RDONLY file into "readable but not
    // writable" and an O_WRONLY one into "writable but not readable". Answer
    // these ourselves. (Host poll bits, like every other return below --
    // POLLIN/POLLOUT and POLL_READ/POLL_WRITE are the same values.)
    if (S_ISREG(fd->stat.mode) || S_ISDIR(fd->stat.mode))
        return POLLIN | POLLOUT;

    struct pollfd p = {.fd = fd->real_fd, .events = 0};
#if defined(__APPLE__)
    // Anonymous pipes (adhoc fds) and named FIFOs (realfs-backed, e.g. a GNU
    // make jobserver on a fakefs/realfs /tmp) are both S_IFIFO host objects
    // subject to Darwin's FIFO poll quirks below: requesting POLLPRI makes the
    // kernel report a spurious POLLIN|POLLPRI even when empty, and it reports a
    // spurious POLLHUP on a read end opened before any writer. Gating on
    // is_adhoc_fd() excluded named FIFOs, so a guest poll/select/pselect on one
    // could see bogus readiness. Cover every FIFO host object.
    bool is_fifo = S_ISFIFO(fd->stat.mode);
    if (!is_fifo)
        p.events |= POLLPRI;
#else
    p.events |= POLLPRI;
#endif
    // prevent POLLNVAL
    int flags = fcntl(fd->real_fd, F_GETFL, 0);
    if ((flags & O_ACCMODE) != O_WRONLY)
        p.events |= POLLIN;
    if ((flags & O_ACCMODE) != O_RDONLY)
        p.events |= POLLOUT;
    if (poll(&p, 1, 0) <= 0)
        return 0;

#if defined(__APPLE__)
    // this is the "WTF is apple smoking" section

    // Darwin reports a spurious POLLHUP on a pipe/FIFO read end that was opened
    // before any writer connected; suppress that. But ONLY when there is no
    // actual data: if POLLIN is set the readiness is real (e.g. a writer wrote
    // then closed, leaving unread bytes), and scrubbing it hid readable data
    // from poll/select -- a guest would see "not ready" with bytes waiting.
    if (is_fifo && !fd->realfs_fifo_had_data &&
            (p.revents & POLLHUP) && !(p.revents & POLLIN))
        p.revents &= ~(POLLIN | POLLHUP | POLLOUT);

    // https://github.com/apple/darwin-xnu/blob/a449c6a3b8014d9406c2ddbdc81795da24aa7443/bsd/kern/sys_generic.c#L1856
    if (p.revents & POLLHUP)
        p.revents |= POLLOUT;
    // Darwin can report spurious POLLPRI on pipes/FIFOs; avoid requesting it
    // there and scrub any residual bit if the host still returns one.
    if (is_fifo)
        p.revents &= ~POLLPRI;

    if (p.revents & POLLNVAL) {
        // printk("WARNING: pollnval %d flags %d events %d revents %d\n", fd->real_fd, flags, p.events, p.revents);
        // Seriously, fuck Darwin. I just want to poll on POLLIN|POLLOUT|POLLPRI.
        // But if there's almost any kind of error, you just get POLLNVAL back,
        // and no information about the bits that are in fact set. So ask for each
        // separately and ignore a POLLNVAL.
        // This is no longer atomic but I don't really know what to do about that.
        int events = 0;
        bool nval[3] = {false, false, false};
        static const int pollbits[] = {POLLIN, POLLOUT, POLLPRI};
        for (unsigned i = 0; i < sizeof(pollbits)/sizeof(pollbits[0]); i++) {
            p.events = pollbits[i];
            if (poll(&p, 1, 0) > 0) {
                if (p.revents & POLLNVAL)
                    nval[i] = true;
                else
                    events |= p.revents;
            }
        }
        // Darwin has no poll implementation at all for some host objects --
        // character devices other than ttys, so /dev/null, /dev/zero and
        // /dev/random -- and every single-bit probe above comes back POLLNVAL
        // rather than a readiness answer. Linux polls those through
        // DEFAULT_POLLMASK, the same always-ready answer the regular-file case
        // above takes (measured on Linux 6.12: /dev/null, /dev/zero and
        // /dev/full are all POLLIN|POLLOUT). Reporting "not ready" here made a
        // guest poll on such an fd block until its timeout, and, together with
        // the registration error real_poll_check_receipts used to pass on,
        // made musl's AT_SECURE startup -- which polls fds 0, 1 and 2 --
        // a_crash() in any setuid binary whose stdio was a host device node.
        //
        // Only when *both* the read and write probes were refused: an object
        // Darwin can poll answers "not ready" (0), not POLLNVAL, and must keep
        // that answer.
        if (nval[0] && nval[1])
            events |= POLLIN | POLLOUT;
        assert(!(events & POLLNVAL));
        return events;
    }
#endif

    assert(!(p.revents & POLLNVAL));
    return p.revents;
}

// Create an unlinked host temp file and return its fd (CLOEXEC), or a negative
// errno. This is the mmapable backing store for guest files that don't live on
// a real host filesystem: tmpfs regular files (fs/tmp.c) and memfds
// (kernel/memfd.c). The fd is the only reference; the name is gone immediately.
int host_unlinked_tmpfd(void) {
    // TMPDIR is always set on iOS (the app sandbox tmp dir); fall back to /tmp
    // for the command-line build.
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s/ish-tmpfile.XXXXXX", tmpdir) >= (int) sizeof(path))
        return _ENAMETOOLONG;
    int host_fd = mkstemp(path);
    if (host_fd < 0)
        return errno_map();
    unlink(path); // the caller owns the fd; the name never needs to exist again
    fcntl(host_fd, F_SETFD, FD_CLOEXEC);
    return host_fd;
}

// Core of realfs_mmap, taking a bare host fd: map any mmapable host file into
// the guest page tables. Shared with tmpfs, whose regular files are backed by
// unlinked host temp files (a struct tmp_inode host_fd) rather than a struct fd
// with a real_fd.
int host_fd_mmap(int host_fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    int mmap_flags = 0;
    if (flags & MMAP_PRIVATE) mmap_flags |= MAP_PRIVATE;
    if (flags & MMAP_SHARED) mmap_flags |= MAP_SHARED;
    int mmap_prot = PROT_READ;
    if (prot & P_WRITE) mmap_prot |= PROT_WRITE;
    // Whenever a guest page's protection can't later be mirrored into a real
    // host mprotect() -- host/guest page-size mismatch (16K-page iOS
    // devices), or mirroring disabled outright (unconditionally on __APPLE__,
    // see mem_host_page_mirroring_enabled) -- a host mapping created less
    // permissive than PROT_WRITE can never be promoted afterward: the guest
    // page tables will report the page writable once mprotect()/COW-break
    // sets P_WRITE in software, but the underlying host mapping stays
    // read-only forever and the actual store faults on the host. So map
    // writable up front and let the guest page tables enforce access instead.
    //
    // For MAP_PRIVATE this is always safe: POSIX permits PROT_WRITE with
    // MAP_PRIVATE regardless of the fd's open mode (writes never reach the
    // file). MAP_SHARED has no such guarantee -- the fd must actually be
    // open for writing, or mmap() fails EACCES -- so try it and fall back to
    // the originally requested protection if the host rejects it.
    bool needs_write_headroom = !mem_host_page_mirroring_available() && !(mmap_prot & PROT_WRITE);
    int try_prot = mmap_prot;
    if (needs_write_headroom)
        try_prot |= PROT_WRITE;

    off_t real_offset = (offset / real_page_size) * real_page_size;
    off_t correction = offset - real_offset;
    size_t map_len = (pages * PAGE_SIZE) + correction;
    char *memory = mmap(NULL, map_len, try_prot, mmap_flags, host_fd, real_offset);
    if (memory == MAP_FAILED && try_prot != mmap_prot)
        memory = mmap(NULL, map_len, mmap_prot, mmap_flags, host_fd, real_offset);
    int err = pt_map(mem, start, pages, memory, correction, prot);
    // Never-writable file-backed mappings can't be COW-broken or otherwise
    // mutated by the guest (write faults check P_WRITE before touching
    // anything), so it's always safe to register them: the underlying host
    // mapping this struct data wraps will stay untouched for its whole
    // life. This is purely an accounting aid for /proc/<pid>/smaps -- see
    // fs/mmap_cache.h. Registering MAP_SHARED alongside MAP_PRIVATE here is
    // fine for the same reason: no P_WRITE means neither can ever be
    // dirtied by the guest.
    if (err == 0 && !(prot & P_WRITE)) {
        struct stat real_stat;
        if (fstat(host_fd, &real_stat) == 0) {
            struct pt_entry *pt = mem_pt(mem, start);
            if (pt != NULL && pt->data != NULL) {
                pt->data->cache_entry = mmap_cache_register(
                        real_stat.st_dev, real_stat.st_ino, real_offset, pt->data->size);
            }
        }
    }
    return err;
}

int realfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    enum { AMD64_REALFS_MMAP_TRACE_BUDGET = 32 };
    static unsigned amd64_realfs_mmap_trace_count;
    int err = host_fd_mmap(fd->real_fd, mem, start, pages, offset, prot, flags);
    if (err < 0 && current != NULL && current->abi == GUEST_ABI_AMD64 &&
            amd64_realfs_mmap_trace_count < AMD64_REALFS_MMAP_TRACE_BUDGET) {
        amd64_realfs_mmap_trace_count++;
        char path[MAX_PATH];
        int path_err = generic_getpath(fd, path);
        printk("amd64 realfs mmap fail: pid=%d comm=%s real_fd=%d path=%s pages=%#x guest_off=%#llx prot=%#x flags=%#x err=%d errno=%d\n",
               current->pid, current->comm, fd->real_fd,
               path_err == 0 ? path : "<path err>",
               (unsigned) pages,
               (unsigned long long) offset,
               prot, flags, err, errno);
    }
    return err;
}

ssize_t realfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    ssize_t size = readlinkat(mount->root_fd, fix_path(path), buf, bufsize);
    if (size < 0)
        return errno_map();
    return size;
}

int realfs_getpath(struct fd *fd, char *buf) {
    int err = getpath(fd->real_fd, buf);
    if (err < 0)
        return err;
    if (strcmp(fd->mount->source, "/") != 0 || strcmp(buf, "/") == 0) {
        size_t source_len = strlen(fd->mount->source);
        memmove(buf, buf + source_len, MAX_PATH - source_len);
    }
    return 0;
}

int realfs_link(struct mount *mount, const char *src, const char *dst) {
    int res = linkat(mount->root_fd, fix_path(src), mount->root_fd, fix_path(dst), 0);
    if (res < 0) {
        realfs_trace_task_path_event("link-src", src, res, 0);
        realfs_trace_task_path_event("link-dst", dst, res, 0);
        realfs_trace_task_path_stat("link-src", mount, src);
        realfs_trace_task_path_stat("link-dst", mount, dst);
        return errno_map();
    }
    realfs_trace_task_path_event("link-src", src, res, 0);
    realfs_trace_task_path_event("link-dst", dst, res, 0);
    realfs_trace_task_path_stat("link-src", mount, src);
    realfs_trace_task_path_stat("link-dst", mount, dst);
    return res;
}

int realfs_unlink(struct mount *mount, const char *path) {
    int res = unlinkat(mount->root_fd, fix_path(path), 0);
    if (res < 0) {
        realfs_trace_path_event("unlink", path, res, 0);
        realfs_trace_path_stat("unlink", mount, path);
        return errno_map();
    }
    realfs_trace_path_event("unlink", path, res, 0);
    realfs_trace_path_stat("unlink", mount, path);
    return res;
}

int realfs_rmdir(struct mount *mount, const char *path) {
    int err = unlinkat(mount->root_fd, fix_path(path), AT_REMOVEDIR);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_rename(struct mount *mount, const char *src, const char *dst) {
    int err = renameat(mount->root_fd, fix_path(src), mount->root_fd, fix_path(dst));
    if (err < 0) {
        realfs_trace_path_event("rename-src", src, err, 0);
        realfs_trace_path_event("rename-dst", dst, err, 0);
        return errno_map();
    }
    realfs_trace_path_event("rename-src", src, err, 0);
    realfs_trace_path_event("rename-dst", dst, err, 0);
    realfs_trace_path_stat("rename-src", mount, src);
    realfs_trace_path_stat("rename-dst", mount, dst);
    return err;
}

int realfs_symlink(struct mount *mount, const char *target, const char *link) {
    int err = symlinkat(target, mount->root_fd, fix_path(link));
    if (err < 0) {
        realfs_trace_symlink_event(mount, target, link, err);
        return errno_map();
    }
    realfs_trace_symlink_event(mount, target, link, err);
    return err;
}

int realfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ UNUSED(dev)) {
    int err;
    mode_t_ perm = mode & ~S_IFMT;
    if (mount->flags & MOUNT_ISH_SHARED_)
        perm |= 0666;
    if (S_ISFIFO(mode)) {
        lock_fchdir(mount->root_fd);
        err = mkfifo(fix_path(path), perm);
        unlock_fchdir();
    } else if (S_ISREG(mode)) {
        err = openat(mount->root_fd, fix_path(path), O_CREAT|O_EXCL|O_RDONLY, perm);
        if (err >= 0)
            err = close(err);
    } else {
        return _EPERM;
    }
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_truncate(struct mount *mount, const char *path, off_t_ size) {
    int fd = openat(mount->root_fd, fix_path(path), O_RDWR);
    if (fd < 0)
        return errno_map();
    int err = 0;
    if (ftruncate(fd, size) < 0)
        err = errno_map();
    close(fd);
    return err;
}

int realfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    path = fix_path(path);
    int root = mount->root_fd;
    int err;
    switch (attr.type) {
        case attr_uid:
            err = fchownat(root, path, attr.uid, -1, 0);
            break;
        case attr_gid:
            err = fchownat(root, path, attr.gid, -1, 0);
            break;
        case attr_mode:
            err = fchmodat(root, path, attr.mode, 0);
            break;
        case attr_size:
            return realfs_truncate(mount, path, attr.size);
        default:
            return _EINVAL;
    }
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_fsetattr(struct fd *fd, struct attr attr) {
    int real_fd = fd->real_fd;
    int err;
    switch (attr.type) {
        case attr_uid:
            err = fchown(real_fd, attr.uid, -1);
            break;
        case attr_gid:
            err = fchown(real_fd, attr.gid, -1);
            break;
        case attr_mode:
            err = fchmod(real_fd, attr.mode);
            break;
        case attr_size:
            err = ftruncate(real_fd, attr.size);
            break;
        default: return _EINVAL;
    }
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    struct timespec times[2] = {atime, mtime};
    int flags = follow_links ? 0 : AT_SYMLINK_NOFOLLOW;
    realfs_trace_path_stat("utime-before", mount, path);
    int err = utimensat(mount->root_fd, fix_path(path), times, flags);
    if (err < 0) {
        realfs_trace_path_event("utime", path, err, 0);
        realfs_trace_path_stat("utime-after", mount, path);
        return errno_map();
    }
    realfs_trace_path_event("utime", path, err, 0);
    realfs_trace_path_stat("utime-after", mount, path);
    return 0;
}

int realfs_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    struct timespec times[2] = {atime, mtime};
    if (futimens(fd->real_fd, times) < 0)
        return errno_map();
    return 0;
}

int realfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    if (mount->flags & MOUNT_ISH_SHARED_)
        mode |= 0777;
    int err = mkdirat(mount->root_fd, fix_path(path), mode);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_flock(struct fd *fd, int operation) {
    int real_op = 0;
    if (operation & LOCK_SH_) real_op |= LOCK_SH;
    if (operation & LOCK_EX_) real_op |= LOCK_EX;
    if (operation & LOCK_UN_) real_op |= LOCK_UN;
    if (operation & LOCK_NB_) real_op |= LOCK_NB;
    return flock(fd->real_fd, real_op);
}

int realfs_statfs(struct mount *mount, struct statfsbuf *stat) {
    struct statvfs vfs = {};
    if (fstatvfs(mount->root_fd, &vfs) < 0)
        return errno_map();
    // f_blocks/f_bfree/f_bavail are counts in units of the fundamental block
    // size (f_frsize). On Linux, statfs f_bsize equals that block size, so
    // callers that size free space as count*f_bsize (apt does) get correct
    // bytes. Darwin's statvfs instead puts the large "optimal I/O size" in
    // f_bsize (e.g. 1 MiB against a 4 KiB f_frsize), which inflates those
    // callers by f_bsize/f_frsize -- 256x, e.g. 46 GB free shown as 11.9 TB.
    // Mirror Linux: present f_bsize == f_frsize == the fundamental block size.
    unsigned long block_size = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
    stat->bsize = block_size;
    stat->blocks = vfs.f_blocks;
    stat->bfree = vfs.f_bfree;
    stat->bavail = vfs.f_bavail;
    stat->files = vfs.f_files;
    stat->ffree = vfs.f_ffree;
    stat->namelen = vfs.f_namemax;
    stat->frsize = block_size;
    return 0;
}

int realfs_mount(struct mount *mount) {
    char *source_realpath = realpath(mount->source, NULL);
    if (source_realpath == NULL)
        return errno_map();
    free((void *) mount->source);
    mount->source = source_realpath;

    mount->root_fd = open(mount->source, O_DIRECTORY);
    if (mount->root_fd < 0)
        return errno_map();
    return 0;
}

int realfs_fsync(struct fd *fd) {
    int err = fsync(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_getflags(struct fd *fd) {
    int flags = fcntl(fd->real_fd, F_GETFL);
    if (flags < 0)
        return errno_map();
    return open_flags_fake_from_real(flags);
}

int realfs_setflags(struct fd *fd, dword_t flags) {
    int ret = fcntl(fd->real_fd, F_SETFL, open_flags_real_from_fake(flags));
    if (ret < 0)
        return errno_map();
    fd->flags = (fd->flags & ~(O_APPEND_ | O_NONBLOCK_)) | (flags & (O_APPEND_ | O_NONBLOCK_));
    return 0;
}

ssize_t realfs_ioctl_size(int cmd) {
    if (cmd == FIONREAD_)
        return sizeof(dword_t);
    return -1;
}

int realfs_ioctl(struct fd *fd, int cmd, void *arg) {
    int err;
    size_t nread;
    switch (cmd) {
        case FIONREAD_:
            err = ioctl(fd->real_fd, FIONREAD, &nread);
            if (err < 0)
                return errno_map();
            *(dword_t *) arg = nread;
            return 0;
    }
    return _ENOTTY;
}

const struct fs_ops realfs = {
    .name = "real", .magic = 0x7265616c,
    .mount = realfs_mount,
    .statfs = realfs_statfs,

    .open = realfs_open,
    .readlink = realfs_readlink,
    .link = realfs_link,
    .unlink = realfs_unlink,
    .rmdir = realfs_rmdir,
    .rename = realfs_rename,
    .symlink = realfs_symlink,
    .mknod = realfs_mknod,

    .close = realfs_close,
    .stat = realfs_stat,
    .fstat = realfs_fstat,
    .setattr = realfs_setattr,
    .fsetattr = realfs_fsetattr,
    .utime = realfs_utime,
    .futime = realfs_futime,
    .getpath = realfs_getpath,
    .flock = realfs_flock,

    .mkdir = realfs_mkdir,
};

const struct fd_ops realfs_fdops = {
    .read = realfs_read,
    .write = realfs_write,
    .pread = realfs_pread,
    .pwrite = realfs_pwrite,
    .readdir = realfs_readdir,
    .telldir = realfs_telldir,
    .seekdir = realfs_seekdir,
    .lseek = realfs_lseek,
    .mmap = realfs_mmap,
    .poll = realfs_poll,
    .ioctl_size = realfs_ioctl_size,
    .ioctl = realfs_ioctl,
    .fsync = realfs_fsync,
    .close = realfs_close,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
};
