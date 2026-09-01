// FUSE for iSH-AOK: the /dev/fuse character device and the "fuse" filesystem
// type, speaking FUSE protocol 7.31 to an ordinary guest daemon (libfuse2/3,
// sshfs, rclone, ...). The daemon opens /dev/fuse -- each open is one
// connection -- and calls mount(2) with fd=<n> in the data string; every path
// operation on the mount then becomes a request the daemon reads from that fd
// and answers with a write.
//
// AOK's VFS is path-based while the FUSE protocol is nodeid-based, so each
// operation resolves its path with a FUSE_LOOKUP walk from the root node.
// Nodeids are not cached between operations, but the references they carry
// are returned: FUSE_FORGET is sent for every node the walk passes through,
// under one rule -- whoever asked for a nodeid owns the reference and forgets
// it AFTER the operation that names it, never before. Having no node cache,
// AOK forgets promptly rather than in bulk, so a daemon sees far more FORGETs
// than a real kernel sends for the same work. Both are legal; what matters is
// that no node is ever forgotten more times than it was handed out.
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
// mmap is backed by a per-(connection, nodeid) unlinked host temp file
// standing in for the page cache, with a second copy of what the daemon last
// saw so writeback can send only the pages that changed. Private, shared
// read-only and shared writable mappings all work, which is what makes it
// possible to execute a program stored on a mount. Its sync points are
// msync(), fsync() and the last close; read()/write() go straight to the
// daemon rather than through the cache, so the two agree at those points
// rather than continuously.
//
// FUSE_INTERRUPT is sent for a request the daemon has already read -- one
// still queued is simply dropped -- and FUSE_LSEEK is asked before falling
// back to "the whole file is data" for SEEK_DATA/SEEK_HOLE.
//
// Not modeled: readdirplus (it needs an attribute cache to be worth having),
// splice, and the fsopen()-based mount API. FUSE_INIT negotiates flags = 0
// and discards the daemon's reply flags, so a daemon is never told AOK
// supports something it does not. See docs/book/ch20-fuse.md.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel/calls.h"
// host_unlinked_tmpfd / host_fd_mmap: an mmap of a FUSE file is backed by a
// host temp file, the same way tmpfs backs one (see fuse_file_snapshot).
#include "fs/real.h"
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
#define FUSE_INTERRUPT 36
#define FUSE_LSEEK 46
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

// FUSE_LSEEK asks the daemon where the next data or hole is. Optional: a
// daemon that does not implement it answers ENOSYS, once, and is not asked
// again.
struct fuse_lseek_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t whence;
    uint32_t padding;
};
struct fuse_lseek_out {
    uint64_t offset;
};

// FUSE_INTERRUPT names, in its body, the unique of the request being
// abandoned. It carries no reply.
struct fuse_interrupt_in {
    uint64_t unique;
};

