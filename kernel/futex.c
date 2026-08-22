#include "kernel/calls.h"
#include <pthread.h>
#include "futex.h"
#include "kernel/time.h"
#include "util/timer.h"
#include "util/sync.h"
// Apple doesn't implement futex, so we have to fake it
#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_FD_        2 // Deprecated in Linux
#define FUTEX_REQUEUE_ 3
#define FUTEX_CMP_REQUEUE_    4
#define FUTEX_WAKE_OP_        5
#define FUTEX_LOCK_PI_        6
#define FUTEX_UNLOCK_PI_        7
#define FUTEX_TRYLOCK_PI_    8
#define FUTEX_WAIT_BITSET_    9
#define FUTEX_WAKE_BITSET_    10
#define FUTEX_WAIT_REQUEUE_PI_    11
#define FUTEX_CMP_REQUEUE_PI_    12
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CLOCK_REALTIME_    256

#define FUTEX_CMD_MASK_        ~(FUTEX_PRIVATE_FLAG_ | FUTEX_CLOCK_REALTIME_)

// FUTEX_WAKE_OP's encoded op word (linux/futex.h): bits 28-31 are the
// arithmetic op, 24-27 the comparison, 12-23 the (signed) operand, 0-11 the
// (signed) comparison argument.
#define FUTEX_OP_SET_ 0
#define FUTEX_OP_ADD_ 1
#define FUTEX_OP_OR_ 2
#define FUTEX_OP_ANDN_ 3
#define FUTEX_OP_XOR_ 4
#define FUTEX_OP_OPARG_SHIFT_ 8
#define FUTEX_OP_CMP_EQ_ 0
#define FUTEX_OP_CMP_NE_ 1
#define FUTEX_OP_CMP_LT_ 2
#define FUTEX_OP_CMP_LE_ 3
#define FUTEX_OP_CMP_GT_ 4
#define FUTEX_OP_CMP_GE_ 5

#define FUTEX_WAIT_PRIVATE_    (FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_PRIVATE_    (FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_REQUEUE_PRIVATE_    (FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_CMP_REQUEUE_PRIVATE_ (FUTEX_CMP_REQUEUE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_OP_PRIVATE_    (FUTEX_WAKE_OP_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_LOCK_PI_PRIVATE_    (FUTEX_LOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_UNLOCK_PI_PRIVATE_    (FUTEX_UNLOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_TRYLOCK_PI_PRIVATE_ (FUTEX_TRYLOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAIT_BITSET_PRIVATE_    (FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_BITSET_PRIVATE_    (FUTEX_WAKE_BITSET_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAIT_REQUEUE_PI_PRIVATE_    (FUTEX_WAIT_REQUEUE_PI_ | \
                     FUTEX_PRIVATE_FLAG_)
#define FUTEX_CMP_REQUEUE_PI_PRIVATE_    (FUTEX_CMP_REQUEUE_PI_ | \
                     FUTEX_PRIVATE_FLAG_)
//#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_)

extern bool doEnableMulticore;

struct futex {
    atomic_uint refcount;
    struct mem *mem;
    guest_addr_t addr;
    uintptr_t shared_key;
    struct list queue;
    struct list chain; // locked by futex_hash_lock
    // Monotonic wake counter, bumped by every FUTEX_WAKE-like op on this futex
    // (under futex_lock). Used by the SA_RESTART lost-wake fix: a waiter that
    // dequeues for a signal restart snapshots this and, on restart re-entry,
    // treats an advance as "a wake arrived while I was off-queue". Only
    // meaningful while the object is pinned across the restart window.
    uint64_t wake_seq;
};

// A queued futex_wait is a STACK LOCAL of futex_wait_masked whose address is
// published on a shared queue, so every wake path below calls
// pthread_cond_broadcast on another task thread's host stack. That is only
// sound while the waiter is still in that frame. This magic is the check:
// set when the object is built, cleared the instant it leaves the queue and
// its frame is about to die. A waker that finds a queued entry without it is
// looking at a corpse -- see docs/TODO.md's pread_stack_thread_race entry,
// where a stray 8 bytes landing at that exact stack depth is what kills the
// thread later, inside pthread_exit.
#define FUTEX_WAIT_MAGIC 0x0FEEDFACEF00D01ULL

struct futex_wait {
    uint64_t magic;
    cond_t cond;
    struct futex *futex; // The futex on which the thread is waiting
    pthread_t thread;    // The thread that is waiting
    dword_t bitset;      // Match mask for FUTEX_WAIT_BITSET / WAKE_BITSET
    bool interrupted;
    struct list queue;   // For linking in the futex's queue
};

static bool futex_heap_wait_enabled(void) {
    static _Atomic int enabled = -1;
    int e = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (e < 0) {
        const char *v = getenv("ISH_FUTEX_HEAP_WAIT");
        e = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
        atomic_store_explicit(&enabled, e, memory_order_relaxed);
    }
    return e == 1;
}

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

