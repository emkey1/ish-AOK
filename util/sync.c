#include <errno.h>
#include <limits.h>
// signal_thread_unwedge_wake_sigs() below uses pthread_sigmask, sigemptyset and
// SIGUSR1. Darwin pulls <signal.h> in transitively through one of the headers
// here; glibc does not, so on Linux every one of them was an implicit
// declaration or an undeclared identifier. Ours to include, not theirs to leak.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include "kernel/task.h"
#include "kernel/checkpoint.h"
#include <stdio.h>
#include "util/sync.h"
#include "util/timer.h"
#include "debug.h"
#include "kernel/errno.h"
#include <string.h>
#if __APPLE__
#include <stdint.h>
#include <sys/event.h>
#include <unistd.h>
#endif

int noprintk = 0; // Used to suppress calls to printk.

// Gate for the "INFO: wait" short-wait tracing sprinkled through kernel/time.c
// and kernel/poll.c (added for the futex/poll SA_RESTART investigation). Those
// printk()s fire on EVERY short nanosleep/poll/select and each takes the global
// log lock, so on a wait-heavy workload (stress-ng --sleep/--syscall/--schedmix
// spawns hundreds of threads hammering nanosleep) they serialize every task and
// slow the run by ~8x. Keep the traces available for debugging but default them
// off; opt in with ISH_TRACE_WAITS=1. Cached so the hot path is one relaxed load.
bool wait_trace_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        const char *v = getenv("ISH_TRACE_WAITS");
        cached = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}
static int wait_for_internal(cond_t *cond, lock_t *lock, struct timespec *timeout,
        bool interruptible, bool precise);
#if __linux__
static struct timespec timespec_add_local(struct timespec x, struct timespec y) {
    x.tv_sec += y.tv_sec;
    x.tv_nsec += y.tv_nsec;
    if (x.tv_nsec >= 1000000000) {
        x.tv_nsec -= 1000000000;
        x.tv_sec++;
    }
    return x;
}
#endif

#if __APPLE__
// A timed wait's deadline, kept on time.
//
// Darwin coalesces a condition variable's timeout like every other sleep: a
// wait can end a quarter of its length late, up to 5ms, and twice that while
// the user is idle (see host_nanosleep_precise in util/timer.c). So every
// guest wait with a deadline that parks here -- FUTEX_WAIT, and with it every
// pthread_cond_timedwait and sem_timedwait in the guest, sigtimedwait,
// semtimedop, mq_timedreceive -- ran that late: a 40ms futex wait took up to
// 50ms on the Mac and on an M4 iPad, where Linux takes 40.06ms.
//
// A condition variable cannot be told to be punctual, and a thread cannot
// wait on one and on a kqueue at once. So the deadline is kept by a kqueue
// timer marked NOTE_CRITICAL, on one service thread, which wakes the waiter
// by broadcasting its condition at the deadline. The waiter's own timed wait
// stays as it was, as the backstop: if the wake is lost -- the kqueue is
// unavailable, or it fires before the waiter is in the wait -- the wait ends
// as late as it always did, never later. Everyone else waiting on the same
// condition sees one spurious wakeup, which condition variables allow and
// every caller of wait_for already re-checks for.
//
// A deadline_wake is shared by the waiter and the service thread. `cond` is
// cleared by the waiter under `lock` before it leaves the wait, so the
// broadcast, made under the same lock, can never reach a condition whose wait
// is over. `refs` counts the waiter and, until the timer is gone for good, the
// kqueue.
struct deadline_wake {
    pthread_mutex_t lock;
    pthread_cond_t *cond;
    int refs;
};

// Below this a wait is late by no more than Linux's own slack would allow.
#define DEADLINE_WAKE_MIN_NS 200000L

static int deadline_kq = -1;
static pthread_once_t deadline_once = PTHREAD_ONCE_INIT;

static void deadline_wake_put(struct deadline_wake *w, int refs) {
    pthread_mutex_lock(&w->lock);
    w->refs -= refs;
    bool last = w->refs == 0;
    pthread_mutex_unlock(&w->lock);
    if (last) {
        pthread_mutex_destroy(&w->lock);
        free(w);
    }
}

