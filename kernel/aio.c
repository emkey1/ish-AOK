// Linux native AIO. See docs/aio_plan.md; the short version is two decisions.
//
// FIRST: what io_setup hands back is not opaque. libaio casts aio_context_t to
// `struct aio_ring *` and reads it in USERSPACE before deciding whether to
// make a syscall at all, so the value has to be a readable guest address or
// libaio faults where the kernel would have returned an error. It does not
// have to be a working ring: aio_ring_is_empty() gives up unless
// ring->magic == AIO_RING_MAGIC, so one zeroed guest page sends every libaio
// caller down the syscall path by construction. That is what makes this
// implementation syscall-only instead of a shared-ring one.
//
// SECOND: io_submit does the I/O synchronously. Nothing requires a completion
// to arrive later -- an event already queued when io_getevents is called is a
// valid outcome, and Linux completes buffered file I/O inline anyway. That
// removes a worker pool and every cross-thread completion race with it, and
// reuses the pread/pwrite path kernel/fs.c already has. The cost is that
// io_submit blocks where a Linux caller expects it to return at once; a
// program that submits from an event loop it cannot afford to block would
// notice, and docs/aio_plan.md has the design for making it asynchronous if
// one ever does.
#include <string.h>

#include "kernel/calls.h"
#include "kernel/aio.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/poll.h"

// The guest ABI. Every field is fixed-width, so unlike almost everything else
// that crosses this boundary these two structs are the SAME on i386, amd64,
// arm64 and riscv64 -- there is no 32/64-bit split to get wrong. Only the
// io_submit pointer array and io_getevents' timespec differ by ABI.
struct iocb_ {
    uint64_t aio_data;
    uint32_t aio_key;
    int32_t  aio_rw_flags;
    uint16_t aio_lio_opcode;
    int16_t  aio_reqprio;
    uint32_t aio_fildes;
    uint64_t aio_buf;
    uint64_t aio_nbytes;
    int64_t  aio_offset;
    uint64_t aio_reserved2;
    uint32_t aio_flags;
    uint32_t aio_resfd;
};
static_assert(sizeof(struct iocb_) == 64, "guest struct iocb is 64 bytes");

struct io_event_ {
    uint64_t data;
    uint64_t obj;
    int64_t  res;
    int64_t  res2;
};
static_assert(sizeof(struct io_event_) == 32, "guest struct io_event is 32 bytes");

#define IOCB_CMD_PREAD_   0
#define IOCB_CMD_PWRITE_  1
#define IOCB_CMD_FSYNC_   2
#define IOCB_CMD_FDSYNC_  3
#define IOCB_CMD_NOOP_    6
#define IOCB_CMD_PREADV_  7
#define IOCB_CMD_PWRITEV_ 8

#define IOCB_FLAG_RESFD_  (1 << 0)

// Linux's default aio-max-nr, and the same reason for it: a guest asking for
// millions of events would have us allocate the ring for them.
#define AIO_MAX_EVENTS 65536

struct aio_ctx {
    guest_addr_t id;              // the decoy page; also the lookup key
    struct tgroup *group;         // who it belongs to, for teardown and lookup
    unsigned max_events;
    lock_t lock;
    cond_t cond;                  // a reaper waits here for min_nr
    struct io_event_ *events;     // ring of completed events
    unsigned head, tail, count;
    // Guarded by aio_list_lock, not by ctx->lock: a put has to be atomic with
    // the lookup that would hand out a new reference.
    unsigned refs;
    bool dead;             // unlinked; wake the reapers and let them go
    struct aio_ctx *next;
};

// One list for every context in the emulator, matched on (id, group) rather
// than id alone: the id is a guest address, and two processes can hold the
// same address without any relationship between them.
//
// The list lock is a leaf -- taken alone, never while holding a context's own
// lock -- so it cannot participate in a cycle with the fd or mem locks that
// the I/O below goes on to take.
static lock_t aio_list_lock = LOCK_INITIALIZER;
static struct aio_ctx *aio_list;

