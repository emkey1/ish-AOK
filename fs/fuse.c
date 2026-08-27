// FUSE for iSH-AOK: the /dev/fuse character device and the "fuse" filesystem
// type, speaking FUSE protocol 7.31 to an ordinary guest daemon (libfuse2/3,
// sshfs, rclone, ...). The daemon opens /dev/fuse -- each open is one
// connection -- and calls mount(2) with fd=<n> in the data string; every path
// operation on the mount then becomes a request the daemon reads from that fd
// and answers with a write.
//
// AOK's VFS is path-based while the FUSE protocol is nodeid-based, so each
// operation resolves its path with a FUSE_LOOKUP walk from the root node.
// Nodeids are not cached and FUSE_FORGET is never sent: repeated lookups of
// the same name bump the daemon's per-node lookup count (a u64) on the same
// node, so daemon-side memory is bounded by the tree the guest has touched,
// not by the number of operations.
//
// Every request wait is interruptible; an interrupted (or fire-and-forget)
// request is marked abandoned and freed by whichever side touches it last.
// The daemon dying with requests in flight fails them all with ENOTCONN, and
// the mount then answers every operation with ENOTCONN until it is unmounted
// -- the same "Transport endpoint is not connected" behavior as real Linux.
//
// fusefs is a may_block filesystem (kernel/fs.h): generic.c and stat.c drop
// inodes_lock across its calls, because a request blocks on a userspace
// daemon whose own filesystem work may need that lock -- holding it here is
// the classic FUSE deadlock.
//
// Not yet modeled: mmap (so no exec from a fuse mount), FUSE_INTERRUPT,
// FUSE_FORGET, readdirplus, splice, and the fd-passing new-mount API.
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/poll.h"
#include "fs/stat.h"
#include "util/list.h"
#include "util/sync.h"

// -- wire protocol (linux/fuse.h ABI, fixed layout on every guest arch) --

#define FUSE_KERNEL_VERSION 7
#define FUSE_KERNEL_MINOR_VERSION 31
#define FUSE_ROOT_ID 1
#define FUSE_SUPER_MAGIC 0x65735546

#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_GETATTR 3
#define FUSE_SETATTR 4
#define FUSE_READLINK 5
#define FUSE_SYMLINK 6
#define FUSE_MKNOD 8
#define FUSE_MKDIR 9
#define FUSE_UNLINK 10
#define FUSE_RMDIR 11
#define FUSE_RENAME 12
#define FUSE_LINK 13
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_WRITE 16
#define FUSE_STATFS 17
#define FUSE_RELEASE 18
#define FUSE_FSYNC 20
#define FUSE_FLUSH 25
#define FUSE_INIT 26
#define FUSE_OPENDIR 27
#define FUSE_READDIR 28
#define FUSE_RELEASEDIR 29
#define FUSE_FSYNCDIR 30
#define FUSE_CREATE 35
#define FUSE_DESTROY 38

#define FATTR_MODE (1 << 0)
#define FATTR_UID (1 << 1)
#define FATTR_GID (1 << 2)
#define FATTR_SIZE (1 << 3)
#define FATTR_ATIME (1 << 4)
#define FATTR_MTIME (1 << 5)
#define FATTR_FH (1 << 6)

#define FUSE_GETATTR_FH (1 << 0)

struct fuse_in_header {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
};

struct fuse_out_header {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};

struct fuse_wire_attr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
    uint32_t padding;
};

struct fuse_entry_out {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    struct fuse_wire_attr attr;
};

struct fuse_init_in {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
};

struct fuse_init_out {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
    uint16_t max_background;
    uint16_t congestion_threshold;
    uint32_t max_write;
    uint32_t time_gran;
    uint16_t max_pages;
    uint16_t map_alignment;
    uint32_t unused[8];
};

struct fuse_getattr_in {
    uint32_t getattr_flags;
    uint32_t dummy;
    uint64_t fh;
};

struct fuse_attr_out {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec;
    uint32_t dummy;
    struct fuse_wire_attr attr;
};

struct fuse_setattr_in {
    uint32_t valid;
    uint32_t padding;
    uint64_t fh;
    uint64_t size;
    uint64_t lock_owner;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t unused4;
    uint32_t uid;
    uint32_t gid;
    uint32_t unused5;
};

struct fuse_open_in {
    uint32_t flags;
    uint32_t open_flags;
};

struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    uint32_t padding;
};

struct fuse_create_in {
    uint32_t flags;
    uint32_t mode;
    uint32_t umask;
    uint32_t padding;
};

struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_write_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t write_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_write_out {
    uint32_t size;
    uint32_t padding;
};

struct fuse_release_in {
    uint64_t fh;
    uint32_t flags;
    uint32_t release_flags;
    uint64_t lock_owner;
};

struct fuse_flush_in {
    uint64_t fh;
    uint32_t unused;
    uint32_t padding;
    uint64_t lock_owner;
};

struct fuse_fsync_in {
    uint64_t fh;
    uint32_t fsync_flags;
    uint32_t padding;
};

struct fuse_mkdir_in {
    uint32_t mode;
    uint32_t umask;
};

struct fuse_mknod_in {
    uint32_t mode;
    uint32_t rdev;
    uint32_t umask;
    uint32_t padding;
};

struct fuse_rename_in {
    uint64_t newdir;
};

struct fuse_link_in {
    uint64_t oldnodeid;
};

struct fuse_kstatfs {
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint32_t bsize;
    uint32_t namelen;
    uint32_t frsize;
    uint32_t padding;
    uint32_t spare[6];
};

struct fuse_dirent {
    uint64_t ino;
    uint64_t off;
    uint32_t namelen;
    uint32_t type;
    // char name[]; padded so the next fuse_dirent is 8-byte aligned
};
#define FUSE_DIRENT_ALIGN(x) (((x) + 7) & ~7ul)

// Without FUSE_MAX_PAGES negotiation the protocol caps a request's payload at
// 32 pages; stay at that for reads, and at min(this, daemon max_write) for
// writes.
#define FUSE_MAX_TRANSFER (128 * 1024)
#define FUSE_READDIR_CHUNK 8192

