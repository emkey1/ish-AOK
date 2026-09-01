#include <stdlib.h>
#include <string.h>
#include "fs/fifo.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "util/list.h"
#include "util/sync.h"

// Linux's default pipe capacity is 16 pages; match it. Allocated on first write
// so an idle FIFO (e.g. /run/initctl sitting open) costs almost nothing.
#define FIFO_FILE_CAPACITY (64 * 1024)
// Linux's PIPE_BUF. Darwin's is 512, which is why the atomicity below has to
// be imposed here rather than inherited from the host.
#define PIPE_BUF_ 4096

struct fifo_file {
    lock_t lock;        // protects buf, size, start, readers, writers
    cond_t cond;        // blocked readers/writers and the open rendezvous
    char *buf;          // ring buffer, allocated lazily on first write
    size_t cap;
    size_t size;        // bytes currently buffered
    size_t start;       // ring read position
    unsigned readers;   // currently-open read ends
    unsigned writers;   // currently-open write ends

    // The fd list is iterated to wake pollers. It has its own lock so the wake
    // (which takes poll locks) never runs under ->lock; the two are never held
    // nested, avoiding any lock-ordering question.
    lock_t fds_lock;
    struct list fds;
};

struct fifo_file *fifo_file_new(void) {
    struct fifo_file *fifo = malloc(sizeof(struct fifo_file));
    if (fifo == NULL)
        return NULL;
    lock_init(&fifo->lock, "fifo_file\0");
    lock_init(&fifo->fds_lock, "fifo_file_fds\0");
    cond_init(&fifo->cond);
    fifo->buf = NULL;
    fifo->cap = fifo->size = fifo->start = 0;
    fifo->readers = fifo->writers = 0;
    list_init(&fifo->fds);
    return fifo;
}

void fifo_file_free(struct fifo_file *fifo) {
    if (fifo == NULL)
        return;
    cond_destroy(&fifo->cond);
    free(fifo->buf);
    free(fifo);
}

static bool fifo_fd_is_reader(struct fd *fd) {
    unsigned acc = fd->flags & O_ACCMODE_;
    return acc == O_RDONLY_ || acc == O_RDWR_;
}
static bool fifo_fd_is_writer(struct fd *fd) {
    unsigned acc = fd->flags & O_ACCMODE_;
    return acc == O_WRONLY_ || acc == O_RDWR_;
}

// Wake any poll/select/epoll waiting on this fifo's fds. Called without ->lock.
static void fifo_file_wake_pollers(struct fifo_file *fifo, int events) {
    struct fd *fd;
    lock(&fifo->fds_lock, 0);
    list_for_each_entry(&fifo->fds, fd, fifo_other_fds) {
        poll_wakeup(fd, events);
    }
    unlock(&fifo->fds_lock);
}

int fifo_file_open(struct fifo_file *fifo, struct fd *fd) {
    bool reader = fifo_fd_is_reader(fd);
    bool writer = fifo_fd_is_writer(fd);

    lock(&fifo->lock, 0);
    // POSIX: O_WRONLY|O_NONBLOCK on a FIFO with no reader present fails ENXIO.
    if (writer && !reader && (fd->flags & O_NONBLOCK_) && fifo->readers == 0) {
        unlock(&fifo->lock);
        return _ENXIO;
    }
    if (reader)
        fifo->readers++;
    if (writer)
        fifo->writers++;
    notify(&fifo->cond); // a newly-arrived peer may complete a rendezvous
    unlock(&fifo->lock);

    lock(&fifo->fds_lock, 0);
    list_add(&fifo->fds, &fd->fifo_other_fds);
    unlock(&fifo->fds_lock);
    fifo_file_wake_pollers(fifo, POLL_READ | POLL_WRITE);

    // Rendezvous: a blocking open waits for the opposite end to appear. O_RDWR
    // is its own peer and never blocks.
    int err = 0;
    if (!(fd->flags & O_NONBLOCK_)) {
        lock(&fifo->lock, 0);
        if (reader && !writer) {
            while (fifo->writers == 0)
                if (wait_for(&fifo->cond, &fifo->lock, NULL)) { err = _EINTR; break; }
        } else if (writer && !reader) {
            while (fifo->readers == 0)
                if (wait_for(&fifo->cond, &fifo->lock, NULL)) { err = _EINTR; break; }
        }
        unlock(&fifo->lock);
    }
    if (err < 0)
        fifo_file_close(fifo, fd); // undo the attach above
    return err;
}