static uintptr_t futex_shared_identity(guest_addr_t addr, guest_addr_t *shared_addr) {
    uintptr_t identity = 0;
    mem_read_lock_quiesce_aware(current->mem);
    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry != NULL && (entry->flags & P_SHARED)) {
        identity = entry->data->shared_key;
        if (identity == 0 && entry->data->fd != NULL)
            identity = (uintptr_t) entry->data->fd;
        if (shared_addr != NULL)
            *shared_addr = entry->offset + PGOFFSET(addr);
    }
    mem_read_unlock_quiesce_aware(current->mem);
    return identity;
}

static struct futex *futex_get_unlocked(guest_addr_t addr, dword_t op) {
    guest_addr_t key_addr = addr;
    uintptr_t shared_key = 0;
    if (!(op & FUTEX_PRIVATE_FLAG_))
        shared_key = futex_shared_identity(addr, &key_addr);

    int hash = (int) (((unsigned long) key_addr ^
            (shared_key != 0 ? shared_key : (uintptr_t) current->mem)) % FUTEX_HASH_SIZE);
    struct list *bucket = &futex_hash[hash];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex->addr == key_addr && futex->shared_key == shared_key &&
                futex->mem == (shared_key != 0 ? NULL : current->mem)) {
            futex->refcount++;
            return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL) {
        unlock(&futex_lock);
        return NULL;
    }
    futex->refcount = 1;
    futex->mem = shared_key != 0 ? NULL : current->mem;
    futex->addr = key_addr;
    futex->shared_key = shared_key;
    futex->wake_seq = 0;
    list_init(&futex->queue);
    list_add(bucket, &futex->chain);
    return futex;
}

// Returns the futex for the current process at the given addr, and locks it
// Unlocked variant is available for times when you need to get two futexes at once
static struct futex *futex_get(guest_addr_t addr, dword_t op) {
    lock(&futex_lock, 0);
    struct futex *futex = futex_get_unlocked(addr, op);
    if (futex == NULL)
        unlock(&futex_lock);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        free(futex);
    }
}

// Must be called on the result of futex_get when you're done with it
// Also has an unlocked version, for releasing the result of futex_get_unlocked
static void futex_put(struct futex *futex) {
    futex_put_unlocked(futex);
    unlock(&futex_lock);
}

static int futex_load(guest_addr_t addr, dword_t *out) {
    // Quiesce-aware, NOT the raw read_lock: this runs while HOLDING the
    // futex bucket lock (futex_wait's atomicity protocol). The raw lock
    // queued this thread inside __psynch_rw_rdlock behind a fork/mmap
    // barrier's registered writer -- stalling every futex op in the process
    // (exiting children block in the clear_tid wake behind futex_lock) for
    // the barrier's whole duration, and parking this thread exactly where
    // the barrier's periodic SIGUSR1 re-pokes keep interrupting the psynch
    // wait (the Darwin rwlock lost-wakeup wedge: cargo's threaded forks +
    // sibling futex waits reproduced writers asleep on a FREE lock).
    // Parking here instead is deadlock-free: the barrier writer never takes
    // futex_lock, so it completes and its release broadcast wakes us.
    mem_read_lock_quiesce_aware(current->mem);
    dword_t *ptr = mem_ptr(current->mem, addr, MEM_READ);
    mem_read_unlock_quiesce_aware(current->mem);
    if (ptr == NULL)
        return 1;
    *out = *ptr;
    return 0;
}

static bool futex_wait_has_pending_signal(void) {
    if (current == NULL)
        return false;
    // Consume any interrupt marker left by a host-side SIGUSR1 poke (mem
    // quiesce while a sibling thread mmaps a stack, for example). A poke is
    // not a guest signal — Linux never returns EINTR from futex without a
    // deliverable signal — so only report one when a signal is actually
    // pending and unblocked. Real deliveries also set wait.interrupted via
    // wake_waiting_task, which the wait loop checks separately.
    __atomic_exchange_n(&current->wait_interrupted, false, __ATOMIC_ACQ_REL);
    lock(&current->sighand->lock, 0);
    bool pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
    unlock(&current->sighand->lock);
    return pending;
}

