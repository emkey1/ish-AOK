// POSIX message queues: mq_open(2), mq_unlink(2), mq_timedsend(2),
// mq_timedreceive(2), mq_notify(2) and mq_getsetattr(2), and the mqueue
// filesystem that shows the same queues as files.
//
// AOK answered ENOSYS for all six. Every behaviour below was measured on Linux
// 6.12 (tests/manual/mqueue_ops.c): the name and attribute rules and their
// errnos, the per-user RLIMIT_MSGQUEUE charge (EMFILE) and the namespace's
// queues_max (ENOSPC), priority order, blocking with an absolute CLOCK_REALTIME
// deadline, poll, the status line read() returns, and notification -- by
// signal, and by SIGEV_THREAD, which glibc and musl both build on a netlink
// socket the kernel sends a 32-byte cookie to.
//
// A queue belongs to an IPC namespace (kernel/ipc_ns.h), which is what names
// it. Its descriptors are files of the mqueue filesystem: mq_open's belong to
// an internal mount of it, as on Linux; a mounted one (/dev/mqueue) opens the
// same queues by path.

#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/ipc_ns.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/time.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"

#define MQ_PRIO_MAX_ 32768
#define HARD_MSGMAX_ 65536
#define HARD_MSGSIZEMAX_ (16 * 1024 * 1024)
// What Linux charges per message slot on top of the message itself: a
// struct msg_msg and a struct posix_msg_tree_node (48 bytes each on a 64-bit
// kernel). Measured: ten default queues of 10 x 8192 bytes do not fit the
// default 819200-byte RLIMIT_MSGQUEUE, nine do, and one of them does not fit
// 82800 bytes.
#define MQ_SLOT_OVERHEAD_ 96
#define NOTIFY_COOKIE_LEN_ 32
#define NOTIFY_WOKENUP_ 1
#define NOTIFY_REMOVED_ 2
#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ 2
// A queue file's st_size: Linux's FILENT_SIZE, whatever the queue holds.
#define MQ_FILE_SIZE_ 80
#define MQUEUE_MAGIC_ 0x19800202

struct mq_msg {
    struct list link;
    dword_t prio;
    size_t len;
    char data[];
};

struct mqueue {
    atomic_uint refs;           // its name, while it has one, and each open description
    struct ipc_namespace *ns;   // held, for the count it is part of
    lock_t lock;
    cond_t cond;                // blocked senders and receivers
    struct list ns_link;        // in ns->mq_queues while it has its name
    bool linked;
    char name[MAX_NAME];

    mode_t_ mode;
    uid_t_ uid, gid;
    qword_t inode;
    struct timespec atime, mtime, ctime;

    long maxmsg, msgsize;
    long curmsgs;
    size_t qsize;               // bytes of message text queued
    struct list msgs;           // highest priority first, in order within one
    int receivers_waiting;

    uid_t_ charged_uid;         // RLIMIT_MSGQUEUE's charge for this queue
    size_t charged_bytes;

    // mq_notify: the process registered (its tgid; 0 for none), how, and the
    // exec generation it registered in -- a process that has since exec'd is
    // not told.
    pid_t_ notify_tgid;
    int notify_method;
    int notify_signo;
    union sigval_ notify_value;
    unsigned notify_exec_gen;
    struct fd *notify_sock;     // SIGEV_THREAD, retained
    char notify_cookie[NOTIFY_COOKIE_LEN_];

    lock_t fds_lock;            // the open descriptors, for poll wakeups
    struct list fds;
};

// One open description of a queue (fd->fs_data).
struct mq_file {
    struct mqueue *q;
    struct fd *fd;
    struct list link;
};

// A registration taken off its queue under the queue's lock, to be acted on
// once the lock is dropped: signalling takes pids_lock and the target's signal
// locks, and the SIGEV_THREAD cookie wakes the helper's socket and may close
// it, none of which should nest inside a queue's lock.
struct mq_note {
    int method;                 // 0: nothing to do
    pid_t_ tgid;
    int signo;
    union sigval_ value;
    unsigned exec_gen;
    struct fd *sock;            // retained; closed once delivered
    char cookie[NOTIFY_COOKIE_LEN_];
};

static struct mq_note mq_notify_take_locked(struct mqueue *q, char why);
static void mq_note_fire(struct mq_note *note);
static void mq_note_remove(struct mq_note *note);

static const struct fd_ops mq_fdops;
static const struct fd_ops mqueuefs_dir_fdops;
// mq_open's descriptors live on this, as Linux's live on the namespace's
// internal mount of the filesystem.
static struct mount mqueue_internal_mount;

static _Atomic qword_t mq_next_inode = 1;

// ----------------------------------------------------------- accounting

// RLIMIT_MSGQUEUE is per user, across every queue that user made.
struct mq_user {
    uid_t_ uid;
    size_t bytes;
};
static lock_t mq_users_lock = LOCK_INITIALIZER;
static struct mq_user *mq_users;
static size_t mq_nusers, mq_users_cap;