static void *deadline_wake_thread(void *arg) {
    int kq = (int) (intptr_t) arg;
    for (;;) {
        struct kevent64_s fired[16];
        int n = kevent64(kq, NULL, 0, fired, 16, 0, NULL);
        for (int i = 0; i < n; i++) {
            if (fired[i].filter != EVFILT_TIMER)
                continue;
            struct deadline_wake *w = (struct deadline_wake *) (uintptr_t) fired[i].udata;
            pthread_mutex_lock(&w->lock);
            if (w->cond != NULL)
                pthread_cond_broadcast(w->cond);
            pthread_mutex_unlock(&w->lock);
            deadline_wake_put(w, 1);
        }
    }
    return NULL;
}

static void deadline_wake_init(void) {
    int kq = kqueue();
    if (kq < 0)
        return;
    // It takes no guest signals and must not be picked for a host one.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    // Above the guest's own threads (USER_INITIATED, kernel/task.c): a
    // thread made without attributes gets DEFAULT, below them, and a device
    // they keep busy would hold its wakes back to the backstop's lateness.
    // It does a broadcast per wake and nothing else.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t thread;
    int err = pthread_create(&thread, &attr, deadline_wake_thread, (void *) (intptr_t) kq);
    pthread_attr_destroy(&attr);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err != 0) {
        close(kq);
        return;
    }
    pthread_detach(thread);
    deadline_kq = kq;
}

static struct deadline_wake *deadline_wake_arm(pthread_cond_t *cond, const struct timespec *timeout) {
    if (timeout->tv_sec == 0 && timeout->tv_nsec < DEADLINE_WAKE_MIN_NS)
        return NULL;
    pthread_once(&deadline_once, deadline_wake_init);
    if (deadline_kq < 0)
        return NULL;
    struct deadline_wake *w = malloc(sizeof(*w));
    if (w == NULL)
        return NULL;
    pthread_mutex_init(&w->lock, NULL);
    w->cond = cond;
    w->refs = 2;
    // wait_for hands over a slice of a second at most; bound it anyway.
    int64_t ns = timeout->tv_sec > 86400 ? (int64_t) 86400 * 1000000000
        : (int64_t) timeout->tv_sec * 1000000000 + timeout->tv_nsec;
    struct kevent64_s timer;
    EV_SET64(&timer, (uint64_t) (uintptr_t) w, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
             NOTE_NSECONDS | NOTE_CRITICAL | NOTE_LEEWAY, ns, (uint64_t) (uintptr_t) w,
             0, HOST_TIMER_SLACK_NS);
    if (kevent64(deadline_kq, &timer, 1, NULL, 0, 0, NULL) < 0) {
        pthread_mutex_destroy(&w->lock);
        free(w);
        return NULL;
    }
    return w;
}

// Called by the waiter, holding the wait's mutex again, once it is out of the
// wait for whatever reason.
static void deadline_wake_disarm(struct deadline_wake *w) {
    pthread_mutex_lock(&w->lock);
    w->cond = NULL;
    pthread_mutex_unlock(&w->lock);
    // Deleted before it fired, the service thread will never hear of it, and
    // its reference is the waiter's to drop too. Once it has fired, the delete
    // fails and the service thread drops it after its (now empty) broadcast --
    // which is also what keeps the address, the timer's name in the kqueue,
    // from being reused while an event for it is outstanding.
    struct kevent64_s timer;
    EV_SET64(&timer, (uint64_t) (uintptr_t) w, EVFILT_TIMER, EV_DELETE, 0, 0, 0, 0, 0);
    bool deleted = kevent64(deadline_kq, &timer, 1, NULL, 0, 0, NULL) == 0;
    deadline_wake_put(w, deleted ? 2 : 1);
}
#endif

