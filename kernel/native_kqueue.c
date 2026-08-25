// See kernel/native_kqueue.h for why this exists at all.
//
// This file is NOT compiled with the shim force-included: it calls nlibc_*
// by name, so that which side of the boundary each call lands on is written
// down rather than inferred from a macro.
#define NATIVE_LIBC_NO_REDIRECT

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__linux__)
#include <sys/event.h>
#endif
#include <time.h>

#include "kernel/native_kqueue.h"
#include "kernel/native_libc.h"

// kqueue is a BSD interface, and this front end exists for exactly one reason:
// a native program is HOST code, so on Apple platforms a runtime built for
// Apple (mio, and therefore tokio) reaches for kqueue and would otherwise get
// the host's event queue. A Linux host has no kqueue for anything to reach
// for -- a runtime built there uses epoll, which needs no interception -- so
// the whole translation layer is Darwin-only, and the Linux build gets stubs
// that keep the four symbols linkable rather than a second implementation of
// something nothing calls.
#if defined(__linux__)

int nlibc_kqueue(void) {
    errno = ENOSYS;
    return -1;
}

int nlibc_kevent(int kq, const void *changelist, int nchanges,
                 void *eventlist, int nevents, const void *timeout) {
    (void) kq; (void) changelist; (void) nchanges;
    (void) eventlist; (void) nevents; (void) timeout;
    errno = ENOSYS;
    return -1;
}

// No descriptor here is ever a queue, so close() has nothing to reclaim and
// dup() has nothing to alias.
bool nlibc_kqueue_close_hook(int fd) {
    (void) fd;
    return false;
}

void nlibc_kqueue_dup_hook(int oldfd, int newfd) {
    (void) oldfd; (void) newfd;
}

#else

// One registration. kqueue is keyed by (ident, filter), so the same descriptor
// can appear twice with different udata -- which is exactly what epoll cannot
// express and why the table is here rather than in the guest.
struct kq_reg {
    uintptr_t ident;        // a descriptor for READ/WRITE, an opaque id for USER
    int16_t   filter;
    uint16_t  flags;        // EV_CLEAR / EV_ONESHOT / EV_DISPATCH, as registered
    bool      enabled;
    // Edge state. EV_CLEAR asks for "report the transition, not the level", and
    // ppoll only ever reports the level. So a registration that has been
    // reported ready is held back until it is next observed NOT ready -- which
    // is when the consumer has drained it. Without this the caller spins:
    // tokio's driver marks readiness, loops straight back into kevent, and
    // ppoll returns the same descriptor immediately, forever.
    bool      suppressed;
    bool      triggered;    // EVFILT_USER, armed by NOTE_TRIGGER
    void     *udata;
};

struct kq {
    // Every descriptor that names this queue. More than one because a dup of a
    // kqueue refers to the same queue, and mio's waker relies on that.
    int *fds;
    size_t nfds, fds_cap;
    int  wake_fd;           // the wake pipe's write end
    struct kq_reg *regs;
    size_t n, cap;
    pthread_mutex_t lock;
    int  refs;              // held across the unlocked ppoll
    bool dead;              // closed while someone was polling
    struct kq *next;
};

static struct kq *kq_list;
static pthread_mutex_t kq_list_lock = PTHREAD_MUTEX_INITIALIZER;

// AOK_KQUEUE_DEBUG=1 traces every call. Kept because the two bugs this layer
// had -- a dup'd queue nobody could find, and a wait that never came back --
// were both invisible from the caller, which reported only "Runtime::build
// failed" and then nothing at all. Reads the HOST environment on purpose: it
// is a developer's switch on the machine running the build, not the guest's.
static int kq_debug(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("AOK_KQUEUE_DEBUG") != NULL;
    return on;
}

#define KQ_TRACE(...) do { \
    if (kq_debug()) { \
        fprintf(stderr, "[kq %p] ", (void *) pthread_self()); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        fflush(stderr); \
    } \
} while (0)

// ------------------------------------------------------------- the registry

// Callers of both of these hold kq_list_lock.
static bool kq_names(const struct kq *q, int fd) {
    for (size_t i = 0; i < q->nfds; i++)
        if (q->fds[i] == fd)
            return true;
    return false;
}