static struct mq_user *mq_user_find(uid_t_ uid, bool create) {
    for (size_t i = 0; i < mq_nusers; i++)
        if (mq_users[i].uid == uid)
            return &mq_users[i];
    if (!create)
        return NULL;
    if (mq_nusers == mq_users_cap) {
        size_t cap = mq_users_cap == 0 ? 8 : mq_users_cap * 2;
        struct mq_user *grown = realloc(mq_users, cap * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        mq_users = grown;
        mq_users_cap = cap;
    }
    mq_users[mq_nusers] = (struct mq_user) {.uid = uid};
    return &mq_users[mq_nusers++];
}

static int mq_charge(uid_t_ uid, size_t bytes) {
    rlim_t_ limit = rlimit(RLIMIT_MSGQUEUE_);
    lock(&mq_users_lock, 0);
    struct mq_user *user = mq_user_find(uid, true);
    int err = 0;
    if (user == NULL)
        err = _ENOMEM;
    else if (user->bytes + bytes < user->bytes ||
            (limit != RLIM_INFINITY_ && user->bytes + bytes > limit))
        err = _EMFILE;
    else
        user->bytes += bytes;
    unlock(&mq_users_lock);
    return err;
}

static void mq_uncharge(uid_t_ uid, size_t bytes) {
    lock(&mq_users_lock, 0);
    struct mq_user *user = mq_user_find(uid, false);
    if (user != NULL)
        user->bytes = user->bytes >= bytes ? user->bytes - bytes : 0;
    unlock(&mq_users_lock);
}

// ----------------------------------------------------------- queues

static struct timespec mq_now(void) {
    return guest_clock_now(CLOCK_REALTIME_, CLOCK_REALTIME);
}

static bool timespec_reached(struct timespec now, struct timespec deadline) {
    return now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

static struct mqueue *mq_retain(struct mqueue *q) {
    atomic_fetch_add(&q->refs, 1);
    return q;
}

static void mq_release(struct mqueue *q) {
    if (atomic_fetch_sub(&q->refs, 1) != 1)
        return;
    struct mq_msg *msg, *tmp;
    list_for_each_entry_safe(&q->msgs, msg, tmp, link) {
        list_remove(&msg->link);
        free(msg);
    }
    if (q->notify_sock != NULL)
        fd_close(q->notify_sock);
    mq_uncharge(q->charged_uid, q->charged_bytes);
    // Linux counts a queue against queues_max until it is gone, not until
    // its name is.
    lock(&q->ns->mq_lock, 0);
    if (q->ns->mq_queues_count > 0)
        q->ns->mq_queues_count--;
    unlock(&q->ns->mq_lock);
    ipc_ns_unhold(q->ns);
    cond_destroy(&q->cond);
    free(q);
}

// Caller holds ns->mq_lock.
static struct mqueue *mq_lookup_locked(struct ipc_namespace *ns, const char *name) {
    struct mqueue *q;
    list_for_each_entry(&ns->mq_queues, q, ns_link)
        if (strcmp(q->name, name) == 0)
            return q;
    return NULL;
}

// The name rules lookup_one_len applies: no slash, not "." or "..", and a
// component's length at most. An empty name never gets this far (ENOENT).
static int mq_check_name(const char *name) {
    if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return _EACCES;
    if (strlen(name) > MAX_NAME - 1)
        return _ENAMETOOLONG;
    return 0;
}

// mqueue_create_attr / mqueue_get_inode: a new queue named `name`, with
// `maxmsg` x `msgsize` (the namespace's defaults when `attr_given` is false),
// linked into `ns`. Caller holds ns->mq_lock and has checked the name is free.
static int mq_create_locked(struct ipc_namespace *ns, const char *name, mode_t_ mode,
        bool attr_given, long maxmsg, long msgsize, struct mqueue **out) {
    if (ns->mq_queues_count >= ns->mq_queues_max && !current_capable(CAP_SYS_RESOURCE_))
        return _ENOSPC;
    if (!attr_given) {
        // The defaults, each capped by its ceiling as Linux's mqueue_get_inode
        // caps them: the sysctl takes a msg_default above msg_max, and a
        // queue made without attributes gets msg_max (measured).
        maxmsg = ns->mq_msg_default < ns->mq_msg_max ? ns->mq_msg_default : ns->mq_msg_max;
        msgsize = ns->mq_msgsize_default < ns->mq_msgsize_max ?
                ns->mq_msgsize_default : ns->mq_msgsize_max;
    }
    if (maxmsg <= 0 || msgsize <= 0)
        return _EINVAL;
    if (current_capable(CAP_SYS_RESOURCE_)) {
        if (maxmsg > HARD_MSGMAX_ || msgsize > HARD_MSGSIZEMAX_)
            return _EINVAL;
    } else if (maxmsg > (long) ns->mq_msg_max || msgsize > (long) ns->mq_msgsize_max) {
        return _EINVAL;
    }
    size_t bytes = (size_t) maxmsg * (size_t) msgsize +
            (size_t) maxmsg * MQ_SLOT_OVERHEAD_;
    int err = mq_charge(current->uid, bytes);
    if (err < 0)
        return err;

    struct mqueue *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        mq_uncharge(current->uid, bytes);
        return _ENOMEM;
    }
    atomic_init(&q->refs, 1);   // the name
    q->ns = ns;
    ipc_ns_hold(ns);
    lock_init(&q->lock, "mqueue\0");
    cond_init(&q->cond);
    lock_init(&q->fds_lock, "mqueue.fds\0");
    list_init(&q->msgs);
    list_init(&q->fds);
    snprintf(q->name, sizeof(q->name), "%s", name);
    q->mode = mode & 07777;
    q->uid = current->fsuid;
    q->gid = current->fsgid;
    q->inode = atomic_fetch_add(&mq_next_inode, 1);
    q->atime = q->mtime = q->ctime = mq_now();
    q->maxmsg = maxmsg;
    q->msgsize = msgsize;
    q->charged_uid = current->uid;
    q->charged_bytes = bytes;
    q->linked = true;
    list_add_tail(&ns->mq_queues, &q->ns_link);
    ns->mq_queues_count++;
    *out = q;
    return 0;
}

// Take the name away. Caller holds ns->mq_lock; drops the name's reference
// after it lets go of the lock.
static void mq_unlink_locked(struct mqueue *q) {
    list_remove(&q->ns_link);
    q->linked = false;
}

void mqueue_ns_teardown(struct ipc_namespace *ns) {
    for (;;) {
        lock(&ns->mq_lock, 0);
        if (list_empty(&ns->mq_queues)) {
            unlock(&ns->mq_lock);
            return;
        }
        struct mqueue *q = list_first_entry(&ns->mq_queues, struct mqueue, ns_link);
        mq_unlink_locked(q);
        unlock(&ns->mq_lock);
        mq_release(q);
    }
}

// Every open description of the queue polls again.
static void mq_wake_pollers(struct mqueue *q, int events) {
    lock(&q->fds_lock, 0);
    struct mq_file *file;
    list_for_each_entry(&q->fds, file, link)
        poll_wakeup(file->fd, events);
    unlock(&q->fds_lock);
}

// ----------------------------------------------------------- descriptors

static struct fd *mq_fd_new(struct mqueue *q, int flags, bool internal) {
    struct fd *fd = fd_create(&mq_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct mq_file *file = malloc(sizeof(*file));
    if (file == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    file->q = mq_retain(q);
    file->fd = fd;
    fd->fs_data = file;
    fd->type = S_IFREG;
    fd->flags = flags & (O_ACCMODE_ | O_NONBLOCK_);
    if (internal) {
        mount_retain(&mqueue_internal_mount);
        fd->mount = &mqueue_internal_mount;
    }
    lock(&q->fds_lock, 0);
    list_add_tail(&q->fds, &file->link);
    unlock(&q->fds_lock);
    return fd;
}

static struct mqueue *mq_fd_queue(struct fd *fd) {
    if (fd == NULL || fd->ops != &mq_fdops || fd->fs_data == NULL)
        return NULL;
    return ((struct mq_file *) fd->fs_data)->q;
}

static int mq_close(struct fd *fd) {
    struct mq_file *file = fd->fs_data;
    if (file == NULL)
        return 0;
    fd->fs_data = NULL;
    struct mqueue *q = file->q;
    lock(&q->fds_lock, 0);
    list_remove(&file->link);
    unlock(&q->fds_lock);
    // A process's registration goes with its descriptor (Linux's
    // mqueue_flush_file).
    struct mq_note note = {0};
    lock(&q->lock, 0);
    if (current != NULL && q->notify_tgid != 0 && q->notify_tgid == current->tgid)
        note = mq_notify_take_locked(q, NOTIFY_REMOVED_);
    unlock(&q->lock);
    mq_note_remove(&note);
    free(file);
    mq_release(q);
    return 0;
}

static int mq_status_line(struct mqueue *q, char *buf, size_t size) {
    lock(&q->lock, 0);
    int notify = q->notify_tgid != 0 ? q->notify_method : 0;
    int signo = q->notify_tgid != 0 && q->notify_method == SIGEV_SIGNAL_ ? q->notify_signo : 0;
    int len = snprintf(buf, size, "QSIZE:%-10lu NOTIFY:%-5d SIGNO:%-5d NOTIFY_PID:%-6d\n",
            (unsigned long) q->qsize, notify, signo, (int) q->notify_tgid);
    q->atime = mq_now();
    unlock(&q->lock);
    return len;
}

static ssize_t mq_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return _EBADF;
    char line[128];
    int len = mq_status_line(q, line, sizeof(line));
    if (off < 0 || off >= len)
        return 0;
    size_t n = (size_t) (len - off);
    if (n > bufsize)
        n = bufsize;
    memcpy(buf, line + off, n);
    return (ssize_t) n;
}

// read() is the queue's status line, the same one the filesystem's file
// shows: QSIZE (bytes queued), NOTIFY and SIGNO (how a registration notifies),
// NOTIFY_PID (who registered).
static ssize_t mq_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t n = mq_pread(fd, buf, bufsize, (off_t) fd->offset);
    if (n > 0)
        fd->offset += (unsigned long) n;
    return n;
}

static off_t_ mq_lseek(struct fd *fd, off_t_ off, int whence) {
    off_t_ base;
    switch (whence) {
        case LSEEK_SET: base = 0; break;
        case LSEEK_CUR: base = (off_t_) fd->offset; break;
        case LSEEK_END: base = MQ_FILE_SIZE_; break;
        default: return _EINVAL;
    }
    if (base + off < 0)
        return _EINVAL;
    fd->offset = (unsigned long) (base + off);
    return (off_t_) fd->offset;
}

static int mq_poll(struct fd *fd) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return 0;
    lock(&q->lock, 0);
    int events = 0;
    if (q->curmsgs > 0)
        events |= POLL_READ;
    if (q->curmsgs < q->maxmsg)
        events |= POLL_WRITE;
    unlock(&q->lock);
    return events;
}