// `precise`: the timeout is the caller's own deadline, not one of wait_for's
// slices, so it is worth keeping on time.
static int cond_wait_with_optional_timeout(cond_t *cond, lock_t *lock, struct timespec *timeout,
        bool precise) {
    if (timeout == NULL) {
        lock->wait4 = true;
        return pthread_cond_wait(&cond->cond, &lock->m);
    }

#if __linux__
    (void) precise;
    struct timespec abs_timeout;
    clock_gettime(CLOCK_MONOTONIC, &abs_timeout);
    abs_timeout = timespec_add_local(abs_timeout, *timeout);
    return pthread_cond_timedwait(&cond->cond, &lock->m, &abs_timeout);
#elif __APPLE__
    struct deadline_wake *w = NULL;
    struct timespec deadline;
    if (precise) {
        deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), *timeout);
        w = deadline_wake_arm(&cond->cond, timeout);
    }
    int rc = pthread_cond_timedwait_relative_np(&cond->cond, &lock->m, timeout);
    if (w != NULL) {
        deadline_wake_disarm(w);
        // Woken by the deadline's own broadcast, most likely: say so, as the
        // timed wait would have. A notify that raced it is found by the
        // caller, which looks at its condition whatever this returns.
        if (rc == 0 && !timespec_positive(timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC))))
            rc = ETIMEDOUT;
    }
    return rc;
#else
#error Unimplemented pthread_cond_wait relative timeout.
#endif
}

void cond_init(cond_t *cond) {
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
#if __linux__
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&cond->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr); // Clean up the condition variable attribute

    // Initialize the mutex without specific attributes
    pthread_mutex_init(&cond->reference.lock, NULL);

    cond->reference.count = 0;
}

void cond_destroy(cond_t *cond) {
    pthread_cond_destroy(&cond->cond);
}

static bool is_signal_pending(lock_t *lock) {
    if (!current)
        return false;
    // A PTRACE_EVENT_STOP the task owes its tracer ends a wait as a deliverable
    // signal does, whatever the mask: the task has to reach the checkpoint
    // where it stops. It is a flag, not a signal (ptrace.trap_stop in
    // kernel/task.h), and task_wake_signal_pending answers for it too.
    if (task_trap_stop_pending(current))
        return true;
    // A process-directed signal (e.g. SIGCHLD delivered via
    // send_signal_to_group, see kernel/exit.c/kernel/signal.c) only ever sets
    // current->sighand->pending, never current->pending -- deliver_signal_to_
    // group_locked has no per-task queue to put it in, only the shared one.
    // Checking current->pending alone here meant sys_rt_sigsuspend_guest's
    // retry loop (`while (wait_for(...) != _EINTR) continue;`) could fail to
    // recognize a real, already-pending group signal as a reason to stop
    // waiting whenever the wake-side race in deliver_signal_to_group_locked
    // didn't manage to mark current->wait_interrupted for it (e.g. it landed
    // in the narrow window between one wait_for call finishing and the next
    // one registering current->waiting_cond) -- the thread would then block
    // again on a fresh pthread_cond_wait with nothing left to ever wake it,
    // even though a zombie was sitting there the whole time. Matches
    // kernel/exit.c's wait_interrupted_by_signal(), which already ORs in
    // sighand->pending for the same reason on the do_wait() path.
    //
    // But only once this thread has been told to take it (task_group_pending):
    // a signal sent to the process ends the wait of the one thread told, not
    // every thread's.
    sigset_t_ pending = __atomic_load_n(&current->pending, __ATOMIC_ACQUIRE);
    sigset_t_ shand_pending = task_group_pending(current);
    // task_wake_blocked, not ->blocked: a native program's handlers are held
    // by the shim with the signal blocked, and such a wait must still end so
    // the handler can run at the next syscall checkpoint (kernel/signal.h).
    sigset_t_ blocked = __atomic_load_n(&current->blocked, __ATOMIC_ACQUIRE) &
        ~__atomic_load_n(&current->native_held, __ATOMIC_ACQUIRE);
    if (((pending | shand_pending) & ~blocked) == 0)
        return false;
    if (lock != &current->sighand->lock)
        lock(&current->sighand->lock, 0);
    bool has_pending = !!((current->pending | task_group_pending(current)) &
            ~task_wake_blocked(current));
    if (lock != &current->sighand->lock)
        unlock(&current->sighand->lock);
    return has_pending;
}