static bool kq_name_add(struct kq *q, int fd) {
    if (q->nfds == q->fds_cap) {
        size_t cap = q->fds_cap != 0 ? q->fds_cap * 2 : 4;
        int *grown = realloc(q->fds, cap * sizeof(*grown));
        if (grown == NULL)
            return false;
        q->fds = grown;
        q->fds_cap = cap;
    }
    q->fds[q->nfds++] = fd;
    return true;
}

static struct kq *kq_lookup(int fd) {
    for (struct kq *q = kq_list; q != NULL; q = q->next)
        if (!q->dead && kq_names(q, fd))
            return q;
    return NULL;
}

static struct kq *kq_acquire(int fd) {
    pthread_mutex_lock(&kq_list_lock);
    struct kq *q = kq_lookup(fd);
    if (q != NULL)
        q->refs++;
    pthread_mutex_unlock(&kq_list_lock);
    return q;
}

static void kq_free(struct kq *q) {
    // close_raw, not close: close() consults this table, and the queue is
    // already off it by the time anything gets here.
    for (size_t i = 0; i < q->nfds; i++)
        nlibc_close_raw(q->fds[i]);
    nlibc_close_raw(q->wake_fd);
    pthread_mutex_destroy(&q->lock);
    free(q->fds);
    free(q->regs);
    free(q);
}

// Dropping the last reference to a queue that was closed mid-poll is what
// frees it. Closing one while another thread is inside kevent() is a caller
// error on a real kqueue too, but here it would be a use-after-free rather
// than an undefined answer, so it is counted.
static void kq_release(struct kq *q) {
    pthread_mutex_lock(&kq_list_lock);
    bool last = (--q->refs == 0) && q->dead;
    if (last) {
        struct kq **pp = &kq_list;
        while (*pp != NULL && *pp != q)
            pp = &(*pp)->next;
        if (*pp == q)
            *pp = q->next;
    }
    pthread_mutex_unlock(&kq_list_lock);
    if (last)
        kq_free(q);
}

int nlibc_kqueue(void) {
    int fds[2];
    if (nlibc_pipe(fds) < 0)
        return -1;
    // Non-blocking on both ends: a waker must never block because nobody has
    // drained the pipe yet, and the drain below must stop at the last byte.
    nlibc_fcntl(fds[0], F_SETFL, (long) O_NONBLOCK);
    nlibc_fcntl(fds[1], F_SETFL, (long) O_NONBLOCK);

    struct kq *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        nlibc_close_raw(fds[0]);
        nlibc_close_raw(fds[1]);
        errno = ENOMEM;
        return -1;
    }
    q->wake_fd = fds[1];
    q->refs = 1;
    pthread_mutex_init(&q->lock, NULL);
    if (!kq_name_add(q, fds[0])) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        nlibc_close_raw(fds[0]);
        nlibc_close_raw(fds[1]);
        errno = ENOMEM;
        return -1;
    }

    pthread_mutex_lock(&kq_list_lock);
    q->next = kq_list;
    kq_list = q;
    pthread_mutex_unlock(&kq_list_lock);
    KQ_TRACE("kqueue() -> fd=%d (queue %p, wake fd=%d)", fds[0], (void *) q, fds[1]);
    return fds[0];
}

void nlibc_kqueue_dup_hook(int oldfd, int newfd) {
    if (oldfd == newfd)
        return;
    KQ_TRACE("dup(%d -> %d)", oldfd, newfd);
    pthread_mutex_lock(&kq_list_lock);
    struct kq *q = kq_lookup(oldfd);
    // If the new number already named a queue, the dup closed it out from
    // under that queue -- the same bookkeeping close() would have done.
    struct kq *stale = kq_lookup(newfd);
    if (stale != NULL && stale != q) {
        for (size_t i = 0; i < stale->nfds; i++) {
            if (stale->fds[i] == newfd) {
                stale->fds[i] = stale->fds[--stale->nfds];
                break;
            }
        }
    }
    if (q != NULL)
        kq_name_add(q, newfd);
    pthread_mutex_unlock(&kq_list_lock);
}