// -- connection --

struct fuse_conn {
    atomic_uint refcount;
    lock_t lock;        // protects everything below
    cond_t cond;        // wakes the daemon's read and every reply waiter
    struct list pending;     // queued for the daemon to read
    struct list processing;  // read by the daemon, awaiting its reply
    uint64_t next_unique;
    bool initialized;   // the FUSE_INIT reply has arrived
    bool dead;          // daemon fd closed, INIT refused, or unmounted
    uint32_t max_write; // from the daemon's init reply

    // The /dev/fuse fd for poll wakeups. Its own lock so a wakeup (which
    // takes poll locks) never runs under ->lock; never held nested with it.
    lock_t dev_fd_lock;
    struct fd *dev_fd;
};

struct fuse_req {
    struct list queue;  // on pending or processing, or detached (list_null)
    uint64_t unique;
    uint32_t opcode;
    char *in;           // fuse_in_header + args, contiguous
    size_t in_len;
    char *out;          // reply body copy, owned by the waiter once answered
    size_t out_len;
    int err;            // the reply header's (negative) error
    bool answered;
    bool abandoned;     // nobody will collect the reply; free on completion
    bool is_init;
};

// Refcount is atomic: retain/release need no lock, and release must be called
// WITHOUT conn->lock held -- dropping the last reference frees the lock too.
static void fuse_conn_retain(struct fuse_conn *conn) {
    conn->refcount++;
}

static void fuse_req_free(struct fuse_req *req) {
    free(req->in);
    free(req->out);
    free(req);
}

static void fuse_conn_release(struct fuse_conn *conn) {
    if (--conn->refcount > 0)
        return;
    // Both holders (device fd and mount) are gone, so nothing can be waiting
    // on any request: every remaining one is abandoned or never-collected.
    struct fuse_req *req, *tmp;
    list_for_each_entry_safe(&conn->pending, req, tmp, queue) {
        list_remove(&req->queue);
        fuse_req_free(req);
    }
    list_for_each_entry_safe(&conn->processing, req, tmp, queue) {
        list_remove(&req->queue);
        fuse_req_free(req);
    }
    cond_destroy(&conn->cond);
    free(conn);
}

// Wake a daemon blocked in poll/select on /dev/fuse. Called without ->lock.
static void fuse_conn_wake_daemon(struct fuse_conn *conn, int events) {
    lock(&conn->dev_fd_lock, 0);
    if (conn->dev_fd != NULL)
        poll_wakeup(conn->dev_fd, events);
    unlock(&conn->dev_fd_lock);
}

// End the connection: fail present and future requests, wake everyone.
// Called without ->lock.
static void fuse_conn_kill(struct fuse_conn *conn) {
    lock(&conn->lock, 0);
    conn->dead = true;
    // Reply waiters see ->dead and clean up their own requests; abandoned
    // ones have no owner left, so sweep them here.
    struct fuse_req *req, *tmp;
    list_for_each_entry_safe(&conn->pending, req, tmp, queue) {
        if (req->abandoned || req->is_init) {
            list_remove(&req->queue);
            fuse_req_free(req);
        }
    }
    list_for_each_entry_safe(&conn->processing, req, tmp, queue) {
        if (req->abandoned || req->is_init) {
            list_remove(&req->queue);
            fuse_req_free(req);
        }
    }
    notify(&conn->cond);
    unlock(&conn->lock);
    fuse_conn_wake_daemon(conn, POLL_READ | POLL_HUP);
}

// Requests the daemon must not answer; freed as soon as it has read them.
static bool fuse_opcode_no_reply(uint32_t opcode) {
    return opcode == FUSE_FORGET || opcode == FUSE_DESTROY;
}

// Build and enqueue one request. With `wait`, blocks (interruptibly) for the
// reply: on success the reply body is left in *out/*out_len (caller frees;
// pass NULL to discard) and the daemon's error comes back as the negative
// return. Without `wait`, the request is fire-and-forget.
static int fuse_conn_call(struct fuse_conn *conn, uint32_t opcode, uint64_t nodeid,
        const void *arg1, size_t len1, const void *arg2, size_t len2,
        void **out, size_t *out_len, bool wait) {
    size_t in_len = sizeof(struct fuse_in_header) + len1 + len2;
    struct fuse_req *req = calloc(1, sizeof(*req));
    if (req == NULL)
        return _ENOMEM;
    req->in = malloc(in_len);
    if (req->in == NULL) {
        free(req);
        return _ENOMEM;
    }
    struct fuse_in_header *hdr = (struct fuse_in_header *) req->in;
    *hdr = (struct fuse_in_header) {
        .len = (uint32_t) in_len,
        .opcode = opcode,
        .nodeid = nodeid,
        .uid = current != NULL ? current->fsuid : 0,
        .gid = current != NULL ? current->fsgid : 0,
        .pid = current != NULL ? (uint32_t) current->pid : 0,
    };
    if (len1 > 0)
        memcpy(req->in + sizeof(*hdr), arg1, len1);
    if (len2 > 0)
        memcpy(req->in + sizeof(*hdr) + len1, arg2, len2);
    req->in_len = in_len;
    req->opcode = opcode;
    req->is_init = opcode == FUSE_INIT;
    req->abandoned = !wait && !req->is_init;
    list_init(&req->queue);

    lock(&conn->lock, 0);
    // Everything except INIT itself queues behind a completed handshake, so
    // the daemon never sees a request it hasn't negotiated struct sizes for.
    if (wait && opcode != FUSE_INIT) {
        while (!conn->initialized && !conn->dead) {
            if (wait_for(&conn->cond, &conn->lock, NULL)) {
                unlock(&conn->lock);
                fuse_req_free(req);
                return _EINTR;
            }
        }
    }
    if (conn->dead) {
        unlock(&conn->lock);
        fuse_req_free(req);
        return _ENOTCONN;
    }
    req->unique = ++conn->next_unique;
    hdr->unique = req->unique;
    list_add_tail(&conn->pending, &req->queue);
    notify(&conn->cond);
    unlock(&conn->lock);
    fuse_conn_wake_daemon(conn, POLL_READ);
    if (!wait)
        return 0;

    lock(&conn->lock, 0);
    while (!req->answered && !conn->dead) {
        if (wait_for(&conn->cond, &conn->lock, NULL)) {
            // Interrupted. If the daemon hasn't read it yet it can be pulled
            // back; once read, the eventual reply (or conn death) frees it.
            if (!list_null(&req->queue) && !req->answered) {
                req->abandoned = true;
                unlock(&conn->lock);
                return _EINTR;
            }
            // Answered in the race window: fall through and use the reply.
            break;
        }
    }
    if (!req->answered) {
        // Connection died. The request may still be on a list; detach it.
        list_remove_safe(&req->queue);
        unlock(&conn->lock);
        fuse_req_free(req);
        return _ENOTCONN;
    }
    int err = req->err;
    if (err == 0 && out != NULL) {
        *out = req->out;
        if (out_len != NULL)
            *out_len = req->out_len;
        req->out = NULL;
    }
    unlock(&conn->lock);
    fuse_req_free(req);
    return err;
}