// Public form of is_signal_pending for blocking sites that are not parked in a
// cond_t (kernel/time.c's sleep loop). Same rule -- task_wake_blocked, and the
// shared sighand queue counts too -- so a poll of this cannot disagree with
// what receive_signals() will do at the next syscall checkpoint.
bool task_wake_signal_pending(void) {
    return is_signal_pending(NULL);
}

static bool consume_wait_interrupted(void) {
    if (!current)
        return false;
    return __atomic_exchange_n(&current->wait_interrupted, false, __ATOMIC_ACQ_REL);
}

static bool wait_flag_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("ISH_WAITFLAG_TRACE");
        enabled = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
    }
    return enabled == 1;
}

static bool wait_flag_leak_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("ISH_WAITFLAG_LEAK");
        enabled = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
    }
    return enabled == 1;
}

// `guest_deadline`: the timeout is a deadline the guest asked for, and worth
// ending on time. A caller that caps its own waits passes false for the caps.
static int wait_for_common(cond_t *cond, lock_t *lock, struct timespec *timeout,
        bool guest_deadline) {
    if (consume_wait_interrupted() || is_signal_pending(lock)) {
        // The caller published the address of one of its own STACK locals in
        // current->waiting_interrupt_flag just before calling (kernel/futex.c
        // does, with &wait.interrupted) and is about to get _EINTR back and
        // destroy that frame. wait_for_internal is the ONLY place that pointer
        // is ever cleared, and this early return skips it -- so from here on
        // wake_waiting_task (kernel/signal.c) stores a byte of `true` into a
        // dead stack frame, from another thread, for the rest of this task's
        // life. Measured: it happens on every run of pread_stack_thread_race.
        //
        // What that byte does when it lands is the pread_stack_thread_race
        // SIGSEGV (docs/TODO.md). `interrupted` sits at offset 4 of its
        // eight-byte word, so the stale store always writes byte 4 of some
        // aligned word; when the frame has been reused by libpthread's
        // pthread_cond_wait cleanup record, that word is the record's `__next`
        // and it becomes 0x100000000. The thread dies chasing it inside
        // pthread_exit, on a frame with nothing of ours on it.
        //
        // ISH_WAITFLAG_LEAK=1 restores the old behaviour, so the fix can be
        // A/B'd against itself on one binary rather than argued.
        bool was_set = current != NULL &&
            __atomic_load_n(&current->waiting_interrupt_flag, __ATOMIC_ACQUIRE) != NULL;
        if (current != NULL && !wait_flag_leak_enabled()) {
            lock(&current->waiting_cond_lock, 0);
            current->waiting_interrupt_flag = NULL;
            unlock(&current->waiting_cond_lock);
        }
        // ISH_WAITFLAG_TRACE=1: a positive control for the fix and for the
        // ISH_WAITFLAG_LEAK knob that A/Bs it. "left dangling" appearing means
        // the leak is live; "cleared" means the fix ran. Without this, an A/B
        // whose two arms behave identically looks exactly like an A/B whose
        // subject does not matter. stderr, never printk -- see the comment on
        // that below.
        if (was_set && wait_flag_trace_enabled()) {
            static long seen;
            long n = __atomic_fetch_add(&seen, 1, __ATOMIC_RELAXED);
            if (n < 3)
                fprintf(stderr, "waitflag: early return, waiting_interrupt_flag was set -> %s\n",
                        wait_flag_leak_enabled() ? "LEFT DANGLING" : "cleared");
        }
        return _EINTR;
    }
    // In slices, so a checkpoint freeze cannot be missed.
    //
    // The ways out of a wait are a notify, a signal, and the freezer's wake --
    // and the freezer's wake is host pokes that a device can lose (fs/poll.c
    // and fs/sock.c cap their host waits for the same reason). A wait with no
    // deadline, or a long one, then outlives the freeze: a shell in `wait`
    // (rt_sigsuspend) refused a suspend on an iPad with "did not reach a
    // syscall boundary within 5000ms (blocked in arm64 syscall 133)", while
    // the same shell froze on macOS, where the wake lands.
    //
    // ONE slice per call, then back to the caller. A slice that ends with
    // nothing to report returns 0, a spurious wakeup, and the caller re-checks
    // what it is waiting for -- every caller loops on its own condition, as a
    // condition variable requires. Looping here instead, inside the wait,
    // meant a notify that was missed was missed for good: a native zsh in the
    // app sat in tty_read's wait with 23 bytes of typed input in the buffer,
    // slicing once a second and never looking at the buffer again, and the
    // terminal took no input.
    //
    // A caller's own deadline is kept across its calls: *timeout comes back
    // holding the time left, so a caller that waits again with the same
    // pointer waits only for the remainder, and _ETIMEDOUT is only reported
    // once the real deadline has passed. A freeze reports _EINTR, which is what
    // a wake that lands produces too, and the dispatcher turns it into a
    // restart.
    //
    // Only a wait the freeze is actually holding up reports it. A wait that
    // was notified, or reached its own deadline, answers as it always did --
    // a ZERO timeout included. Asking about the freeze first turned native
    // zsh's describe-yourself-for-the-checkpoint into an endless loop: its
    // signal poll (rt_sigtimedwait with a zero timeout) came back EINTR, the
    // shim re-issued it, and the shell never reached the point where it parks.
    struct timespec slice = {.tv_sec = 1, .tv_nsec = 0};
    bool last_slice = false;
    struct timespec deadline = {0};
    if (timeout != NULL) {
        struct timespec left = timespec_positive(*timeout) ? *timeout : (struct timespec) {0};
        deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), left);
        // Up to a tenth over the slice is waited in one: a slice that is not
        // the last is not kept precise, and may end 10ms late, so it has to
        // leave more than that before the deadline.
        if (left.tv_sec < slice.tv_sec ||
                (left.tv_sec == slice.tv_sec && left.tv_nsec <= 100000000)) {
            slice = left;
            last_slice = true;
        }
    }
    // Only the last slice ends at the caller's deadline; the others are this
    // function's own caps, and nobody minds when they end.
    int err = wait_for_internal(cond, lock, &slice, true, guest_deadline && last_slice);
    if (timeout != NULL) {
        struct timespec left = timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC));
        *timeout = timespec_positive(left) ? left : (struct timespec) {0};
    }
    if (consume_wait_interrupted() || is_signal_pending(lock))
        return _EINTR;
    if (err == 0)
        return 0;
    if (last_slice)
        return _ETIMEDOUT;
    if (checkpoint_freeze_pending())
        return _EINTR;
    return 0;
}