// ----------------------------------------------------------- notification

// Take the registration off the queue, forgetting it (it is one-shot), with
// the cookie's last byte saying why: NOTIFY_WOKENUP_ for a message,
// NOTIFY_REMOVED_ for a registration withdrawn or closed. Caller holds q->lock.
static struct mq_note mq_notify_take_locked(struct mqueue *q, char why) {
    struct mq_note note = {0};
    if (q->notify_tgid == 0)
        return note;
    note.method = q->notify_method;
    note.tgid = q->notify_tgid;
    note.signo = q->notify_signo;
    note.value = q->notify_value;
    note.exec_gen = q->notify_exec_gen;
    note.sock = q->notify_sock;
    memcpy(note.cookie, q->notify_cookie, sizeof(note.cookie));
    note.cookie[NOTIFY_COOKIE_LEN_ - 1] = why;
    q->notify_tgid = 0;
    q->notify_sock = NULL;
    return note;
}

// A message arrived in the empty queue: tell the process that registered, by
// signal (from the sender, as SI_MESGQ) or through its helper's socket -- if
// it is still the program that registered.
static void mq_note_fire(struct mq_note *note) {
    if (note->method == SIGEV_SIGNAL_ && note->signo != 0) {
        struct siginfo_ info = {
            .sig = note->signo,
            .code = SI_MESGQ_,
            .rt.pid = current->tgid,
            .rt.uid = current->uid,
            .rt.value = note->value,
        };
        complex_lockt(&pids_lock, 0);
        struct task *owner = pid_get_task(note->tgid);
        if (owner != NULL && owner->exec_gen == note->exec_gen)
            task_ref_cnt_mod(owner, 1);
        else
            owner = NULL;
        unlock(&pids_lock);
        if (owner != NULL) {
            send_signal_to_process(owner, note->signo, info);
            task_ref_cnt_mod(owner, -1);
        }
    } else if (note->method == SIGEV_THREAD_ && note->sock != NULL) {
        netlink_deliver_datagram(note->sock, note->cookie, sizeof(note->cookie));
    }
    if (note->sock != NULL)
        fd_close(note->sock);
    note->sock = NULL;
}

// A registration withdrawn (mq_notify(NULL), or its descriptor closed): a
// SIGEV_THREAD helper is told it is over (remove_notification), so it can free
// what it kept for the call; a signal registration just goes.
static void mq_note_remove(struct mq_note *note) {
    if (note->method == SIGEV_THREAD_ && note->sock != NULL)
        netlink_deliver_datagram(note->sock, note->cookie, sizeof(note->cookie));
    if (note->sock != NULL)
        fd_close(note->sock);
    note->sock = NULL;
}