// -- /dev/fuse --

static ssize_t fuse_dev_read(struct fd *fd, void *buf, size_t bufsize) {
    struct fuse_conn *conn = fd->data;
    lock(&conn->lock, 0);
    struct fuse_req *req;
    for (;;) {
        if (!list_empty(&conn->pending)) {
            req = list_first_entry(&conn->pending, struct fuse_req, queue);
            break;
        }
        if (conn->dead) {
            unlock(&conn->lock);
            return _ENODEV; // aborted/unmounted: tells libfuse's loop to exit
        }
        if (fd->flags & O_NONBLOCK_) {
            unlock(&conn->lock);
            return _EAGAIN;
        }
        if (wait_for(&conn->cond, &conn->lock, NULL)) {
            unlock(&conn->lock);
            return _EINTR;
        }
    }
    if (bufsize < req->in_len) {
        // The daemon's buffer must fit any request whole (this is EINVAL by
        // protocol; libfuse sizes its buffer from max_write and never hits it).
        unlock(&conn->lock);
        return _EINVAL;
    }
    size_t len = req->in_len;
    memcpy(buf, req->in, len);
    list_remove(&req->queue);
    if (fuse_opcode_no_reply(req->opcode)) {
        fuse_req_free(req);
    } else {
        list_init(&req->queue);
        list_add_tail(&conn->processing, &req->queue);
    }
    unlock(&conn->lock);
    return (ssize_t) len;
}

// Parse the daemon's FUSE_INIT reply. Called with conn->lock held.
static void fuse_conn_handle_init(struct fuse_conn *conn, struct fuse_req *req) {
    if (req->err < 0 || req->out_len < 8) {
        conn->dead = true;
        return;
    }
    struct fuse_init_out init;
    memset(&init, 0, sizeof(init));
    memcpy(&init, req->out, req->out_len < sizeof(init) ? req->out_len : sizeof(init));
    if (init.major != FUSE_KERNEL_VERSION) {
        conn->dead = true;
        return;
    }
    conn->max_write = init.max_write >= 4096 ? init.max_write : 4096;
    if (conn->max_write > FUSE_MAX_TRANSFER)
        conn->max_write = FUSE_MAX_TRANSFER;
    conn->initialized = true;
}

static ssize_t fuse_dev_write(struct fd *fd, const void *buf, size_t bufsize) {
    struct fuse_conn *conn = fd->data;
    struct fuse_out_header hdr;
    if (bufsize < sizeof(hdr))
        return _EINVAL;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.len != bufsize)
        return _EINVAL;
    if (hdr.error > 0 || hdr.error < -4095)
        return _EINVAL;
    if (hdr.error != 0 && bufsize != sizeof(hdr))
        return _EINVAL; // an error reply carries no body

    lock(&conn->lock, 0);
    struct fuse_req *req = NULL, *cur;
    list_for_each_entry(&conn->processing, cur, queue) {
        if (cur->unique == hdr.unique) {
            req = cur;
            break;
        }
    }
    if (req == NULL) {
        // Unknown unique: the waiter was interrupted and its request swept,
        // or the daemon replied twice. Linux answers ENOENT; so do we.
        unlock(&conn->lock);
        return _ENOENT;
    }
    list_remove(&req->queue);
    list_init(&req->queue);
    req->err = hdr.error;
    req->out_len = bufsize - sizeof(hdr);
    if (req->out_len > 0) {
        req->out = malloc(req->out_len);
        if (req->out == NULL) {
            req->err = _ENOMEM;
            req->out_len = 0;
        } else {
            memcpy(req->out, (const char *) buf + sizeof(hdr), req->out_len);
        }
    }
    if (req->is_init) {
        fuse_conn_handle_init(conn, req);
        fuse_req_free(req);
        notify(&conn->cond);
        unlock(&conn->lock);
        return (ssize_t) bufsize;
    }
    if (req->abandoned) {
        fuse_req_free(req);
        unlock(&conn->lock);
        return (ssize_t) bufsize;
    }
    req->answered = true;
    notify(&conn->cond);
    unlock(&conn->lock);
    return (ssize_t) bufsize;
}

static int fuse_dev_poll(struct fd *fd) {
    struct fuse_conn *conn = fd->data;
    int events = POLL_WRITE;
    lock(&conn->lock, 0);
    if (!list_empty(&conn->pending) || conn->dead)
        events |= POLL_READ;
    if (conn->dead)
        events |= POLL_HUP;
    unlock(&conn->lock);
    return events;
}

static int fuse_dev_close(struct fd *fd) {
    struct fuse_conn *conn = fd->data;
    if (conn == NULL)
        return 0;
    lock(&conn->dev_fd_lock, 0);
    conn->dev_fd = NULL;
    unlock(&conn->dev_fd_lock);
    fuse_conn_kill(conn);
    fuse_conn_release(conn);
    return 0;
}