// ---------------------------------------------------------------------------
// SA_RESTART lost-wake diagnostic ring (enable with ISH_TRACE_FUTEX=1).
//
// The futex_core "signal restart" failure is load-dependent and does NOT
// reproduce on the macOS CLI, so it must be observed on-device -- but a
// per-op printk perturbs the timing and hides the race (observer effect).
// This ring records events into a fixed array behind an atomic index (no I/O
// on the hot path) and only printk-dumps when a long-timeout FUTEX_WAIT (the
// restart's fresh >=1s deadline) hits ETIMEDOUT -- i.e. after the race is
// already lost, so there is zero observer effect on the race itself.
// ---------------------------------------------------------------------------
enum futex_ev_kind { FUTEX_EV_ENTER, FUTEX_EV_EXIT, FUTEX_EV_WAKE, FUTEX_EV_PARK, FUTEX_EV_RESUME };
struct futex_ev {
    uint32_t ms;
    int32_t tid;
    uint64_t uaddr;
    int32_t arg;
    uint8_t kind;
};
#define FUTEX_EV_RING 64
static struct futex_ev futex_ev_ring[FUTEX_EV_RING];
static unsigned futex_ev_idx; // plain; accessed only via __atomic_* builtins

static bool futex_tracing(void) {
    static int enabled = -1;
    int v = __atomic_load_n(&enabled, __ATOMIC_RELAXED);
    if (v < 0) {
        v = getenv("ISH_TRACE_FUTEX") != NULL;
        __atomic_store_n(&enabled, v, __ATOMIC_RELAXED);
    }
    return v != 0;
}

static void futex_trace(enum futex_ev_kind kind, guest_addr_t uaddr, int arg) {
    if (!futex_tracing())
        return;
    struct timespec ts = timespec_now(CLOCK_MONOTONIC);
    unsigned i = __atomic_fetch_add(&futex_ev_idx, 1, __ATOMIC_RELAXED) % FUTEX_EV_RING;
    futex_ev_ring[i] = (struct futex_ev){
        .ms = (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000),
        .tid = current != NULL ? current->pid : -1,
        .uaddr = uaddr,
        .arg = arg,
        .kind = (uint8_t) kind,
    };
}

static void futex_trace_dump(guest_addr_t uaddr) {
    if (!futex_tracing())
        return;
    static const char *const names[] = {"ENTER", "EXIT ", "WAKE ", "PARK ", "RESUM"};
    unsigned end = __atomic_load_n(&futex_ev_idx, __ATOMIC_RELAXED);
    printk("[futex-trace] tid=%d long-wait ETIMEDOUT uaddr=%#llx -- last %d events (oldest first):\n",
           current != NULL ? current->pid : -1, (unsigned long long) uaddr, FUTEX_EV_RING);
    for (unsigned k = 0; k < FUTEX_EV_RING; k++) {
        struct futex_ev *e = &futex_ev_ring[(end + k) % FUTEX_EV_RING];
        if (e->ms == 0 && e->tid == 0)
            continue;
        printk("  %ums tid=%d %s uaddr=%#llx arg=%d\n",
               e->ms, e->tid, names[e->kind < 5 ? e->kind : 0],
               (unsigned long long) e->uaddr, e->arg);
    }
}

// Drop the pinned ref from a parked restart wait, if any (see the
// futex_restart_* fields in struct task). The _locked variant runs with
// futex_lock held; futex_release_restart_park() takes it. Safe when nothing is
// parked. A missed cleanup only leaks one pinned futex object (bounded --
// wake_seq keeps counting harmlessly) and is never a correctness problem: the
// pinned wait is NOT on any queue, so it can neither be woken nor steal a wake.
static void futex_clear_restart_park_locked(void) {
    if (current->futex_restart_futex != NULL) {
        futex_put_unlocked(current->futex_restart_futex);
        current->futex_restart_futex = NULL;
    }
}
void futex_release_restart_park(void) {
    if (current == NULL || current->futex_restart_futex == NULL)
        return;
    lock(&futex_lock, 0);
    futex_clear_restart_park_locked();
    unlock(&futex_lock);
}