// FUSE_FORGET carries no reply. nlookup is how many of the daemon's lookup
// references to drop.
struct fuse_forget_in {
    uint64_t nlookup;
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

// -- the page cache stand-in behind mmap ---------------------------------
//
// Linux backs a FUSE mapping with the page cache: every process that maps a
// node maps the SAME pages, so a store through one mapping is visible through
// another, and dirty pages go back to the daemon as FUSE_WRITEs. Measured
// against Devuan (Linux 6.12): MAP_PRIVATE, read-only MAP_SHARED and writable
// MAP_SHARED all succeed on a FUSE file, and a store through a writable
// shared mapping does reach the daemon.
//
// AOK has no page cache, so one stands in: an unlinked host temp file per
// (connection, nodeid), created on the first mmap of that node and shared by
// every mapping of it afterwards. Mapping that host file gives the guest the
// same sharing the page cache would, for nothing -- two processes mapping one
// node land on the same host pages.
//
// Writeback has to know WHICH bytes the guest changed, and there is no dirty
// bit to consult. So a second, identical temp file keeps a copy of what the
// daemon last saw; writeback diffs the two a page at a time and sends only
// the pages that differ. An untouched mapping therefore costs no FUSE_WRITEs
// at all, and a one-page store in a large file sends exactly one page --
// which is the granularity Linux writes back at too.
//
// Where this still differs from Linux, and the docs say so: read() and
// write() go straight to the daemon rather than through the cache, so a
// mapping and a read() of the same node are coherent only at the sync
// points -- msync(), fsync(), and the last close of the file.
struct fuse_cache {
    struct list node;   // conn->caches
    lock_t lock;        // the two host files' contents; never held over ->lock
    uint64_t nodeid;
    unsigned refcount;  // open files holding it; guarded by conn->lock
    int host_fd;        // what the guest maps
    int pristine_fd;    // what the daemon last saw
    uint64_t size;      // the node's size when the cache was filled
    // A writable shared mapping was created on it. Atomic and not under
    // ->lock because it is set from fusefs_fd_mmap, which may take no lock a
    // daemon round trip could be holding -- see the note there.
    atomic_bool writable;
};

struct fuse_conn {
    atomic_uint refcount;
    lock_t lock;        // protects everything below
    cond_t cond;        // wakes the daemon's read and every reply waiter
    struct list pending;     // queued for the daemon to read
    struct list processing;  // read by the daemon, awaiting its reply
    struct list caches;      // struct fuse_cache, one per mmap'd nodeid
    uint64_t next_unique;
    bool initialized;   // the FUSE_INIT reply has arrived
    bool dead;          // daemon fd closed, INIT refused, or unmounted
    uint32_t max_write; // from the daemon's init reply
    // The daemon answered ENOSYS to FUSE_LSEEK, so do not ask again. Linux
    // keeps the same latch (fc->no_lseek) for the same reason: one refusal
    // is a property of the daemon, not of the call.
    bool no_lseek;
    // A dying task's bounded wait expired on this connection, so the daemon
    // is not answering. Exit closes every descriptor a process holds, and
    // paying the bound again for each of them would make a wedged mount cost
    // seconds per open file. Cleared by the next reply that does arrive, so a
    // daemon that was merely slow is not written off permanently.
    bool exit_gave_up;

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
    // The daemon has read this request off /dev/fuse. Only then is there any
    // point interrupting it: one still on `pending` has never been seen, and
    // telling a daemon to interrupt a request it has not read is a request
    // for a unique it cannot match. Set under conn->lock in fuse_dev_read.
    bool sent;
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

static void fuse_cache_free(struct fuse_cache *cache) {
    if (cache->host_fd >= 0)
        close(cache->host_fd);
    if (cache->pristine_fd >= 0)
        close(cache->pristine_fd);
    free(cache);
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
    // Any cache still here outlived every fd that could write it back -- the
    // mapping holds a reference to the fd, so a live mapping keeps its fd,
    // and its fd keeps this connection. Nothing to flush, just host fds to
    // return.
    struct fuse_cache *cache, *cache_tmp;
    list_for_each_entry_safe(&conn->caches, cache, cache_tmp, node) {
        list_remove(&cache->node);
        fuse_cache_free(cache);
    }
    cond_destroy(&conn->cond);
    free(conn);
}

// Wake a daemon blocked in poll/select on /dev/fuse. Called without ->lock.
// Tell the daemon a request it is working on has been abandoned.
//
// Advisory by protocol: the daemon may act on it and answer the original with
// EINTR, may ignore it and answer normally, or may not implement it at all.
// AOK handles every one of those already -- the abandoned request is freed by
// whichever side touches it last, exactly as before. What changes is that a
// daemon doing something expensive now has the chance to stop, instead of
// finishing work for a caller that walked away. sshfs over a dead link is the
// case that matters.
//
// Must be called WITHOUT conn->lock: fuse_conn_call takes it.
static void fuse_interrupt(struct fuse_conn *conn, uint64_t unique);

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
// Opcodes the protocol says get no reply. A request for one of these is freed
// the moment the daemon reads it, rather than being parked on `processing`
// waiting for an answer that is never coming.
static bool fuse_opcode_no_reply(uint32_t opcode) {
    return opcode == FUSE_FORGET || opcode == FUSE_DESTROY ||
           opcode == FUSE_INTERRUPT;
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

    // A task on its way out cannot wait indefinitely for anything. Its
    // address space is torn down before its fd table (kernel/exit.c), so a
    // mapping's writeback runs while it is dying -- and if the daemon was one
    // of this process's own threads, that thread is already gone and no
    // answer is ever coming. Linux reaches the same place from the other
    // side: its FUSE waits are killable, and a dying task's return at once.
    //
    // A bound rather than a refusal to wait, because refusing loses data.
    // Measured on Devuan: a process that stores through a shared mapping and
    // exits without unmapping or syncing still has those stores reach the
    // daemon, which is only possible if the exit path waits for it. Two
    // seconds is far longer than a daemon in another process -- the normal
    // case, and the recoverable one -- needs, and finite for the one that
    // cannot answer at all.
    static const struct timespec exit_bound = {.tv_sec = 2, .tv_nsec = 0};
    static const struct timespec gave_up_bound = {.tv_sec = 0, .tv_nsec = 0};
    bool dying = current != NULL && (current->exiting || current->mm_teardown);
    struct timespec deadline = conn->exit_gave_up ? gave_up_bound : exit_bound;
    struct timespec *wait_bound = dying ? &deadline : NULL;

    lock(&conn->lock, 0);
    // Everything except INIT itself queues behind a completed handshake, so
    // the daemon never sees a request it hasn't negotiated struct sizes for.
    if (wait && opcode != FUSE_INIT) {
        while (!conn->initialized && !conn->dead) {
            if (wait_for(&conn->cond, &conn->lock, wait_bound)) {
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
        if (wait_for(&conn->cond, &conn->lock, wait_bound)) {
            if (dying)
                conn->exit_gave_up = true;
            // Interrupted. If the daemon hasn't read it yet it can be pulled
            // back; once read, the eventual reply (or conn death) frees it.
            if (!list_null(&req->queue) && !req->answered) {
                // Only a request the daemon has actually READ is worth
                // interrupting; one still queued has never been seen.
                bool tell_daemon = req->sent;
                uint64_t abandoned_unique = req->unique;
                req->abandoned = true;
                unlock(&conn->lock);
                if (tell_daemon)
                    fuse_interrupt(conn, abandoned_unique);
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

static void fuse_interrupt(struct fuse_conn *conn, uint64_t unique) {
    struct fuse_interrupt_in in = {.unique = unique};
    fuse_conn_call(conn, FUSE_INTERRUPT, 0, &in, sizeof(in), NULL, 0,
            NULL, NULL, false);
}

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
        req->sent = true;
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
    // The daemon is answering after all.
    conn->exit_gave_up = false;
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
    list_init(&conn->caches);
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

// Drop ONE of the daemon's lookup references on a node.
//
// Every FUSE_LOOKUP, and every entry returned by CREATE/MKDIR/MKNOD/SYMLINK/
// LINK, increments a per-node lookup count in the daemon. FORGET is the only
// thing that decrements it. AOK never sent one, so every path AOK ever walked
// left the daemon holding a reference that could not be released -- a
// filesystem doing bookkeeping on those counts (which is what the count is
// FOR) could never free a node, however long ago the guest stopped caring.
//
// This is not the nodeid cache the design deliberately does not have. Caching
// would mean OWNING a nodeid across operations and getting the lifetime right
// against a daemon that may crash. This just balances the books: each
// reference AOK takes, AOK drops, within the operation that took it. The
// lifetime is one operation long, which is short enough to reason about
// completely.
//
// Fire-and-forget by protocol -- there is no reply, and none is waited for, so
// this never blocks and never deadlocks. The root nodeid is never looked up
// (every walk STARTS there), so it is never forgotten.
static void fuse_forget(struct fuse_conn *conn, uint64_t nodeid) {
    if (conn == NULL || nodeid == 0 || nodeid == FUSE_ROOT_ID)
        return;
    struct fuse_forget_in in = {.nlookup = 1};
    fuse_conn_call(conn, FUSE_FORGET, nodeid, &in, sizeof(in), NULL, 0,
            NULL, NULL, false);
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
        if (n > NAME_MAX) {
            // Same as the failed-lookup path below: the walk stops, so the
            // node we were standing on is ours to drop.
            fuse_forget(conn, nodeid);
            return _ENAMETOOLONG;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        p += n;
        int err = fuse_lookup(conn, nodeid, name, &entry);
        if (err < 0) {
            // The walk stops here; the parent we were standing on is ours to
            // drop. (A negative lookup takes no reference of its own.)
            fuse_forget(conn, nodeid);
            return err;
        }
        // We have the child, so the parent's reference has done its job.
        fuse_forget(conn, nodeid);
        nodeid = entry.nodeid;
        have_attr = true;
    }
    if (attr_out != NULL) {
        if (have_attr) {
            *attr_out = entry.attr;
        } else {
            int err = fuse_getattr(conn, nodeid, false, 0, attr_out);
            if (err < 0) {
                fuse_forget(conn, nodeid);
                return err;
            }
        }
    }
    // A caller that asked for the nodeid OWNS the reference and must
    // fuse_forget() it when done -- after the operation that names it, never
    // before, or the daemon may free the node out from under that operation.
    // A caller that did not ask cannot drop it, so drop it here.
    if (nodeid_out != NULL)
        *nodeid_out = nodeid;
    else
        fuse_forget(conn, nodeid);
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
    // mmap's backing, shared with every other open file on the same node.
    // NULL until the first mmap. Set under fd->lock; the reference is dropped
    // in fusefs_close. See struct fuse_cache.
    struct fuse_cache *cache;
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
        case LSEEK_DATA:
        case LSEEK_HOLE: {
            struct fuse_conn *conn = fuse_mount_conn(fd->mount);
            if (conn == NULL)
                return _ENOTCONN;
            // Measured on Devuan: a negative offset is ENXIO here, not the
            // EINVAL every other whence gives -- the fallback treats the
            // offset as unsigned, so "before the start" lands past the end.
            if (off < 0)
                return _ENXIO;

            // Ask the daemon first -- a filesystem over an archive or a
            // sparse remote file knows where its holes are, and FUSE_LSEEK is
            // how it says so. Measured on Devuan: Linux sends opcode 46 here
            // and falls back on ENOSYS, which is what this does.
            if (file->fh_valid && !conn->no_lseek) {
                struct fuse_lseek_in in = {
                    .fh = file->fh,
                    .offset = (uint64_t) off,
                    .whence = (uint32_t) whence,
                };
                void *out = NULL;
                size_t out_len = 0;
                int err = fuse_conn_call(conn, FUSE_LSEEK, file->nodeid, &in,
                        sizeof(in), NULL, 0, &out, &out_len, true);
                if (err == 0 && out_len >= sizeof(struct fuse_lseek_out)) {
                    off_t_ answer = (off_t_) ((struct fuse_lseek_out *) out)->offset;
                    free(out);
                    // SEEK_DATA and SEEK_HOLE reposition the file offset like
                    // any other whence -- measured on Devuan, where a
                    // SEEK_HOLE to EOF leaves the next read at EOF. Returning
                    // the answer without moving the offset makes the common
                    // sparse-copy loop (seek to data, read, seek again) read
                    // from the wrong place.
                    fd->offset = (unsigned long) answer;
                    return answer;
                }
                free(out);
                if (err == _ENOSYS)
                    conn->no_lseek = true;
                else if (err < 0)
                    return err;     // the daemon's own answer, ENXIO included
            }

            // No daemon support: a filesystem is always allowed to report
            // that a file has no holes, and that is the honest fallback --
            // DATA is wherever you already are and the only HOLE is the one
            // at EOF, with past-EOF being ENXIO for both. EINVAL, which this
            // used to give, tells a caller the interface does not exist at
            // all, and the sparse-copy paths in cp, tar and rsync act on that
            // difference.
            struct fuse_wire_attr attr;
            int err = fuse_getattr(conn, file->nodeid, file->fh_valid, file->fh, &attr);
            if (err < 0)
                return err;
            if ((uint64_t) off >= attr.size)
                return _ENXIO;
            off_t_ answer = whence == LSEEK_DATA ? off : (off_t_) attr.size;
            fd->offset = (unsigned long) answer;
            return answer;
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

// Linux has no ->poll for a regular file or a directory, FUSE included: both
// are polled through DEFAULT_POLLMASK -- always readable, always writable,
// whatever the open access mode. fusefs_fd_ops had no .poll at all, so the
// poll layer read back 0 and a FUSE file was NEVER ready for anything. A
// program that polls before reading -- which is the ordinary shape for
// anything driving several inputs at once, and what select() loops do to
// files as a matter of course -- waited for a readiness that could not
// arrive. The daemon was never asked, so nothing in the log said why.
//
// (A daemon CAN make a file genuinely pollable with FUSE_POLL, which is a
// different feature and is not implemented; a daemon that wants it gets the
// same answer Linux gives a filesystem that does not implement ->poll.)
// mmap of a FUSE file.
//
// There is no host descriptor behind a FUSE file -- its contents live in the
// daemon, reachable only by asking. So the mapping is backed the way tmpfs
// backs one for a file that started life in a malloc'd buffer (fs/tmp.c):
// take an unlinked host temp file, fill it, and let the host map THAT. The
// fill is a FUSE_READ loop over the whole file, done once and kept on the
// open file for as long as it lives, so a second mmap of the same fd costs
// nothing.
//
// This is a SNAPSHOT, and that is the honest limit of the approach. Two
// consequences, both stated in the documentation rather than hidden:
//
//   MAP_SHARED with PROT_WRITE is REFUSED. Stores into a shared mapping would
//   land in the temp file and never reach the daemon -- the file would appear
//   to change to this process and to nobody else, including its own later
//   read()s. Linux implements that case with a writeback path AOK has no hook
//   for. ENODEV is what Linux gives for a filesystem whose mmap cannot do what
//   was asked, and it is a refusal a caller can act on; silently dropping the
//   writes is not.
//
//   A change made through the daemon after the mapping exists is not seen
//   through it. Linux would show it (until the first COW break, for a private
//   mapping). Nothing here can, without a page-fault hook into the daemon.
//
// What it buys is the thing the chapter called out: MAP_PRIVATE is what the
// ELF loader uses (kernel/exec.c), so a program stored on a FUSE mount can
// now be executed instead of having to be copied off first.
// Write `len` bytes at `off` to a host fd, looping over short writes.
static int host_pwrite_all(int host_fd, const char *buf, size_t len, uint64_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(host_fd, buf + done, len - done, (off_t) (off + done));
        if (n <= 0)
            return n < 0 ? errno_map() : _EIO;
        done += (size_t) n;
    }
    return 0;
}

// Fill a fresh cache from the daemon: the node's contents into the file the
// guest will map, and the same bytes into the pristine copy writeback diffs
// against. Blocks on the daemon, so no connection lock may be held.
static int fuse_cache_fill(struct fd *fd, struct fuse_cache *cache, uint64_t size) {
    // A node the daemon reports as empty still needs something to map: mmap
    // of a zero-length host file fails. Give it a page, and leave ->size at
    // zero so writeback never extends the file behind the guest's back --
    // stores past the end of a mapping do not reach the file on Linux either.
    if (size == 0) {
        if (ftruncate(cache->host_fd, PAGE_SIZE) < 0 ||
                ftruncate(cache->pristine_fd, PAGE_SIZE) < 0)
            return errno_map();
        return 0;
    }

    char *buf = malloc(FUSE_MAX_TRANSFER);
    if (buf == NULL)
        return _ENOMEM;
    uint64_t off = 0;
    int err = 0;
    while (off < size) {
        size_t want = size - off;
        if (want > FUSE_MAX_TRANSFER)
            want = FUSE_MAX_TRANSFER;
        ssize_t got = fuse_fd_do_read(fd, buf, want, off);
        if (got < 0) {
            err = (int) got;
            break;
        }
        if (got == 0) {
            // Short file: the daemon knows its own contents better than the
            // size it reported. Believe the reads.
            size = off;
            break;
        }
        if ((err = host_pwrite_all(cache->host_fd, buf, (size_t) got, off)) < 0)
            break;
        if ((err = host_pwrite_all(cache->pristine_fd, buf, (size_t) got, off)) < 0)
            break;
        off += (uint64_t) got;
    }
    free(buf);
    if (err < 0)
        return err;
    cache->size = size;
    return 0;
}

// The cache for this file's node, creating it on first use. Every open file
// on one node shares one cache, so two processes mapping the same node map
// the same host pages -- which is the sharing Linux's page cache provides.
// Called with fd->lock held.
static struct fuse_cache *fuse_cache_get(struct fd *fd, int *err_out) {
    struct fuse_file *file = fd->data;
    if (file->cache != NULL)
        return file->cache;
    struct fuse_conn *conn = fuse_mount_conn(fd->mount);
    if (conn == NULL) {
        *err_out = _ENOTCONN;
        return NULL;
    }

    struct fuse_cache *existing;
    lock(&conn->lock, 0);
    list_for_each_entry(&conn->caches, existing, node) {
        if (existing->nodeid == file->nodeid) {
            existing->refcount++;
            unlock(&conn->lock);
            file->cache = existing;
            return existing;
        }
    }
    unlock(&conn->lock);

    // Building one blocks on the daemon, so it happens with no lock held and
    // the list is rechecked afterwards: two openers racing on one node both
    // build a cache, and the loser throws its own away rather than leaving
    // two views of the same file in existence.
    struct fuse_wire_attr attr;
    int err = fuse_getattr(conn, file->nodeid, file->fh_valid, file->fh, &attr);
    if (err < 0) {
        *err_out = err;
        return NULL;
    }
    struct fuse_cache *cache = calloc(1, sizeof(*cache));
    if (cache == NULL) {
        *err_out = _ENOMEM;
        return NULL;
    }
    lock_init(&cache->lock, "fuse_cache\0");
    cache->nodeid = file->nodeid;
    cache->host_fd = host_unlinked_tmpfd();
    cache->pristine_fd = cache->host_fd < 0 ? -1 : host_unlinked_tmpfd();
    if (cache->host_fd < 0 || cache->pristine_fd < 0) {
        *err_out = cache->host_fd < 0 ? cache->host_fd : cache->pristine_fd;
        fuse_cache_free(cache);
        return NULL;
    }
    if ((err = fuse_cache_fill(fd, cache, attr.size)) < 0) {
        *err_out = err;
        fuse_cache_free(cache);
        return NULL;
    }

    lock(&conn->lock, 0);
    list_for_each_entry(&conn->caches, existing, node) {
        if (existing->nodeid == file->nodeid) {
            existing->refcount++;
            unlock(&conn->lock);
            fuse_cache_free(cache);
            file->cache = existing;
            return existing;
        }
    }
    cache->refcount = 1;
    list_add(&conn->caches, &cache->node);
    unlock(&conn->lock);
    file->cache = cache;
    return cache;
}

static void fuse_cache_put(struct fuse_conn *conn, struct fuse_cache *cache) {
    if (cache == NULL || conn == NULL)
        return;     // no connection left to unlink from; conn teardown frees it
    bool last = false;
    lock(&conn->lock, 0);
    if (--cache->refcount == 0) {
        list_remove(&cache->node);
        last = true;
    }
    unlock(&conn->lock);
    // Closing the host fds does not disturb a mapping still standing on them:
    // the file is unlinked but held alive by the mapping itself.
    if (last)
        fuse_cache_free(cache);
}

// Send one dirty run back to the daemon, and bring the pristine copy up to
// what was sent so a second writeback does not resend it.
static int fuse_cache_flush_run(struct fd *fd, struct fuse_cache *cache,
        uint64_t start, uint64_t end, char *buf, size_t bufcap) {
    uint64_t off = start;
    while (off < end) {
        size_t want = end - off;
        if (want > bufcap)
            want = bufcap;
        ssize_t got = pread(cache->host_fd, buf, want, (off_t) off);
        if (got <= 0)
            return got < 0 ? errno_map() : _EIO;
        ssize_t done = 0;
        while (done < got) {
            ssize_t written = fuse_fd_do_write(fd, buf + done,
                    (size_t) (got - done), off + (uint64_t) done);
            if (written < 0)
                return (int) written;
            if (written == 0)
                return _EIO;
            int err = host_pwrite_all(cache->pristine_fd, buf + done,
                    (size_t) written, off + (uint64_t) done);
            if (err < 0)
                return err;
            done += written;
        }
        off += (uint64_t) got;
    }
    return 0;
}

// Write a mapping's changed pages back to the daemon. Linux does this from
// msync(), from fsync(), and when the page cache writes back a dirty page;
// AOK does it from msync(), fsync() and the last close of the file.
//
// Only pages that actually differ from what the daemon last gave us are sent,
// so a read-only mapping -- by far the common case, and the one that makes
// execution from a FUSE mount work -- costs nothing here.
static int fuse_cache_writeback(struct fd *fd) {
    struct fuse_file *file = fd->data;
    if (file == NULL || file->cache == NULL || !file->fh_valid)
        return 0;
    struct fuse_cache *cache = file->cache;
    if (!atomic_load_explicit(&cache->writable, memory_order_relaxed) || cache->size == 0)
        return 0;
    // The write goes out over this file's handle, so it has to be one the
    // daemon will accept a write on. Several files can share one cache, and
    // the read-only ones among them have nothing to flush: a writable shared
    // mapping requires an O_RDWR file, and that file's own close writes the
    // cache back.
    if ((fd->flags & O_ACCMODE_) == O_RDONLY_)
        return 0;

    char *mapped = malloc(PAGE_SIZE);
    char *clean = malloc(PAGE_SIZE);
    char *run = malloc(FUSE_MAX_TRANSFER);
    if (mapped == NULL || clean == NULL || run == NULL) {
        free(mapped); free(clean); free(run);
        return _ENOMEM;
    }

    lock(&cache->lock, 0);
    int err = 0;
    uint64_t off = 0, run_start = 0;
    bool in_run = false;
    while (off < cache->size) {
        size_t want = cache->size - off;
        if (want > PAGE_SIZE)
            want = PAGE_SIZE;
        ssize_t a = pread(cache->host_fd, mapped, want, (off_t) off);
        ssize_t b = pread(cache->pristine_fd, clean, want, (off_t) off);
        if (a < 0 || b < 0) {
            err = errno_map();
            break;
        }
        // A short read on either side means the two no longer line up. Treat
        // that as dirty rather than silently dropping the guest's stores.
        bool dirty = a != b || memcmp(mapped, clean, (size_t) a) != 0;
        if (dirty && !in_run) {
            run_start = off;
            in_run = true;
        } else if (!dirty && in_run) {
            if ((err = fuse_cache_flush_run(fd, cache, run_start, off, run,
                            FUSE_MAX_TRANSFER)) < 0)
                break;
            in_run = false;
        }
        off += want;
    }
    if (err == 0 && in_run)
        err = fuse_cache_flush_run(fd, cache, run_start, cache->size, run,
                FUSE_MAX_TRANSFER);
    unlock(&cache->lock);

    free(mapped); free(clean); free(run);
    return err;
}

// Fetch the file before the address space is locked. See fd_ops.mmap_prepare:
// by the time fusefs_fd_mmap runs, every other thread of this process is
// quiesced, and a FUSE round trip made there would wait on a daemon that may
// itself be one of them.
//
// The access checks are deliberately NOT repeated here -- this only fetches;
// fusefs_fd_mmap decides. A failed fetch is reported, so a caller that would
// have been refused anyway gets the refusal from ->mmap either way.
static int fusefs_fd_mmap_prepare(struct fd *fd, off_t offset, size_t UNUSED(len)) {
    struct fuse_file *file = fd->data;
    // Fetch nothing for a request fusefs_fd_mmap is going to refuse, and
    // report no error for it either: the refusal is ->mmap's to make, and it
    // has to be the errno the caller sees. Fetching first would turn an
    // O_WRONLY mapping's EACCES into whatever the daemon says about reading
    // through a write-only handle.
    if (file == NULL || file->is_dir)
        return 0;
    if ((fd->flags & O_ACCMODE_) == O_WRONLY_)
        return 0;
    if (offset % PAGE_SIZE != 0)
        return 0;
    lock(&fd->lock, 0);
    int err = 0;
    (void) fuse_cache_get(fd, &err);
    unlock(&fd->lock);
    return err < 0 ? err : 0;
}

static int fusefs_fd_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages,
        off_t offset, int prot, int flags) {
    struct fuse_file *file = fd->data;
    if (file == NULL)
        return _ENODEV;
    // Linux (verified for tmpfs, and the same rule): mmap of a directory or a
    // non-regular file is ENODEV.
    if (file->is_dir)
        return _ENODEV;
    // The same access checks tmpfs makes explicitly, for the same reason: the
    // backing host fd is always O_RDWR, so the guest fd's mode has to be
    // checked here rather than inherited from the host mmap.
    int accmode = fd->flags & O_ACCMODE_;
    if (accmode == O_WRONLY_)
        return _EACCES;
    if (offset % PAGE_SIZE != 0)
        return _EINVAL;
    // A writable shared mapping writes through to the file, so the fd has to
    // be open for writing.
    bool shared_write = (flags & MMAP_SHARED) && (prot & P_WRITE);
    if (shared_write && accmode != O_RDWR_)
        return _EACCES;

    // NO LOCK IS TAKEN HERE, deliberately, and that is the whole reason
    // fusefs_fd_mmap_prepare exists.
    //
    // This runs under the address-space write lock with every sibling thread
    // of this process quiesced. Both locks a FUSE file has can be held across
    // a wait on the daemon -- fd->lock by the read, pread and readdir paths
    // (kernel/fs.c holds it across fd->ops->pread), and cache->lock by
    // writeback -- so blocking on either one here means waiting, with the
    // address space frozen, for a thread that may itself be frozen, or for
    // the daemon, which cannot answer if it is one of them.
    //
    // The two things read below are set once and never change afterwards:
    // the cache pointer, by mmap_prepare on this same thread a moment ago,
    // and host_fd when the cache was built.
    struct fuse_cache *cache = file->cache;
    if (cache == NULL)
        // mmap_prepare did not run or could not build a cache. Reached only
        // through mremap's grow path, where the mapping being grown always
        // has one already. An honest refusal beats fetching from under the
        // barrier: see the note above.
        return _ENODEV;
    // Marked for writeback on the strength of MAP_SHARED alone, not of the
    // protection asked for here: mprotect can add PROT_WRITE afterwards, and
    // nothing tells this filesystem when it does. Deciding from the map-time
    // protection meant a guest that mapped PROT_READ and then mprotected
    // PROT_WRITE had its stores silently discarded at every sync point.
    //
    // Costing a read-only shared mapping nothing is what makes this safe to
    // do unconditionally: writeback compares against the pristine copy and
    // sends only pages that differ, so a mapping that was never written to
    // still produces no FUSE_WRITEs. A private mapping is excluded because
    // its stores are not meant to reach the file at all, whatever it is later
    // mprotected to.
    if (flags & MMAP_SHARED)
        atomic_store_explicit(&cache->writable, true, memory_order_relaxed);
    return host_fd_mmap(cache->host_fd, mem, start, pages, offset, prot, flags);
}

static int fusefs_fd_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
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
    // Linux's fsync writes the mapping's dirty pages back before asking the
    // daemon to sync, and a caller that msync'd nothing still expects an
    // fsync to persist what it stored through the mapping.
    int wb = fuse_cache_writeback(fd);
    if (wb < 0)
        return wb;
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
    int err = 0;
    // Before anything else: a mapping's stores have to reach the daemon while
    // the file handle they belong to is still open. By the time this runs the
    // mapping itself is already gone -- a mapping holds a reference to its
    // fd, so this close is the one after the last munmap -- but the bytes are
    // still in the cache, which is exactly what is being flushed.
    // Done even when this task is exiting: measured on Devuan, a process that
    // stores through a shared mapping and exits without unmapping still has
    // those stores reach the daemon, so skipping it here would lose data that
    // Linux keeps. fuse_conn_call bounds the wait for a dying task instead.
    if (conn != NULL && file->fh_valid && !file->is_dir) {
        // Reported, not discarded: close(2) is where a filesystem gets to say
        // the data never landed, and it is the last chance anyone has to hear
        // it.
        int wb = fuse_cache_writeback(fd);
        if (wb < 0)
            err = wb;
    }
    if (conn != NULL && file->fh_valid) {
        if (!file->is_dir) {
            // FLUSH is how a daemon reports close-time write errors (sshfs);
            // wait for it, interruptibly. RELEASE is fire-and-forget so a
            // wedged daemon can never make close hang.
            struct fuse_flush_in flush = {.fh = file->fh};
            int ferr = fuse_conn_call(conn, FUSE_FLUSH, file->nodeid, &flush,
                    sizeof(flush), NULL, 0, NULL, NULL, true);
            if (ferr < 0 && err == 0)
                err = ferr;
        }
        struct fuse_release_in release = {
            .fh = file->fh,
            .flags = fuse_open_flags(fd->flags),
        };
        fuse_conn_call(conn, file->is_dir ? FUSE_RELEASEDIR : FUSE_RELEASE,
                file->nodeid, &release, sizeof(release), NULL, 0, NULL, NULL, false);
    }
    // The reference fusefs_open kept, dropped last: the RELEASE queued above
    // still names this node, and FORGET goes behind it on the same queue.
    if (conn != NULL)
        fuse_forget(conn, file->nodeid);
    fuse_cache_put(conn, file->cache);
    free(file->path);
    free(file->dirbuf);
    free(file);
    fd->data = NULL;
    return err;
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
    err = fuse_setattr_from_attr(conn, nodeid, false, 0, attr);
    fuse_forget(conn, nodeid);   // after the operation that names it
    return err;
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
    err = fuse_setattr_common(conn, nodeid, &in);
    fuse_forget(conn, nodeid);
    return err;
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

// Write a FUSE mapping's dirty pages back to the daemon, for msync().
//
// Linux writes a shared mapping's dirty pages back to the filesystem there,
// and msync(MS_SYNC) is the sync point a program that stores through a
// mapping is entitled to rely on. kernel/mmap.c calls this for every
// file-backed shared run it flushes; anything that is not a FUSE file has
// nothing to do here and says so.
int fuse_fd_msync_writeback(struct fd *fd);

static const struct fd_ops fusefs_fd_ops = {
    .read = fusefs_fd_read,
    .write = fusefs_fd_write,
    .pread = fusefs_fd_pread,
    .pwrite = fusefs_fd_pwrite,
    .lseek = fusefs_fd_lseek,
    .mmap = fusefs_fd_mmap,
    .mmap_prepare = fusefs_fd_mmap_prepare,
    .poll = fusefs_fd_poll,
    .readdir = fusefs_fd_readdir,
    .telldir = fusefs_fd_telldir,
    .seekdir = fusefs_fd_seekdir,
    .fsync = fusefs_fd_fsync,
    .close = fusefs_close,
};

int fuse_fd_msync_writeback(struct fd *fd) {
    if (fd == NULL || fd->ops != &fusefs_fd_ops)
        return 0;
    return fuse_cache_writeback(fd);
}

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
                fuse_forget(conn, dir);
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
            fuse_forget(conn, dir);
        } else if (err == _ENOSYS) {
            // Old daemons without CREATE: MKNOD then fall through to OPEN.
            struct fuse_mknod_in mk = {
                .mode = (uint32_t) ((mode & 07777) | S_IFREG),
            };
            void *mkout = NULL;
            size_t mkout_len = 0;
            err = fuse_conn_call(conn, FUSE_MKNOD, dir, &mk, sizeof(mk),
                    name, strlen(name) + 1, &mkout, &mkout_len, true);
            // MKNOD's entry is a reference too, and this path throws it away
            // and resolves the path again for a fresh one.
            if (err >= 0 && mkout != NULL && mkout_len >= sizeof(struct fuse_entry_out))
                fuse_forget(conn, ((struct fuse_entry_out *) mkout)->nodeid);
            free(mkout);
            fuse_forget(conn, dir);
            if (err < 0)
                return ERR_PTR(err);
            err = fuse_resolve(conn, path, &nodeid, &attr);
            if (err < 0)
                return ERR_PTR(err);
        } else {
            fuse_forget(conn, dir);
            return ERR_PTR(err);
        }
    } else if (err == 0 && (flags & O_CREAT_) && (flags & O_EXCL_)) {
        fuse_forget(conn, nodeid);
        return ERR_PTR(_EEXIST);
    } else if (err < 0) {
        // fuse_resolve dropped everything it took before failing.
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
            if (err < 0) {
                fuse_forget(conn, nodeid);
                return ERR_PTR(err);
            }
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
                fuse_forget(conn, nodeid);
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
            fuse_forget(conn, nodeid);
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
    // Queued behind the RELEASE above, which still names the node.
    fuse_forget(conn, nodeid);
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
    fuse_forget(conn, nodeid);
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
    size_t out_len = 0;
    err = fuse_conn_call(conn, opcode, dir, arg, arg_len,
            name, strlen(name) + 1, &out, &out_len, true);
    // MKDIR, MKNOD and LINK all answer with a fuse_entry_out, and that entry
    // is a lookup reference of its own. Nothing here keeps the new node, so
    // it is dropped immediately -- otherwise every directory or device the
    // guest ever created left the daemon a reference forever.
    if (err >= 0 && out != NULL && out_len >= sizeof(struct fuse_entry_out))
        fuse_forget(conn, ((struct fuse_entry_out *) out)->nodeid);
    free(out);
    fuse_forget(conn, dir);
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
    if (err < 0) {
        fuse_forget(conn, src_dir);
        return err;
    }
    // Body: fuse_rename_in, then oldname NUL newname NUL.
    size_t src_len = strlen(src_name) + 1;
    size_t dst_len = strlen(dst_name) + 1;
    char names[2 * (NAME_MAX + 1)];
    if (src_len + dst_len > sizeof(names)) {
        fuse_forget(conn, src_dir);
        fuse_forget(conn, dst_dir);
        return _ENAMETOOLONG;
    }
    memcpy(names, src_name, src_len);
    memcpy(names + src_len, dst_name, dst_len);
    struct fuse_rename_in in = {.newdir = dst_dir};
    void *out = NULL;
    err = fuse_conn_call(conn, FUSE_RENAME, src_dir, &in, sizeof(in),
            names, src_len + dst_len, &out, NULL, true);
    free(out);
    fuse_forget(conn, src_dir);
    fuse_forget(conn, dst_dir);
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
    if (body == NULL) {
        fuse_forget(conn, dir);
        return _ENOMEM;
    }
    memcpy(body, name, name_len);
    memcpy(body + name_len, target, target_len);
    void *out = NULL;
    size_t out_len = 0;
    err = fuse_conn_call(conn, FUSE_SYMLINK, dir, body, name_len + target_len,
            NULL, 0, &out, &out_len, true);
    free(body);
    if (err >= 0 && out != NULL && out_len >= sizeof(struct fuse_entry_out))
        fuse_forget(conn, ((struct fuse_entry_out *) out)->nodeid);
    free(out);
    fuse_forget(conn, dir);
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
    // fuse_dirop resolves dst's parent and issues the LINK, so src_node is
    // still named by an in-flight request until it returns.
    err = fuse_dirop(mount, FUSE_LINK, dst, &in, sizeof(in));
    fuse_forget(conn, src_node);
    return err;
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