static int fuse_dev_open_fd(int major, int minor, struct fd *fd) {
    (void) major;
    if (minor != DEV_FUSE_MINOR)
        return _ENXIO;
    struct fuse_conn *conn = calloc(1, sizeof(*conn));
    if (conn == NULL)
        return _ENOMEM;
    conn->refcount = 1; // the device fd's reference
    lock_init(&conn->lock, "fuse_conn\0");
    lock_init(&conn->dev_fd_lock, "fuse_dev_fd\0");
    cond_init(&conn->cond);
    list_init(&conn->pending);
    list_init(&conn->processing);
    conn->max_write = FUSE_MAX_TRANSFER;
    conn->dev_fd = fd;
    fd->data = conn;
    return 0;
}

struct dev_ops fuse_dev = {
    .open = fuse_dev_open_fd,
    .fd = {
        .read = fuse_dev_read,
        .write = fuse_dev_write,
        .poll = fuse_dev_poll,
        .close = fuse_dev_close,
        .anon_inode_class = "[fuse]",
    },
};

// -- the fuse filesystem --

static struct fuse_conn *fuse_mount_conn(struct mount *mount) {
    return mount->data;
}

// Mount data is libfuse's "fd=7,rootmode=40000,user_id=0,group_id=0[,...]".
static int fusefs_mount(struct mount *mount) {
    const char *info = mount->info != NULL ? mount->info : "";
    const char *fdstr = NULL;
    if (strncmp(info, "fd=", 3) == 0)
        fdstr = info + 3;
    else if ((fdstr = strstr(info, ",fd=")) != NULL)
        fdstr += 4;
    if (fdstr == NULL)
        return _EINVAL;
    fd_t f = (fd_t) atoi(fdstr);
    if (current == NULL)
        return _EINVAL; // fd= only means something on a guest task's table
    struct fd *dev = f_get(f);
    if (dev == NULL)
        return _EBADF;
    if (dev->ops != &fuse_dev.fd)
        return _EINVAL;
    struct fuse_conn *conn = dev->data;

    lock(&conn->lock, 0);
    if (conn->dead) {
        unlock(&conn->lock);
        return _ENOTCONN;
    }
    fuse_conn_retain(conn); // the mount's reference
    unlock(&conn->lock);
    mount->data = conn;

    struct fuse_init_in init = {
        .major = FUSE_KERNEL_VERSION,
        .minor = FUSE_KERNEL_MINOR_VERSION,
        .max_readahead = FUSE_MAX_TRANSFER,
        .flags = 0,
    };
    // Fire-and-collect: the daemon can only read this after mount(2) returns,
    // so the reply is parsed by the device write path and the first real
    // operation waits on conn->initialized.
    int err = fuse_conn_call(conn, FUSE_INIT, 0, &init, sizeof(init), NULL, 0,
            NULL, NULL, false);
    if (err < 0) {
        fuse_conn_release(conn);
        mount->data = NULL;
        return err;
    }
    return 0;
}

// Runs under mounts_lock (mount_remove), so nothing here may wait on the
// daemon: tell it the session is over and cut every request loose.
static int fusefs_umount(struct mount *mount) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return 0;
    fuse_conn_call(conn, FUSE_DESTROY, 0, NULL, 0, NULL, 0, NULL, NULL, false);
    fuse_conn_kill(conn);
    fuse_conn_release(conn);
    mount->data = NULL;
    return 0;
}

static void fuse_attr_to_statbuf(const struct fuse_wire_attr *attr, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->inode = attr->ino;
    stat->mode = attr->mode;
    stat->nlink = attr->nlink;
    stat->uid = attr->uid;
    stat->gid = attr->gid;
    stat->rdev = attr->rdev;
    stat->size = attr->size;
    stat->blksize = attr->blksize != 0 ? attr->blksize : 4096;
    stat->blocks = attr->blocks;
    stat->atime = (dword_t) attr->atime;
    stat->atime_nsec = attr->atimensec;
    stat->mtime = (dword_t) attr->mtime;
    stat->mtime_nsec = attr->mtimensec;
    stat->ctime = (dword_t) attr->ctime;
    stat->ctime_nsec = attr->ctimensec;
}

static int fuse_getattr(struct fuse_conn *conn, uint64_t nodeid, bool fh_valid,
        uint64_t fh, struct fuse_wire_attr *attr_out) {
    struct fuse_getattr_in in = {
        .getattr_flags = fh_valid ? FUSE_GETATTR_FH : 0,
        .fh = fh,
    };
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_GETATTR, nodeid, &in, sizeof(in),
            NULL, 0, &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len < sizeof(struct fuse_attr_out)) {
        free(out);
        return _EIO;
    }
    *attr_out = ((struct fuse_attr_out *) out)->attr;
    free(out);
    return 0;
}

// One FUSE_LOOKUP step: parent nodeid + name -> child entry.
static int fuse_lookup(struct fuse_conn *conn, uint64_t parent, const char *name,
        struct fuse_entry_out *entry_out) {
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_LOOKUP, parent, name, strlen(name) + 1,
            NULL, 0, &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len < sizeof(struct fuse_entry_out)) {
        free(out);
        return _EIO;
    }
    *entry_out = *(struct fuse_entry_out *) out;
    free(out);
    if (entry_out->nodeid == 0)
        return _ENOENT; // a cached-negative entry: same answer as an error
    return 0;
}

// Resolve a mount-relative path ("" or "/a/b") to its nodeid by walking
// FUSE_LOOKUP component by component from the root. attr_out (optional)
// receives the final node's attributes.
static int fuse_resolve(struct fuse_conn *conn, const char *path,
        uint64_t *nodeid_out, struct fuse_wire_attr *attr_out) {
    uint64_t nodeid = FUSE_ROOT_ID;
    bool have_attr = false;
    struct fuse_entry_out entry;
    const char *p = path;
    char name[NAME_MAX + 1];
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        size_t n = strcspn(p, "/");
        if (n > NAME_MAX)
            return _ENAMETOOLONG;
        memcpy(name, p, n);
        name[n] = '\0';
        p += n;
        int err = fuse_lookup(conn, nodeid, name, &entry);
        if (err < 0)
            return err;
        nodeid = entry.nodeid;
        have_attr = true;
    }
    if (nodeid_out != NULL)
        *nodeid_out = nodeid;
    if (attr_out != NULL) {
        if (have_attr) {
            *attr_out = entry.attr;
        } else {
            int err = fuse_getattr(conn, nodeid, false, 0, attr_out);
            if (err < 0)
                return err;
        }
    }
    return 0;
}