int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    return wait_for_common(cond, lock, timeout, true);
}

int wait_for_capped(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    return wait_for_common(cond, lock, timeout, false);
}

static int wait_for_internal(cond_t *cond, lock_t *lock, struct timespec *timeout,
        bool interruptible, bool precise) {
    if (current) {
        // A voluntary context switch, in the sense getrusage means: the task
        // gave up the CPU to wait for something rather than being preempted.
        // This is the honest place to count it -- TASK_MAY_BLOCK wraps every
        // read and write whether or not it actually sleeps, so counting there
        // would report a switch per syscall.
        current->nvcsw++;
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = cond;
        current->waiting_lock = lock;
        current->waiting_interruptible = interruptible;
        unlock(&current->waiting_cond_lock);
    }
    int rc = 0;
    char saveme[16];
    strncpy(saveme, lock->lname, 16); // Save for later
#if LOCK_DEBUG
    struct lock_debug lock_tmp = lock->debug;
    lock->debug = (struct lock_debug) { .initialized = lock->debug.initialized };
#endif

    if (interruptible && (consume_wait_interrupted() || is_signal_pending(lock)))
        goto out;
    bool old_should_mark_wait_interrupted = should_mark_wait_interrupted;
    if (interruptible)
        should_mark_wait_interrupted = true;
    rc = cond_wait_with_optional_timeout(cond, lock, timeout, precise);
    should_mark_wait_interrupted = old_should_mark_wait_interrupted;
#if LOCK_DEBUG
out:
    lock->debug = lock_tmp;
#else
out:
#endif

    if(current) {
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = NULL;
        current->waiting_lock = NULL;
        current->waiting_interruptible = false;
        current->waiting_interrupt_flag = NULL;
        unlock(&current->waiting_cond_lock);
    }
    lock->wait4 = false;
    if(rc == ETIMEDOUT)
        return _ETIMEDOUT;
    return 0;
}