// Reference counted, because io_destroy can land while another thread of the
// same group is inside io_submit or parked in io_getevents on this very
// context -- and freeing it there is a use-after-free of the lock and the cond
// those threads are holding. Submissions being synchronous means there is no
// AIO work in flight to drain; it does NOT mean no thread is inside the calls,
// and an earlier version of this file argued the first to conclude the second.
//
// The list holds one reference. Every caller that looks a context up holds one
// for the duration of its call. The last one out frees it.
static struct aio_ctx *aio_ctx_get(guest_addr_t id) {
    lock(&aio_list_lock, 0);
    struct aio_ctx *ctx = aio_list;
    while (ctx != NULL && (ctx->id != id || ctx->group != current->group))
        ctx = ctx->next;
    if (ctx != NULL)
        ctx->refs++;
    unlock(&aio_list_lock);
    return ctx;
}

static void aio_ctx_free(struct aio_ctx *ctx);

static void aio_ctx_put(struct aio_ctx *ctx) {
    lock(&aio_list_lock, 0);
    bool last = --ctx->refs == 0;
    unlock(&aio_list_lock);
    if (last)
        aio_ctx_free(ctx);
}

// Already unlinked: wake anyone parked in io_getevents so they return rather
// than wait on a context nothing will ever post to again.
static void aio_ctx_retire(struct aio_ctx *ctx) {
    lock(&ctx->lock, 0);
    ctx->dead = true;
    notify(&ctx->cond);
    unlock(&ctx->lock);
}

static void aio_ctx_free(struct aio_ctx *ctx) {
    cond_destroy(&ctx->cond);
    free(ctx->events);
    free(ctx);
}

void aio_discard_tgroup(struct tgroup *group) {
    if (group == NULL)
        return;
    struct aio_ctx *dead = NULL;
    lock(&aio_list_lock, 0);
    struct aio_ctx **pp = &aio_list;
    while (*pp != NULL) {
        struct aio_ctx *ctx = *pp;
        if (ctx->group != group) {
            pp = &ctx->next;
            continue;
        }
        *pp = ctx->next;
        ctx->next = dead;
        dead = ctx;
    }
    unlock(&aio_list_lock);
    // The decoy pages are NOT unmapped here: this runs as the group's address
    // space is going away, and unmapping a page out of a dying mm from the
    // last thread's teardown path would be reaching into something already
    // being dismantled. Only the kernel-side state is ours to free.
    while (dead != NULL) {
        struct aio_ctx *next = dead->next;
        aio_ctx_retire(dead);
        aio_ctx_put(dead);   // the list's reference; frees if nobody else holds one
        dead = next;
    }
}

// ------------------------------------------------------------------ setup

int_t sys_io_setup_guest(uint_t nr_events, guest_addr_t ctx_idp) {
    STRACE("io_setup(%u, %#llx)", nr_events, (unsigned long long) ctx_idp);
    if (nr_events == 0 || nr_events > AIO_MAX_EVENTS)
        return _EINVAL;

    // Linux requires the caller to have zeroed it, and callers rely on the
    // check -- it is how a double io_setup on one variable is caught.
    uint64_t existing = 0;
    if (guest_abi_is_64bit(current->abi)) {
        if (user_get(ctx_idp, existing))
            return _EFAULT;
    } else {
        uint32_t existing32 = 0;
        if (user_get(ctx_idp, existing32))
            return _EFAULT;
        existing = existing32;
    }
    if (existing != 0)
        return _EINVAL;

    struct aio_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return _ENOMEM;
    ctx->events = calloc(nr_events, sizeof(*ctx->events));
    if (ctx->events == NULL) {
        free(ctx);
        return _ENOMEM;
    }
    ctx->max_events = nr_events;
    ctx->group = current->group;
    lock_init(&ctx->lock, "aio_ctx\0");
    cond_init(&ctx->cond);

    // The decoy. Zeroed, so ring->magic is 0 rather than AIO_RING_MAGIC and
    // libaio always falls through to the syscall. Read-only: nothing here ever
    // reads it back, and a guest that writes to it should be told it is not
    // its to write.
    guest_addr_t page = sys_mmap_guest(0, PAGE_SIZE, P_READ,
            MMAP_PRIVATE | MMAP_ANONYMOUS, -1, 0);
    if ((long) page < 0 && (long) page > -0x1000) {
        aio_ctx_free(ctx);
        return (int_t) page;
    }
    ctx->id = page;

    if (guest_abi_is_64bit(current->abi)) {
        uint64_t id = page;
        if (user_put(ctx_idp, id)) {
            sys_munmap_guest(page, PAGE_SIZE);
            aio_ctx_free(ctx);
            return _EFAULT;
        }
    } else {
        uint32_t id = (uint32_t) page;
        if (user_put(ctx_idp, id)) {
            sys_munmap_guest(page, PAGE_SIZE);
            aio_ctx_free(ctx);
            return _EFAULT;
        }
    }

    lock(&aio_list_lock, 0);
    ctx->refs = 1;             // the list's own
    ctx->next = aio_list;
    aio_list = ctx;
    unlock(&aio_list_lock);
    STRACE(" = %#llx", (unsigned long long) page);
    return 0;
}