// Resolve a path's parent directory nodeid and point name_out at the final
// component (inside `path`, which must outlive the use).
static int fuse_resolve_parent(struct fuse_conn *conn, const char *path,
        uint64_t *dir_out, const char **name_out) {
    const char *last = strrchr(path, '/');
    if (last == NULL) {
        // "" or a bare name: the parent is the root
        *dir_out = FUSE_ROOT_ID;
        *name_out = path;
        if (path[0] == '\0')
            return _EINVAL; // the root has no parent to mutate
        return 0;
    }
    const char *name = last + 1;
    if (name[0] == '\0')
        return _EINVAL;
    char dir[MAX_PATH];
    size_t dir_len = (size_t) (last - path);
    if (dir_len >= sizeof(dir))
        return _ENAMETOOLONG;
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';
    int err = fuse_resolve(conn, dir, dir_out, NULL);
    if (err < 0)
        return err;
    *name_out = name;
    return 0;
}

static int fusefs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    struct fuse_wire_attr attr;
    int err = fuse_resolve(conn, path, NULL, &attr);
    if (err < 0)
        return err;
    fuse_attr_to_statbuf(&attr, stat);
    return 0;
}

// -- fuse file fds --

struct fuse_file {
    uint64_t nodeid;
    uint64_t fh;
    bool fh_valid;
    bool is_dir;
    char *path; // mount-relative, for getpath
    // readdir stream state, guarded by fd->lock
    char *dirbuf;
    size_t dirbuf_len, dirbuf_pos;
    uint64_t dir_off; // FUSE offset for the next READDIR request
    bool dir_eof;
};

// The open flags forwarded to the daemon. AOK's internal flag encoding uses
// the x86 Linux values while the daemon may be an arm64/riscv64 binary whose
// libc disagrees on the rare bits (O_DIRECTORY and friends), so only the
// universally-identical bits that daemons actually act on are forwarded.
static uint32_t fuse_open_flags(int flags) {
    return (uint32_t) (flags & (O_ACCMODE_ | O_APPEND_ | O_NONBLOCK_));
}

static ssize_t fuse_fd_do_read(struct fd *fd, void *buf, size_t bufsize, uint64_t off) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    if (bufsize > FUSE_MAX_TRANSFER)
        bufsize = FUSE_MAX_TRANSFER;
    struct fuse_read_in in = {
        .fh = file->fh,
        .offset = off,
        .size = (uint32_t) bufsize,
    };
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_READ, file->nodeid, &in, sizeof(in),
            NULL, 0, &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len > bufsize)
        out_len = bufsize;
    if (out_len > 0)
        memcpy(buf, out, out_len);
    free(out);
    return (ssize_t) out_len;
}

static ssize_t fuse_fd_do_write(struct fd *fd, const void *buf, size_t bufsize, uint64_t off) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    if (bufsize > conn->max_write)
        bufsize = conn->max_write;
    struct fuse_write_in in = {
        .fh = file->fh,
        .offset = off,
        .size = (uint32_t) bufsize,
    };
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_WRITE, file->nodeid, &in, sizeof(in),
            buf, bufsize, &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len < sizeof(struct fuse_write_out)) {
        free(out);
        return _EIO;
    }
    ssize_t written = (ssize_t) ((struct fuse_write_out *) out)->size;
    free(out);
    if (written < 0 || (size_t) written > bufsize)
        return _EIO;
    return written;
}

// O_APPEND is honored here rather than trusting the daemon's backing fd:
// find the current size and write there.
static uint64_t fuse_fd_write_offset(struct fd *fd) {
    struct fuse_file *file = fd->data;
    if (fd->flags & O_APPEND_) {
        struct fuse_conn *conn = fuse_mount_conn(fd->mount);
        struct fuse_wire_attr attr;
        if (conn != NULL &&
                fuse_getattr(conn, file->nodeid, file->fh_valid, file->fh, &attr) == 0)
            return attr.size;
    }
    return fd->offset;
}

static ssize_t fusefs_fd_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t n = fuse_fd_do_read(fd, buf, bufsize, fd->offset);
    if (n > 0)
        fd->offset += (unsigned long) n;
    return n;
}

static ssize_t fusefs_fd_write(struct fd *fd, const void *buf, size_t bufsize) {
    uint64_t off = fuse_fd_write_offset(fd);
    ssize_t n = fuse_fd_do_write(fd, buf, bufsize, off);
    if (n > 0)
        fd->offset = (unsigned long) (off + (uint64_t) n);
    return n;
}

static ssize_t fusefs_fd_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    return fuse_fd_do_read(fd, buf, bufsize, (uint64_t) off);
}

static ssize_t fusefs_fd_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    return fuse_fd_do_write(fd, buf, bufsize, (uint64_t) off);
}

static off_t_ fusefs_fd_lseek(struct fd *fd, off_t_ off, int whence) {
    struct fuse_file *file = fd->data;
    off_t_ base;
    switch (whence) {
        case LSEEK_SET:
            base = 0;
            break;
        case LSEEK_CUR:
            base = (off_t_) fd->offset;
            break;
        case LSEEK_END: {
            struct fuse_conn *conn = fuse_mount_conn(fd->mount);
            if (conn == NULL)
                return _ENOTCONN;
            struct fuse_wire_attr attr;
            int err = fuse_getattr(conn, file->nodeid, file->fh_valid, file->fh, &attr);
            if (err < 0)
                return err;
            base = (off_t_) attr.size;
            break;
        }
        default:
            return _EINVAL;
    }
    off_t_ pos = base + off;
    if (pos < 0)
        return _EINVAL;
    fd->offset = (unsigned long) pos;
    return pos;
}