// ----------------------------------------------------------- the syscalls

// getname() and then lookup_one_len()'s rules. Empty is ENOENT, longer than a
// path may be is ENAMETOOLONG, and the component rules are mq_check_name's.
static int mq_import_name(guest_addr_t addr, char *name, size_t size) {
    char path[MAX_PATH];
    int err = user_read_path(addr, path, sizeof(path));
    if (err < 0)
        return err;
    if (path[0] == '\0')
        return _ENOENT;
    err = mq_check_name(path);
    if (err < 0)
        return err;
    snprintf(name, size, "%s", path);
    return 0;
}

// struct mq_attr: eight longs, so four bytes each on i386 and eight on the
// 64-bit ABIs.
struct mq_attr_ {
    sqword_t flags, maxmsg, msgsize, curmsgs;
};

static int mq_read_attr(guest_addr_t addr, struct mq_attr_ *attr) {
    if (guest_abi_is_64bit(current->abi)) {
        sqword_t raw[8];
        if (user_read(addr, raw, sizeof(raw)))
            return _EFAULT;
        *attr = (struct mq_attr_) {raw[0], raw[1], raw[2], raw[3]};
    } else {
        sdword_t raw[8];
        if (user_read(addr, raw, sizeof(raw)))
            return _EFAULT;
        *attr = (struct mq_attr_) {raw[0], raw[1], raw[2], raw[3]};
    }
    return 0;
}

static int mq_write_attr(guest_addr_t addr, const struct mq_attr_ *attr) {
    if (guest_abi_is_64bit(current->abi)) {
        sqword_t raw[8] = {attr->flags, attr->maxmsg, attr->msgsize, attr->curmsgs};
        return user_write(addr, raw, sizeof(raw)) ? _EFAULT : 0;
    }
    sdword_t raw[8] = {(sdword_t) attr->flags, (sdword_t) attr->maxmsg,
            (sdword_t) attr->msgsize, (sdword_t) attr->curmsgs};
    return user_write(addr, raw, sizeof(raw)) ? _EFAULT : 0;
}

static int mq_access(struct mqueue *q, int oflag) {
    int acc;
    switch (oflag & O_ACCMODE_) {
        case O_RDONLY_: acc = AC_R; break;
        case O_WRONLY_: acc = AC_W; break;
        case O_RDWR_: acc = AC_R | AC_W; break;
        default: return _EINVAL;
    }
    struct statbuf stat = {.mode = S_IFREG | q->mode, .uid = q->uid, .gid = q->gid};
    return access_check(&stat, acc);
}

fd_t sys_mq_open_guest(guest_addr_t name_addr, dword_t oflag, mode_t_ mode, guest_addr_t attr_addr) {
    struct mq_attr_ attr = {};
    if (attr_addr != 0 && mq_read_attr(attr_addr, &attr) < 0)
        return _EFAULT;
    char name[MAX_NAME];
    int err = mq_import_name(name_addr, name, sizeof(name));
    STRACE("mq_open(\"%s\", %#x, %#o, %#llx)", err < 0 ? "?" : name, oflag, mode,
            (unsigned long long) attr_addr);
    if (err < 0)
        return err;
    // Creating takes the umask, as a file does.
    mode &= ~current->fs->umask;

    struct ipc_namespace *ns = ipc_ns_current();
    lock(&ns->mq_lock, 0);
    struct mqueue *q = mq_lookup_locked(ns, name);
    if (q == NULL) {
        if (!(oflag & O_CREAT_)) {
            unlock(&ns->mq_lock);
            return _ENOENT;
        }
        // The attributes are looked at only here, for a queue being made:
        // an existing queue's open ignores even malformed ones.
        err = mq_create_locked(ns, name, mode, attr_addr != 0, (long) attr.maxmsg,
                (long) attr.msgsize, &q);
        if (err < 0) {
            unlock(&ns->mq_lock);
            return err;
        }
        mq_retain(q);
    } else {
        if ((oflag & (O_CREAT_ | O_EXCL_)) == (O_CREAT_ | O_EXCL_)) {
            unlock(&ns->mq_lock);
            return _EEXIST;
        }
        mq_retain(q);
        lock(&q->lock, 0);
        err = mq_access(q, (int) oflag);
        unlock(&q->lock);
        if (err < 0) {
            unlock(&ns->mq_lock);
            mq_release(q);
            return err;
        }
    }
    unlock(&ns->mq_lock);

    struct fd *fd = mq_fd_new(q, (int) oflag, true);
    mq_release(q);
    if (IS_ERR(fd))
        return (fd_t) PTR_ERR(fd);
    // Close-on-exec always: POSIX closes message queue descriptors on exec.
    return f_install(fd, O_CLOEXEC_);
}

fd_t sys_mq_open(addr_t name_addr, dword_t oflag, mode_t_ mode, addr_t attr_addr) {
    return sys_mq_open_guest(name_addr, oflag, mode, attr_addr);
}

static int mq_unlink_name(struct ipc_namespace *ns, const char *name) {
    lock(&ns->mq_lock, 0);
    struct mqueue *q = mq_lookup_locked(ns, name);
    if (q == NULL) {
        unlock(&ns->mq_lock);
        return _ENOENT;
    }
    // The queues' directory is sticky and world-writable: only a queue's
    // owner, or someone with CAP_FOWNER, may remove it (may_delete's EPERM).
    if (current->fsuid != q->uid && current->fsuid != 0 && !current_capable(CAP_FOWNER_)) {
        unlock(&ns->mq_lock);
        return _EPERM;
    }
    mq_unlink_locked(q);
    unlock(&ns->mq_lock);
    mq_release(q);
    return 0;
}

dword_t sys_mq_unlink_guest(guest_addr_t name_addr) {
    char name[MAX_NAME];
    int err = mq_import_name(name_addr, name, sizeof(name));
    STRACE("mq_unlink(\"%s\")", err < 0 ? "?" : name);
    if (err < 0)
        return err;
    return mq_unlink_name(ipc_ns_current(), name);
}

dword_t sys_mq_unlink(addr_t name_addr) {
    return sys_mq_unlink_guest(name_addr);
}