void fifo_file_close(struct fifo_file *fifo, struct fd *fd) {
    lock(&fifo->fds_lock, 0);
    bool was_attached = !list_null(&fd->fifo_other_fds);
    list_remove_safe(&fd->fifo_other_fds);
    unlock(&fifo->fds_lock);
    if (!was_attached)
        return; // never attached (e.g. a failed ENXIO open) -- nothing to undo

    lock(&fifo->lock, 0);
    if (fifo_fd_is_reader(fd) && fifo->readers > 0)
        fifo->readers--;
    if (fifo_fd_is_writer(fd) && fifo->writers > 0)
        fifo->writers--;
    // Wake blocked peers: readers see EOF once writers hit 0, writers get EPIPE
    // once readers hit 0, and rendezvous waiters re-check.
    notify(&fifo->cond);
    unlock(&fifo->lock);
    fifo_file_wake_pollers(fifo, POLL_READ | POLL_WRITE | POLL_HUP);
}

ssize_t fifo_file_read(struct fifo_file *fifo, struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;
    lock(&fifo->lock, 0);
    while (fifo->size == 0) {
        if (fifo->writers == 0) { // empty and no writers => end of file
            unlock(&fifo->lock);
            return 0;
        }
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fifo->lock);
            return _EAGAIN;
        }
        if (wait_for(&fifo->cond, &fifo->lock, NULL)) {
            unlock(&fifo->lock);
            return _EINTR;
        }
    }
    size_t n = bufsize < fifo->size ? bufsize : fifo->size;
    size_t first = fifo->cap - fifo->start;
    if (first > n)
        first = n;
    memcpy(buf, fifo->buf + fifo->start, first);
    memcpy((char *) buf + first, fifo->buf, n - first);
    fifo->start = (fifo->start + n) % fifo->cap;
    fifo->size -= n;
    notify(&fifo->cond);
    unlock(&fifo->lock);
    fifo_file_wake_pollers(fifo, POLL_WRITE);
    return (ssize_t) n;
}

ssize_t fifo_file_write(struct fifo_file *fifo, struct fd *fd, const void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;
    lock(&fifo->lock, 0);
    if (fifo->buf == NULL) {
        fifo->buf = malloc(FIFO_FILE_CAPACITY);
        if (fifo->buf == NULL) {
            unlock(&fifo->lock);
            return _ENOMEM;
        }
        fifo->cap = FIFO_FILE_CAPACITY;
    }

    size_t written = 0;
    for (;;) {
        if (fifo->readers == 0) {
            unlock(&fifo->lock);
            if (written > 0)
                return (ssize_t) written;
            send_signal(current, SIGPIPE_, SIGINFO_NIL);
            return _EPIPE;
        }
        size_t space = fifo->cap - fifo->size;
        // POSIX and Linux guarantee a write of at most PIPE_BUF is ATOMIC:
        // with insufficient room it writes nothing at all and blocks (or
        // returns EAGAIN), rather than putting in what fits. That guarantee
        // is the entire reason several processes may share one FIFO for log
        // lines or records -- a partial write splits a record and the next
        // writer's bytes land in the middle of it. A write LARGER than
        // PIPE_BUF may still be partial, which is why the rule is bounded.
        if (bufsize <= PIPE_BUF_ && space < bufsize)
            space = 0;
        if (space > 0) {
            size_t n = bufsize - written;
            if (n > space)
                n = space;
            size_t end = (fifo->start + fifo->size) % fifo->cap;
            size_t first = fifo->cap - end;
            if (first > n)
                first = n;
            memcpy(fifo->buf + end, (const char *) buf + written, first);
            memcpy(fifo->buf, (const char *) buf + written + first, n - first);
            fifo->size += n;
            written += n;
            notify(&fifo->cond);
            unlock(&fifo->lock);
            fifo_file_wake_pollers(fifo, POLL_READ);
            if (written == bufsize)
                return (ssize_t) written;
            lock(&fifo->lock, 0);
            continue;
        }
        // Buffer full: block for a reader to drain it (or fail if O_NONBLOCK).
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fifo->lock);
            return written > 0 ? (ssize_t) written : _EAGAIN;
        }
        if (wait_for(&fifo->cond, &fifo->lock, NULL)) {
            unlock(&fifo->lock);
            return written > 0 ? (ssize_t) written : _EINTR;
        }
    }
}

int fifo_file_poll(struct fifo_file *fifo, struct fd *fd) {
    lock(&fifo->lock, 0);
    int types = 0;
    if (fifo_fd_is_reader(fd)) {
        if (fifo->size > 0)
            types |= POLL_READ;
        if (fifo->writers == 0)
            types |= POLL_READ | POLL_HUP; // EOF: a read would return 0
    }
    if (fifo_fd_is_writer(fd)) {
        if (fifo->buf == NULL || fifo->size < fifo->cap)
            types |= POLL_WRITE;
        if (fifo->readers == 0)
            types |= POLL_ERR; // a write would raise EPIPE
    }
    unlock(&fifo->lock);
    return types;
}