static int futex_wait_masked(guest_addr_t uaddr, dword_t op, dword_t val, struct timespec *timeout, dword_t bitset) {
    struct futex *futex = futex_get(uaddr, op);
    if (futex == NULL)
        return _ENOMEM; // futex_get already released futex_lock on alloc failure
    int err = 0;
    futex_trace(FUTEX_EV_ENTER, uaddr, (int) val);

    // SA_RESTART resume: a prior interrupted wait on this same futex parked it
    // (see the park block at the end of this function) -- the object stayed
    // pinned via a held ref, so its wake_seq survived while we ran the signal
    // handler and re-executed the syscall off-queue. futex_get above found
    // that same object and took a *second* ref; drop the redundant one. If
    // wake_seq advanced meanwhile, a FUTEX_WAKE fired for us during the window
    // and would otherwise have been lost -- honor it now. (FUTEX_WAIT is
    // permitted to wake spuriously, so this can never fabricate a stolen wake
    // for another waiter: the wake still dequeued only truly-queued waiters.)
    if (current->futex_restart_futex != NULL) {
        if (current->futex_restart_futex == futex) {
            futex_put_unlocked(futex); // drop the redundant get ref; the pinned ref remains
            uint64_t parked_seq = current->futex_restart_wake_seq;
            current->futex_restart_futex = NULL; // consume the park; `futex` is now our ref
            if (futex->wake_seq != parked_seq) {
                futex_trace(FUTEX_EV_RESUME, uaddr, 1);
                futex_put(futex); // release the pinned ref + unlock
                return 0; // woken by a FUTEX_WAKE during the restart window
            }
            futex_trace(FUTEX_EV_RESUME, uaddr, 0);
            // No wake arrived while parked: re-validate the word and re-block
            // below, reusing the pinned ref (still held, futex_lock still ours).
        } else {
            // The park is for a different futex -- a nested futex called from a
            // signal handler, or a prior EINTR that did not restart to this
            // address. Drop its pinned ref before proceeding.
            futex_clear_restart_park_locked();
        }
    }

    dword_t tmp;
    if (futex_load(uaddr, &tmp))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else {
        const struct timespec wait_slice = {
            .tv_sec = 0,
            .tv_nsec = 50000000,
        };
        struct timespec deadline = {};
        if (timeout != NULL)
            deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), *timeout);
        // ISH_FUTEX_HEAP_WAIT=1: take this object off the stack. It is
        // published on a shared queue and every wake path runs
        // pthread_cond_broadcast over it, so on the stack it aliases the very
        // bytes libpthread uses for that thread's pthread_cond_wait cleanup
        // record -- measured, see docs/TODO.md. The heap copy is deliberately
        // never freed: the point is to give it a lifetime that outlives the
        // frame, so a stale wake lands somewhere harmless. That makes this an
        // A/B for whether the stack lifetime is what kills the thread later,
        // not a fix, and it leaks a few tens of MB over an 8-second stress run.
        struct futex_wait wait_storage;
        struct futex_wait *w = &wait_storage;
        if (futex_heap_wait_enabled()) {
            struct futex_wait *heap = calloc(1, sizeof(*heap));
            if (heap != NULL)
                w = heap;
        }
        *w = (struct futex_wait) {
            .magic = FUTEX_WAIT_MAGIC,
            .cond = COND_INITIALIZER,
        };
        w->futex = futex;
        w->thread = pthread_self();
        w->bitset = bitset;
        list_add_tail(&futex->queue, &w->queue);
        for (;;) {
            struct timespec remaining = wait_slice;
            if (timeout != NULL) {
                remaining = timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC));
                if (!timespec_positive(remaining)) {
                    err = futex_wait_has_pending_signal() ? _EINTR : _ETIMEDOUT;
                    // A long-timeout wait (the SA_RESTART restart re-blocks for
                    // the guest's full timeout, typically >=1s) that reaches its
                    // deadline is the lost-wake symptom -- dump the ring here,
                    // after the race is already decided (zero observer effect).
                    if (err == _ETIMEDOUT && timeout->tv_sec >= 1)
                        futex_trace_dump(uaddr);
                    break;
                }
                if (remaining.tv_sec > wait_slice.tv_sec ||
                        (remaining.tv_sec == wait_slice.tv_sec &&
                         remaining.tv_nsec > wait_slice.tv_nsec))
                    remaining = wait_slice;
            }
            TASK_MAY_BLOCK {
                lock(&current->waiting_cond_lock, 0);
                current->waiting_interrupt_flag = &w->interrupted;
                unlock(&current->waiting_cond_lock);
                should_mark_wait_interrupted = true;
                err = wait_for(&w->cond, &futex_lock, &remaining);
                should_mark_wait_interrupted = false;
            }
            if (__atomic_load_n(&w->interrupted, __ATOMIC_ACQUIRE) || futex_wait_has_pending_signal()) {
                err = _EINTR;
                break;
            }
            // wait_for reported an interrupt but no guest signal is pending
            // and no delivery marked this wait: a host-side poke woke us.
            // Keep waiting instead of leaking a spurious EINTR to the guest.
            if (err == _EINTR)
                continue;
            if (list_null(&w->queue)) {
                // FUTEX_WAKE removed us from the queue. The wake may have
                // fired while we were between wait_for iterations (not
                // sleeping), so the cond notification was lost and the next
                // wait_for timed out. Regardless of what wait_for returned
                // last, the semantic result is "woken" (0), not ETIMEDOUT.
                // Returning ETIMEDOUT here would trigger glibc's
                // futex_fatal_error() from futex_wait_simple().
                err = 0;
                break;
            }
            if (err != _ETIMEDOUT)
                break;
        }
        futex = w->futex;
        list_remove_safe(&w->queue);
        // Off the queue: from here the frame may die at any point, so no waker
        // may touch this object again.
        w->magic = 0;

        if (err == _EINTR) {
            // The wait was interrupted by a signal that may restart the syscall
            // (SA_RESTART). Park across the restart: keep this futex PINNED (do
            // NOT drop the ref) so its wake_seq survives while we are off-queue
            // running the handler + re-executing the syscall, and snapshot
            // wake_seq now. We still hold futex_lock and have already dequeued,
            // so no FUTEX_WAKE can slip in between the dequeue and this
            // snapshot. On the restart, the resume block above compares the
            // snapshot: an advance means a wake fired for us during the window
            // (recovered instead of lost). If the syscall does NOT restart,
            // sys_futex_common drops the park via futex_release_restart_park().
            current->futex_restart_futex = futex;
            current->futex_restart_uaddr = uaddr;
            current->futex_restart_wake_seq = futex->wake_seq;
            futex_trace(FUTEX_EV_PARK, uaddr, 0);
            unlock(&futex_lock); // release the lock but KEEP the pinned ref
            STRACE("%d park futex(FUTEX_WAIT) for restart", current->pid);
            return _EINTR;
        }
    }
    futex_put(futex);
    futex_trace(FUTEX_EV_EXIT, uaddr, err);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return err;
}