int_t sys_io_destroy_guest(guest_addr_t ctx_id) {
    STRACE("io_destroy(%#llx)", (unsigned long long) ctx_id);
    lock(&aio_list_lock, 0);
    struct aio_ctx **pp = &aio_list;
    while (*pp != NULL && ((*pp)->id != ctx_id || (*pp)->group != current->group))
        pp = &(*pp)->next;
    struct aio_ctx *ctx = *pp;
    if (ctx != NULL)
        *pp = ctx->next;
    unlock(&aio_list_lock);
    if (ctx == NULL)
        return _EINVAL;

    // Unlinked first, so nothing can look it up to submit into while it is
    // being taken apart; the list's reference is now ours. Threads already
    // inside io_submit or io_getevents still hold their own, so the free
    // happens on whichever of us leaves last -- here, or in their aio_ctx_put.
    aio_ctx_retire(ctx);
    // Only a handle, never dereferenced by the kernel, so unmapping it while
    // another thread still holds the context is safe.
    sys_munmap_guest(ctx->id, PAGE_SIZE);
    aio_ctx_put(ctx);
    return 0;
}

// ----------------------------------------------------------------- submit

// The completed event goes on the ring. Caller holds ctx->lock.
static void aio_push_event(struct aio_ctx *ctx, uint64_t data, uint64_t obj,
                           int64_t res, int64_t res2) {
    if (ctx->count == ctx->max_events)
        return;    // cannot happen: submit refuses past max_events
    struct io_event_ *ev = &ctx->events[ctx->tail];
    ev->data = data;
    ev->obj = obj;
    ev->res = res;
    ev->res2 = res2;
    ctx->tail = (ctx->tail + 1) % ctx->max_events;
    ctx->count++;
}

// IOCB_FLAG_RESFD: add one to an eventfd so a reaper polling it wakes. This is
// what MariaDB's thread pool actually waits on, so it is not decoration.
static void aio_notify_resfd(fd_t resfd) {
    struct fd *fd = f_get(resfd);
    if (fd == NULL)
        return;
    uint64_t one = 1;
    if (fd->ops != NULL && fd->ops->write != NULL)
        fd->ops->write(fd, &one, sizeof(one));
}

// Run one iocb to completion. Returns the value for the event's res field:
// a byte count, or a negative errno. Failures land HERE, in the event, rather
// than failing the submit -- that is where Linux puts them, and a caller that
// checks only the submit return would otherwise never see them.
// Every sys_* below returns a dword_t, so a negative errno arrives as
// 0xfffffff7-style bit patterns. Widening that to a 64-bit io_event.res
// WITHOUT sign-extending turns -EBADF into a ~4GB successful byte count --
// which is exactly what the first run did, and is the worst possible way for
// an AIO error to fail: a caller like MariaDB reads it as a completed write.
// Linux caps a single transfer at 0x7ffff000, so no honest result can collide
// with the sign-extended range.
static inline int64_t aio_widen(dword_t result) {
    return (int64_t) (sdword_t) result;
}