bool nlibc_kqueue_close_hook(int fd) {
    pthread_mutex_lock(&kq_list_lock);
    struct kq *q = kq_lookup(fd);
    if (q == NULL) {
        pthread_mutex_unlock(&kq_list_lock);
        return false;
    }
    KQ_TRACE("close(%d) (queue %p, %zu name(s))", fd, (void *) q, q->nfds);
    // Closing one name for the queue is not closing the queue. It goes when
    // the last descriptor naming it does.
    for (size_t i = 0; i < q->nfds; i++) {
        if (q->fds[i] == fd) {
            q->fds[i] = q->fds[--q->nfds];
            break;
        }
    }
    nlibc_close_raw(fd);
    if (q->nfds > 0) {
        pthread_mutex_unlock(&kq_list_lock);
        return true;
    }
    q->dead = true;
    bool last = (--q->refs == 0);
    if (last) {
        struct kq **pp = &kq_list;
        while (*pp != NULL && *pp != q)
            pp = &(*pp)->next;
        if (*pp == q)
            *pp = q->next;
    }
    pthread_mutex_unlock(&kq_list_lock);
    if (last)
        kq_free(q);
    return true;
}

// --------------------------------------------------------------- the table

static struct kq_reg *kq_find(struct kq *q, uintptr_t ident, int16_t filter) {
    for (size_t i = 0; i < q->n; i++)
        if (q->regs[i].ident == ident && q->regs[i].filter == filter)
            return &q->regs[i];
    return NULL;
}

