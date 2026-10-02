// Reader/writer accounting for host-backed named FIFOs. See fs/host_fifo.h.
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <stdlib.h>
#include "fs/host_fifo.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/fs.h"
#include "util/list.h"
#include "util/sync.h"

struct host_fifo {
    dev_t dev;
    ino_t ino;
    struct list chain;      // in host_fifos
    // Under host_fifos_lock.
    unsigned readers;       // open descriptions that read
    unsigned writers;       // open descriptions that write
    // Writers ever counted, Linux's pipe->w_counter: a reader is hung up
    // only once a writer it did not open before has come and gone, so one
    // opened O_NONBLOCK before any writer is not told of a hangup that never
    // happened (fd->fifo_version holds the value at its open).
    unsigned w_counter;
    // Descriptions attached plus wakes in flight; the entry goes when this
    // reaches 0.
    unsigned refs;
    // The attached descriptions, walked to wake their pollers. A lock of its
    // own, held across poll_wakeup, and never taken from a poll operation --
    // host_fifo_poll takes only host_fifos_lock -- so the two never nest, as
    // in fs/fifo.c.
    lock_t fds_lock;
    struct list fds;
};

static lock_t host_fifos_lock = LOCK_INITIALIZER;
static struct list host_fifos = LIST_INITIALIZER(host_fifos);

static bool reads(int accmode) {
    return accmode == O_RDONLY_ || accmode == O_RDWR_;
}
static bool writes(int accmode) {
    return accmode == O_WRONLY_ || accmode == O_RDWR_;
}

void host_fifo_attach(struct fd *fd, int accmode) {
    struct stat st;
    if (fd->real_fd < 0 || fstat(fd->real_fd, &st) < 0 || !S_ISFIFO(st.st_mode))
        return;
    lock(&host_fifos_lock, 0);
    struct host_fifo *fifo = NULL, *it;
    list_for_each_entry(&host_fifos, it, chain) {
        if (it->dev == st.st_dev && it->ino == st.st_ino) {
            fifo = it;
            break;
        }
    }
    if (fifo == NULL) {
        fifo = calloc(1, sizeof(*fifo));
        if (fifo == NULL) {
            unlock(&host_fifos_lock);
            return; // untracked: polls as the host says, as before
        }
        fifo->dev = st.st_dev;
        fifo->ino = st.st_ino;
        lock_init(&fifo->fds_lock, "host_fifo_fds\0");
        list_init(&fifo->fds);
        list_add(&host_fifos, &fifo->chain);
    }
    // Linux's fifo_open pins the reader's version only when no writer is
    // there yet; one that opens alongside a live writer is hung up when that
    // writer goes, so its version must not match.
    fd->fifo_version = fifo->writers == 0 ? fifo->w_counter : fifo->w_counter - 1;
    if (reads(accmode))
        fifo->readers++;
    if (writes(accmode))
        fifo->writers++, fifo->w_counter++;
    fd->host_fifo_accmode = accmode;
    fifo->refs++;
    fd->host_fifo = fifo;
    unlock(&host_fifos_lock);

    lock(&fifo->fds_lock, 0);
    list_add(&fifo->fds, &fd->host_fifo_fds);
    unlock(&fifo->fds_lock);
}

static void host_fifo_put_locked(struct host_fifo *fifo) {
    if (--fifo->refs == 0) {
        list_remove(&fifo->chain);
        free(fifo);
    }
}

void host_fifo_detach(struct fd *fd) {
    struct host_fifo *fifo = fd->host_fifo;
    if (fifo == NULL)
        return;
    lock(&fifo->fds_lock, 0);
    list_remove_safe(&fd->host_fifo_fds);
    unlock(&fifo->fds_lock);

    lock(&host_fifos_lock, 0);
    int wake = 0;
    if (reads(fd->host_fifo_accmode) && --fifo->readers == 0)
        wake |= POLL_ERR | POLL_WRITE;   // writers: a write would now be EPIPE
    if (writes(fd->host_fifo_accmode) && --fifo->writers == 0)
        wake |= POLL_HUP | POLL_READ;    // readers: end of file
    fd->host_fifo = NULL;
    // The attachment's reference becomes the wake's, so the entry outlives
    // the walk below even if every other description closes meanwhile.
    unlock(&host_fifos_lock);

    if (wake != 0) {
        struct fd *peer;
        lock(&fifo->fds_lock, 0);
        list_for_each_entry(&fifo->fds, peer, host_fifo_fds)
            poll_wakeup(peer, wake);
        unlock(&fifo->fds_lock);
    }
    lock(&host_fifos_lock, 0);
    host_fifo_put_locked(fifo);
    unlock(&host_fifos_lock);
}

bool host_fifo_tracked(struct fd *fd) {
    return fd->host_fifo != NULL;
}

int host_fifo_poll(struct fd *fd, int host_revents) {
    struct host_fifo *fifo = fd->host_fifo;
    if (fifo == NULL)
        return host_revents;
    int accmode = fd->host_fifo_accmode;
    // The host's own word on readability and hangups is not used: it reports
    // no hangup at all, and Darwin's quirks on a FIFO read end (a spurious
    // POLLHUP before any writer, POLLIN at end of file) are what realfs_poll
    // otherwise has to scrub.
    int events = host_revents & ~(POLLIN | POLLHUP | POLLERR | POLLPRI);
    if (!writes(accmode))
        events &= ~POLLOUT;
    if (reads(accmode)) {
        int avail = 0;
        if (ioctl(fd->real_fd, FIONREAD, &avail) == 0 && avail > 0)
            events |= POLLIN;
    }
    lock(&host_fifos_lock, 0);
    if (reads(accmode) && fifo->writers == 0 && fd->fifo_version != fifo->w_counter)
        events |= POLLHUP;
    if (writes(accmode) && fifo->readers == 0)
        events |= POLLERR;
    unlock(&host_fifos_lock);
    return events;
}