static int futex_read_timeout(guest_addr_t timeout_addr, bool time64, struct timespec *timeout) {
    if (!time64) {
        struct timespec_ timeout_guest;
        if (user_get(timeout_addr, timeout_guest))
            return _EFAULT;
        timeout->tv_sec = timeout_guest.sec;
        timeout->tv_nsec = timeout_guest.nsec;
    } else {
        struct timespec64_ timeout_guest;
        if (user_get(timeout_addr, timeout_guest))
            return _EFAULT;
        timeout->tv_sec = timeout_guest.sec;
        timeout->tv_nsec = timeout_guest.nsec;
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)
        return _EINVAL;
    return 0;
}

// Returns true if `wait` still looks like a live queued waiter. A false here
// means a wake was about to run pthread_cond_broadcast over stack that its
// owner has already left, which is a use-after-free of another thread's frame.
static bool futex_wait_is_live(struct futex_wait *wait, const char *where) {
    if (wait != NULL && wait->magic == FUTEX_WAIT_MAGIC)
        return true;
    static _Atomic int reported;
    if (atomic_fetch_add_explicit(&reported, 1, memory_order_relaxed) < 8)
        printk("URGENT: futex %s found a STALE waiter at %p (magic=%#llx, thread=%p): "
               "its frame is gone and notifying it would write into that thread's stack\n",
               where, (void *) wait,
               wait != NULL ? (unsigned long long) wait->magic : 0ULL,
               wait != NULL ? (void *) wait->thread : NULL);
    return false;
}

static int futex_wakelike(int op, guest_addr_t uaddr, dword_t wake_max, dword_t requeue_max,
        guest_addr_t requeue_addr, dword_t wake_mask) {
    struct futex *futex = futex_get(uaddr, op);
    if (futex == NULL)
        return 0; // alloc failure: no futex exists, so nothing is queued to wake

    // Advance the wake counter (under futex_lock) BEFORE walking the queue, so
    // a waiter that dequeued for an SA_RESTART restart and is momentarily
    // off-queue can detect on resume that a wake fired for this futex during
    // its restart window (see futex_wait_masked). Only meaningful while such a
    // waiter keeps the object pinned; otherwise it is a harmless counter bump.
    futex->wake_seq++;

    struct futex_wait *wait, *tmp;
    unsigned woken = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (woken >= wake_max)
            break;
        if ((wait->bitset & wake_mask) == 0)
            continue;
        if (!futex_wait_is_live(wait, "wake")) {
            list_remove(&wait->queue);
            continue;
        }
        notify(&wait->cond);
        list_remove(&wait->queue);
        woken++;
    }

    if ((op & FUTEX_CMD_MASK_) == FUTEX_REQUEUE_) {
        struct futex *futex2 = futex_get_unlocked(requeue_addr, op);
        unsigned requeued = 0;
        list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
            if (requeued >= requeue_max)
                break;
            // sketchy as hell
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            assert(futex->refcount > 1); // should be true because this function keeps a reference
            futex->refcount--;
            futex2->refcount++;
            wait->futex = futex2;
            requeued++;
        }
        futex_put_unlocked(futex2);
        woken += requeued;
    }

    futex_trace(FUTEX_EV_WAKE, uaddr, (int) woken);
    futex_put(futex);
    return woken;
}

int futex_wake(guest_addr_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_, uaddr, wake_max, 0, 0, ~0u);
}

static int32_t futex_op_sign_extend12(uint32_t v) {
    v &= 0xfff;
    if (v & 0x800)
        v |= 0xfffff000;
    return (int32_t) v;
}