static int fusefs_fd_readdir(struct fd *fd, struct dir_entry *entry) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    if (!file->is_dir)
        return _ENOTDIR;

    lock(&fd->lock, 0);
    int res;
    for (;;) {
        // Serve from the buffered chunk first.
        while (file->dirbuf != NULL &&
                file->dirbuf_pos + sizeof(struct fuse_dirent) <= file->dirbuf_len) {
            struct fuse_dirent de;
            memcpy(&de, file->dirbuf + file->dirbuf_pos, sizeof(de));
            size_t rec = sizeof(de) + de.namelen;
            if (de.namelen == 0 || file->dirbuf_pos + rec > file->dirbuf_len) {
                res = _EIO; // truncated or corrupt stream from the daemon
                goto out;
            }
            const char *name = file->dirbuf + file->dirbuf_pos + sizeof(de);
            file->dirbuf_pos += FUSE_DIRENT_ALIGN(rec);
            file->dir_off = de.off;
            if (de.namelen > NAME_MAX)
                continue; // unrepresentable; skip rather than truncate
            entry->inode = de.ino;
            entry->type = (byte_t) de.type;
            memcpy(entry->name, name, de.namelen);
            entry->name[de.namelen] = '\0';
            res = 1;
            goto out;
        }
        if (file->dir_eof) {
            res = 0;
            goto out;
        }
        // Fetch the next chunk.
        struct fuse_read_in in = {
            .fh = file->fh,
            .offset = file->dir_off,
            .size = FUSE_READDIR_CHUNK,
        };
        void *out = NULL;
        size_t out_len = 0;
        int err = fuse_conn_call(conn, FUSE_READDIR, file->nodeid, &in, sizeof(in),
                NULL, 0, &out, &out_len, true);
        if (err < 0) {
            res = err;
            goto out;
        }
        free(file->dirbuf);
        file->dirbuf = out;
        file->dirbuf_len = out_len;
        file->dirbuf_pos = 0;
        if (out_len == 0)
            file->dir_eof = true;
    }
out:
    unlock(&fd->lock);
    return res;
}

static unsigned long fusefs_fd_telldir(struct fd *fd) {
    struct fuse_file *file = fd->data;
    return (unsigned long) file->dir_off;
}

static void fusefs_fd_seekdir(struct fd *fd, unsigned long ptr) {
    struct fuse_file *file = fd->data;
    lock(&fd->lock, 0);
    // Only a rewind (or a seek back to an offset the daemon handed out) is
    // representable: drop the buffered chunk and restart the stream there.
    free(file->dirbuf);
    file->dirbuf = NULL;
    file->dirbuf_len = file->dirbuf_pos = 0;
    file->dir_off = ptr;
    file->dir_eof = false;
    unlock(&fd->lock);
}

static int fusefs_fd_fsync(struct fd *fd) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    if (!file->fh_valid)
        return 0;
    struct fuse_fsync_in in = {.fh = file->fh};
    int err = fuse_conn_call(conn, file->is_dir ? FUSE_FSYNCDIR : FUSE_FSYNC,
            file->nodeid, &in, sizeof(in), NULL, 0, NULL, NULL, true);
    // Daemons commonly leave fsync unimplemented; that is not a data error.
    if (err == _ENOSYS)
        err = 0;
    return err;
}

static int fusefs_close(struct fd *fd) {
    struct fuse_file *file = fd->data;
    if (file == NULL)
        return 0;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn != NULL && file->fh_valid) {
        if (!file->is_dir) {
            // FLUSH is how a daemon reports close-time write errors (sshfs);
            // wait for it, interruptibly. RELEASE is fire-and-forget so a
            // wedged daemon can never make close hang.
            struct fuse_flush_in flush = {.fh = file->fh};
            fuse_conn_call(conn, FUSE_FLUSH, file->nodeid, &flush, sizeof(flush),
                    NULL, 0, NULL, NULL, true);
        }
        struct fuse_release_in release = {
            .fh = file->fh,
            .flags = fuse_open_flags(fd->flags),
        };
        fuse_conn_call(conn, file->is_dir ? FUSE_RELEASEDIR : FUSE_RELEASE,
                file->nodeid, &release, sizeof(release), NULL, 0, NULL, NULL, false);
    }
    free(file->path);
    free(file->dirbuf);
    free(file);
    fd->data = NULL;
    return 0;
}

static int fusefs_fstat(struct fd *fd, struct statbuf *stat) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    struct fuse_wire_attr attr;
    int err = fuse_getattr(conn, file->nodeid, file->fh_valid, file->fh, &attr);
    if (err < 0)
        return err;
    fuse_attr_to_statbuf(&attr, stat);
    return 0;
}

static int fuse_setattr_common(struct fuse_conn *conn, uint64_t nodeid,
        const struct fuse_setattr_in *in) {
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_SETATTR, nodeid, in, sizeof(*in),
            NULL, 0, &out, &out_len, true);
    free(out);
    return err;
}

static int fuse_setattr_from_attr(struct fuse_conn *conn, uint64_t nodeid,
        bool fh_valid, uint64_t fh, struct attr attr) {
    struct fuse_setattr_in in = {0};
    if (fh_valid) {
        in.valid |= FATTR_FH;
        in.fh = fh;
    }
    switch (attr.type) {
        case attr_uid:
            in.valid |= FATTR_UID;
            in.uid = attr.uid;
            break;
        case attr_gid:
            in.valid |= FATTR_GID;
            in.gid = attr.gid;
            break;
        case attr_mode:
            in.valid |= FATTR_MODE;
            in.mode = attr.mode;
            break;
        case attr_size:
            in.valid |= FATTR_SIZE;
            in.size = (uint64_t) attr.size;
            break;
        default:
            return _EINVAL;
    }
    return fuse_setattr_common(conn, nodeid, &in);
}

static int fusefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t nodeid;
    int err = fuse_resolve(conn, path, &nodeid, NULL);
    if (err < 0)
        return err;
    return fuse_setattr_from_attr(conn, nodeid, false, 0, attr);
}

static int fusefs_fsetattr(struct fd *fd, struct attr attr) {
    struct fuse_file *file = fd->data;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL)
        return _ENOTCONN;
    return fuse_setattr_from_attr(conn, file->nodeid, file->fh_valid, file->fh, attr);
}