int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    return wait_for_internal(cond, lock, timeout, false, timeout != NULL);
}

// wait_for, for a wait a guest syscall is blocked in. A bare wait_for gets two
// things wrong there, and both were measured on an arm64 guest.
//
// The task reads as running. /proc/<pid>/stat says 'S' only for a task with
// io_block set (fs/proc/pid.c), and the guest load average counts every task
// without it (guest_count_runnable in kernel/task.c). A child parked in msgrcv,
// msgsnd, semop, semtimedop or io_getevents showed 'R' for as long as it
// waited, where Linux shows 'S'. read, poll, futex and the rest set io_block
// with TASK_MAY_BLOCK; these waits never did.
//
// A poke ends the wait. The address-space barrier (task_poke_shared_mem)
// SIGUSR1s every sibling that is not io_block when a thread maps or unmaps
// memory, and wait_for reports the poke as _EINTR without asking whether any
// signal is pending. So a thread in msgrcv, msgsnd, semop or io_getevents
// failed with EINTR 0-10ms into the wait, with no signal sent, while its
// siblings ran and called mmap. io_block keeps the barrier away, but it can
// read the flag just before this sets it, so the answer is checked as well:
// only a pending signal or a checkpoint freeze is an interruption. Anything
// else is a spurious wakeup and returns 0. The caller re-checks its condition,
// as it must after any wakeup, and waits again -- and that wait does not come
// straight back, because wait_for has already consumed the poke's mark. Same
// two halves as the netlink receive wait in fs/sock.c.
//
// io_block is put back as it was rather than cleared: a FUSE request can be
// made from inside read(2) or write(2), which have already set it.
int wait_for_blocked(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    bool was_blocked = false;
    if (current != NULL) {
        was_blocked = current->io_block;
        current->io_block = true;
    }
    int err = wait_for(cond, lock, timeout);
    if (current != NULL)
        current->io_block = was_blocked;
    if (err == _EINTR && !task_wake_signal_pending() && !checkpoint_freeze_pending())
        return 0;
    return err;
}

void notify(cond_t *cond) {
    pthread_cond_broadcast(&cond->cond);
}
void notify_once(cond_t *cond) {
    pthread_cond_signal(&cond->cond);
}

__thread sigjmp_buf unwind_buf;
__thread bool should_unwind = false;
__thread bool should_mark_wait_interrupted = false;

// Set on a thread once signal_thread_locals_init() has instantiated the
// __thread storage below, so the wake handlers can tell whether reading those
// variables is safe *from a signal handler*. See the comment on
// signal_thread_locals_init(); the key exists because a thread-specific-data
// slot lives inside the pthread struct, so reading it neither allocates nor
// takes a lock, while reading a not-yet-instantiated __thread variable does
// both. A thread that never ran the init reads NULL here, which is exactly the
// right answer: it has no task and nothing for a wake to interrupt.
static pthread_key_t thread_locals_ready_key;
__attribute__((constructor)) static void thread_locals_ready_key_init(void) {
    // A constructor, not pthread_once from the handler: the key must already
    // exist the first time any handler runs, and key 0 is a live slot on
    // Darwin (it holds pthread_self), so an uncreated key would read non-NULL
    // and defeat the whole check.
    pthread_key_create(&thread_locals_ready_key, NULL);
}
static bool thread_locals_ready(void) {
    return pthread_getspecific(thread_locals_ready_key) != NULL;
}

void sigusr1_handler(int UNUSED(sig)) {
    if (!thread_locals_ready())
        return;
    if (should_mark_wait_interrupted && current != NULL)
        __atomic_store_n(&current->wait_interrupted, true, __ATOMIC_RELEASE);
    if (should_unwind) {
        should_unwind = false;
        // NOTHING that reads a __thread variable may be added here. This
        // handler can interrupt a thread that is inside malloc -- pthread_exit
        // freeing its TSD, for instance -- and the FIRST read of an
        // uninstantiated __thread variable goes through dyld's
        // _tlv_get_addr, which mallocs. That re-enters the lock the
        // interrupted code holds and aborts the process in
        // _os_unfair_lock_recursive_abort. The thread_locals_ready() guard at
        // the top of this function only covers the variables
        // signal_thread_locals_init() explicitly instantiates; any new one is
        // a fresh landmine. A canary hook added here cost exactly that.
        siglongjmp(unwind_buf, 1);
    }
}