static struct kq_reg *kq_add(struct kq *q, uintptr_t ident, int16_t filter) {
    struct kq_reg *r = kq_find(q, ident, filter);
    if (r != NULL)
        return r;
    if (q->n == q->cap) {
        size_t cap = q->cap != 0 ? q->cap * 2 : 8;
        struct kq_reg *grown = realloc(q->regs, cap * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        q->regs = grown;
        q->cap = cap;
    }
    r = &q->regs[q->n++];
    memset(r, 0, sizeof(*r));
    r->ident = ident;
    r->filter = filter;
    return r;
}

static void kq_remove(struct kq *q, struct kq_reg *r) {
    size_t i = (size_t) (r - q->regs);
    if (i + 1 < q->n)
        q->regs[i] = q->regs[q->n - 1];
    q->n--;
}

// One read, not a loop. poll() has just said the pipe is readable, so a single
// read cannot block -- where a second one could, if O_NONBLOCK had not
// survived. Anything left over is drained by the next call, and the pipe only
// has to mean "something happened", not "how many times".
static void kq_drain_wake(struct kq *q) {
    char buf[256];
    (void) nlibc_read(q->fds[0], buf, sizeof(buf));
}

// ------------------------------------------------------------ the interface

static short kq_poll_bit(int16_t filter) {
    return filter == EVFILT_READ ? POLLIN : POLLOUT;
}

static void kq_emit(struct kevent *out, const struct kq_reg *r,
                    intptr_t data, uint32_t fflags, uint16_t flags) {
    memset(out, 0, sizeof(*out));
    out->ident = r->ident;
    out->filter = r->filter;
    out->flags = flags;
    out->fflags = fflags;
    out->data = data;
    out->udata = r->udata;
}

// Applies one change. Returns the errno a EV_RECEIPT should report, 0 for ok.
// Sets *woke when the change fires the waker, which the caller must then
// carry through to the pipe -- OUTSIDE the lock, because the thread it has
// to wake is inside ppoll and will want that same lock on the way out.
static int kq_apply(struct kq *q, const struct kevent *ch, bool *woke) {
    if (ch->filter != EVFILT_READ && ch->filter != EVFILT_WRITE &&
        ch->filter != EVFILT_USER)
        return ENOTSUP;      // a filter with no guest counterpart, said plainly

    struct kq_reg *r = kq_find(q, ch->ident, ch->filter);
    if (ch->flags & EV_DELETE) {
        if (r == NULL)
            return ENOENT;
        kq_remove(q, r);
        return 0;
    }
    if (ch->flags & EV_ADD) {
        r = kq_add(q, ch->ident, ch->filter);
        if (r == NULL)
            return ENOMEM;
        r->flags = ch->flags & (EV_CLEAR | EV_ONESHOT | EV_DISPATCH);
        r->udata = ch->udata;
        r->enabled = true;
        r->suppressed = false;
    }
    if (r == NULL)
        return ENOENT;
    if (ch->flags & EV_ENABLE)
        r->enabled = true;
    if (ch->flags & EV_DISABLE)
        r->enabled = false;
    // The waker. mio arms an EVFILT_USER registration once and then fires it
    // from any thread with NOTE_TRIGGER.
    if (ch->filter == EVFILT_USER && (ch->fflags & NOTE_TRIGGER)) {
        r->triggered = true;
        *woke = true;
    }
    return 0;
}

// Fills pfds with the registrations matching `want_suppressed`, merging the
// two filters that can share a descriptor into one entry. Returns the count.
static unsigned kq_build_pollfds(struct kq *q, struct pollfd *pfds,
                                 unsigned max, bool want_suppressed) {
    unsigned n = 0;
    for (size_t i = 0; i < q->n && n < max; i++) {
        struct kq_reg *r = &q->regs[i];
        if (r->filter == EVFILT_USER || !r->enabled)
            continue;
        if (r->suppressed != want_suppressed)
            continue;
        short bit = kq_poll_bit(r->filter);
        unsigned j = 0;
        for (; j < n; j++) {
            if (pfds[j].fd == (int) r->ident) {
                pfds[j].events |= bit;
                break;
            }
        }
        if (j == n) {
            pfds[n].fd = (int) r->ident;
            pfds[n].events = bit;
            pfds[n].revents = 0;
            n++;
        }
    }
    return n;
}

static short kq_revents_for(const struct pollfd *pfds, unsigned n, int fd) {
    for (unsigned i = 0; i < n; i++)
        if (pfds[i].fd == fd)
            return pfds[i].revents;
    return 0;
}

int nlibc_kevent(int kq, const void *changelist, int nchanges,
                 void *eventlist, int nevents, const void *timeout) {
    const struct kevent *changes = changelist;
    struct kevent *events = eventlist;
    const struct timespec *ts = timeout;

    if (nchanges < 0 || nevents < 0) {
        errno = EINVAL;
        return -1;
    }
    struct kq *q = kq_acquire(kq);
    if (q == NULL) {
        KQ_TRACE("kevent(fd=%d): no such queue -> EBADF", kq);
        errno = EBADF;
        return -1;
    }
    KQ_TRACE("kevent(fd=%d) nchanges=%d nevents=%d timeout=%s", kq, nchanges,
             nevents, ts == NULL ? "block" : "set");

    int out = 0;
    bool user_ready = false;
    bool woke = false;

    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < nchanges; i++) {
        int err = kq_apply(q, &changes[i], &woke);
        // EV_RECEIPT asks for one result per change whether or not it failed,
        // which is how mio checks a batch without a second call. A failure
        // with no room to report it is still a failure, so it is returned.
        if (err != 0 || (changes[i].flags & EV_RECEIPT)) {
            if (out < nevents) {
                struct kq_reg tmp = { .ident = changes[i].ident,
                                      .filter = changes[i].filter,
                                      .udata = changes[i].udata };
                kq_emit(&events[out++], &tmp, err, 0, EV_ERROR);
            } else if (err != 0) {
                pthread_mutex_unlock(&q->lock);
                kq_release(q);
                errno = err;
                return -1;
            }
        }
    }
    for (size_t i = 0; i < q->n; i++)
        if (q->regs[i].filter == EVFILT_USER && q->regs[i].triggered)
            user_ready = true;
    size_t nregs = q->n;
    pthread_mutex_unlock(&q->lock);

    // The whole point of EVFILT_USER: a thread that is not this one is asleep
    // in ppoll, and setting a flag it cannot see does not wake it. This byte
    // does. Without it the trigger was recorded correctly and the runtime
    // still hung, because the sleeper had no reason to look.
    if (woke) {
        KQ_TRACE("  waking the queue");
        char one = 1;
        (void) nlibc_write(q->wake_fd, &one, 1);
    }

    if (out >= nevents) {           // receipts filled it; do not also block
        kq_release(q);
        return out;
    }

    // +1 for the queue's own read end, which is how a waker on another thread
    // gets us out of ppoll.
    unsigned cap = (unsigned) nregs + 1;
    struct pollfd *pfds = calloc(cap, sizeof(*pfds));
    if (pfds == NULL) {
        kq_release(q);
        errno = ENOMEM;
        return -1;
    }