static int64_t aio_run_one(const struct iocb_ *cb) {
    switch (cb->aio_lio_opcode) {
        case IOCB_CMD_NOOP_:
            return 0;
        case IOCB_CMD_PREAD_:
            return aio_widen(sys_pread_guest(cb->aio_fildes, cb->aio_buf,
                                             cb->aio_nbytes, cb->aio_offset));
        case IOCB_CMD_PWRITE_:
            return aio_widen(sys_pwrite_guest(cb->aio_fildes, cb->aio_buf,
                                              cb->aio_nbytes, cb->aio_offset));
        case IOCB_CMD_PREADV_:
            // For the vectored forms aio_buf is the iovec array and aio_nbytes
            // its element COUNT, not a byte count -- the field names are the
            // scalar ones reused.
            return aio_widen(sys_preadv_guest(cb->aio_fildes, cb->aio_buf,
                                              (dword_t) cb->aio_nbytes,
                                              cb->aio_offset));
        case IOCB_CMD_PWRITEV_:
            return aio_widen(sys_pwritev_guest(cb->aio_fildes, cb->aio_buf,
                                               (dword_t) cb->aio_nbytes,
                                               cb->aio_offset));
        case IOCB_CMD_FSYNC_:
        case IOCB_CMD_FDSYNC_:
            // AOK has no separate fdatasync: fsync already writes only what it
            // has, so the weaker promise is satisfied by the stronger one.
            return aio_widen(sys_fsync(cb->aio_fildes));
        default:
            // Including IOCB_CMD_POLL, which needs the asynchronous design.
            return _EINVAL;
    }
}

// Linux validates an iocb BEFORE it queues anything, and reports the failure
// from io_submit itself -- a rejected iocb produces no completion event at
// all. Only errors the transfer itself hits (a short write, EIO) go into
// io_event.res. The distinction is load-bearing: a caller told "1 submitted"
// is entitled to wait for exactly one event, and InnoDB does exactly that.
//
// aio_prep_rw's fget is where the real kernel rejects a bad fd, so an fd
// existence check here is the same gate at the same moment.
static int_t aio_validate(const struct iocb_ *cb) {
    if (cb->aio_reserved2 != 0)
        return _EINVAL;
    switch (cb->aio_lio_opcode) {
        case IOCB_CMD_NOOP_:
        case IOCB_CMD_PREAD_:
        case IOCB_CMD_PWRITE_:
        case IOCB_CMD_PREADV_:
        case IOCB_CMD_PWRITEV_:
        case IOCB_CMD_FSYNC_:
        case IOCB_CMD_FDSYNC_:
            break;
        default:
            // IOCB_CMD_POLL lands here: it needs the asynchronous design, and
            // EINVAL from the submit is how a caller discovers that.
            return _EINVAL;
    }
    if (cb->aio_lio_opcode != IOCB_CMD_NOOP_) {
        struct fd *fd = f_get(cb->aio_fildes);
        if (fd == NULL)
            return _EBADF;
        // aio_read/aio_write check FMODE_READ/FMODE_WRITE and return EBADF,
        // not EPERM -- a read submitted against a write-only fd is refused
        // here rather than deep in the transfer, which is where the caller
        // expects to find out.
        unsigned mode = fd->flags & O_ACCMODE_;
        bool reading = cb->aio_lio_opcode == IOCB_CMD_PREAD_ ||
                       cb->aio_lio_opcode == IOCB_CMD_PREADV_;
        bool writing = cb->aio_lio_opcode == IOCB_CMD_PWRITE_ ||
                       cb->aio_lio_opcode == IOCB_CMD_PWRITEV_;
        if (reading && mode == O_WRONLY_)
            return _EBADF;
        if (writing && mode == O_RDONLY_)
            return _EBADF;
    }
    if ((cb->aio_flags & IOCB_FLAG_RESFD_) && f_get(cb->aio_resfd) == NULL)
        return _EINVAL;
    return 0;
}