// FUTEX_WAKE_OP: atomically apply an op to *uaddr2 (remembering the value it
// held before), wake up to wake_max waiters on uaddr, then -- only if the old
// value at uaddr2 satisfies the encoded comparison -- also wake up to
// wake_max2 waiters on uaddr2. Returns the total woken, or a negative errno.
// Models the two-futex locking futex_cmp_requeue above uses: lock uaddr's
// futex via futex_get (taking the global futex_lock), then uaddr2's via
// futex_get_unlocked (reusing that same lock) -- so the read-modify-write
// below is already serialized against every other futex op, matching how
// futex_load provides atomicity for plain FUTEX_WAIT.
static int futex_wake_op(guest_addr_t uaddr, dword_t wake_max, dword_t wake_max2,
        guest_addr_t uaddr2, dword_t encoded_op) {
    unsigned raw_op = (encoded_op >> 28) & 0xf;
    unsigned cmp = (encoded_op >> 24) & 0xf;
    int32_t oparg = futex_op_sign_extend12(encoded_op >> 12);
    int32_t cmparg = futex_op_sign_extend12(encoded_op);
    bool shift = raw_op & FUTEX_OP_OPARG_SHIFT_;
    unsigned op = raw_op & ~FUTEX_OP_OPARG_SHIFT_;
    if (op > FUTEX_OP_XOR_ || cmp > FUTEX_OP_CMP_GE_)
        return _EINVAL;
    if (shift) {
        if (oparg < 0 || oparg > 31)
            return _EINVAL;
        oparg = 1 << oparg;
    }

    struct futex *futex1 = futex_get(uaddr, FUTEX_WAKE_OP_);
    struct futex *futex2 = futex_get_unlocked(uaddr2, FUTEX_WAKE_OP_);

    mem_read_lock_quiesce_aware(current->mem);
    dword_t *ptr = mem_ptr(current->mem, uaddr2, MEM_WRITE);
    if (ptr == NULL) {
        mem_read_unlock_quiesce_aware(current->mem);
        futex_put_unlocked(futex2);
        futex_put(futex1);
        return _EFAULT;
    }
    int32_t oldval = (int32_t) *ptr;
    int32_t newval;
    switch (op) {
        case FUTEX_OP_SET_:  newval = oparg; break;
        case FUTEX_OP_ADD_:  newval = oldval + oparg; break;
        case FUTEX_OP_OR_:   newval = oldval | oparg; break;
        case FUTEX_OP_ANDN_: newval = oldval & ~oparg; break;
        default: /* FUTEX_OP_XOR_, the only value left after the range check above */
                              newval = oldval ^ oparg; break;
    }
    *ptr = (dword_t) newval;
    mem_read_unlock_quiesce_aware(current->mem);

    unsigned woken = 0;
    struct futex_wait *wait, *tmp;
    list_for_each_entry_safe(&futex1->queue, wait, tmp, queue) {
        if (woken >= wake_max)
            break;
        if (!futex_wait_is_live(wait, "wake_op")) {
            list_remove(&wait->queue);
            continue;
        }
        notify(&wait->cond);
        list_remove(&wait->queue);
        woken++;
    }

    bool cmp_result;
    switch (cmp) {
        case FUTEX_OP_CMP_EQ_: cmp_result = oldval == cmparg; break;
        case FUTEX_OP_CMP_NE_: cmp_result = oldval != cmparg; break;
        case FUTEX_OP_CMP_LT_: cmp_result = oldval < cmparg; break;
        case FUTEX_OP_CMP_LE_: cmp_result = oldval <= cmparg; break;
        case FUTEX_OP_CMP_GT_: cmp_result = oldval > cmparg; break;
        default: /* FUTEX_OP_CMP_GE_, the only value left after the range check above */
                              cmp_result = oldval >= cmparg; break;
    }
    if (cmp_result) {
        unsigned woken2 = 0;
        list_for_each_entry_safe(&futex2->queue, wait, tmp, queue) {
            if (woken2 >= wake_max2)
                break;
            if (!futex_wait_is_live(wait, "wake_op2")) {
                list_remove(&wait->queue);
                continue;
            }
            notify(&wait->cond);
            list_remove(&wait->queue);
            woken2++;
        }
        woken += woken2;
    }

    futex_put_unlocked(futex2);
    futex_put(futex1);
    return (int) woken;
}

static int futex_cmp_requeue(guest_addr_t uaddr1, dword_t op, dword_t val, guest_addr_t uaddr2, dword_t val2,
        dword_t UNUSED(val3)) {
    struct futex *futex1 = futex_get(uaddr1, op);
    struct futex *futex2 = futex_get_unlocked(uaddr2, op);
    int err = 0;
    dword_t tmp;

    if (futex_load(uaddr1, &tmp)) {
        err = _EFAULT;
    } else if (tmp != val) {
        err = _EAGAIN;
    } else {
        struct futex_wait *wait, *tmp_wait;
        dword_t requeued = 0;
        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            if (requeued >= val2) {
                break;
            }
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            wait->futex = futex2;
            requeued++;
        }
        err = requeued;
    }

    futex_put(futex1);
    futex_put_unlocked(futex2);
    return err;
}