// The absolute CLOCK_REALTIME deadline a timed call was given: validated
// before anything else, as Linux's prepare_timeout does -- a malformed one is
// EINVAL even when the call would not have waited.
static int mq_read_deadline(guest_addr_t addr, bool time64, struct timespec *deadline) {
    if (addr == 0)
        return 1;
    int err = read_guest_timespec_abi(time64 ? GUEST_ABI_AMD64 : current->abi, addr, deadline);
    if (err < 0)
        return err;
    // A 32-bit caller's 64-bit timespec has 32 bits of nanoseconds and 32 of
    // padding, which Linux's get_timespec64 drops for a compat call.
    if (time64 && !guest_abi_is_64bit(current->abi))
        deadline->tv_nsec = (long) (uint32_t) deadline->tv_nsec;
    if (deadline->tv_sec < 0 || deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000)
        return _EINVAL;
    return 0;
}

// Wait on the queue until woken, the deadline passes (ETIMEDOUT) or a signal
// comes (EINTR, or a restart for an SA_RESTART handler). Caller holds q->lock.
static int mq_wait_locked(struct mqueue *q, bool timed, struct timespec deadline) {
    struct timespec rel, *timeout = NULL;
    if (timed) {
        struct timespec now = mq_now();
        if (timespec_reached(now, deadline))
            return _ETIMEDOUT;
        rel = timespec_subtract(deadline, now);
        timeout = &rel;
    }
    int err = wait_for_blocked(&q->cond, &q->lock, timeout);
    if (err == _EINTR && signal_should_restart_syscall())
        err = _ERESTART;
    return err;
}

static dword_t mq_send_fd(struct fd *fd, guest_addr_t msg_addr, qword_t len, dword_t prio,
        bool timed, struct timespec deadline);
static dword_t mq_receive_fd(struct fd *fd, guest_addr_t msg_addr, qword_t len,
        guest_addr_t prio_addr, bool timed, struct timespec deadline);

static dword_t mq_timedsend_common(fd_t mqd, guest_addr_t msg_addr, qword_t len, dword_t prio,
        guest_addr_t timeout_addr, bool time64) {
    STRACE("mq_timedsend(%d, %#llx, %llu, %u, %#llx)", mqd, (unsigned long long) msg_addr,
            (unsigned long long) len, prio, (unsigned long long) timeout_addr);
    struct timespec deadline = {};
    int timed = mq_read_deadline(timeout_addr, time64, &deadline);
    if (timed < 0)
        return timed;
    if (prio >= MQ_PRIO_MAX_)
        return _EINVAL;
    // Held for the call: a send can block, and another thread closing the
    // descriptor meanwhile must not free the queue out from under it.
    struct fd *fd = f_get_retain(mqd);
    if (fd == NULL)
        return _EBADF;
    dword_t res = mq_send_fd(fd, msg_addr, len, prio, timed == 0, deadline);
    fd_close(fd);
    return res;
}

static dword_t mq_send_fd(struct fd *fd, guest_addr_t msg_addr, qword_t len, dword_t prio,
        bool timed, struct timespec deadline) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return _EBADF;
    int acc = fd->flags & O_ACCMODE_;
    if (acc != O_WRONLY_ && acc != O_RDWR_)
        return _EBADF;
    if (len > (qword_t) q->msgsize)
        return _EMSGSIZE;
    struct mq_msg *msg = malloc(sizeof(*msg) + (size_t) len);
    if (msg == NULL)
        return _ENOMEM;
    msg->prio = prio;
    msg->len = (size_t) len;
    if (len != 0 && user_read(msg_addr, msg->data, (size_t) len)) {
        free(msg);
        return _EFAULT;
    }

    lock(&q->lock, 0);
    int err = 0;
    while (q->curmsgs >= q->maxmsg) {
        if (fd->flags & O_NONBLOCK_) {
            err = _EAGAIN;
            break;
        }
        err = mq_wait_locked(q, timed, deadline);
        if (err < 0)
            break;
    }
    if (err < 0) {
        unlock(&q->lock);
        free(msg);
        return err;
    }
    // Behind every message of its own priority or higher.
    struct mq_msg *pos;
    struct list *before = &q->msgs;
    list_for_each_entry(&q->msgs, pos, link) {
        if (pos->prio < prio) {
            before = &pos->link;
            break;
        }
    }
    list_add_tail(before, &msg->link);
    q->curmsgs++;
    q->qsize += msg->len;
    q->mtime = q->ctime = q->atime = mq_now();
    // A process waiting in mq_receive takes the message straight away, and
    // then no notification is sent: Linux hands it over without it ever being
    // queued. Otherwise a queue that was empty tells its registered process.
    struct mq_note note = {0};
    if (q->receivers_waiting == 0 && q->curmsgs == 1)
        note = mq_notify_take_locked(q, NOTIFY_WOKENUP_);
    notify(&q->cond);
    unlock(&q->lock);
    mq_note_fire(&note);
    mq_wake_pollers(q, POLL_READ);
    return 0;
}

dword_t sys_mq_timedsend_guest(fd_t mqd, guest_addr_t msg_addr, qword_t len, dword_t prio,
        guest_addr_t timeout_addr) {
    return mq_timedsend_common(mqd, msg_addr, len, prio, timeout_addr, false);
}
dword_t sys_mq_timedsend(fd_t mqd, addr_t msg_addr, dword_t len, dword_t prio, addr_t timeout_addr) {
    return mq_timedsend_common(mqd, msg_addr, len, prio, timeout_addr, false);
}
dword_t sys_mq_timedsend_time64(fd_t mqd, addr_t msg_addr, dword_t len, dword_t prio, addr_t timeout_addr) {
    return mq_timedsend_common(mqd, msg_addr, len, prio, timeout_addr, true);
}

static dword_t mq_timedreceive_common(fd_t mqd, guest_addr_t msg_addr, qword_t len,
        guest_addr_t prio_addr, guest_addr_t timeout_addr, bool time64) {
    STRACE("mq_timedreceive(%d, %#llx, %llu, %#llx, %#llx)", mqd, (unsigned long long) msg_addr,
            (unsigned long long) len, (unsigned long long) prio_addr, (unsigned long long) timeout_addr);
    struct timespec deadline = {};
    int timed = mq_read_deadline(timeout_addr, time64, &deadline);
    if (timed < 0)
        return timed;
    // Held for the call, as a send holds it.
    struct fd *fd = f_get_retain(mqd);
    if (fd == NULL)
        return _EBADF;
    dword_t res = mq_receive_fd(fd, msg_addr, len, prio_addr, timed == 0, deadline);
    fd_close(fd);
    return res;
}