int_t sys_io_submit_guest(guest_addr_t ctx_id, sqword_t nr, guest_addr_t iocbpp) {
    STRACE("io_submit(%#llx, %lld, %#llx)", (unsigned long long) ctx_id,
           (long long) nr, (unsigned long long) iocbpp);
    if (nr < 0)
        return _EINVAL;
    struct aio_ctx *ctx = aio_ctx_get(ctx_id);
    if (ctx == NULL)
        return _EINVAL;
    if (nr == 0) {
        aio_ctx_put(ctx);
        return 0;
    }

    bool abi64 = guest_abi_is_64bit(current->abi);
    size_t ptr_size = abi64 ? 8 : 4;
    sqword_t submitted = 0;

    // Whatever stopped the batch, remembered so a failure on the very first
    // iocb can be reported as itself rather than flattened to EAGAIN.
    int_t first_err = 0;

    for (sqword_t i = 0; i < nr; i++) {
        guest_addr_t cb_addr;
        if (abi64) {
            uint64_t p = 0;
            if (user_get(iocbpp + (guest_addr_t) i * ptr_size, p)) {
                first_err = _EFAULT;
                break;
            }
            cb_addr = p;
        } else {
            uint32_t p = 0;
            if (user_get(iocbpp + (guest_addr_t) i * ptr_size, p)) {
                first_err = _EFAULT;
                break;
            }
            cb_addr = p;
        }

        struct iocb_ cb;
        if (user_get(cb_addr, cb)) {
            first_err = _EFAULT;
            break;
        }

        int_t invalid = aio_validate(&cb);
        if (invalid != 0) {
            first_err = invalid;
            break;
        }

        // Room is checked per iocb rather than up front: a short submit is a
        // documented outcome and the caller reaps and retries, where refusing
        // the whole batch would strand work it had every right to queue.
        lock(&ctx->lock, 0);
        // A destroy that landed mid-batch stops it here rather than posting
        // events onto a ring nobody will read.
        bool full = ctx->dead || ctx->count >= ctx->max_events;
        unlock(&ctx->lock);
        if (full) {
            first_err = _EAGAIN;
            break;
        }

        int64_t res = aio_run_one(&cb);

        lock(&ctx->lock, 0);
        aio_push_event(ctx, cb.aio_data, cb_addr, res, 0);
        notify(&ctx->cond);
        unlock(&ctx->lock);

        if (cb.aio_flags & IOCB_FLAG_RESFD_)
            aio_notify_resfd(cb.aio_resfd);
        submitted++;
    }

    // Failing on the FIRST iocb is an error; failing on a later one is a short
    // count. The asymmetry is in the man page and callers depend on it -- one
    // that got 0 back would otherwise spin resubmitting a batch it can never
    // make progress on.
    aio_ctx_put(ctx);
    if (submitted == 0)
        return first_err != 0 ? first_err : _EAGAIN;
    return (int_t) submitted;
}

// --------------------------------------------------------------- getevents