// The backup poke. signal_wake_task sends this immediately after SIGUSR1
// because SIGUSR1 alone is not reliable: on Darwin a poke is occasionally
// swallowed in a way that leaves SIGUSR1 blocked and pending in the target
// thread's mask with this handler's SIGUSR1 twin never running, and the target
// then sits in its host syscall until it finishes on its own -- ignoring even a
// pending SIGKILL. A second, independent signal gives the wake another way in.
//
// Deliberately weaker than sigusr1_handler: it never unwinds. fs/real.c,
// fs/poll.c and fs/sock.c block SIGUSR1 around the window where they arm
// sigunwind_start(), precisely so no poke can siglongjmp out of it; SIGUSR2 is
// NOT blocked there, so unwinding from here would jump out of exactly the
// window they protect. All this needs to do is make the host syscall return
// EINTR, which every blocking site in the tree already handles by re-checking
// the guest's pending set -- they have to, since a spurious SIGUSR1 can already
// produce the same EINTR today.
//
// (What blocked SIGUSR1 turned out to be another thread's siglongjmp, which on
// Darwin rewrites every thread's mask: see signal_thread_unwedge_wake_sigs.)
void sigusr2_handler(int UNUSED(sig)) {
    if (!thread_locals_ready())
        return;
    if (should_mark_wait_interrupted && current != NULL)
        __atomic_store_n(&current->wait_interrupted, true, __ATOMIC_RELEASE);
}

// Force this thread's thread-local storage for everything the wake handlers
// touch to be instantiated *now*, on a normal call stack where malloc is safe,
// and then mark the thread ready so the handlers will actually read it.
//
// On Darwin the first access to a __thread variable is resolved lazily by
// _tlv_get_addr, which malloc()s storage for it. If a wake signal is delivered
// before that has happened, the handler's own __thread access re-enters malloc
// from async-signal context; if the interrupted code already holds the
// (non-recursive) malloc lock, the process aborts in
// _os_unfair_lock_recursive_abort -- SIGKILL, no core, host exit 137, and on a
// pipe not even the program's buffered output survives. Measured at roughly
// one run in four of tests/manual/pidfd_epoll_deadlock.c, whose 200 rounds of
// fork + 4 threads make guest task threads faster than they can be initialized;
// every crash report was byte-for-byte this stack:
//
//   _os_unfair_lock_recursive_abort <- malloc <- _tlv_get_addr
//     <- sigusr2_handler <- _sigtramp <- malloc <- _tlv_get_addr <- task_thread
//
// Blocking the wake signals until this has run (kernel/task.c task_start) is
// necessary but NOT sufficient, and that was the hole: a task thread was
// measured entering task_thread with SIGUSR2 already unblocked in about 2% of
// creations, and others lost it from the mask later with no handler of ours
// having run on them -- the same Darwin wake-mask weirdness that
// signal_thread_unwedge_wake_sigs() below exists to repair, and since explained
// there: another thread's siglongjmp set every thread's mask, the new one's
// included. That source is gone, but any host sigprocmask does the same, and
// this guard does not depend on there being none. So the handlers
// cannot assume the mask protected them; they check thread_locals_ready()
// instead, which is true only once the instantiation below has finished.
//
// Taking each variable's address forces _tlv_get_addr; the volatile loads keep
// the compiler from eliding the accesses. Every thread that runs guest work or
// can be woken must call this, and it is idempotent so overlapping callers are
// fine: task_thread and timer_thread do it at their own entry,
// task_run_current() covers whichever thread ends up running init (the CLI's
// main thread, the app's boot thread), kernel/init.c does it earlier still for
// the CLI, and nlibc_thread_trampoline covers native-program threads.
void signal_thread_locals_init(void) {
    volatile struct task *const *cur = (volatile struct task *const *) &current;
    volatile bool *unwind = &should_unwind;
    volatile bool *mark = &should_mark_wait_interrupted;
    volatile char *buf = (volatile char *) &unwind_buf;
    (void) *cur;
    (void) *unwind;
    (void) *mark;
    (void) *buf;
    // Last, and only after every one of them exists.
    pthread_setspecific(thread_locals_ready_key, (void *) 1);
}