static dword_t mq_receive_fd(struct fd *fd, guest_addr_t msg_addr, qword_t len,
        guest_addr_t prio_addr, bool timed, struct timespec deadline) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return _EBADF;
    int acc = fd->flags & O_ACCMODE_;
    if (acc != O_RDONLY_ && acc != O_RDWR_)
        return _EBADF;
    if (len < (qword_t) q->msgsize)
        return _EMSGSIZE;

    lock(&q->lock, 0);
    int err = 0;
    q->receivers_waiting++;
    while (q->curmsgs == 0) {
        if (fd->flags & O_NONBLOCK_) {
            err = _EAGAIN;
            break;
        }
        err = mq_wait_locked(q, timed, deadline);
        if (err < 0)
            break;
    }
    q->receivers_waiting--;
    if (err < 0) {
        unlock(&q->lock);
        return err;
    }
    struct mq_msg *msg = list_first_entry(&q->msgs, struct mq_msg, link);
    list_remove(&msg->link);
    q->curmsgs--;
    q->qsize -= msg->len;
    q->mtime = q->ctime = q->atime = mq_now();
    notify(&q->cond);
    unlock(&q->lock);
    mq_wake_pollers(q, POLL_WRITE);

    // Taken off the queue already: a fault copying it out loses it, as on
    // Linux.
    dword_t res = (dword_t) msg->len;
    if ((prio_addr != 0 && user_put(prio_addr, msg->prio)) ||
            (msg->len != 0 && user_write(msg_addr, msg->data, msg->len)))
        res = _EFAULT;
    free(msg);
    return res;
}

dword_t sys_mq_timedreceive_guest(fd_t mqd, guest_addr_t msg_addr, qword_t len,
        guest_addr_t prio_addr, guest_addr_t timeout_addr) {
    return mq_timedreceive_common(mqd, msg_addr, len, prio_addr, timeout_addr, false);
}
dword_t sys_mq_timedreceive(fd_t mqd, addr_t msg_addr, dword_t len, addr_t prio_addr, addr_t timeout_addr) {
    return mq_timedreceive_common(mqd, msg_addr, len, prio_addr, timeout_addr, false);
}
dword_t sys_mq_timedreceive_time64(fd_t mqd, addr_t msg_addr, dword_t len, addr_t prio_addr, addr_t timeout_addr) {
    return mq_timedreceive_common(mqd, msg_addr, len, prio_addr, timeout_addr, true);
}

// struct sigevent, as mq_notify reads it: the value, the signal and the method
// are in the same order on every ABI, after a sigval of the ABI's pointer size.
static int mq_read_sigevent(guest_addr_t addr, int *method, int *signo, union sigval_ *value) {
    if (guest_abi_is_64bit(current->abi)) {
        struct {
            qword_t value;
            int_t signo;
            int_t method;
        } raw;
        if (user_get(addr, raw))
            return _EFAULT;
        value->sv_ptr = raw.value;
        *signo = raw.signo;
        *method = raw.method;
    } else {
        struct {
            dword_t value;
            int_t signo;
            int_t method;
        } raw;
        if (user_get(addr, raw))
            return _EFAULT;
        value->sv_ptr = raw.value;
        *signo = raw.signo;
        *method = raw.method;
    }
    return 0;
}

dword_t sys_mq_notify_guest(fd_t mqd, guest_addr_t sev_addr) {
    STRACE("mq_notify(%d, %#llx)", mqd, (unsigned long long) sev_addr);
    int method = 0, signo = 0;
    union sigval_ value = {};
    char cookie[NOTIFY_COOKIE_LEN_];
    struct fd *sock = NULL;
    if (sev_addr != 0) {
        int err = mq_read_sigevent(sev_addr, &method, &signo, &value);
        if (err < 0)
            return err;
        if (method != SIGEV_NONE_ && method != SIGEV_SIGNAL_ && method != SIGEV_THREAD_)
            return _EINVAL;
        // 0 is accepted, and then sends nothing.
        if (method == SIGEV_SIGNAL_ && (signo < 0 || signo >= NUM_SIGS))
            return _EINVAL;
        if (method == SIGEV_THREAD_) {
            // The cookie libc's helper thread reads back, from sival_ptr; the
            // socket it reads it on, from sigev_signo.
            if (user_read(value.sv_ptr, cookie, sizeof(cookie)))
                return _EFAULT;
            struct fd *s = f_get(signo);
            if (s == NULL)
                return _EBADF;
            err = netlink_fd_check(s);
            if (err < 0)
                return err;
            sock = fd_retain(s);
        }
    }

    struct fd *fd = f_get(mqd);
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL) {
        if (sock != NULL)
            fd_close(sock);
        return _EBADF;
    }

    struct mq_note removed = {0};
    lock(&q->lock, 0);
    int err = 0;
    if (sev_addr == 0) {
        // Only the registered process may take it back; anyone else's
        // attempt changes nothing and still succeeds.
        if (q->notify_tgid == current->tgid) {
            removed = mq_notify_take_locked(q, NOTIFY_REMOVED_);
            q->atime = q->ctime = mq_now();
        }
    } else if (q->notify_tgid != 0) {
        err = _EBUSY;
    } else {
        q->notify_tgid = current->tgid;
        q->notify_method = method;
        q->notify_signo = method == SIGEV_SIGNAL_ ? signo : 0;
        q->notify_value = value;
        q->notify_exec_gen = current->exec_gen;
        if (method == SIGEV_THREAD_) {
            q->notify_sock = sock;
            memcpy(q->notify_cookie, cookie, sizeof(cookie));
            sock = NULL;
        }
        q->atime = q->ctime = mq_now();
    }
    unlock(&q->lock);
    mq_note_remove(&removed);
    if (sock != NULL)
        fd_close(sock);
    return err;
}

dword_t sys_mq_notify(fd_t mqd, addr_t sev_addr) {
    return sys_mq_notify_guest(mqd, sev_addr);
}

