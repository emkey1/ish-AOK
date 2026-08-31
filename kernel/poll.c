#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if defined(__APPLE__)
#include <netinet/tcp.h>
#endif
#include "debug.h"
#include "kernel/fs.h"
#include "kernel/time.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "kernel/calls.h"

static int user_read_or_zero(guest_addr_t addr, void *data, size_t size) {
    if (addr == 0)
        memset(data, 0, size);
    else if (user_read(addr, data, size))
        return _EFAULT;
    return 0;
}

static bool poll_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "apt") == 0 ||
        strcmp(comm, "apt-get") == 0 ||
        strncmp(comm, "http", 4) == 0 ||
        strcmp(comm, "wget") == 0 ||
        strcmp(comm, "curl") == 0 ||
        strcmp(comm, "ping") == 0 ||
        strcmp(comm, "cat") == 0 ||
        strcmp(comm, "grep") == 0 ||
        strcmp(comm, "which") == 0 ||
        strcmp(comm, "install") == 0 ||
        strncmp(comm, "deboots", 7) == 0 ||
        strncmp(comm, "debootstrap", 11) == 0 ||
        strncmp(comm, "update-ca-certi", 15) == 0;
}

static bool poll_trace_short_timeout(const struct timespec *timeout_ts_ptr, int timeout_trace) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_POLL_WAIT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (timeout_ts_ptr != NULL) {
        if (timeout_ts_ptr->tv_sec < 0 || timeout_ts_ptr->tv_sec > 2)
            return false;
        return true;
    }
    if (timeout_trace < 0)
        return false;
    return timeout_trace <= 2000;
}

static void poll_trace_short_wait_fd(struct fd *fd, int requested, int ready, int revents, const char *phase) {
    if (current == NULL || fd == NULL)
        return;
    if ((requested & (POLL_READ | POLL_PRI | POLL_HUP | POLL_ERR | POLL_WRITE)) == 0)
        return;
    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        printk("INFO: wait %s pid=%d comm=%s tty=%d:%d requested=%#x ready=%#x revents=%#x\n",
               phase, current->pid, current->comm,
               tty != NULL && tty->driver != NULL ? tty->driver->major : -1,
               tty != NULL ? tty->num : -1,
               requested, ready, revents);
        return;
    }

    printk("INFO: wait %s pid=%d comm=%s real=%d requested=%#x ready=%#x revents=%#x file=%p\n",
           phase, current->pid, current->comm, fd->real_fd,
           requested, ready, revents, (void *) fd);
}

static bool poll_trace_net_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_POLL_NET") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return poll_trace_comm(current->comm);
}

static void poll_trace_net_fd(struct fd *fd, int requested, int ready, int revents, const char *phase) {
    if (fd == NULL || !poll_trace_net_enabled())
        return;

    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        printk("INFO: net poll %s pid=%d comm=%s tty=%d:%d requested=%#x ready=%#x revents=%#x\n",
               phase, current->pid, current->comm,
               tty != NULL ? tty->driver->major : -1,
               tty != NULL ? tty->num : -1,
               requested, ready, revents);
        return;
    }

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    printk("INFO: net poll %s pid=%d comm=%s real=%d requested=%#x ready=%#x revents=%#x path=%s\n",
           phase, current->pid, current->comm, fd->real_fd, requested, ready, revents, path);
}

#define SELECT_READ (POLL_READ | POLL_HUP | POLL_ERR)
#define SELECT_WRITE (POLL_WRITE | POLL_ERR)
#define SELECT_EX (POLL_PRI)
struct select_context {
    char *readfds;
    char *writefds;
    char *exceptfds;
};
static int select_event_callback(void *context, int types, union poll_fd_info info) {
    struct select_context *c = context;
    // Linux's core_sys_select bumps the return count once per descriptor set
    // the fd is reported in, so an fd that is both readable and writable
    // contributes 2. Count the sets rather than the fd.
    int count = 0;
    if (types & SELECT_READ) {
        bit_set(info.fd, c->readfds);
        count++;
    }
    if (types & SELECT_WRITE) {
        bit_set(info.fd, c->writefds);
        count++;
    }
    if (types & SELECT_EX) {
        bit_set(info.fd, c->exceptfds);
        count++;
    }
    return count;
}