// Get the priority of a thread
int get_thread_priority(pthread_t thread) {
    struct sched_param param;
    int policy;
    pthread_getschedparam(thread, &policy, &param);
    return param.sched_priority;
}

// Set the priority of a thread
void set_thread_priority(pthread_t thread, int priority) {
    struct sched_param param;
    int policy;
    pthread_getschedparam(thread, &policy, &param);
    param.sched_priority = priority;
    pthread_setschedparam(thread, policy, &param);
}

static int futex_cmp_requeue_pi(guest_addr_t uaddr1, dword_t op, dword_t val, guest_addr_t uaddr2, dword_t val2,
        dword_t UNUSED(val3)) {
    struct futex *futex1 = futex_get(uaddr1, op);
    struct futex *futex2 = futex_get_unlocked(uaddr2, op);
    int err = 0;
    dword_t tmp;

    if (futex_load(uaddr1, &tmp)) {
        err = _EFAULT;
    } else if (tmp != val) {
        err = _EAGAIN;
    } else {
        struct futex_wait *wait, *tmp_wait;
        int requeued = 0;
        int current_priority = get_thread_priority(pthread_self());
        int highest_waiting_priority = current_priority;

        // Find the highest priority among waiting threads
        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            int wait_priority = get_thread_priority(wait->thread);
            if (wait_priority > highest_waiting_priority) {
                highest_waiting_priority = wait_priority;
            }
        }

        // Inherit the highest priority if necessary
        if (highest_waiting_priority > current_priority) {
            set_thread_priority(pthread_self(), highest_waiting_priority);
        }

        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            if ((dword_t) requeued >= val2) {
                break;
            }

            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            wait->futex = futex2;
            requeued++;
        }

        // Restore original priority
        set_thread_priority(pthread_self(), current_priority);
        err = requeued;
    }

    futex_put(futex1);
    futex_put_unlocked(futex2);
    return err;
}

dword_t sys_futex_common(guest_addr_t uaddr, dword_t op, dword_t val, guest_addr_t timeout_or_val2,
        guest_addr_t uaddr2, dword_t val3, bool timeout_time64) {
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        STRACE("!FUTEX_PRIVATE ");
    }
    struct timespec timeout = {0};
    if (((op & FUTEX_CMD_MASK_) == FUTEX_WAIT_ || (op & FUTEX_CMD_MASK_) == FUTEX_WAIT_BITSET_) && timeout_or_val2) {
        int err = futex_read_timeout(timeout_or_val2, timeout_time64, &timeout);
        if (err < 0)
            return err;
        if ((op & FUTEX_CMD_MASK_) == FUTEX_WAIT_BITSET_) {
            clockid_t clock = (op & FUTEX_CLOCK_REALTIME_) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
            timeout = timespec_subtract(timeout, timespec_now(clock));
            if (!timespec_positive(timeout))
                return _ETIMEDOUT;
        }
    }
    
    switch (op & FUTEX_CMD_MASK_) {
        case FUTEX_WAIT_:
            STRACE("futex(FUTEX_WAIT, %#x, %d, 0x%x {%ds %dns}) = ...\n", uaddr, val, timeout_or_val2, timeout.tv_sec, timeout.tv_nsec);
            dword_t return_val;
            return_val = futex_wait_masked(uaddr, op, val, timeout_or_val2 ? &timeout : NULL, ~0u);
            if ((int) return_val == _EINTR) {
                if (signal_should_restart_syscall())
                    return _ERESTART; // keep the parked wait; the restart resumes it
                futex_release_restart_park(); // EINTR to the guest, no restart: drop the park
            }
            return return_val;
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wakelike(op, uaddr, val, 0, 0, ~0u);
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_wakelike(op, uaddr, val, timeout_or_val2, uaddr2, ~0u);
        case FUTEX_FD_: // Deprecated, little need to support
            STRACE("Unimplemented futex(FUTEX_FD, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_FD) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
            return _ENOSYS;
        case FUTEX_CMP_REQUEUE_:
            STRACE("Unimplemented futex(FUTEX_CMP_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_cmp_requeue(uaddr, op, val, uaddr2, timeout_or_val2, val3);
        case FUTEX_WAKE_OP_:
            STRACE("futex(FUTEX_WAKE_OP, %#x, %d, %d, %#x, %#x)", uaddr, val, timeout_or_val2, uaddr2, val3);
            return futex_wake_op(uaddr, val, timeout_or_val2, uaddr2, val3);
        case FUTEX_LOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_LOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_LOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_LOCK_PI) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
            return _ENOSYS;
        case FUTEX_UNLOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_UNLOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_UNLOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_UNLOCK_PI) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
            return _ENOSYS;
        case FUTEX_TRYLOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_TRYLOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_TRYLOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_TRYLOCK_PI) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
            return _ENOSYS;
        case FUTEX_WAIT_BITSET_:
            STRACE("futex(FUTEX_WAIT_BITSET, %#x, %d, timeout=%#x, bitset=%#x)", uaddr, val, timeout_or_val2, val3);
            if (val3 == 0)
                return _EINVAL;
            {
                dword_t return_val = futex_wait_masked(uaddr, op, val, timeout_or_val2 ? &timeout : NULL, val3);
                if ((int) return_val == _EINTR) {
                    if (signal_should_restart_syscall())
                        return _ERESTART; // keep the parked wait; the restart resumes it
                    futex_release_restart_park(); // EINTR to the guest, no restart: drop the park
                }
                return return_val;
            }
        case FUTEX_WAKE_BITSET_:
            STRACE("futex(FUTEX_WAKE_BITSET, %#x, %d, bitset=%#x)", uaddr, val, val3);
            if (val3 == 0)
                return _EINVAL;
            return futex_wakelike(op, uaddr, val, 0, 0, val3);
        case FUTEX_WAIT_REQUEUE_PI_:
            STRACE("Unimplemented futex(FUTEX_WAIT_REQUEUE_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_WAIT_REQUEUE_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_WAIT_REQUEUE_PI) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
            return _ENOSYS;
        case FUTEX_CMP_REQUEUE_PI_:
            STRACE("Unimplemented futex(FUTEX_CMP_REQUEUE_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_cmp_requeue_pi(uaddr, op, val, uaddr2, timeout_or_val2, val3);
    }
    STRACE("futex(%#x, %d, %d, timeout=%#x, %#x, %d) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
    FIXME("Unsupported futex(%#x, %d, %d, timeout=%#x, %#x, %d) from %s[%d]", uaddr, op, val, timeout_or_val2, uaddr2, val3, current->comm, current->pid);
    return _ENOSYS;
}

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, false);
}