dword_t sys_mq_getsetattr_guest(fd_t mqd, guest_addr_t new_addr, guest_addr_t old_addr) {
    STRACE("mq_getsetattr(%d, %#llx, %#llx)", mqd, (unsigned long long) new_addr,
            (unsigned long long) old_addr);
    struct mq_attr_ want = {};
    if (new_addr != 0) {
        if (mq_read_attr(new_addr, &want) < 0)
            return _EFAULT;
        // Only O_NONBLOCK can be changed, and asking for anything else is
        // malformed. The other fields are ignored.
        if (want.flags & ~(sqword_t) O_NONBLOCK_)
            return _EINVAL;
    }
    struct fd *fd = f_get(mqd);
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return _EBADF;
    lock(&q->lock, 0);
    struct mq_attr_ old = {
        .flags = fd->flags & O_NONBLOCK_,
        .maxmsg = q->maxmsg,
        .msgsize = q->msgsize,
        .curmsgs = q->curmsgs,
    };
    if (new_addr != 0) {
        if (want.flags & O_NONBLOCK_)
            fd->flags |= O_NONBLOCK_;
        else
            fd->flags &= ~O_NONBLOCK_;
        q->atime = q->ctime = mq_now();
    }
    unlock(&q->lock);
    if (old_addr != 0 && mq_write_attr(old_addr, &old) < 0)
        return _EFAULT;
    return 0;
}

dword_t sys_mq_getsetattr(fd_t mqd, addr_t new_addr, addr_t old_addr) {
    return sys_mq_getsetattr_guest(mqd, new_addr, old_addr);
}

// ----------------------------------------------------------- the filesystem
//
// A flat directory of the namespace's queues: the one mq_open's descriptors
// belong to, and any the guest mounts (mount -t mqueue none /dev/mqueue),
// which shows the queues of the IPC namespace it was mounted from. Creating a
// file makes a queue with the default attributes; removing one is mq_unlink;
// nothing else can be made here (EPERM).

static struct ipc_namespace *mqueuefs_ns(struct mount *mount) {
    if (mount == &mqueue_internal_mount || mount->data == NULL)
        return ipc_ns_current();
    return mount->data;
}

static int mqueuefs_mount(struct mount *mount) {
    mount->data = ipc_ns_retain(ipc_ns_current());
    return 0;
}

static int mqueuefs_umount(struct mount *mount) {
    if (mount->data != NULL)
        ipc_ns_release(mount->data);
    mount->data = NULL;
    return 0;
}

// "" is the root; "/name" a queue.
static const char *mqueuefs_name(const char *path) {
    if (path[0] == '\0')
        return NULL;
    return path[0] == '/' ? path + 1 : path;
}

static void mqueuefs_root_stat(struct statbuf *stat) {
    *stat = (struct statbuf) {
        .inode = 1,
        .mode = S_IFDIR | 01777,
        .nlink = 2,
        .blksize = 4096,
    };
}

static void mq_stat(struct mqueue *q, struct statbuf *stat) {
    lock(&q->lock, 0);
    *stat = (struct statbuf) {
        .inode = q->inode + 1,
        .mode = S_IFREG | q->mode,
        .nlink = q->linked ? 1 : 0,
        .uid = q->uid,
        .gid = q->gid,
        .size = MQ_FILE_SIZE_,
        .blksize = 4096,
        .atime = (dword_t) q->atime.tv_sec, .atime_nsec = (dword_t) q->atime.tv_nsec,
        .mtime = (dword_t) q->mtime.tv_sec, .mtime_nsec = (dword_t) q->mtime.tv_nsec,
        .ctime = (dword_t) q->ctime.tv_sec, .ctime_nsec = (dword_t) q->ctime.tv_nsec,
    };
    unlock(&q->lock);
}

static struct mqueue *mqueuefs_lookup(struct mount *mount, const char *name) {
    struct ipc_namespace *ns = mqueuefs_ns(mount);
    lock(&ns->mq_lock, 0);
    struct mqueue *q = mq_lookup_locked(ns, name);
    if (q != NULL)
        mq_retain(q);
    unlock(&ns->mq_lock);
    return q;
}

static int mqueuefs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    const char *name = mqueuefs_name(path);
    if (name == NULL) {
        mqueuefs_root_stat(stat);
        return 0;
    }
    struct mqueue *q = mqueuefs_lookup(mount, name);
    if (q == NULL)
        return _ENOENT;
    mq_stat(q, stat);
    mq_release(q);
    return 0;
}

static int mqueuefs_fstat(struct fd *fd, struct statbuf *stat) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL) {
        mqueuefs_root_stat(stat);
        return 0;
    }
    mq_stat(q, stat);
    return 0;
}

static struct fd *mqueuefs_open(struct mount *mount, const char *path, int flags, int mode) {
    const char *name = mqueuefs_name(path);
    if (name == NULL) {
        struct fd *fd = fd_create(&mqueuefs_dir_fdops);
        if (fd == NULL)
            return ERR_PTR(_ENOMEM);
        fd->fs_data = NULL;
        return fd;
    }
    int err = mq_check_name(name);
    if (err < 0)
        return ERR_PTR(err);
    struct ipc_namespace *ns = mqueuefs_ns(mount);
    lock(&ns->mq_lock, 0);
    struct mqueue *q = mq_lookup_locked(ns, name);
    if (q == NULL) {
        if (!(flags & O_CREAT_)) {
            unlock(&ns->mq_lock);
            return ERR_PTR(_ENOENT);
        }
        err = mq_create_locked(ns, name, (mode_t_) mode, false, 0, 0, &q);
        if (err < 0) {
            unlock(&ns->mq_lock);
            return ERR_PTR(err);
        }
    } else if ((flags & (O_CREAT_ | O_EXCL_)) == (O_CREAT_ | O_EXCL_)) {
        unlock(&ns->mq_lock);
        return ERR_PTR(_EEXIST);
    }
    mq_retain(q);
    unlock(&ns->mq_lock);
    struct fd *fd = mq_fd_new(q, flags, false);
    mq_release(q);
    return fd;
}

static int mqueuefs_unlink(struct mount *mount, const char *path) {
    const char *name = mqueuefs_name(path);
    if (name == NULL)
        return _EISDIR;
    return mq_unlink_name(mqueuefs_ns(mount), name);
}