static void select_trace_net_fd(struct fd *fd, int requested, int ready, const char *phase) {
    if (fd == NULL || !poll_trace_net_enabled())
        return;

    if (fd->ops == &tty_dev.fd) {
        struct tty *tty = fd->tty;
        fprintf(stderr, "ish-select: %s pid=%d comm=%s tty=%d:%d requested=%#x ready=%#x\n",
               phase, current->pid, current->comm,
               tty != NULL ? tty->driver->major : -1,
               tty != NULL ? tty->num : -1,
               requested, ready);
        return;
    }

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(fd, path);
    int recv_q = -1;
    int so_error = -1;
#if defined(__APPLE__)
    struct tcp_connection_info tcp_info = {};
    socklen_t tcp_info_len = sizeof(tcp_info);
    bool have_tcp_info = false;
#endif
    if (fd->real_fd >= 0) {
        (void) ioctl(fd->real_fd, FIONREAD, &recv_q);
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt(fd->real_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0)
            so_error = -1;
#if defined(__APPLE__)
        have_tcp_info = getsockopt(fd->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO,
                                   &tcp_info, &tcp_info_len) == 0;
#endif
    }
    fprintf(stderr, "ish-select: %s pid=%d comm=%s real=%d requested=%#x ready=%#x recv_q=%d so_error=%d path=%s\n",
           phase, current->pid, current->comm, fd->real_fd, requested, ready, recv_q, so_error, path);
#if defined(__APPLE__)
    if (have_tcp_info) {
        fprintf(stderr, "ish-select-tcp: %s pid=%d comm=%s real=%d state=%u options=%#x flags=%#x snd_sbbytes=%u snd_cwnd=%u snd_wnd=%u rcv_wnd=%u rtt=%u srtt=%u txbytes=%llu rxbytes=%llu retrans=%llu\n",
               phase, current->pid, current->comm, fd->real_fd,
               tcp_info.tcpi_state, tcp_info.tcpi_options, tcp_info.tcpi_flags,
               tcp_info.tcpi_snd_sbbytes, tcp_info.tcpi_snd_cwnd,
               tcp_info.tcpi_snd_wnd, tcp_info.tcpi_rcv_wnd,
               tcp_info.tcpi_rttcur, tcp_info.tcpi_srtt,
               (unsigned long long) tcp_info.tcpi_txbytes,
               (unsigned long long) tcp_info.tcpi_rxbytes,
               (unsigned long long) tcp_info.tcpi_txretransmitbytes);
    }
#endif
}

static bool select_timeout_valid(struct timespec timeout_ts) {
    return timeout_ts.tv_sec >= 0 && timeout_ts.tv_nsec >= 0 && timeout_ts.tv_nsec < 1000000000;
}

static int read_select_timeout(enum guest_abi abi, guest_addr_t timeout_addr, struct timespec *timeout_ts) {
    struct timeval timeout_timeval;
    if (read_guest_timeval_abi(abi, timeout_addr, &timeout_timeval))
        return _EFAULT;
    timeout_ts->tv_sec = timeout_timeval.tv_sec;
    timeout_ts->tv_nsec = timeout_timeval.tv_usec * 1000;
    *timeout_ts = timespec_normalize(*timeout_ts);
    return 0;
}

static int read_pselect_timeout(enum guest_abi abi, guest_addr_t timeout_addr, struct timespec *timeout_ts) {
    if (read_guest_timespec_abi(abi, timeout_addr, timeout_ts))
        return _EFAULT;
    if (!select_timeout_valid(*timeout_ts))
        return _EINVAL;
    return 0;
}

static int read_ppoll_timeout(enum guest_abi abi, guest_addr_t timeout_addr, struct timespec *timeout_ts, int *timeout_ms) {
    if (read_guest_timespec_abi(abi, timeout_addr, timeout_ts))
        return _EFAULT;
    if (!select_timeout_valid(*timeout_ts))
        return _EINVAL;
    int64_t timeout_ms64 = timeout_ts->tv_sec * 1000 + timeout_ts->tv_nsec / 1000000;
    *timeout_ms = timeout_ms64 > INT_MAX ? INT_MAX : (int) timeout_ms64;
    return 0;
}