static int fusefs_utime(struct mount *mount, const char *path,
        struct timespec atime, struct timespec mtime, bool follow_links) {
    (void) follow_links; // path resolution already followed what it should
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t nodeid;
    int err = fuse_resolve(conn, path, &nodeid, NULL);
    if (err < 0)
        return err;
    struct fuse_setattr_in in = {
        .valid = FATTR_ATIME | FATTR_MTIME,
        .atime = (uint64_t) atime.tv_sec,
        .atimensec = (uint32_t) atime.tv_nsec,
        .mtime = (uint64_t) mtime.tv_sec,
        .mtimensec = (uint32_t) mtime.tv_nsec,
    };
    return fuse_setattr_common(conn, nodeid, &in);
}

static int fusefs_getpath(struct fd *fd, char *buf) {
    struct fuse_file *file = fd->data;
    if (file == NULL || file->path == NULL)
        return _EIO;
    if (strlen(file->path) >= MAX_PATH)
        return _ENAMETOOLONG;
    strcpy(buf, file->path);
    return 0;
}

static const struct fd_ops fusefs_fd_ops = {
    .read = fusefs_fd_read,
    .write = fusefs_fd_write,
    .pread = fusefs_fd_pread,
    .pwrite = fusefs_fd_pwrite,
    .lseek = fusefs_fd_lseek,
    .readdir = fusefs_fd_readdir,
    .telldir = fusefs_fd_telldir,
    .seekdir = fusefs_fd_seekdir,
    .fsync = fusefs_fd_fsync,
    .close = fusefs_close,
};

static struct fd *fusefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return ERR_PTR(_ENOTCONN);

    struct fuse_wire_attr attr;
    uint64_t nodeid = 0;
    bool created = false;
    uint64_t fh = 0;
    bool fh_valid = false;

    int err = fuse_resolve(conn, path, &nodeid, &attr);
    if (err == _ENOENT && (flags & O_CREAT_)) {
        uint64_t dir;
        const char *name;
        err = fuse_resolve_parent(conn, path, &dir, &name);
        if (err < 0)
            return ERR_PTR(err);
        struct fuse_create_in in = {
            .flags = fuse_open_flags(flags),
            .mode = (uint32_t) ((mode & 07777) | S_IFREG),
        };
        void *out = NULL;
        size_t out_len = 0;
        err = fuse_conn_call(conn, FUSE_CREATE, dir, &in, sizeof(in),
                name, strlen(name) + 1, &out, &out_len, true);
        if (err == 0) {
            if (out_len < sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out)) {
                free(out);
                return ERR_PTR(_EIO);
            }
            struct fuse_entry_out *entry = out;
            struct fuse_open_out *open_out =
                (struct fuse_open_out *) ((char *) out + sizeof(*entry));
            nodeid = entry->nodeid;
            attr = entry->attr;
            fh = open_out->fh;
            fh_valid = true;
            created = true;
            free(out);
        } else if (err == _ENOSYS) {
            // Old daemons without CREATE: MKNOD then fall through to OPEN.
            struct fuse_mknod_in mk = {
                .mode = (uint32_t) ((mode & 07777) | S_IFREG),
            };
            void *mkout = NULL;
            err = fuse_conn_call(conn, FUSE_MKNOD, dir, &mk, sizeof(mk),
                    name, strlen(name) + 1, &mkout, NULL, true);
            free(mkout);
            if (err < 0)
                return ERR_PTR(err);
            err = fuse_resolve(conn, path, &nodeid, &attr);
            if (err < 0)
                return ERR_PTR(err);
        } else {
            return ERR_PTR(err);
        }
    } else if (err == 0 && (flags & O_CREAT_) && (flags & O_EXCL_)) {
        return ERR_PTR(_EEXIST);
    } else if (err < 0) {
        return ERR_PTR(err);
    }

    bool is_dir = S_ISDIR(attr.mode);
    if (!fh_valid && (is_dir || S_ISREG(attr.mode) || S_ISLNK(attr.mode))) {
        // Devices and FIFOs get their real ops from dev_open/fifo machinery in
        // generic_openat; asking the daemon to open them would be wrong.
        if (S_ISREG(attr.mode) && (flags & O_TRUNC_) &&
                (flags & O_ACCMODE_) != O_RDONLY_) {
            // No FUSE_ATOMIC_O_TRUNC was negotiated, so truncate up front and
            // strip the flag, like Linux does in that mode.
            struct fuse_setattr_in trunc = {.valid = FATTR_SIZE, .size = 0};
            err = fuse_setattr_common(conn, nodeid, &trunc);
            if (err < 0)
                return ERR_PTR(err);
        }
        struct fuse_open_in in = {
            .flags = is_dir ? O_RDONLY_ : fuse_open_flags(flags),
        };
        void *out = NULL;
        size_t out_len = 0;
        err = fuse_conn_call(conn, is_dir ? FUSE_OPENDIR : FUSE_OPEN, nodeid,
                &in, sizeof(in), NULL, 0, &out, &out_len, true);
        if (err == 0) {
            if (out_len < sizeof(struct fuse_open_out)) {
                free(out);
                return ERR_PTR(_EIO);
            }
            fh = ((struct fuse_open_out *) out)->fh;
            fh_valid = true;
            free(out);
        } else if (err == _ENOSYS) {
            // open not implemented: stateless daemon, reads/writes still work
            fh = 0;
            fh_valid = true;
        } else {
            return ERR_PTR(err);
        }
    }
    (void) created;

    struct fuse_file *file = calloc(1, sizeof(*file));
    if (file == NULL)
        goto err_release;
    file->path = strdup(path);
    if (file->path == NULL) {
        free(file);
        goto err_release;
    }
    file->nodeid = nodeid;
    file->fh = fh;
    file->fh_valid = fh_valid;
    file->is_dir = is_dir;

    struct fd *fd = fd_create(&fusefs_fd_ops);
    if (fd == NULL) {
        free(file->path);
        free(file);
        goto err_release;
    }
    fd->data = file;
    return fd;

err_release:
    if (fh_valid) {
        struct fuse_release_in release = {.fh = fh};
        fuse_conn_call(conn, is_dir ? FUSE_RELEASEDIR : FUSE_RELEASE, nodeid,
                &release, sizeof(release), NULL, 0, NULL, NULL, false);
    }
    return ERR_PTR(_ENOMEM);
}