static int mq_apply_attr(struct mqueue *q, struct attr attr) {
    lock(&q->lock, 0);
    int err = 0;
    switch (attr.type) {
        case attr_uid: q->uid = attr.uid; break;
        case attr_gid: q->gid = attr.gid; break;
        case attr_mode: q->mode = attr.mode & 07777; break;
        // A queue has no size to change; Linux answers 0 and changes nothing.
        case attr_size: break;
        default: err = _EINVAL;
    }
    if (err == 0)
        q->ctime = mq_now();
    unlock(&q->lock);
    return err;
}

static int mqueuefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    const char *name = mqueuefs_name(path);
    if (name == NULL)
        return _EPERM;
    struct mqueue *q = mqueuefs_lookup(mount, name);
    if (q == NULL)
        return _ENOENT;
    int err = mq_apply_attr(q, attr);
    mq_release(q);
    return err;
}

static int mqueuefs_fsetattr(struct fd *fd, struct attr attr) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return _EPERM;
    return mq_apply_attr(q, attr);
}

static int mq_set_times(struct mqueue *q, struct timespec atime, struct timespec mtime) {
    lock(&q->lock, 0);
    q->atime = atime;
    q->mtime = mtime;
    q->ctime = mq_now();
    unlock(&q->lock);
    return 0;
}

static int mqueuefs_utime(struct mount *mount, const char *path, struct timespec atime,
        struct timespec mtime, bool UNUSED(follow_links)) {
    const char *name = mqueuefs_name(path);
    if (name == NULL)
        return 0;
    struct mqueue *q = mqueuefs_lookup(mount, name);
    if (q == NULL)
        return _ENOENT;
    mq_set_times(q, atime, mtime);
    mq_release(q);
    return 0;
}

static int mqueuefs_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL)
        return 0;
    return mq_set_times(q, atime, mtime);
}

static int mqueuefs_getpath(struct fd *fd, char *buf) {
    struct mqueue *q = mq_fd_queue(fd);
    if (q == NULL) {
        buf[0] = '\0';
        return 0;
    }
    snprintf(buf, MAX_PATH + 1, "/%s", q->name);
    return 0;
}

static int mqueuefs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    *stat = (struct statfsbuf) {
        .type = MQUEUE_MAGIC_,
        .bsize = 4096,
        .namelen = MAX_NAME - 1,
        .frsize = 4096,
    };
    return 0;
}

// ".", "..", then the queues in the order they were made. fd->offset is how
// far along that list the next entry is.
static int mqueuefs_readdir(struct fd *fd, struct dir_entry *entry) {
    unsigned long pos = fd->offset;
    if (pos < 2) {
        snprintf(entry->name, sizeof(entry->name), "%s", pos == 0 ? "." : "..");
        entry->inode = 1;
        entry->type = DT_DIR;
        fd->offset = pos + 1;
        return 1;
    }
    struct ipc_namespace *ns = mqueuefs_ns(fd->mount);
    lock(&ns->mq_lock, 0);
    unsigned long i = 2;
    struct mqueue *q;
    int found = 0;
    list_for_each_entry(&ns->mq_queues, q, ns_link) {
        if (i++ < pos)
            continue;
        snprintf(entry->name, sizeof(entry->name), "%s", q->name);
        entry->inode = q->inode + 1;
        entry->type = DT_REG;
        found = 1;
        break;
    }
    unlock(&ns->mq_lock);
    if (found)
        fd->offset = pos + 1;
    return found;
}

static off_t_ mqueuefs_dir_lseek(struct fd *fd, off_t_ off, int whence) {
    if (whence != LSEEK_SET || off < 0)
        return _EINVAL;
    fd->offset = (unsigned long) off;
    return off;
}

// A queue file has no write method, so Linux's vfs_write refuses a write(2)
// or pwrite(2) with EINVAL -- after refusing one on a descriptor not open for
// writing with EBADF, which it checks first. Without these AOK answered EBADF
// for both.
static ssize_t mq_write(struct fd *fd, const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return (fd->flags & O_ACCMODE_) == O_RDONLY_ ? _EBADF : _EINVAL;
}
static ssize_t mq_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t UNUSED(off)) {
    return mq_write(fd, buf, bufsize);
}

static const struct fd_ops mq_fdops = {
    .name = "mqueue",
    .read = mq_read,
    .pread = mq_pread,
    .write = mq_write,
    .pwrite = mq_pwrite,
    .lseek = mq_lseek,
    .poll = mq_poll,
    .close = mq_close,
};

static const struct fd_ops mqueuefs_dir_fdops = {
    .name = "mqueue",
    .readdir = mqueuefs_readdir,
    .lseek = mqueuefs_dir_lseek,
};

const struct fs_ops mqueuefs = {
    .name = "mqueue", .magic = MQUEUE_MAGIC_,
    .mount = mqueuefs_mount,
    .umount = mqueuefs_umount,
    .statfs = mqueuefs_statfs,
    .open = mqueuefs_open,
    .stat = mqueuefs_stat,
    .fstat = mqueuefs_fstat,
    .setattr = mqueuefs_setattr,
    .fsetattr = mqueuefs_fsetattr,
    .utime = mqueuefs_utime,
    .futime = mqueuefs_futime,
    .getpath = mqueuefs_getpath,
    .unlink = mqueuefs_unlink,
    .close = mq_close,
};

static struct mount mqueue_internal_mount = {
    .fs = &mqueuefs,
    .point = "",
    .refcount = 1,
};

// /proc/sys/fs/mqueue: the calling task's namespace's limits.
unsigned *mqueue_sysctl(const char *name) {
    struct ipc_namespace *ns = ipc_ns_current();
    if (strcmp(name, "queues_max") == 0) return &ns->mq_queues_max;
    if (strcmp(name, "msg_max") == 0) return &ns->mq_msg_max;
    if (strcmp(name, "msgsize_max") == 0) return &ns->mq_msgsize_max;
    if (strcmp(name, "msg_default") == 0) return &ns->mq_msg_default;
    if (strcmp(name, "msgsize_default") == 0) return &ns->mq_msgsize_default;
    return NULL;
}