static dword_t sys_select_common(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr,
        guest_addr_t exceptfds_addr, const struct timespec *timeout_ts_ptr) {
    // nfds is entirely the guest's, and everything below is sized from it on
    // the STACK -- three bitmap VLAs and an array of fd pointers. A negative
    // nfds made BITS_SIZE compute an enormous size and the memset walked off
    // the stack, taking the whole emulator down with SIGBUS; a merely huge one
    // did the same more slowly. Linux checks both, differently: negative is
    // EINVAL, and anything past the fd table is clamped rather than refused
    // (core_sys_select), so a caller passing FD_SETSIZE on a small table still
    // works.
    if (nfds < 0)
        return _EINVAL;
    lock(&current->files->lock, 0);
    fd_t max_fds = (fd_t) current->files->size;
    unlock(&current->files->lock);
    if (nfds > max_fds)
        nfds = max_fds;

    size_t fdset_size = BITS_SIZE(nfds);
    char readfds[fdset_size];
    if (user_read_or_zero(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    char writefds[fdset_size];
    if (user_read_or_zero(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    char exceptfds[fdset_size];
    if (user_read_or_zero(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;
    struct timespec timeout_ts = {};
    if (timeout_ts_ptr != NULL)
        timeout_ts = *timeout_ts_ptr;

    STRACE("select(%d, %#llx, %#llx, %#llx, %s{%lds %ldns}) ",
            nfds, (unsigned long long) readfds_addr, (unsigned long long) writefds_addr,
            (unsigned long long) exceptfds_addr,
            timeout_ts_ptr == NULL ? "NULL " : "", timeout_ts.tv_sec, timeout_ts.tv_nsec);

    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);
    struct fd *files[nfds];
    memset(files, 0, sizeof(files));
    int add_err = 0;

    for (fd_t i = 0; i < nfds; i++) {
        int events = 0;
        if (bit_test(i, readfds))
            events |= SELECT_READ;
        if (bit_test(i, writefds))
            events |= SELECT_WRITE;
        if (bit_test(i, exceptfds))
            events |= SELECT_EX;
        if (events != 0) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
            struct fd *fd = f_get_retain(i);
            if (fd == NULL) {
                poll_destroy(poll);
                for (fd_t j = 0; j < i; j++) {
                    if (files[j] != NULL)
                        fd_close(files[j]);
                }
                return _EBADF;
            }
            files[i] = fd;
            add_err = poll_add_fd(poll, fd, -1, events, (union poll_fd_info) i);
            if (add_err < 0)
                goto out;
        }
    }
    STRACE("...\n");

    if (poll_trace_net_enabled()) {
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int requested = 0;
            if (bit_test(i, readfds))
                requested |= SELECT_READ;
            if (bit_test(i, writefds))
                requested |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                requested |= SELECT_EX;
            if (requested == 0)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            select_trace_net_fd(files[i], requested, ready, "enter");
        }
    }
    bool trace_short_select = poll_trace_short_timeout(timeout_ts_ptr, -1);
    if (trace_short_select) {
        printk("INFO: wait select enter pid=%d comm=%s nfds=%d timeout=%lds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               nfds,
               timeout_ts_ptr != NULL ? timeout_ts.tv_sec : -1L,
               timeout_ts_ptr != NULL ? timeout_ts.tv_nsec : -1L);
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int requested = 0;
            if (bit_test(i, readfds))
                requested |= SELECT_READ;
            if (bit_test(i, writefds))
                requested |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                requested |= SELECT_EX;
            if (requested == 0)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_short_wait_fd(files[i], requested, ready, 0, "select-enter");
        }
    }

    memset(readfds, 0, fdset_size);
    memset(writefds, 0, fdset_size);
    memset(exceptfds, 0, fdset_size);
    struct select_context context = {readfds, writefds, exceptfds};
    int err = 0;
    TASK_MAY_BLOCK {
        err = poll_wait(poll, select_event_callback, &context, timeout_ts_ptr == NULL ? NULL : &timeout_ts);
    }
out:
    STRACE("%d end select ", current->pid);
    for (fd_t i = 0; i < nfds; i++) {
        if (bit_test(i, readfds) || bit_test(i, writefds) || bit_test(i, exceptfds)) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
        }
    }
    poll_destroy(poll);
    for (fd_t i = 0; i < nfds; i++) {
        if (files[i] != NULL)
            fd_close(files[i]);
    }
    if (add_err < 0)
        return add_err;
    if (err < 0)
        return err;

    if (poll_trace_net_enabled()) {
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int revents = 0;
            if (bit_test(i, readfds))
                revents |= SELECT_READ;
            if (bit_test(i, writefds))
                revents |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                revents |= SELECT_EX;
            if (revents == 0)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            select_trace_net_fd(files[i], revents, ready, "exit");
        }
    }
    if (trace_short_select) {
        printk("INFO: wait select exit pid=%d comm=%s err=%d\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               err);
        for (fd_t i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            int revents = 0;
            if (bit_test(i, readfds))
                revents |= SELECT_READ;
            if (bit_test(i, writefds))
                revents |= SELECT_WRITE;
            if (bit_test(i, exceptfds))
                revents |= SELECT_EX;
            poll_trace_short_wait_fd(files[i], revents, ready, revents, "select-exit");
        }
    }

    if (readfds_addr && user_write(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    if (writefds_addr && user_write(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    if (exceptfds_addr && user_write(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;
    return err;
}

dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        int err = read_select_timeout(GUEST_ABI_I386, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_select_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr,
        guest_addr_t exceptfds_addr, guest_addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        int err = read_select_timeout(GUEST_ABI_I386, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_select_amd64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        int err = read_select_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_select_amd64_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr,
        guest_addr_t exceptfds_addr, guest_addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        int err = read_select_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

struct poll_context {
    struct pollfd_ *polls;
    struct fd **files;
    int nfds;
};

// Entry used to sort pollfd indices by the underlying `struct fd *` identity,
// so pollfd_ entries for the same fd (a program may list one fd more than
// once) end up adjacent and can be merged in a single linear pass.
struct poll_merge_entry {
    struct fd *file;
    word_t events;
};

static int poll_merge_entry_cmp(const void *a, const void *b) {
    struct fd *fa = ((const struct poll_merge_entry *) a)->file;
    struct fd *fb = ((const struct poll_merge_entry *) b)->file;
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

#define POLL_ALWAYS_LISTENING (POLL_ERR|POLL_HUP|POLL_NVAL)
static int poll_event_callback(void *context, int types, union poll_fd_info info) {
    struct poll_context *c = context;
    struct pollfd_ *polls = c->polls;
    int nfds = c->nfds;
    // Linux's do_sys_poll counts every pollfd entry that ends up with a
    // non-zero revents, so the same fd listed several times counts once per
    // entry. An entry whose ready types are all masked out by its own events
    // is not ready and must not be counted.
    int res = 0;
    for (int i = 0; i < nfds; i++) {
        if (c->files[i] == info.ptr) {
            polls[i].revents = types & (polls[i].events | POLL_ALWAYS_LISTENING);
            if (polls[i].revents != 0)
                res++;
        }
    }
    return res;
}
dword_t sys_poll_common(guest_addr_t fds, dword_t nfds, const struct timespec *timeout_ts_ptr, int_t timeout_trace) {
    STRACE("poll(%#llx, %d, %d)", (unsigned long long) fds, nfds, timeout_trace);
    // Same stack-VLA exposure as select above, and Linux bounds it the other
    // way: do_sys_poll refuses anything over RLIMIT_NOFILE outright rather
    // than clamping, because a pollfd array that large is a caller bug.
    if (nfds > (dword_t) rlimit(RLIMIT_NOFILE_))
        return _EINVAL;
    struct pollfd_ polls[nfds];
    if (fds != 0 || nfds != 0)
        if (user_read(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);

    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].events);
    STRACE("...\n");

    struct fd *files[nfds];
    int add_err = 0;
    int nval_count = 0;
    for (unsigned i = 0; i < nfds; i++) {
        files[i] = f_get_retain(polls[i].fd);
        // clear revents, which is reused to mark whether a pollfd has been added or not
        polls[i].revents = 0;
    }
    // Convert polls array into poll_add_fd calls, merging duplicate fds'
    // event bits together. Sort by `struct fd *` identity so duplicates end
    // up adjacent -- O(n log n) instead of the previous O(n^2) all-pairs scan.
    struct poll_merge_entry merge[nfds];
    unsigned nmerge = 0;
    for (unsigned i = 0; i < nfds; i++) {
        if (polls[i].fd < 0)
            continue;
        merge[nmerge].file = files[i];
        merge[nmerge].events = polls[i].events;
        nmerge++;
    }
    qsort(merge, nmerge, sizeof(merge[0]), poll_merge_entry_cmp);
    for (unsigned i = 0; i < nmerge; ) {
        struct fd *file = merge[i].file;
        word_t events = merge[i].events;
        unsigned j = i + 1;
        while (j < nmerge && merge[j].file == file) {
            events |= merge[j].events;
            j++;
        }
        i = j;
        if (file == NULL)
            continue;

        add_err = poll_add_fd(poll, file, -1, events | POLL_ALWAYS_LISTENING, (union poll_fd_info) (void *) file);
        if (add_err < 0)
            goto out;
    }

    for (unsigned i = 0; i < nfds; i++) {
        polls[i].revents = 0;
        // A negative fd is ignored entirely (revents stays 0, not counted) --
        // Linux skips it. Only a non-negative but invalid (e.g. closed) fd is
        // POLLNVAL, and that fd counts toward the return value.
        if (polls[i].fd < 0)
            continue;
        if (f_get(polls[i].fd) == NULL) {
            polls[i].revents = POLL_NVAL;
            nval_count++;
        }
    }
    struct poll_context context = {polls, files, nfds};
    if (poll_trace_net_enabled()) {
        for (unsigned i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_net_fd(files[i], polls[i].events | POLL_ALWAYS_LISTENING, ready, polls[i].revents, "enter");
        }
    }
    bool trace_short_poll = poll_trace_short_timeout(timeout_ts_ptr, timeout_trace);
    if (trace_short_poll) {
        printk("INFO: wait poll enter pid=%d comm=%s nfds=%u timeout_ms=%d\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               nfds, timeout_trace);
        for (unsigned i = 0; i < nfds; i++) {
            int ready = files[i] != NULL && files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            if (files[i] == NULL)
                continue;
            poll_trace_short_wait_fd(files[i], polls[i].events, ready, polls[i].revents, "poll-enter");
        }
    }
    int res = 0;
    struct timespec mutable_timeout;
    struct timespec zero_timeout = {0};
    struct timespec *poll_timeout = NULL;
    if (timeout_ts_ptr != NULL) {
        mutable_timeout = *timeout_ts_ptr;
        poll_timeout = &mutable_timeout;
    }
    // An invalid fd (POLLNVAL) makes poll(2) return immediately rather than
    // blocking on the valid fds -- match Linux by forcing a zero timeout.
    if (nval_count > 0)
        poll_timeout = &zero_timeout;
    TASK_MAY_BLOCK {
        res = poll_wait(poll, poll_event_callback, &context, poll_timeout);
    }
out:
    poll_destroy(poll);
    for (unsigned i = 0; i < nfds; i++) {
        if (files[i] != NULL)
            fd_close(files[i]);
    }
    STRACE("%d end poll", current->pid);
    if (poll_trace_net_enabled()) {
        printk("INFO: net poll return pid=%d comm=%s res=%d timeout_ms=%d\n",
               current->pid, current->comm, res, timeout_trace);
        for (unsigned i = 0; i < nfds; i++) {
            if (files[i] == NULL)
                continue;
            int ready = files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            poll_trace_net_fd(files[i], polls[i].events | POLL_ALWAYS_LISTENING, ready, polls[i].revents, "exit");
        }
    }
    if (trace_short_poll) {
        printk("INFO: wait poll exit pid=%d comm=%s res=%d timeout_ms=%d\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, timeout_trace);
        for (unsigned i = 0; i < nfds; i++) {
            int ready = files[i] != NULL && files[i]->ops->poll ? files[i]->ops->poll(files[i]) : 0;
            if (files[i] == NULL)
                continue;
            poll_trace_short_wait_fd(files[i], polls[i].events, ready, polls[i].revents, "poll-exit");
        }
    }

    if (add_err < 0)
        return add_err;
    if (res < 0)
        return res;
    // Invalid (POLLNVAL) fds were pre-marked in revents but are never "ready"
    // through poll_wait; count them toward the number of fds with events.
    res += nval_count;
    if (fds != 0 || nfds != 0)
        if (user_write(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    return res;
}

dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout) {
    struct timespec timeout_ts;

    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_poll_common(fds, nfds, timeout_ts_ptr, timeout);
}

dword_t sys_poll_guest(guest_addr_t fds, dword_t nfds, int_t timeout) {
    struct timespec timeout_ts;
    const struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }
    return sys_poll_common(fds, nfds, timeout_ts_ptr, timeout);
}

dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    // a system call can only take 6 parameters, so the last two need to be passed as a pointer to a struct
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        int err = read_pselect_timeout(GUEST_ABI_I386, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // Keep the temporary mask installed until interrupt-exit signal delivery
    // runs. That path knows how to restore saved_mask after deciding whether
    // the pending signal should interrupt the wait.
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_pselect_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr,
        guest_addr_t exceptfds_addr, guest_addr_t timeout_addr, guest_addr_t sigmask_addr) {
    struct {
        guest_addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        int err = read_pselect_timeout(GUEST_ABI_I386, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_pselect_amd64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        int err = read_pselect_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_pselect_amd64_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr,
        guest_addr_t exceptfds_addr, guest_addr_t timeout_addr, guest_addr_t sigmask_addr) {
    struct {
        guest_addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        int err = read_pselect_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_ts);
        if (err < 0)
            return err;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_pselect_time64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr,
        addr_t timeout_addr, addr_t sigmask_addr) {
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {};
    if (sigmask_addr != 0) {
        if (user_get(sigmask_addr, sigmask))
            return _EFAULT;
    }
    sigset_t_ mask;
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ts_ptr = NULL;

    if (timeout_addr != 0) {
        struct timespec64_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_timespec.sec;
        timeout_ts.tv_nsec = timeout_timespec.nsec;
        if (!select_timeout_valid(timeout_ts))
            return _EINVAL;
        timeout_ts_ptr = &timeout_ts;
    }

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // See sys_pselect(): receive_signals() restores the saved mask after it
    // handles any signal that interrupted the wait.
    return sys_select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_ptr);
}

dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_timespec = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int err = read_ppoll_timeout(GUEST_ABI_I386, timeout_addr, &timeout_timespec, &timeout_ms);
        if (err < 0)
            return err;
        timeout_ptr = &timeout_timespec;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    // Leave restoration to receive_signals() so an interrupting signal is
    // observed against the temporary mask instead of the restored one.
    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}

dword_t sys_ppoll_guest(guest_addr_t fds, dword_t nfds, guest_addr_t timeout_addr,
        guest_addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_timespec = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int err = read_ppoll_timeout(GUEST_ABI_I386, timeout_addr, &timeout_timespec, &timeout_ms);
        if (err < 0)
            return err;
        timeout_ptr = &timeout_timespec;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}

dword_t sys_ppoll_amd64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_timespec = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int err = read_ppoll_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_timespec, &timeout_ms);
        if (err < 0)
            return err;
        timeout_ptr = &timeout_timespec;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}

dword_t sys_ppoll_amd64_guest(guest_addr_t fds, dword_t nfds, guest_addr_t timeout_addr,
        guest_addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_timespec = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int err = read_ppoll_timeout(GUEST_ABI_AMD64, timeout_addr, &timeout_timespec, &timeout_ms);
        if (err < 0)
            return err;
        timeout_ptr = &timeout_timespec;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}