dword_t sys_futex_time64(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, true);
}

static dword_t robust_list_head_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? 24 : 12;
}

static int_t sys_set_robust_list_common(guest_addr_t robust_list, dword_t len, enum guest_abi abi) {
    STRACE("set_robust_list(%#llx, %u)", (unsigned long long) robust_list, len);
    if (len != robust_list_head_size(abi))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

static int_t sys_get_robust_list_common(pid_t_ pid, guest_addr_t robust_list_ptr, guest_addr_t len_ptr,
        enum guest_abi abi) {
    STRACE("get_robust_list(%d, %#llx, %#llx)", pid,
            (unsigned long long) robust_list_ptr, (unsigned long long) len_ptr);

    struct task *task = pid_get_task_ref(pid);
    bool is_current = task == current;
    if (task != NULL)
        task_ref_cnt_mod(task, -1);
    if (!is_current)
        return _EPERM;

    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    if (abi == GUEST_ABI_AMD64) {
        qword_t len = robust_list_head_size(abi);
        if (user_put(len_ptr, len))
            return _EFAULT;
    } else {
        dword_t len = robust_list_head_size(abi);
        if (user_put(len_ptr, len))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_futex_guest(guest_addr_t uaddr, dword_t op, dword_t val, guest_addr_t timeout_or_val2,
        guest_addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, false);
}

dword_t sys_futex_time64_guest(guest_addr_t uaddr, dword_t op, dword_t val, guest_addr_t timeout_or_val2,
        guest_addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, true);
}

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    return sys_set_robust_list_common(robust_list, len, GUEST_ABI_I386);
}

int_t sys_set_robust_list_guest(guest_addr_t robust_list, dword_t len) {
    return sys_set_robust_list_common(robust_list, len, GUEST_ABI_I386);
}

int_t sys_set_robust_list_amd64(addr_t robust_list, dword_t len) {
    return sys_set_robust_list_common(robust_list, len, GUEST_ABI_AMD64);
}

int_t sys_set_robust_list_amd64_guest(guest_addr_t robust_list, dword_t len) {
    return sys_set_robust_list_common(robust_list, len, GUEST_ABI_AMD64);
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    return sys_get_robust_list_common(pid, robust_list_ptr, len_ptr, GUEST_ABI_I386);
}

int_t sys_get_robust_list_guest(pid_t_ pid, guest_addr_t robust_list_ptr, guest_addr_t len_ptr) {
    return sys_get_robust_list_common(pid, robust_list_ptr, len_ptr, GUEST_ABI_I386);
}

int_t sys_get_robust_list_amd64(addr_t pid, addr_t robust_list_ptr, addr_t len_ptr) {
    return sys_get_robust_list_common((pid_t_) pid, robust_list_ptr, len_ptr, GUEST_ABI_AMD64);
}

int_t sys_get_robust_list_amd64_guest(pid_t_ pid, guest_addr_t robust_list_ptr, guest_addr_t len_ptr) {
    return sys_get_robust_list_common(pid, robust_list_ptr, len_ptr, GUEST_ABI_AMD64);
}