// Undo the "thread went permanently deaf to its wake poke" state.
//
// A task is woken out of a host blocking call by pthread_kill(thread, SIGUSR1)
// (kernel/signal.c signal_wake_task). On Darwin that poke is occasionally
// swallowed in a way that leaves SIGUSR1 *blocked and pending* in the target
// thread's own host signal mask with sigusr1_handler never having run --
// observed while the thread sat in nanosleep(), across host thread churn from
// concurrent guest fork/exec. It is permanent: the signal is masked, so it is
// never redelivered, and every later poke to that thread is equally deaf.
//
// The cause, found 2026-09-24: nothing swallowed the poke. ANOTHER thread set
// this one's mask. On Darwin sigprocmask sets the mask of every thread in the
// process, and siglongjmp from a buffer saved with its mask calls it -- which
// sigunwind_start's buffer was, saved with SIGUSR1 blocked, so each poke that
// unwound a poll, socket or pipe wait blocked SIGUSR1 in every thread of the
// app. More guest activity meant more unwinds, hence "grows with churn". Those
// buffers no longer save a mask (util/sync.h), native programs' jumps no
// longer restore one (kernel/native_libc.h), and
// tests/manual/wake_mask_isolation.c watches for it coming back. This repair
// and its counters stay, as the instrument that says whether it has.
//
// A thread that finds a wake signal masked when it expected it unblocked can
// repair itself: unblocking delivers the queued signal immediately (the handler
// runs inside this call) and the thread is receptive again. Covers SIGUSR2 as
// well as SIGUSR1, so the two pokes stay independent -- the whole point of
// sending both is that one being swallowed does not take the other with it.
//
// This has to run on a normal call stack, NOT from inside a signal handler: a
// handler's sigreturn restores the mask as it was at delivery, which would put
// the wedged bit straight back.
//
// Returns true if a repair was actually needed, for the caller to count.
bool signal_thread_unwedge_wake_sigs(void) {
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0)
        return false;
    sigset_t stuck;
    sigemptyset(&stuck);
    bool any = false;
    if (sigismember(&mask, SIGUSR1)) {
        sigaddset(&stuck, SIGUSR1);
        any = true;
    }
    if (sigismember(&mask, SIGUSR2)) {
        sigaddset(&stuck, SIGUSR2);
        any = true;
    }
    if (!any)
        return false;
    pthread_sigmask(SIG_UNBLOCK, &stuck, NULL);
    return true;
}

bool signal_thread_wake_sigs_unblocked(void) {
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0)
        return false;
    return !sigismember(&mask, SIGUSR1) && !sigismember(&mask, SIGUSR2);
}

// This is how you would mitigate the unlock/wait race if the wait
// is async signal safe. wait_for *should* be safe from this race
// because of synchronization involving the waiting_cond_lock.
#if 0
    sigset_t sigusr1;
    sigemptyset(&sigusr1);
    sigaddset(&sigusr1, SIGUSR1);

    if (current) {
        if (sigsetjmp(unwind_buf, 0)) {   // never 1: see util/sync.h
            return _EINTR;
        }
        should_unwind = true;
        pthread_sigmask(SIG_BLOCK, &sigusr1, NULL);
        if (lock != &current->sighand->lock)
            lock(&current->sighand->lock, 0);
        bool pending = !!(current->pending & ~task_wake_blocked(current));
        if (lock != &current->sighand->lock)
            unlock(&current->sighand->lock);
        pthread_sigmask(SIG_UNBLOCK, &sigusr1, NULL);
        if (pending) {
            should_unwind = false;
            return _EINTR;
        }
    }
#endif