static ssize_t fusefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t nodeid;
    int err = fuse_resolve(conn, path, &nodeid, NULL);
    if (err < 0)
        return err;
    void *out = NULL;
    size_t out_len = 0;
    err = fuse_conn_call(conn, FUSE_READLINK, nodeid, NULL, 0, NULL, 0,
            &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len > bufsize)
        out_len = bufsize;
    memcpy(buf, out, out_len);
    free(out);
    return (ssize_t) out_len;
}

// The shared shape of every parent-directory + name operation.
static int fuse_dirop(struct mount *mount, uint32_t opcode, const char *path,
        const void *arg, size_t arg_len) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t dir;
    const char *name;
    int err = fuse_resolve_parent(conn, path, &dir, &name);
    if (err < 0)
        return err;
    void *out = NULL;
    err = fuse_conn_call(conn, opcode, dir, arg, arg_len,
            name, strlen(name) + 1, &out, NULL, true);
    free(out);
    return err;
}

static int fusefs_unlink(struct mount *mount, const char *path) {
    return fuse_dirop(mount, FUSE_UNLINK, path, NULL, 0);
}

static int fusefs_rmdir(struct mount *mount, const char *path) {
    return fuse_dirop(mount, FUSE_RMDIR, path, NULL, 0);
}

static int fusefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    struct fuse_mkdir_in in = {.mode = mode};
    return fuse_dirop(mount, FUSE_MKDIR, path, &in, sizeof(in));
}

static int fusefs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    struct fuse_mknod_in in = {.mode = mode, .rdev = (uint32_t) dev};
    return fuse_dirop(mount, FUSE_MKNOD, path, &in, sizeof(in));
}

static int fusefs_rename(struct mount *mount, const char *src, const char *dst) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t src_dir, dst_dir;
    const char *src_name, *dst_name;
    int err = fuse_resolve_parent(conn, src, &src_dir, &src_name);
    if (err < 0)
        return err;
    err = fuse_resolve_parent(conn, dst, &dst_dir, &dst_name);
    if (err < 0)
        return err;
    // Body: fuse_rename_in, then oldname NUL newname NUL.
    size_t src_len = strlen(src_name) + 1;
    size_t dst_len = strlen(dst_name) + 1;
    char names[2 * (NAME_MAX + 1)];
    if (src_len + dst_len > sizeof(names))
        return _ENAMETOOLONG;
    memcpy(names, src_name, src_len);
    memcpy(names + src_len, dst_name, dst_len);
    struct fuse_rename_in in = {.newdir = dst_dir};
    void *out = NULL;
    err = fuse_conn_call(conn, FUSE_RENAME, src_dir, &in, sizeof(in),
            names, src_len + dst_len, &out, NULL, true);
    free(out);
    return err;
}

static int fusefs_symlink(struct mount *mount, const char *target, const char *link) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t dir;
    const char *name;
    int err = fuse_resolve_parent(conn, link, &dir, &name);
    if (err < 0)
        return err;
    // Body: linkname NUL target NUL (name first, per the protocol).
    size_t name_len = strlen(name) + 1;
    size_t target_len = strlen(target) + 1;
    char *body = malloc(name_len + target_len);
    if (body == NULL)
        return _ENOMEM;
    memcpy(body, name, name_len);
    memcpy(body + name_len, target, target_len);
    void *out = NULL;
    err = fuse_conn_call(conn, FUSE_SYMLINK, dir, body, name_len + target_len,
            NULL, 0, &out, NULL, true);
    free(body);
    free(out);
    return err;
}

static int fusefs_link(struct mount *mount, const char *src, const char *dst) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    uint64_t src_node;
    int err = fuse_resolve(conn, src, &src_node, NULL);
    if (err < 0)
        return err;
    struct fuse_link_in in = {.oldnodeid = src_node};
    return fuse_dirop(mount, FUSE_LINK, dst, &in, sizeof(in));
}

static int fusefs_statfs(struct mount *mount, struct statfsbuf *stat) {
    struct fuse_conn *conn = fuse_mount_conn(mount);
    if (conn == NULL)
        return _ENOTCONN;
    void *out = NULL;
    size_t out_len = 0;
    int err = fuse_conn_call(conn, FUSE_STATFS, FUSE_ROOT_ID, NULL, 0, NULL, 0,
            &out, &out_len, true);
    if (err < 0)
        return err;
    if (out_len < sizeof(struct fuse_kstatfs)) {
        free(out);
        return _EIO;
    }
    struct fuse_kstatfs *k = out;
    memset(stat, 0, sizeof(*stat));
    stat->type = FUSE_SUPER_MAGIC;
    stat->bsize = k->bsize != 0 ? (long) k->bsize : 4096;
    stat->blocks = k->blocks;
    stat->bfree = k->bfree;
    stat->bavail = k->bavail;
    stat->files = k->files;
    stat->ffree = k->ffree;
    stat->namelen = k->namelen != 0 ? (long) k->namelen : NAME_MAX;
    stat->frsize = k->frsize != 0 ? (long) k->frsize : stat->bsize;
    free(out);
    return 0;
}

const struct fs_ops fusefs = {
    .name = "fuse",
    .magic = FUSE_SUPER_MAGIC,
    .may_block = true,

    .mount = fusefs_mount,
    .umount = fusefs_umount,
    .statfs = fusefs_statfs,

    .open = fusefs_open,
    .readlink = fusefs_readlink,

    .link = fusefs_link,
    .unlink = fusefs_unlink,
    .rmdir = fusefs_rmdir,
    .rename = fusefs_rename,
    .symlink = fusefs_symlink,
    .mknod = fusefs_mknod,
    .mkdir = fusefs_mkdir,

    .close = fusefs_close,

    .stat = fusefs_stat,
    .fstat = fusefs_fstat,
    .setattr = fusefs_setattr,
    .fsetattr = fusefs_fsetattr,
    .utime = fusefs_utime,
    .getpath = fusefs_getpath,
};