    // Pass one, never blocking: has anything held back under EV_CLEAR gone
    // quiet? That is the only signal that the consumer drained it, and until
    // it arrives the descriptor stays out of the blocking set below -- which
    // is what stops a still-ready descriptor from turning ppoll into a spin.
    pthread_mutex_lock(&q->lock);
    unsigned ns = kq_build_pollfds(q, pfds, cap, true);
    pthread_mutex_unlock(&q->lock);
    if (ns > 0) {
        struct timespec zero = { 0, 0 };
        if (nlibc_ppoll(pfds, ns, &zero) >= 0) {
            pthread_mutex_lock(&q->lock);
            for (size_t i = 0; i < q->n; i++) {
                struct kq_reg *r = &q->regs[i];
                if (!r->suppressed || r->filter == EVFILT_USER)
                    continue;
                short rev = kq_revents_for(pfds, ns, (int) r->ident);
                if ((rev & (kq_poll_bit(r->filter) | POLLHUP | POLLERR)) == 0)
                    r->suppressed = false;
            }
            pthread_mutex_unlock(&q->lock);
        }
    }

    // Pass two: the real wait.
    pthread_mutex_lock(&q->lock);
    unsigned n = kq_build_pollfds(q, pfds, cap - 1, false);
    pfds[n].fd = q->fds[0];
    pfds[n].events = POLLIN;
    pfds[n].revents = 0;
    unsigned total = n + 1;
    pthread_mutex_unlock(&q->lock);

    struct timespec zero = { 0, 0 };
    // A trigger that landed before this call must not be made to wait for
    // another one, so the wait collapses to a poll when there is already
    // something to report.
    KQ_TRACE("  wait on %u fd(s), %s", total,
             user_ready ? "no wait (user event pending)"
                        : (ts == NULL ? "no timeout"
                                      : (ts->tv_sec == 0 && ts->tv_nsec == 0 ? "0s" : "timed")));
    int rc = nlibc_ppoll(pfds, total, user_ready ? &zero : ts);
    KQ_TRACE("  wait returned %d (errno %d)", rc, rc < 0 ? errno : 0);
    if (rc < 0 && errno != EINTR) {
        free(pfds);
        kq_release(q);
        return out > 0 ? out : -1;
    }

    pthread_mutex_lock(&q->lock);
    if (pfds[n].revents != 0)
        kq_drain_wake(q);

    for (size_t i = 0; i < q->n && out < nevents; ) {
        struct kq_reg *r = &q->regs[i];
        if (!r->enabled) { i++; continue; }

        if (r->filter == EVFILT_USER) {
            if (r->triggered) {
                kq_emit(&events[out++], r, 0, NOTE_TRIGGER, 0);
                // EVFILT_USER is registered EV_CLEAR by mio; either way a
                // trigger is a one-shot thing and re-arming is the caller's.
                r->triggered = false;
            }
            i++;
            continue;
        }
        if (r->suppressed) { i++; continue; }

        short rev = kq_revents_for(pfds, n, (int) r->ident);
        if (rev == 0) { i++; continue; }
        if (rev & POLLNVAL) {
            // The descriptor went away underneath the registration. Saying so
            // once and dropping it beats reporting it ready forever.
            kq_emit(&events[out++], r, EBADF, 0, EV_ERROR);
            kq_remove(q, r);
            continue;                  // r now holds a different registration
        }
        short want = kq_poll_bit(r->filter);
        bool hup = (rev & (POLLHUP | POLLERR)) != 0;
        if ((rev & want) == 0 && !hup) { i++; continue; }

        // A hangup or a socket error reaches a kqueue consumer as EV_EOF on
        // the readiness event, not as EV_ERROR -- EV_ERROR means the CHANGE
        // failed. mio reads it as read_closed/write_closed.
        kq_emit(&events[out++], r, 0, 0, (uint16_t) (hup ? EV_EOF : 0));
        if (r->flags & EV_ONESHOT) {
            kq_remove(q, r);
            continue;
        }
        if (r->flags & EV_DISPATCH)
            r->enabled = false;
        else if (r->flags & EV_CLEAR)
            r->suppressed = true;
        i++;
    }
    pthread_mutex_unlock(&q->lock);

    free(pfds);
    KQ_TRACE("  -> %d event(s)", out);
    kq_release(q);
    return out;
}

#endif  // !__linux__