int_t sys_io_getevents_guest(guest_addr_t ctx_id, sqword_t min_nr, sqword_t nr,
                       guest_addr_t events_addr, guest_addr_t timeout_addr) {
    STRACE("io_getevents(%#llx, %lld, %lld, %#llx, %#llx)",
           (unsigned long long) ctx_id, (long long) min_nr, (long long) nr,
           (unsigned long long) events_addr, (unsigned long long) timeout_addr);
    if (nr < 0 || min_nr < 0 || min_nr > nr)
        return _EINVAL;
    struct aio_ctx *ctx = aio_ctx_get(ctx_id);
    if (ctx == NULL)
        return _EINVAL;

    struct timespec timeout_mem;
    struct timespec *timeout = NULL;
    if (timeout_addr != 0) {
        // The 32-bit timespec was read out of a 64-bit guest's buffer, so
        // tv_sec swallowed both halves and tv_nsec came back 0: every
        // sub-second timeout became "no timeout at all" and io_getevents
        // returned immediately instead of waiting. A caller polling an
        // otherwise-idle context with a 500ms budget got a busy loop.
        if (read_guest_timespec_abi(current->abi, timeout_addr, &timeout_mem)) {
            aio_ctx_put(ctx);
            return _EFAULT;
        }
        timeout = &timeout_mem;
    }

    lock(&ctx->lock, 0);
    while (!ctx->dead && ctx->count < (unsigned) min_nr) {
        // A zero timeout means "do not wait", which is not the same as
        // "wait forever" -- passing it to wait_for would do the latter.
        if (timeout != NULL && timeout->tv_sec == 0 && timeout->tv_nsec == 0)
            break;
        int err = wait_for(&ctx->cond, &ctx->lock, timeout);
        if (err == _ETIMEDOUT)
            break;
        if (err < 0) {
            unlock(&ctx->lock);
            aio_ctx_put(ctx);
            // Anything already copied would be lost, so nothing is copied
            // before the wait: EINTR here means no events were consumed.
            return _EINTR;
        }
    }

    sqword_t copied = 0;
    while (copied < nr && ctx->count > 0) {
        struct io_event_ ev = ctx->events[ctx->head];
        if (user_put(events_addr + (guest_addr_t) copied * sizeof(ev), ev)) {
            if (copied == 0) {
                unlock(&ctx->lock);
                aio_ctx_put(ctx);
                return _EFAULT;
            }
            break;   // partial copy: report what did land rather than losing it
        }
        ctx->head = (ctx->head + 1) % ctx->max_events;
        ctx->count--;
        copied++;
    }
    bool destroyed = ctx->dead && copied == 0;
    unlock(&ctx->lock);
    aio_ctx_put(ctx);
    // Destroyed out from under us with nothing left to hand back: the context
    // is gone, which is what EINVAL means here. Events already on the ring are
    // still worth returning, so this only fires once it is drained.
    if (destroyed)
        return _EINVAL;
    STRACE(" = %lld", (long long) copied);
    return (int_t) copied;
}

int_t sys_io_cancel_guest(guest_addr_t ctx_id, guest_addr_t iocb_addr,
                    guest_addr_t result_addr) {
    STRACE("io_cancel(%#llx, %#llx, %#llx)", (unsigned long long) ctx_id,
           (unsigned long long) iocb_addr, (unsigned long long) result_addr);
    struct aio_ctx *ctx = aio_ctx_get(ctx_id);
    if (ctx == NULL)
        return _EINVAL;
    aio_ctx_put(ctx);
    // Submissions complete inside io_submit, so by the time anyone can call
    // this there is nothing left to cancel. EINVAL is also what Linux returns
    // for all but a narrow window, so every caller already handles it.
    return _EINVAL;
}

// The legacy (32-bit-marshalled) entry points, for i386's syscall table.
//
// A 32-bit guest's whole address space fits in a dword, so widening each of
// these handles is lossless there. The 64-bit ABIs must NOT come through
// here: an aio_context_t is the address of the ring page, and on amd64 that
// page lands above 4GB -- the legacy marshaller refused to truncate it and
// SIGSYS-killed io_submit, which is why 206-210 and 0-4 are dispatched
// natively instead. See handle_amd64_native_memory_syscall and
// handle_asm_generic_native_syscall.
int_t sys_io_setup(uint_t nr_events, addr_t ctx_idp) {
    return sys_io_setup_guest(nr_events, ctx_idp);
}
int_t sys_io_destroy(addr_t ctx_id) {
    return sys_io_destroy_guest(ctx_id);
}
int_t sys_io_submit(addr_t ctx_id, sdword_t nr, addr_t iocbpp) {
    return sys_io_submit_guest(ctx_id, nr, iocbpp);
}
int_t sys_io_getevents(addr_t ctx_id, sdword_t min_nr, sdword_t nr,
                       addr_t events_addr, addr_t timeout_addr) {
    return sys_io_getevents_guest(ctx_id, min_nr, nr, events_addr, timeout_addr);
}
int_t sys_io_cancel(addr_t ctx_id, addr_t iocb_addr, addr_t result_addr) {
    return sys_io_cancel_guest(ctx_id, iocb_addr, result_addr);
}
