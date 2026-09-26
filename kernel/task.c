#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/ipc_ns.h"
#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/task.h"
#include "kernel/seccomp.h"
#include "kernel/checkpoint.h"
#include "kernel/hostinfo.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "jit/jit.h"
#include "platform/platform.h"
#include "util/sync.h"
#include "util/timer.h"
#if defined(__APPLE__)
#include <libkern/OSAtomic.h>
#include <os/proc.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#endif
#include <dlfcn.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>

#define GRACE_PERIOD 2 // How long we want to deallocate tasks that have exited

extern lock_t atomic_l_lock;
time_t boot_time;  // Store the boot time.

// Self-pointing from load, for the same reason as alive_pids_list below: a
// bare global starts NULL/NULL, list_for_each_entry has no NULL check, and
// cleanup_pending_deletions walks this one. init_pending_queues still
// list_inits it, which is a no-op on an already-empty list.
struct list tasks_pending_deletion_queue = LIST_INITIALIZER(tasks_pending_deletion_queue);
pthread_mutex_t tasks_pending_deletion_lock = PTHREAD_MUTEX_INITIALIZER;

int iOSMajorRelease;

bool doEnableMulticore; // Enable multicore if toggled, should default to false
bool isGlibC = false; // Try to guess if we're running a non-musl distro.
// Set from the "Enable Additional Locking" preference, but nothing in the
// kernel reads it any more: the reference counts it used to switch off are
// maintained unconditionally (see task_ref_cnt_mod and mem_ref_cnt_mod).
// Kept so the preference still round-trips; retire the setting to drop it.
bool doEnableExtraLocking;

__thread struct task *current;

static dword_t last_allocated_pid = 0;
static struct pid pids[MAX_PID + 1] = {};

// The pid the allocator wraps at, which /proc/sys/kernel/pid_max sets. The
// table is sized at compile time, so this can only ever be lowered from
// MAX_PID -- but lowering it is what the knob is actually used for (keeping a
// container's pids in a small range), and a knob that refuses every write while
// advertising mode 0644 is worse than one with a ceiling.
static _Atomic dword_t pid_max_value = MAX_PID;

dword_t task_pid_max(void) {
    return atomic_load_explicit(&pid_max_value, memory_order_relaxed);
}

int task_set_pid_max(dword_t value) {
    // Linux's own floor is 301 (PIDS_PER_CPU_MIN); above the table size there
    // is nothing to hand out, so that is the ceiling.
    if (value < 301 || value > MAX_PID)
        return _EINVAL;
    atomic_store_explicit(&pid_max_value, value, memory_order_relaxed);
    return 0;
}
lock_t pids_lock;
// Statically self-pointing, NOT left zeroed for become_first_process to
// list_init (kernel/init.c). A bare global starts as NULL/NULL, and
// list_for_each_entry has no NULL check -- so anything that walked this list
// before the guest booted followed `next` into 0 and faulted at
// 0xfffffffffffffff8 on the FIRST iteration. That is reachable: the app's
// background save calls checkpoint_save_external without requiring a booted
// guest, and the session-resume picker defers the boot until someone answers
// it. ckpt_check_scope does refuse with "there is no guest running", but it
// has to call task_snapshot_collect to learn that, and that call was the
// crash. An empty list must READ as empty before boot, not fault.
// init.c's list_init stays: re-initialising an empty list is a no-op, and it
// still matters if the kernel is ever brought up twice.
struct list alive_pids_list = LIST_INITIALIZER(alive_pids_list);

void init_pending_queues(void) {
// Initialize the pending deletion queues.  Tasks, memory and file descriptors (eventually)
    list_init(&tasks_pending_deletion_queue);
    
}

// Synthetic kernel threads.
//
// A Linux system always has at least one, and their ABSENCE is what several
// programs use to decide they are inside a container -- eudev's init script
// tests `ps ax | egrep '^\['` and refuses to start udevd when nothing matches,
// with the misleading message "eudev does not support containers". AOK is not
// a container: udevd runs perfectly once past that gate (30 devices
// enumerated, trigger and settle both fine). It simply had no kernel threads
// to show.
//
// It is also not a fiction. AOK genuinely runs kernel-side threads -- the
// timer, the netlink watcher, the JIT -- doing kernel work on the guest's
// behalf; they were merely never guest-visible. kthreadd alone is enough for
// the heuristic and is the honest minimum: on Linux it is the one kernel
// thread that always exists and is the parent of the rest. Inventing a
// plausible-looking crowd of others would be claiming more than is true.
//
// pid 2 to match Linux, where kthreadd is always pid 2. The allocator below
// skips it so no real task can ever collide.
struct kthread_entry { dword_t pid; const char *name; };
static const struct kthread_entry kthreads[] = {
    {2, "kthreadd"},
};

bool pid_is_kthread(dword_t pid, const char **name_out) {
    for (size_t i = 0; i < sizeof(kthreads)/sizeof(kthreads[0]); i++) {
        if (kthreads[i].pid == pid) {
            if (name_out != NULL)
                *name_out = kthreads[i].name;
            return true;
        }
    }
    return false;
}

dword_t pid_kthread_at(size_t index) {
    if (index >= sizeof(kthreads)/sizeof(kthreads[0]))
        return 0;
    return kthreads[index].pid;
}

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

_Atomic uint64_t total_forks = 0;

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

struct task *pid_get_task_ref(dword_t id) {
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(id);
    if (task != NULL)
        task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);
    return task;
}

// Same, but a zombie counts as existing. A task that has exited and not yet
// been reaped is still a process: it holds its pid, wait() can still find it,
// and pidfd_open(2) on Linux succeeds for one -- an immediately-readable
// pidfd is how a pidfd reports an exit at all.
struct task *pid_get_task_zombie_ref(dword_t id) {
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL)
        task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);
    return task;
}

// do_exit() takes the exiting task's general_lock and, still holding it, waits
// in exit_wait_needed() for every other reference on the task to go. So a
// caller that holds a reference -- which is every caller reaching another task
// through a pid -- and then blocks on that lock waits for the exit while the
// exit waits for it, and both stop for good: the reader, the exiting process,
// and whoever waits for that process. c0ccaed3 found it in /proc/meminfo and
// /proc/net, fs/sock.c in the socket walks, and /proc/<pid>/* had a dozen more
// copies, one per handler.
//
// Any other holder is an ordinary critical section and is waited out -- a busy
// process must not drop out of a listing, and a zombie, whose lock do_exit
// released, must stay readable even while another reader holds it. Only the
// exiting task holding its OWN lock is the teardown, and then the answer is
// false: whatever the caller wanted (mm, fd table, fs, credentials) is being
// released, and the caller treats it as the process having gone.
static bool task_lock_held_by_its_exit(struct task *task) {
    if (!task->exiting)
        return false;
    // The CLI's first task runs on the main thread and never had task->thread
    // pointed at it, so there is nothing to compare the owner with.
    if (!task->host_thread_started)
        return true;
    return pthread_equal(__atomic_load_n(&task->general_lock.owner, __ATOMIC_RELAXED),
                         task->thread);
}

bool task_lock_unless_exiting(struct task *task) {
    while (trylock(&task->general_lock) != 0) {
        if (task_lock_held_by_its_exit(task))
            return false;
        nanosleep(&lock_pause, NULL);
    }
    return true;
}

void task_snapshot_release(struct task_snapshot *snapshot) {
    for (unsigned i = 0; i < snapshot->count; i++)
        task_ref_cnt_mod(snapshot->tasks[i], -1);
    free(snapshot->tasks);
    snapshot->tasks = NULL;
    snapshot->count = 0;
}

static int task_snapshot_collect_common(struct task_snapshot *snapshot,
        bool leaders_only, bool with_leaving) {
    unsigned cap = 0;
    complex_lockt(&pids_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL)
            continue;
        if (!with_leaving && (task->zombie || task->exiting))
            continue;
        if (leaders_only && !task_is_leader(task))
            continue;
        if (snapshot->count == cap) {
            unsigned new_cap = cap ? cap * 2 : 64;
            struct task **new_tasks = realloc(snapshot->tasks, sizeof(*new_tasks) * new_cap);
            if (new_tasks == NULL) {
                unlock(&pids_lock);
                task_snapshot_release(snapshot);
                return _ENOMEM;
            }
            snapshot->tasks = new_tasks;
            cap = new_cap;
        }
        task_ref_cnt_mod(task, 1);
        snapshot->tasks[snapshot->count++] = task;
    }
    unlock(&pids_lock);
    return 0;
}

int task_snapshot_collect(struct task_snapshot *snapshot, bool leaders_only) {
    return task_snapshot_collect_common(snapshot, leaders_only, false);
}

// Zombies and tasks inside or past do_exit too. A checkpoint needs both: a
// zombie is a status its parent has not collected, and a thread group's
// leader that exited ahead of its threads stays in the pid table as the task
// the group's exit is reported as (kernel/exit.c). The collection above skips
// both -- right for everything else that walks tasks, and it is how a save
// silently lost every zombie in the image.
int task_snapshot_collect_all(struct task_snapshot *snapshot) {
    return task_snapshot_collect_common(snapshot, false, true);
}

struct pid *pid_get_last_allocated(void) {
    if (!last_allocated_pid) {
        return NULL;
    }
    return pid_get(last_allocated_pid);
}

inline void task_ref_cnt_mod(struct task *task, int value) { // value should only be -1 or 1.
    // Keep track of how many threads are referencing this task. This used to
    // be skipped when doEnableExtraLocking was off, but the flag is a live
    // preference toggle: flipping it mid-run left counts taken under one
    // setting and released under the other permanently imbalanced. The count
    // gates task teardown, so it is now maintained unconditionally; as a
    // lock-free atomic it is cheap enough to always be on.
    if(task == NULL) {
        if(current != NULL) {
            task = current;
        } else {
            return;
        }
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid task refcount delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    int old_count = atomic_load_explicit(&task->reference.count, memory_order_relaxed);
    do {
        if(((old_count + value) < 0) && (task->pid > 9)) { // Prevent the count from going negative.
            void *caller = __builtin_return_address(0);
            Dl_info caller_info = {};
            const char *caller_name = "?";
            ptrdiff_t caller_offset = 0;
            if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
                caller_name = caller_info.dli_sname;
                caller_offset = (char *) caller - (char *) caller_info.dli_saddr;
            }
            printk("ERROR: Attempt to decrement task reference count to be negative, ignoring(%s:%d) (%d - %d) caller=%s+%td addr=%p\n",
                   task->comm, task->pid, old_count, value, caller_name, caller_offset, caller);
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(&task->reference.count, &old_count, old_count + value,
                                                    memory_order_acq_rel, memory_order_relaxed));
}

// How many PROCESSES one user has, for RLIMIT_NPROC. Thread group leaders
// only: a threaded process is one process however many host threads it runs.
dword_t task_count_for_uid(uid_t_ uid) {
    dword_t count = 0;
    complex_lockt(&pids_lock, 0);
    for (dword_t id = 1; id < MAX_PID; id++) {
        struct task *task = pid_get_task(id);
        if (task != NULL && task->uid == uid && task->pid == task->tgid)
            count++;
    }
    unlock(&pids_lock);
    return count;
}

dword_t get_count_of_blocked_tasks(void) {
    // task_ref_cnt_mod(current, 1);  // Not needed?
    dword_t res = 0;
    struct pid *pid_entry;
    complex_lockt(&pids_lock, 0);
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        if (pid_entry->task->io_block) {
            res++;
        }
    }
    // task_ref_cnt_mod(current, -1);
    unlock(&pids_lock);
    return res;
}

dword_t get_count_of_alive_tasks(void) {
    complex_lockt(&pids_lock, 0);
    dword_t res = 0;
    struct list *item;
    list_for_each(&alive_pids_list, item) {
        res++;
    }
    unlock(&pids_lock);
    return res;
}

// ---- the guest's clock -----------------------------------------------------
//
// Uptime is the time since the GUEST booted -- kernel/init.c's
// become_first_process, which is what sets boot_time -- measured on a
// monotonic host clock.
//
// It used to be `(gettimeofday().tv_sec - boot_time) * 100`: whole seconds of
// WALL clock. /proc/uptime only ever read N.0, it would have jumped had the
// host clock been set, and everything derived from it inherited the
// one-second steps. Most visibly /proc/stat's idle time, which is capacity
// minus busy time: busy time grows continuously, so between two reads inside
// the same second idle SHRANK. A waybar survey measured idle going backward in
// 65 of 74 samples taken 50 ms apart; native Linux, 0 of 80.
//
// The host clock is the one the guest's own CLOCK_BOOTTIME reads
// (clockid_to_real in kernel/time.c): CLOCK_BOOTTIME where the host has one,
// which is Linux, and CLOCK_MONOTONIC on Darwin, which keeps counting through
// sleep there. Measured on macOS, Darwin's CLOCK_MONOTONIC is gettimeofday()
// minus kern.boottime: across 200000 paired reads REALTIME minus MONOTONIC
// never exceeded kern.boottime and stayed within 22 microseconds of it, the
// width of the gap between the two reads. So it is slewed along with the wall
// clock and not stepped by it, which is the relationship Linux's CLOCK_BOOTTIME
// has to CLOCK_REALTIME. CLOCK_MONOTONIC_RAW is NOT that clock: after 14 days
// of host uptime it was 1.7 s behind CLOCK_MONOTONIC, and btime, which
// fs/proc/root.c derives as "realtime minus uptime", would drift with it.
//
// The origin is placed so that the realtime at uptime zero is boot_time
// exactly, a whole second. That is what keeps btime from flickering: root.c
// computes it as floor(realtime - uptime), which comes out as boot_time for
// every fraction of a second the uptime has run into only if the boot instant
// has no fraction of its own.
#ifdef CLOCK_BOOTTIME
#define GUEST_UPTIME_HOST_CLOCK CLOCK_BOOTTIME
#else
#define GUEST_UPTIME_HOST_CLOCK CLOCK_MONOTONIC
#endif
#define NSEC_PER_SEC_U64 1000000000ull
#define NSEC_PER_TICK 10000000ull   // USER_HZ is 100: /proc/stat's unit

// Uptime is not the only thing the guest measures from its own boot. Linux
// guarantees CLOCK_BOOTTIME and CLOCK_MONOTONIC are boot-relative too --
// CLOCK_BOOTTIME *is* uptime (measured on Devuan 6 / Linux 6.12:
// /proc/uptime and CLOCK_BOOTTIME agreed to within the gap between the two
// reads, and floor(CLOCK_REALTIME - uptime) was btime exactly) -- so every
// boot-relative clock the guest can read needs an origin of its own, placed
// the same way and at the same instant.
//
// Handing the guest the HOST's reading instead leaks the host's uptime, and
// not only as a cosmetic wrong number: anything that reconstructs a wall-clock
// instant as "now minus CLOCK_BOOTTIME" lands that far in the past. util-linux
// dmesg does exactly that in get_boot_time_hires, so `dmesg -T` in a Devuan
// guest on a Mac up 14.6 days printed timestamps 14.6 days early -- Sep 3 for
// records logged on Sep 17. Relative measurements were all fine, which is why
// this survived so long.
//
// They cannot share ONE origin, because the host clocks they ride on do not
// advance together: CLOCK_MONOTONIC stops across host suspend and
// CLOCK_BOOTTIME does not, and CLOCK_MONOTONIC_RAW is unslewed (measured
// 2.04 s behind CLOCK_MONOTONIC at 14.6 days of host uptime). Each gets its
// own, all subtracted
// from the same `since_boot`, which keeps the Linux relation
// MONOTONIC <= BOOTTIME: both read `since_boot` at the rebase instant and
// thereafter advance at their own host clock's rate.
//
// The origins belong to the GUEST's clocks, not to the host clocks under them.
// On Darwin, which has no CLOCK_BOOTTIME, the guest's MONOTONIC and BOOTTIME
// both ride the host's CLOCK_MONOTONIC and used to share one origin -- right
// until a checkpoint is restored, which has to move them apart: MONOTONIC goes
// on from where it was, and BOOTTIME also counts the time the machine spent
// stopped. See guest_clock_resume.
enum guest_clock {
    GUEST_CLOCK_MONOTONIC,
    GUEST_CLOCK_BOOTTIME,
    GUEST_CLOCK_RAW,
    GUEST_CLOCK_COUNT,
};

// The host clock under each, which must be the one clockid_to_real in
// kernel/time.c reads for the same guest clock.
static const clockid_t guest_clock_host_id[GUEST_CLOCK_COUNT] = {
    [GUEST_CLOCK_MONOTONIC] = CLOCK_MONOTONIC,
    [GUEST_CLOCK_BOOTTIME] = GUEST_UPTIME_HOST_CLOCK,
#ifdef CLOCK_MONOTONIC_RAW
    [GUEST_CLOCK_RAW] = CLOCK_MONOTONIC_RAW,
#else
    [GUEST_CLOCK_RAW] = CLOCK_MONOTONIC,
#endif
};

// Which origin a guest clockid is measured from, or -1 for a clock with none
// -- CLOCK_REALTIME and its variants, CLOCK_TAI and the CPU-time clocks, which
// the guest reads straight through.
static int guest_clock_index(uint_t clock) {
    switch (clock) {
        case CLOCK_MONOTONIC_:
        case CLOCK_MONOTONIC_COARSE_:
            return GUEST_CLOCK_MONOTONIC;
        case CLOCK_BOOTTIME_:
        case CLOCK_BOOTTIME_ALARM_:
            return GUEST_CLOCK_BOOTTIME;
        case CLOCK_MONOTONIC_RAW_:
            return GUEST_CLOCK_RAW;
        default:
            return -1;
    }
}

static pthread_mutex_t guest_clock_lock = PTHREAD_MUTEX_INITIALIZER;
// The boot_time the origins below were placed for. boot_time changes when the
// guest boots (run_at_boot seeds it earlier, and a guest can be booted again
// in the same process), and the origins move with it. A re-boot therefore
// restarts the guest's monotonic clocks near zero, which is what a reboot
// means -- and in AOK it happens in become_first_process, before any guest
// task exists to see it. A checkpoint restore sets both itself, under
// guest_clock_lock, and a restore is not a boot.
static _Atomic time_t guest_clock_boot = (time_t) -1;
// Each guest clock's origin: the reading of its host clock, in ns, at which
// the guest clock read zero.
static _Atomic int64_t guest_clock_origin_ns[GUEST_CLOCK_COUNT];

static int64_t timespec_to_ns(struct timespec ts) {
    return (int64_t) ts.tv_sec * (int64_t) NSEC_PER_SEC_U64 + ts.tv_nsec;
}

// Place every origin, once per boot. Called with guest_clock_lock held.
static void guest_clock_place_origins(time_t boot) {
    // Realtime first, then the host clocks: the gap between the reads can then
    // only make uptime smaller, so btime never comes out a second EARLY. The
    // extra microsecond covers Darwin, which reports both clocks in whole
    // microseconds.
    int64_t real = timespec_to_ns(timespec_now(CLOCK_REALTIME));
    int64_t since_boot = real - (int64_t) boot * (int64_t) NSEC_PER_SEC_U64;
    for (int i = 0; i < GUEST_CLOCK_COUNT; i++) {
        int64_t host = timespec_to_ns(timespec_now(guest_clock_host_id[i]));
        // Origin before boot, so a reader that sees the new boot sees it.
        atomic_store_explicit(&guest_clock_origin_ns[i], host - since_boot + 1000,
                              memory_order_release);
    }
    atomic_store_explicit(&guest_clock_boot, boot, memory_order_release);
}

// Rebase one host reading onto the guest's origin for that clock: the host's
// reading minus the origin. A pure offset within a boot, so the result never
// runs backward and never changes rate -- which is what lets the timer layer
// go on working entirely in host time, with only the guest-facing conversions
// below moved.
static int64_t guest_clock_rebase_ns(int idx, int64_t host_ns) {
    // boot_time is written by kernel/init.c without a lock, once per boot;
    // every reader of it reads it like this.
    time_t boot = boot_time;
    if (atomic_load_explicit(&guest_clock_boot, memory_order_acquire) != boot) {
        pthread_mutex_lock(&guest_clock_lock);
        // Read again under the lock. The one taken above can be stale by now:
        // a restore that moved the clocks while this thread waited here set
        // boot_time too, and placing the origins for the old value would put
        // every clock back to a fresh boot's.
        boot = boot_time;
        if (atomic_load_explicit(&guest_clock_boot, memory_order_acquire) != boot)
            guest_clock_place_origins(boot);
        pthread_mutex_unlock(&guest_clock_lock);
    }
    int64_t up = host_ns - atomic_load_explicit(&guest_clock_origin_ns[idx],
                                                memory_order_acquire);
    return up > 0 ? up : 0;
}

static struct timespec ns_to_timespec(int64_t ns) {
    return (struct timespec) {
        .tv_sec = (time_t) (ns / (int64_t) NSEC_PER_SEC_U64),
        .tv_nsec = (long) (ns % (int64_t) NSEC_PER_SEC_U64),
    };
}

struct timespec guest_clock_from_host(uint_t clock, struct timespec host) {
    int idx = guest_clock_index(clock);
    if (idx < 0)
        return host;                        // no boot origin: realtime, cpu clocks
    return ns_to_timespec(guest_clock_rebase_ns(idx, timespec_to_ns(host)));
}

struct timespec guest_clock_now(uint_t clock, clockid_t host_clock) {
    int idx = guest_clock_index(clock);
    if (idx < 0)
        return timespec_now(host_clock);
    // The origin's own host clock, which is what host_clock is for every
    // caller that got it from clockid_to_real.
    return ns_to_timespec(guest_clock_rebase_ns(
        idx, timespec_to_ns(timespec_now(guest_clock_host_id[idx]))));
}

uint64_t guest_uptime_ns(void) {
    // Deliberately the same origin the guest's own CLOCK_BOOTTIME goes
    // through, on the same host clock, so the two are not merely close but
    // identical -- that identity is what makes "realtime minus
    // CLOCK_BOOTTIME", which is what util-linux's dmesg computes, come out as
    // btime.
    return (uint64_t) guest_clock_rebase_ns(
        GUEST_CLOCK_BOOTTIME,
        timespec_to_ns(timespec_now(guest_clock_host_id[GUEST_CLOCK_BOOTTIME])));
}

void guest_clock_read(struct guest_clock_reading *out) {
    int64_t host[GUEST_CLOCK_COUNT];
    for (int i = 0; i < GUEST_CLOCK_COUNT; i++)
        host[i] = timespec_to_ns(timespec_now(guest_clock_host_id[i]));
    // The wall clock LAST, for the reason guest_clock_place_origins reads it
    // first: realtime minus uptime then comes out no earlier than the boot
    // instant, so btime is still boot_time's second when this is resumed.
    int64_t real = timespec_to_ns(timespec_now(CLOCK_REALTIME));
    int64_t guest[GUEST_CLOCK_COUNT];
    for (int i = 0; i < GUEST_CLOCK_COUNT; i++)
        guest[i] = guest_clock_rebase_ns(i, host[i]);
    // After the rebasing, which places the origins if nothing has yet this
    // boot, so this is the boot they were placed for.
    time_t boot = atomic_load_explicit(&guest_clock_boot, memory_order_acquire);
    *out = (struct guest_clock_reading) {
        .monotonic_ns = guest[GUEST_CLOCK_MONOTONIC],
        .boottime_ns = guest[GUEST_CLOCK_BOOTTIME],
        .raw_ns = guest[GUEST_CLOCK_RAW],
        .realtime_ns = real,
        .boot_time = (int64_t) boot,
    };
}

// A restored machine's clocks have to go on from the image, not from zero.
// Its programs hold ABSOLUTE deadlines on them in their own memory, which the
// image carries verbatim: Python's time.sleep is clock_nanosleep(
// CLOCK_MONOTONIC, TIMER_ABSTIME), and so are glibc's CLOCK_MONOTONIC
// condition variables and every event loop's timer wheel. Restarted at zero,
// every one of those deadlines lay "uptime at the save" in the future -- a
// restored Python daemon on the iPad woke a minute late, and after hours of
// uptime it would have been hours. `uptime` read "up 0 min" after every
// restore.
//
// Linux's answer for a machine that stopped and came back is hibernation's,
// and this copies it: MONOTONIC and MONOTONIC_RAW do not count the time the
// machine was stopped, so they continue from their saved values; BOOTTIME does,
// so it also advances by the host's wall-clock time between the save and now.
// That keeps MONOTONIC <= BOOTTIME, keeps uptime (which IS BOOTTIME) from
// ever going backward, and leaves btime -- realtime minus uptime -- exactly
// where it was, as it is after a resume from hibernation. A wall clock that
// went BACKWARD in between counts as no time at all rather than as negative
// time. boot_time comes back too, so everything dated from it still agrees.
//
// The rebasing sites only ever compute "guest now" from these origins, so
// moving them here, before any restored task runs, is all a deadline needs:
// the task that was frozen inside clock_nanosleep re-executes it after the
// restore, and its deadline is interpreted against the continued clock.
int64_t guest_clock_resume(const struct guest_clock_reading *saved) {
    int64_t value[GUEST_CLOCK_COUNT] = {
        [GUEST_CLOCK_MONOTONIC] = saved->monotonic_ns,
        [GUEST_CLOCK_BOOTTIME] = saved->boottime_ns,
        [GUEST_CLOCK_RAW] = saved->raw_ns,
    };
    pthread_mutex_lock(&guest_clock_lock);
    // Realtime first and the host clocks after it, which is the order that
    // keeps btime on its second (see guest_clock_read).
    int64_t stopped = timespec_to_ns(timespec_now(CLOCK_REALTIME)) - saved->realtime_ns;
    if (stopped < 0)
        stopped = 0;
    value[GUEST_CLOCK_BOOTTIME] += stopped;
    for (int i = 0; i < GUEST_CLOCK_COUNT; i++) {
        if (value[i] < 0)
            value[i] = 0;
        int64_t host = timespec_to_ns(timespec_now(guest_clock_host_id[i]));
        atomic_store_explicit(&guest_clock_origin_ns[i], host - value[i],
                              memory_order_release);
    }
    // The key before boot_time, both after the origins: a reader that sees
    // boot_time match the key sees these origins, and one that sees it not
    // match takes this lock and finds them already placed.
    atomic_store_explicit(&guest_clock_boot, (time_t) saved->boot_time,
                          memory_order_release);
    boot_time = (time_t) saved->boot_time;
    pthread_mutex_unlock(&guest_clock_lock);
    return stopped;
}

void guest_clock_restart(time_t boot) {
    pthread_mutex_lock(&guest_clock_lock);
    guest_clock_place_origins(boot);
    boot_time = boot;
    pthread_mutex_unlock(&guest_clock_lock);
}

uint64_t guest_uptime_ticks(void) {
    // USER_HZ ticks, Linux's 100 Hz. (It was rounded to tenths while
    // /proc/uptime printed its hundredths without zero-padding.)
    return guest_uptime_ns() / NSEC_PER_TICK;
}

// ---- the guest's load average ---------------------------------------------
//
// Linux's calc_load over the guest's OWN runnable tasks, so /proc/loadavg and
// sysinfo(2) describe the guest rather than the host.
//
// It read 0.00 under load: after 65 s of one busy loop, still 0.00, where Linux
// reads 0.66. The count was "alive minus io-blocked minus one", the one being
// the reader -- but a task reading /proc/loadavg is inside read(2), and
// TASK_MAY_BLOCK has already marked it io-blocked, so it was subtracted twice
// and one busy task counted as none. sysinfo(2) arrived with its caller NOT
// blocked, so there the same "- 1" was right. guest_count_runnable() now
// leaves out `current` by identity, which is right for both, whether or not
// the caller happens to be marked blocked when it counts.
//
// The average also used to advance only when somebody read it, applying the
// reader's instantaneous count to every 5-second step it had missed. For a
// steady load and a frequent poller that is the same answer, but a load that
// ended just before a read was never seen at all (15 s of a busy loop, then a
// read 2 s after it stopped: 0.00 with the count fixed, where Linux reads the
// decaying average), and a reader that polls every 10 s samples at half
// Linux's rate, and only ever at the instant it wakes. So, as on Linux, a timer
// takes the samples: a detached thread that wakes every 5 s, the same shape as
// the netlink link watcher (fs/sock.c), started when the first process is
// created. A reader still applies an overdue sample itself, so if that thread
// cannot be created, or has not been scheduled yet, the average degrades to
// the old read-driven behaviour rather than freezing.
#define GUEST_LOAD_FSHIFT 11
#define GUEST_LOAD_FIXED_1 (1u << GUEST_LOAD_FSHIFT)
#define GUEST_LOAD_PERIOD_NS (5 * NSEC_PER_SEC_U64)  // Linux's LOAD_FREQ
#define GUEST_LOAD_MAX_STEPS 64

static lock_t guest_load_lock = LOCK_INITIALIZER;
static uint64_t guest_load[3];       // FIXED_1-scaled; under guest_load_lock
static uint64_t guest_load_due_ns;   // uptime of the next sample; 0 = not started
// guest_load_due_ns, readable without the lock by the fast path.
static _Atomic uint64_t guest_load_due_hint;

// Runnable in Linux's sense, as near as AOK can tell: alive, not parked in a
// blocking call, and not the caller. A task sampling the average is running at
// that instant only BECAUSE it is sampling, so counting it would put a floor
// of 1.00 under an idle guest; the timer thread has no `current` and excludes
// nothing. Zombies and exiting tasks stay on alive_pids_list until they are
// reaped, and do not run. Nor does a task parked by a checkpoint freeze
// (checkpoint_park_if_frozen), which waits on a condition variable of its own
// with io_block clear: counted, a guest frozen for a save would have added
// every one of its tasks to the average once the timer took samples on its
// own.
static long guest_count_runnable(void) {
    long count = 0;
    complex_lockt(&pids_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL || task == current || task->io_block ||
                task->zombie || task->exiting ||
                atomic_load_explicit(&task->ckpt_frozen, memory_order_relaxed))
            continue;
        count++;
    }
    unlock(&pids_lock);
    return count;
}

// Linux's calc_load, including the round-up that lets a rising average
// actually reach the count instead of stalling a fraction below it.
static uint64_t guest_calc_load(uint64_t load, uint64_t exp, uint64_t active) {
    uint64_t newload = load * exp + active * (GUEST_LOAD_FIXED_1 - exp);
    if (active >= load)
        newload += GUEST_LOAD_FIXED_1 - 1;
    return newload / GUEST_LOAD_FIXED_1;
}

// Take whatever samples are due. Cheap when none is: one clock read and one
// atomic load, no lock.
static void guest_loadavg_sample(void) {
    static const uint64_t exp[3] = {1884, 2014, 2037}; // exp(-5s/{1,5,15}min) in FIXED_1
    uint64_t now = guest_uptime_ns();
    uint64_t due = atomic_load_explicit(&guest_load_due_hint, memory_order_acquire);
    if (due != 0 && now < due && due - now <= GUEST_LOAD_PERIOD_NS)
        return;

    // Counted before guest_load_lock, which therefore never nests pids_lock.
    uint64_t active = (uint64_t) guest_count_runnable() * GUEST_LOAD_FIXED_1;
    lock(&guest_load_lock, 0);
    // The clock again, under the lock. A `now` read before it could predate a
    // catch-up another caller has just applied, and a schedule that moved past
    // a stale `now` by more than a period looks exactly like a new boot below
    // -- which would zero the average.
    now = guest_uptime_ns();
    if (guest_load_due_ns == 0 || guest_load_due_ns > now + GUEST_LOAD_PERIOD_NS) {
        // The first sample of this boot -- or uptime restarted under a new
        // one, and the old machine's average is not this one's.
        memset(guest_load, 0, sizeof(guest_load));
        guest_load_due_ns = (now / GUEST_LOAD_PERIOD_NS + 1) * GUEST_LOAD_PERIOD_NS;
    } else if (now >= guest_load_due_ns) {
        // Several due at once means the sampler was not running (an app that
        // was suspended, or no sampler at all); like Linux catching up after a
        // tickless idle, apply the present count to each, up to a limit.
        uint64_t steps = 1 + (now - guest_load_due_ns) / GUEST_LOAD_PERIOD_NS;
        uint64_t apply = steps < GUEST_LOAD_MAX_STEPS ? steps : GUEST_LOAD_MAX_STEPS;
        for (uint64_t s = 0; s < apply; s++)
            for (int i = 0; i < 3; i++)
                guest_load[i] = guest_calc_load(guest_load[i], exp[i], active);
        guest_load_due_ns += steps * GUEST_LOAD_PERIOD_NS;
    }
    atomic_store_explicit(&guest_load_due_hint, guest_load_due_ns, memory_order_release);
    unlock(&guest_load_lock);
}

static void *guest_loadavg_thread(void *unused) {
    (void) unused;
    // Darwin names the calling thread and takes only the name; see
    // update_thread_name and kernel/swap.c's kswapd.
#if __APPLE__
    pthread_setname_np("loadavg");
#else
    pthread_setname_np(pthread_self(), "loadavg");
#endif
    for (;;) {
        guest_loadavg_sample();
        uint64_t now = guest_uptime_ns();
        uint64_t due = atomic_load_explicit(&guest_load_due_hint, memory_order_acquire);
        uint64_t wait = due > now ? due - now : 0;
        if (wait > GUEST_LOAD_PERIOD_NS)
            wait = GUEST_LOAD_PERIOD_NS;
        // A millisecond past the boundary, so the sample is due when it looks.
        wait += 1000000;
        struct timespec nap = {
            .tv_sec = (time_t) (wait / NSEC_PER_SEC_U64),
            .tv_nsec = (long) (wait % NSEC_PER_SEC_U64),
        };
        nanosleep(&nap, NULL);
    }
    return NULL;
}

static void guest_loadavg_start(void) {
    static atomic_bool started;
    if (atomic_exchange_explicit(&started, true, memory_order_acq_rel))
        return;
    guest_loadavg_sample();   // starts the schedule at this boot
    pthread_t thread;
    if (pthread_create(&thread, NULL, guest_loadavg_thread, NULL) == 0)
        pthread_detach(thread);
    else
        printk("WARNING: no load average sampler thread; the average advances only when read\n");
}

void get_guest_loadavg(uint64_t out[3]) {
    guest_loadavg_sample();   // nothing to do unless a sample is overdue
    lock(&guest_load_lock, 0);
    for (int i = 0; i < 3; i++)
        out[i] = guest_load[i] << (16 - GUEST_LOAD_FSHIFT);
    unlock(&guest_load_lock);
}

// ---- CPU time counters that never run backward ------------------------------
//
// /proc/stat's user/system/idle must only ever grow, and each line's total
// should grow with the clock. AOK measures busy time (host thread or process
// CPU time) and has no scheduler to measure idle with, so idle is capacity
// minus busy. Two things make that subtraction go negative, and both are
// normal:
//
//  - Sampling skew. A thread's CPU time as another thread reads it advances
//    in steps. Measured on macOS, a busy thread's CPU time grew up to 3.1 ms
//    MORE than the wall clock over the same 50 ms interval (and up to 5.5 ms
//    less).
//  - More work than capacity. A slot here is pid % ncpu, not a real CPU, so
//    two busy tasks can share one; and the aggregate line is process CPU time,
//    which can span more host cores than the guest is told it has.
//
// So each line keeps a ledger. Idle only takes what capacity is left after
// busy, and never gives any back. Busy may run ahead of the capacity left for
// it by CPU_TIME_SLACK_NS -- the skew, which the next read repays -- and
// anything beyond that is DISCARDED, not owed: a slot at 200% for ten seconds
// reads 100% busy, and when it goes idle it reads idle, rather than reading
// 100% busy for ten more seconds while idle waited out the debt.
#define CPU_TIME_SLACK_NS (2 * NSEC_PER_TICK)

struct cpu_time_ledger {
    uint64_t capacity;                     // at the last update
    uint64_t user, system, idle;           // reported; each only grows
    uint64_t dropped_user, dropped_system; // busy time discarded as over capacity
};

static void cpu_time_ledger_update(struct cpu_time_ledger *l, uint64_t capacity,
                                   uint64_t slack, uint64_t raw_user, uint64_t raw_system) {
    // Capacity only goes backward when uptime restarted under a new boot.
    if (capacity < l->capacity)
        *l = (struct cpu_time_ledger) {0};
    l->capacity = capacity;

    uint64_t user = raw_user > l->dropped_user ? raw_user - l->dropped_user : 0;
    uint64_t system = raw_system > l->dropped_system ? raw_system - l->dropped_system : 0;
    // A sample can dip (a thread whose times the host would not report this
    // once); what was already reported stands.
    if (user < l->user)
        user = l->user;
    if (system < l->system)
        system = l->system;

    uint64_t room = (capacity > l->idle ? capacity - l->idle : 0) + slack;
    if (user + system > room) {
        // Only this update's growth is cut, in proportion. What was reported
        // before always fits, because every update leaves
        // user + system + idle <= capacity + slack.
        uint64_t excess = user + system - room;
        uint64_t grew_user = user - l->user, grew_system = system - l->system;
        uint64_t grew = grew_user + grew_system;
        uint64_t cut_user = grew == 0 ? 0 :
            (uint64_t) ((double) excess * ((double) grew_user / (double) grew));
        if (cut_user > grew_user)
            cut_user = grew_user;
        uint64_t cut_system = excess - cut_user;
        if (cut_system > grew_system) {
            cut_user += cut_system - grew_system;
            cut_system = grew_system;
            if (cut_user > grew_user)
                cut_user = grew_user;
        }
        user -= cut_user;
        system -= cut_system;
        l->dropped_user += cut_user;
        l->dropped_system += cut_system;
    }

    uint64_t idle = capacity > user + system ? capacity - user - system : 0;
    if (idle < l->idle)
        idle = l->idle;
    l->user = user;
    l->system = system;
    l->idle = idle;
}

static void cpu_time_ledger_ticks(const struct cpu_time_ledger *l, struct cpu_usage *out) {
    out->user_ticks = l->user / NSEC_PER_TICK;
    out->system_ticks = l->system / NSEC_PER_TICK;
    out->idle_ticks = l->idle / NSEC_PER_TICK;
    out->nice_ticks = 0;
}

static lock_t cpu_total_lock = LOCK_INITIALIZER;
static struct cpu_time_ledger cpu_total_ledger;   // under cpu_total_lock

void guest_cpu_usage_total(uint64_t user_ns, uint64_t system_ns, struct cpu_usage *out) {
    int ncpu = get_cpu_count();
    if (ncpu < 1)
        ncpu = 1;
    uint64_t capacity = (uint64_t) ncpu * guest_uptime_ns();
    lock(&cpu_total_lock, 0);
    cpu_time_ledger_update(&cpu_total_ledger, capacity, (uint64_t) ncpu * CPU_TIME_SLACK_NS,
                           user_ns, system_ns);
    cpu_time_ledger_ticks(&cpu_total_ledger, out);
    unlock(&cpu_total_lock);
}

// ---- per-emulated-CPU time accounting (for /proc/stat's cpuN lines) --------
//
// iSH has no real CPU affinity: every guest task is a host pthread that the
// host kernel schedules wherever it likes, so "which emulated CPU did the
// work" has no ground truth. Define it here instead: each task is bucketed
// into virtual-CPU slot pid % ncpu and its REAL thread CPU time is charged to
// that slot -- live tasks are sampled on each /proc/stat read, and do_exit
// banks a task's final time so short-lived processes (compilers!) stay
// visible in the counters. This replaces the old even split of the process
// total, which made every cpuN line identical and diluted a busy task's usage
// by 1/ncpu. The slot sums won't exactly match the aggregate "cpu" line
// (which uses process-wide accounting including non-task host threads), and a
// slot holding several busy tasks can saturate at 100%; both are acceptable
// for what these lines are for (top/htop-style meters).

#define CPU_SLOTS_MAX 64
// Nanoseconds, like everything the ledgers above work in. They were ticks, and
// every exiting task gave up the fraction of a tick it had run past its last
// whole one -- up to 10 ms each, which a compile's thousands of short-lived
// processes turn into seconds of busy time that never appeared.
static _Atomic uint64_t cpu_slot_dead_user[CPU_SLOTS_MAX];
static _Atomic uint64_t cpu_slot_dead_system[CPU_SLOTS_MAX];
static struct cpu_time_ledger cpu_slot_ledger[CPU_SLOTS_MAX];   // under cpu_slots_lock

// Serializes banking against the /proc/stat reader. Without this, a task
// exiting mid-read could be counted twice in one snapshot: sampled live
// during the reader's task-list walk, then ALSO included via the dead-slot
// totals it banked before the reader loaded them. That read reports an
// inflated cpuN value and the next read drops back down -- a backward-moving
// /proc/stat counter, which real kernels never produce and which top-style
// tools turn into a huge unsigned delta (ktop crashed on exactly this).
// Lock order: pids_lock -> cpu_slots_lock (the reader); do_exit's banking
// takes only cpu_slots_lock.
static lock_t cpu_slots_lock = LOCK_INITIALIZER;

static int task_cpu_slot(struct task *task, int ncpu) {
    if (ncpu > CPU_SLOTS_MAX)
        ncpu = CPU_SLOTS_MAX;
    if (ncpu < 1)
        ncpu = 1;
    return (int) (task->pid % (dword_t) ncpu);
}

void task_thread_cpu_time_ns(struct task *task, uint64_t *user_ns, uint64_t *system_ns) {
    *user_ns = 0;
    *system_ns = 0;
#ifdef __APPLE__
    mach_port_t mach_thread = pthread_mach_thread_np(task->thread);
    if (mach_thread != MACH_PORT_NULL) {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(mach_thread, THREAD_BASIC_INFO,
                        (thread_info_t) &info, &count) == KERN_SUCCESS) {
            *user_ns = (uint64_t) info.user_time.seconds * NSEC_PER_SEC_U64
                       + (uint64_t) info.user_time.microseconds * 1000;
            *system_ns = (uint64_t) info.system_time.seconds * NSEC_PER_SEC_U64
                         + (uint64_t) info.system_time.microseconds * 1000;
        }
    }
#else
    clockid_t clkid;
    if (pthread_getcpuclockid(task->thread, &clkid) == 0) {
        struct timespec ts;
        if (clock_gettime(clkid, &ts) == 0)
            *user_ns = (uint64_t) ts.tv_sec * NSEC_PER_SEC_U64 + (uint64_t) ts.tv_nsec;
    }
#endif
}

void task_bank_cpu_time(struct task *task) {
    lock(&cpu_slots_lock, 0);
    if (!task->cpu_time_banked) {
        uint64_t user_ns, system_ns;
        task_thread_cpu_time_ns(task, &user_ns, &system_ns);
        int slot = task_cpu_slot(task, get_cpu_count_allowed());
        atomic_fetch_add(&cpu_slot_dead_user[slot], user_ns);
        atomic_fetch_add(&cpu_slot_dead_system[slot], system_ns);
        task->cpu_time_banked = true;
    }
    unlock(&cpu_slots_lock);
}

int get_emulated_per_cpu_usage(struct cpu_usage **cpus_usage) {
    int ncpu = get_cpu_count();
    if (ncpu < 1)
        ncpu = 1;
    struct cpu_usage *cpus = calloc((size_t) ncpu, sizeof(*cpus));
    if (cpus == NULL)
        return _ENOMEM;
    uint64_t busy_user[CPU_SLOTS_MAX] = {0}, busy_system[CPU_SLOTS_MAX] = {0};

    complex_lockt(&pids_lock, 0);
    // Hold cpu_slots_lock across BOTH the live walk and the dead-slot loads:
    // an exiting task then either banked before this snapshot (skipped live,
    // counted dead) or banks after it (counted live now, dead next time) --
    // never both in one read, so the reported counters stay monotonic.
    lock(&cpu_slots_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        // A banked task's time is already in the dead-slot totals; its host
        // thread may be gone, so don't sample it (and don't double-count).
        // A task whose own host thread hasn't started yet still carries its
        // PARENT's pthread from the task_create_ struct copy -- sampling it
        // would charge the parent's whole CPU time to this task's slot, then
        // retract it on the next read (backward counters).
        if (task == NULL || task->cpu_time_banked || !task->host_thread_started)
            continue;
        uint64_t user_ns, system_ns;
        task_thread_cpu_time_ns(task, &user_ns, &system_ns);
        int slot = task_cpu_slot(task, get_cpu_count_allowed());
        busy_user[slot] += user_ns;
        busy_system[slot] += system_ns;
    }

    // Each virtual CPU has the guest's uptime of capacity; whatever its tasks
    // did not use was idle. Read after the walk, so it covers every sample the
    // walk took. The ledgers keep each field from ever running backward.
    uint64_t capacity = guest_uptime_ns();
    for (int i = 0; i < ncpu; i++) {
        if (i >= CPU_SLOTS_MAX) {
            // task_cpu_slot never charges a task here: idle all along.
            cpus[i].idle_ticks = capacity / NSEC_PER_TICK;
            continue;
        }
        busy_user[i] += atomic_load(&cpu_slot_dead_user[i]);
        busy_system[i] += atomic_load(&cpu_slot_dead_system[i]);
        cpu_time_ledger_update(&cpu_slot_ledger[i], capacity, CPU_TIME_SLACK_NS,
                               busy_user[i], busy_system[i]);
        cpu_time_ledger_ticks(&cpu_slot_ledger[i], &cpus[i]);
    }
    unlock(&cpu_slots_lock);
    unlock(&pids_lock);
    *cpus_usage = cpus;
    return 0;
}

static struct task *task_create_pid_(struct task *parent, pid_t_ want_pid) {
    struct task *task = malloc(sizeof(struct task));
    if (task == NULL)
        return NULL;

    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent; // uts_ns and ipc_ns are only aliased here; copy_task retains or copies them
    else {
        task->uts_ns = uts_ns_retain(&init_uts_ns);
        task->ipc_ns = ipc_ns_retain(&init_ipc_ns);
        // init starts as Linux's does: every capability permitted, effective
        // and in the bounding set, and nothing inheritable or ambient
        // (/proc/1/status on 6.12: CapInh 0, CapPrm/CapEff/CapBnd full). The
        // inheritable set used to be full too, which would hand anything a
        // file lists as inheritable (+i) to every process descended from init.
        task->abi = GUEST_ABI_I386;
        task->cap_effective[0] = task->cap_permitted[0] =
            task->cap_bounding[0] = CAP_FULL_LOW_;
        task->cap_effective[1] = task->cap_permitted[1] =
            task->cap_bounding[1] = CAP_FULL_HIGH_;
        task->cap_inheritable[0] = task->cap_inheritable[1] = 0;
    }
    // NOT inherited: which native program is running, and what it said about
    // itself.
    //
    // `*task = *parent` above copies both, and native_running is set only
    // while prog->main() is on the stack -- so a child forked by a native
    // shell came out claiming to BE that shell. kernel/checkpoint.c then asked
    // it for zsh's state, on a task that has none, and read zsh's trap table
    // through a null pointer: EXC_BAD_ACCESS at address 0x84, on the thread of
    // an emulated /bin/sh. The state pointer would have been a double free by
    // the same route.
    task->ckpt_restored = false;
    task->ckpt_syscalls_traced = 0;
    task->native_running = NULL;
    task->ckpt_native_state = NULL;

    // Every task's poke flag is its OWN, from the moment it exists.
    //
    // poked_ptr points into its own cpu_state (&cpu->_poked), and each engine's
    // entry sets it -- but `*task = *parent` above copies the parent's whole
    // cpu_state, POINTER INCLUDED, so until the child first entered the
    // emulator its "stop executing" flag was its parent's. A task whose image
    // is a native program never enters the emulator at all, so its flag stayed
    // NULL and poking it dereferenced NULL: an app that vanished rather than a
    // guest that reported anything.
    task->cpu.poked_ptr = &task->cpu._poked;
    task->cpu._poked = false;
    task->prof_slot = -1; // never inherited; see the field's comment
    task->cpu_time_banked = false; // per-task, not inherited via the parent copy
    task->host_thread_started = false; // ditto; task_start sets it
    task->exit_rusage_counted = false; // ditto; do_exit sets it
    // Not inherited either: the copy above would give a child its parent's
    // age. /proc/<pid>/stat field 22, which was hardcoded 0.
    //
    // The first process is created AT boot, and is 0 by definition. Reading
    // the clock for it was wrong, not merely redundant: kernel/init.c creates
    // it and only THEN sets boot_time, so this read measured from run_at_boot's
    // earlier seed -- on a device, from the app's launch, possibly long before
    // the session picker let the guest boot -- and pid 1 started in the future
    // of the uptime that restarted a moment later.
    task->start_time_ticks = parent == NULL ? 0 : get_uptime().uptime_ticks;
    atomic_fetch_add_explicit(&total_forks, 1, memory_order_relaxed);
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    list_init(&task->ptracees);
    list_init(&task->ptrace_siblings);
    list_init(&task->pidfds);
    task->pending = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->pdeath_signal = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->exit_requested = false;
    task->exit_finished = false;
    task->io_block = false;
    task->vfork = NULL;
    // SIGCHLD until told otherwise. clone sets its own (copy_task) and a
    // restore puts back the saved one; what keeps this default is a process
    // construct_task makes for init -- every terminal session, the app's
    // one-shot commands, the fallback console shell. Each stands for a child
    // init forked, and was 0 here: a "clone child", which init's plain wait
    // skips now that wait asks (wait_eligible), and whose exit a SysV init,
    // reaping only on SIGCHLD, was never sent a signal for. Set before the
    // task is linked to its parent below, so that no wait sees it without one.
    task->exit_signal = SIGCHLD_;
    // A peak is the CHILD's own, not something it starts life owing to its
    // parent. Linux gives a new process a fresh ru_maxrss -- its high-water
    // mark tracks its own mm from now on -- and `*task = *parent` above had
    // been handing it the parent's all-time peak instead.
    //
    // Ordinarily that is merely wrong by a bit. It stopped being merely wrong
    // when one bad sample got latched (see task_maxrss_kb): every descendant
    // of that task then reported 2.7 TB, for as long as the shell lived, and a
    // fresh login looked fine -- which is exactly why it presented as a
    // device-only bug that would not reproduce.
    task->maxrss_kb = 0;

    // The rest of the same ledger, missed when maxrss was fixed. `*task =
    // *parent` above hands a child its parent's LIFETIME resource counters,
    // and nothing here was zeroing them. Linux's copy_process() zeroes
    // p->min_flt, p->maj_flt, p->nvcsw and p->nivcsw and calls
    // task_io_accounting_init() on p->ioac, for every new task -- fork, vfork
    // and clone alike, CLONE_VM and CLONE_THREAD included -- so a fresh task
    // always starts at 0 and a process's counters are its own.
    //
    // MEASURED on build/alpine-arm64-test with the pre-fix binary, running
    // `busybox time -v /bin/true` at the bottom of a chain of d nested
    // fork+exec'd /bin/sh: 29, 47, 61, 75, 89, 103, 117, 131, 145, 159, 173
    // minor faults for d = 0..10 -- 29, 47, then a flat +14 for every further
    // level. /bin/true does identical work at every depth and each nested sh
    // execs into a fresh address space, so every extra fault there is ancestor
    // history the child never took. The I/O counters show the same shape from
    // the same struct copy: `cat /proc/self/io` reported rchar 87 (the size of
    // the script each shell reads) at depth 0 and 783 -- nine times that -- at
    // depth 8, for a cat that read the same 87 bytes in both runs.
    //
    // This is not a display nit, because these are not independent numbers.
    // rusage_fill_task_counters (kernel/resource.c) builds ONE
    // struct rusage_ out of task->minflt, task->nvcsw, task->io and
    // task->maxrss_kb, and `time -v` prints them one under the other; a
    // process's peak RSS was already its own while its fault count was its
    // grandparent's. A half-honest block is worse than a uniformly wrong one:
    // no consumer has ever been tested against that combination, because no
    // Linux can produce it. Threads make it worse than depth alone suggests --
    // a task that clones N threads used to have each one start life holding a
    // full copy of the creator's counts, and /proc/<tgid>/stat and
    // /proc/<tgid>/io sum the group.
    //
    // NOT reset on exec, deliberately, because Linux does not reset there
    // either: execve keeps the task_struct, so min_flt and ioac survive the
    // image swap and the faults taken loading the new image belong to the
    // process that took them. The reset belongs here, at the one funnel every
    // task passes through: task_create_'s only callers are
    // sys_clone_common_ and task_fork_for_exec, both in kernel/fork.c, plus
    // construct_task in kernel/init.c. sys_fork, sys_vfork, sys_clone and
    // sys_clone3 all route through sys_clone_common_, so CLONE_VM and
    // CLONE_THREAD are covered -- and the malloc above is the only allocation
    // of a struct task in the tree, so there is no fourth path to miss.
    task->minflt = 0;
    task->nvcsw = 0;
    // memset rather than a struct assignment because the members are _Atomic;
    // kernel/exit.c clears the same struct the same way when it rolls a dying
    // thread's I/O into the group. Nothing else has a pointer to this task
    // yet, so there is no publication to order against.
    memset(&task->io, 0, sizeof(task->io));

    // Both of these are OWNED heap pointers, and `*task = *parent` above is a
    // shallow copy, so leaving them aliased gives two tasks one allocation and
    // whichever dies first frees it under the other. task_free_final does
    // exactly that (native_exec_discard_pending, native_env_discard), and so
    // does native_env_init on the child's own exec.
    //
    // That is not theoretical. Native bash assigns `environ = export_env`, so
    // the task's environment vector IS bash's exported-variable array; every
    // command bash ran spawned a child task that inherited the pointer and
    // freed the array on its way out. The shell then read freed memory in
    // add_or_supercede_exported_var and the app died on a null entry -- while
    // running bash's own test suite, several commands after the one that
    // caused it.
    //
    // Null rather than duplicate: a task made here is on its way to an execve,
    // and the environment it ends up with is that call's envp. A native
    // program asking before then gets an empty vector from native_env_slot.
    task->native_env = NULL;
    task->native_exec = NULL;
    // Same shallow-copy hazard, same answer. These describe the arguments of a
    // program running in the PARENT, and native_cmdline is an owned allocation
    // that task_free_final frees -- two tasks holding the pointer is a double
    // free, on top of a child reporting its parent's command line to procfs.
    task->native_argv = NULL;
    task->native_argc = 0;
    task->native_cmdline = NULL;
    task->native_cmdline_len = 0;
    // Supplementary groups ARE inherited across fork (unlike the two above),
    // so this one is duplicated rather than dropped -- but duplicated it must
    // be, for the same shallow-copy reason.
    if (task->ngroups != 0 && task->groups != NULL) {
        size_t bytes = (size_t) task->ngroups * sizeof(uid_t_);
        uid_t_ *copy = malloc(bytes);
        if (copy == NULL) {
            free(task);
            return NULL;
        }
        memcpy(copy, task->groups, bytes);
        task->groups = copy;
    } else {
        task->groups = NULL;
        task->ngroups = 0;
    }
    // The shim's signal bookkeeping describes the native program running in
    // the PARENT; a fresh task has none until it becomes one.
    task->native_prog_blocked = 0;
    task->native_held = 0;
    task->native_restart = 0;
    task->native_sigtable = NULL;

    lock_init(&task->general_lock, "task_creat_gen\0");

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_interruptible = false;
    task->waiting_lock = NULL;
    task->waiting_interrupt_flag = NULL;
    task->wait_interrupted = false;
    task->restart_interrupted_syscall = false;
    task->restart_interrupted_syscall_nohand = false;
    // Nothing queued for the process has been handed to a thread that did not
    // exist, as Linux's copy_process clears TIF_SIGPENDING.
    task->group_sigpending = false;
    task->group_handoff = 0;
    task->poll_restart_valid = false;
    task->sleep_restart_valid = false;
    task->restart_nohand_pending = false;
    task->restart_sys_pending = false;
    task->futex_restart_futex = NULL;
    task->futex_restart_uaddr = 0;
    task->futex_restart_wake_seq = 0;
    task->futex_restart_timed = false;
    task->poll_notify_fd = -1;
    lock_init(&task->waiting_cond_lock, "task_creat_wait\0");
    cond_init(&task->pause);

    task->ptrace = (typeof(task->ptrace)) {};
    task->ptrace_link_capable = false;
    task->ptrace_trap_notify = false;
    task->native_helper_threads = 0;
    lock_init(&task->ptrace.lock, "task_creat_ptr\0");
    cond_init(&task->ptrace.cond);

    task->locks_held.count = 0;
    pthread_mutex_init(&task->locks_held.lock, NULL);
    atomic_store_explicit(&task->reference.count, 0, memory_order_relaxed);
    task->reference.ready_to_be_freed = false;

    complex_lockt(&pids_lock, 0);
    if (want_pid != 0) {
        // A CHOSEN pid, for a restore (kernel/checkpoint.c). Refused rather
        // than quietly given a different one: a process that comes back under
        // another number is not the process that was saved, and everything
        // holding its old pid -- its parent's wait, a shell's $!, a pidfile --
        // is then wrong in a way nothing would report.
        if (want_pid >= task_pid_max() || !pid_empty(&pids[want_pid]) ||
                pid_is_kthread(want_pid, NULL)) {
            unlock(&pids_lock);
            free(task);
            return NULL;
        }
        last_allocated_pid = want_pid;
    } else {
        do {
            last_allocated_pid++;
            if (last_allocated_pid >= task_pid_max()) last_allocated_pid = 1;
            // Reserved for a synthetic kernel thread: handing it to a real task
            // would make two different processes answer to one pid.
        } while (!pid_empty(&pids[last_allocated_pid]) ||
                 pid_is_kthread(last_allocated_pid, NULL));
    }
    struct pid *pid = &pids[last_allocated_pid];
    pid->id = last_allocated_pid;
    list_init(&pid->alive);
    list_init(&pid->session);
    list_init(&pid->pgroup);
    task->pid = pid->id;

    pid->task = task;
    list_add(&alive_pids_list, &pid->alive);
    if (parent != NULL) {
        task->parent = parent;
        // At the end: a children list is oldest first, as Linux's
        // copy_process keeps it (list_add_tail), and wait walks it from the
        // front, so of several zombies wait(-1) reaps the oldest. At the head
        // it reaped the youngest (tests/manual/wait_child_order.c). A
        // checkpoint restore builds each parent's children in list order
        // through here too (ckpt_order_tasks).
        list_add_tail(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);
    // The parent's seccomp filters are the child's too; the struct copy above
    // shares the pointer, and task_free_final drops this reference.
    seccomp_filter_retain(task->seccomp_filter);
    // The machine now exists, so its load average starts being sampled.
    if (parent == NULL)
        guest_loadavg_start();
    return task;
}

struct task *task_create_(struct task *parent) {
    return task_create_pid_(parent, 0);
}

struct task *task_create_with_pid(struct task *parent, pid_t_ want) {
    return task_create_pid_(parent, want);
}

// We consolidate the check for whether the task is in a critical section,
// holds locks, or has pending signals into a single function.
bool should_wait(struct task *t) {
    // sighand is released (and nulled) before a task reaches this path during
    // teardown; a nulled sighand has nothing left to check.
    sigset_t_ group_pending = t->sighand != NULL ? t->sighand->pending : 0;
    return task_ref_cnt_get(t, 0) > 1 || locks_held_count(t) || !!((t->pending | group_pending) & ~t->blocked);
}

void task_unlink_locked(struct task *task) {
    task->exiting = true;
    list_remove(&task->siblings);
    list_remove_safe(&task->ptrace_siblings);
    struct pid *pid = pid_get(task->pid);
    // pid_get returns NULL for a pid it considers empty, and pid_empty is
    // satisfied by `task == NULL` alone once session and pgroup are gone -- so
    // a second unlink of the same task arrives here with nothing to unlink.
    if (pid == NULL)
        return;
    pid->task = NULL;
    // Re-init rather than leave it NULL. list_remove sets next and prev to
    // NULL (util/list.h), list_init leaves a node pointing at itself, and
    // list_for_each_entry stops only on `&item->member != (list)` -- it has no
    // NULL check, and SEVEN sites walk alive_pids_list with it. A node left
    // NULL is therefore a live hazard for every one of those walks: the walk
    // follows `next` into 0 and the container_of subtraction faults at
    // 0xfffffffffffffff8, which is the shape of the device checkpoint crash in
    // docs/TODO.md. Self-pointing is the state list_init already gives a fresh
    // pid (task_create_pid_), and nothing reads this node's emptiness --
    // pid_empty tests task/session/pgroup and never `alive`.
    list_remove(&pid->alive);
    list_init(&pid->alive);
}

static void task_free_final(struct task *task) {
    if (unlikely(guestprof_on))
        guestprof_slot_release(task);
    // A native program recorded by execve but never reached -- the task died
    // between the exec and its first execution (task_start failing, say).
    native_exec_discard_pending(task);
    native_env_discard(task);
    native_cmdline_discard(task);
    native_sigtable_discard(task);
    // The group goes with its leader. Whether this IS the leader is asked
    // without reading a thread's group pointer first: a thread's struct can
    // outlive its process's group -- one left on the deferred queue below
    // while its process is reaped, or the last thread of a process whose exit
    // freed the leader before this thread had finished freeing itself -- and
    // task_is_leader would read the freed group. A leader's pid is its tgid;
    // a thread's never is, nor that of a leader execve retired (pid 0).
    if (task != NULL && task->group != NULL && task->pid == task->tgid &&
            task->group->leader == task) {
        // Before the group struct goes: an AIO context is keyed by a guest
        // address, and this address space is on its way out.
        aio_discard_tgroup(task->group);
        cond_destroy(&task->group->child_exit);
        free(task->group->cgroup_path);
        free(task->group);
        task->group = NULL;
    }
    free(task->groups);
    task->groups = NULL;
    seccomp_filter_release(task->seccomp_filter);
    task->seccomp_filter = NULL;
    free(task);
}

void task_destroy_unlinked(struct task *task, int caller) {
    task->exiting = true;

    // We use a single loop to wait for the task to be ready to destroy.
    // This loop replaces all the similar while-loops in the original code.
    // Reap paths should not stall a waiting parent just to synchronously free
    // the task object. If references are still draining, defer cleanup.
    int count = caller == 2 ? 0 : -4000; // Counter to limit the number of times we check.
    while (count < 0 && should_wait(task)) {
        nanosleep(&lock_pause, NULL); // Sleep for a defined amount of time.
        count++;
    }

    // A zombie can be reaped the moment do_exit marks it, while its own thread
    // is still finishing do_exit on the struct -- freeing the queued signals,
    // sending its parent's SIGCHLD, publishing exit_finished. A reap used to
    // free it regardless. Now that a tracer reaps zombie threads and a
    // process's exit can release a leader from another thread, that window is
    // wider, so such a zombie waits on the deferred queue for exit_finished.
    bool still_exiting = task->zombie &&
        !atomic_load_explicit(&task->exit_finished, memory_order_acquire);
    if (task_ref_cnt_get(task, 1) || still_exiting) { // Check to see if another thread is accessing this process.  If yes, note that and defer freeing it
        struct task_pending_deletion *pd = malloc(sizeof(struct task_pending_deletion));
        if (pd) {
            task->reference.ready_to_be_freed = true;
            pd->task = task;
            pd->added_time = time(NULL);
            list_init(&pd->list);
            pthread_mutex_lock(&tasks_pending_deletion_lock);
            list_add(&tasks_pending_deletion_queue, &pd->list);
            pthread_mutex_unlock(&tasks_pending_deletion_lock);
        }
        // Lets cleanup any expired pending deletions here for now
        cleanup_pending_deletions();
        return;
    } else {
        task_free_final(task);
    }
}

void task_destroy(struct task *task, int caller) {
    task_unlink_locked(task);
    unlock(&pids_lock);
    task_destroy_unlinked(task, caller);
    complex_lockt(&pids_lock, 0);
}

// Cleanup function to delete tasks after the grace period
void cleanup_pending_deletions(void) {
    pthread_mutex_lock(&tasks_pending_deletion_lock);
    struct task_pending_deletion *pd, *tmp;
    list_for_each_entry_safe(&tasks_pending_deletion_queue, pd, tmp, list) {
        if (difftime(time(NULL), pd->added_time) >= GRACE_PERIOD &&
                atomic_load_explicit(&pd->task->reference.count, memory_order_acquire) == 0 &&
                (!pd->task->zombie ||
                 atomic_load_explicit(&pd->task->exit_finished, memory_order_acquire))) { // Delete reaped tasks old and no longer referenced
            task_free_final(pd->task);
            list_remove(&pd->list);
            free(pd);
        }
    }
    pthread_mutex_unlock(&tasks_pending_deletion_lock);
}

// ISH_TASK_DUMP_EVERY=<seconds>: every so often, print every task -- who it
// is, what it is waiting in, and where its HOST thread actually is.
//
// For the hang that only happens on the device and leaves no shell to look
// with. The case that asked for it: after a restore, every new login's shell
// failed to start -- ssh authenticated and then hung, new terminals never
// printed a prompt -- and every way in went through the thing that was stuck.
// This needs no guest cooperation at all: the environment reaches the app
// through `devicectl device process launch --environment-variables`, and the
// output through --console. Frames come from task_host_backtrace, the
// freezer's own instrument; see [[stuck-task-host-backtrace]] in the notes.
static double task_dump_every;
// The thread run_at_boot ran on: the app's main thread, and the CLI's. Where
// the boot itself is, when the hang is before there is any task to dump -- a
// launch that sat for minutes before init existed is what made this worth it.
static pthread_t task_dump_boot_thread;

static void task_dump_frames(uintptr_t *frames, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        Dl_info info;
        if (dladdr((void *) frames[i], &info) && info.dli_sname != NULL)
            fprintf(stderr, "taskdump:     %2u %s\n", i, info.dli_sname);
        else
            fprintf(stderr, "taskdump:     %2u %#lx\n", i, (unsigned long) frames[i]);
    }
}

// ISH_TASK_DUMP_ALL_THREADS=1: every host thread in the process too, named.
// For a stall on a thread that is neither a task's nor the boot thread -- a
// launch sat for minutes while the main thread idled in its run loop, so the
// work was somewhere else. Each thread is suspended through its Mach port
// BEFORE its pthread is looked at: a thread that exited between the listing
// and the walk would otherwise leave a dangling pthread_t, and a suspended
// thread cannot exit.
static bool task_dump_all_threads;

static void task_dump_every_host_thread(void) {
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return;
    mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (threads[i] != self && thread_suspend(threads[i]) == KERN_SUCCESS) {
            pthread_t pt = pthread_from_mach_thread_np(threads[i]);
            char name[64] = {0};
            uintptr_t frames[16];
            unsigned n = 0;
            if (pt != NULL) {
                pthread_getname_np(pt, name, sizeof(name));
                uintptr_t top = (uintptr_t) pthread_get_stackaddr_np(pt);
                uintptr_t bottom = top - pthread_get_stacksize_np(pt);
                arm_thread_state64_t st;
                mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(threads[i], ARM_THREAD_STATE64,
                                     (thread_state_t) &st, &sc) == KERN_SUCCESS) {
                    frames[n++] = (uintptr_t) arm_thread_state64_get_pc(st) & 0x0000ffffffffffffULL;
                    frames[n++] = (uintptr_t) arm_thread_state64_get_lr(st) & 0x0000ffffffffffffULL;
                    uintptr_t fp = (uintptr_t) arm_thread_state64_get_fp(st);
                    while (n < 16 && fp >= bottom && fp + 16 <= top && (fp & 7) == 0) {
                        const uintptr_t *rec = (const uintptr_t *) fp;
                        uintptr_t next = rec[0];
                        uintptr_t ret = rec[1] & 0x0000ffffffffffffULL;
                        if (ret == 0)
                            break;
                        frames[n++] = ret;
                        if (next <= fp)
                            break;
                        fp = next;
                    }
                }
            }
            thread_resume(threads[i]);
            fprintf(stderr, "taskdump: host thread %u \"%s\":\n", i, name);
            task_dump_frames(frames, n);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t) threads,
                  count * sizeof(thread_act_t));
#endif
}

static void task_dump_one(struct task *t) {
    const struct native_program *native = t->native_running;
    fprintf(stderr, "taskdump: pid %d tgid %d ppid %d uid %u euid %u %s%s%s%s%s\n",
            t->pid, t->tgid, t->parent != NULL ? t->parent->pid : 0,
            (unsigned) t->uid, (unsigned) t->euid, t->comm,
            t->zombie ? " ZOMBIE" : "", t->exiting ? " EXITING" : "",
            t->io_block ? " io_block" : "",
            native != NULL ? " native" : "");
    if (t->zombie || !atomic_load_explicit(&t->host_thread_started,
                                           memory_order_acquire))
        return;
    if (pthread_equal(t->thread, pthread_self()))
        return;
    uintptr_t frames[24];
    task_dump_frames(frames, task_host_backtrace(t, frames, 24));
}

static void *task_dump_thread(void *unused) {
    (void) unused;
    for (;;) {
        usleep((useconds_t) (task_dump_every * 1000000));
        struct task_snapshot snap = {0};
        if (task_snapshot_collect(&snap, false) < 0)
            continue;
        fprintf(stderr, "taskdump: ---- %u tasks ----\n", snap.count);
        uintptr_t boot_frames[40];
        fprintf(stderr, "taskdump: boot thread (main):\n");
        task_dump_frames(boot_frames,
                         host_thread_backtrace(task_dump_boot_thread, boot_frames, 40));
        if (task_dump_all_threads)
            task_dump_every_host_thread();
        for (unsigned i = 0; i < snap.count; i++)
            task_dump_one(snap.tasks[i]);
        fprintf(stderr, "taskdump: ---- end ----\n");
        task_snapshot_release(&snap);
    }
    return NULL;
}

static void task_dump_start_if_asked(void) {
    const char *e = getenv("ISH_TASK_DUMP_EVERY");
    if (e == NULL || e[0] == '\0')
        return;
    task_dump_every = atof(e);
    task_dump_boot_thread = pthread_self();
    const char *all = getenv("ISH_TASK_DUMP_ALL_THREADS");
    task_dump_all_threads = all != NULL && all[0] != '\0' && all[0] != '0';
    if (task_dump_every < 1)
        task_dump_every = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, task_dump_thread, NULL) == 0)
        pthread_detach(th);
}

void run_at_boot(void) {  // Stuff we run only once, at boot time.
    //atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct uname uts;
    do_uname(&uts);
    unsigned short ncpu = get_cpu_count();
    lock_init(&pids_lock, "pids");
    lock_init(&atomic_l_lock, "run_at_boot");
    task_dump_start_if_asked();
    // No guest arch named here: this runs once at boot, and one session
    // can run i386, x86_64, and arm64 guests (per-task ABI).
    //
    // The executable's timestamp, NOT __DATE__/__TIME__.
    //
    // This line is what everyone reads to answer "is the thing I am testing
    // the thing I just built?", and __DATE__/__TIME__ cannot answer it: they
    // are THIS FILE's compile time, and an incremental build that changes only
    // app/ code relinks without recompiling task.c. Measured on two bundles
    // here: one linked at 18:36 reported 18:21, another linked at 18:04
    // reported 12:06. I read one of those lags as "you tested a stale build"
    // and was wrong about it. buildTimestamp() is the same source of truth
    // hostinfo.m already uses, and it has an implementation in both the app
    // (kernel/hostinfo.m) and the CLI (platform/standalone.c).
    char built[64];
    time_t built_at = buildTimestamp();
    if (built_at != 0) {
        struct tm built_tm;
        localtime_r(&built_at, &built_tm);
        strftime(built, sizeof(built), "%b %d %Y %H:%M:%S", &built_tm);
    } else {
        // Nothing to stat -- better a compile time than no answer at all.
        snprintf(built, sizeof(built), "%s %s", __DATE__, __TIME__);
    }
    printk("iSH-AOK %s built %s booted on %d emulated CPU(s)\n",
            uts.release, built, ncpu);
    // Get boot time
    extern time_t boot_time;
         
    boot_time = time(NULL);
    //printk("Seconds since January 1, 1970 = %ld\n", boot_time);
}

extern _Atomic long quiesce_poke_calls;
extern _Atomic long quiesce_poke_noop;
extern _Atomic long quiesce_pokes_sent;
extern _Atomic long quiesce_pokes_skipped;
extern _Atomic long quiesce_reader_naps;

// ISH_TEST_POKE_BLOCKED_TASKS=<comm prefix>: poke a task whose comm starts with
// this even while it is io_block, as though the barrier had read the flag a
// moment before the task set it. That race is real -- an inotify read, which is
// io_block, came back EINTR with no signal sent once in some hundreds of tries
// -- and too rare to test, so without this the other half of the defence
// (wait_for_blocked in util/sync.c reporting a bare poke as a spurious wakeup)
// could only ever be reasoned about. tests/manual/blocked_wait_state.c is the
// reader. Deliberately narrow, like ISH_TEST_LOSE_WAKE_POKES in kernel/signal.c:
// a comm prefix, never every task.
static bool poke_blocked_task_for_test(const struct task *task) {
    static const char *prefix = NULL;
    static int looked_up = 0;
    if (!looked_up) {
        prefix = getenv("ISH_TEST_POKE_BLOCKED_TASKS");
        looked_up = 1;
    }
    if (prefix == NULL || *prefix == '\0' || task == NULL)
        return false;
    return strncmp(task->comm, prefix, strlen(prefix)) == 0;
}

void task_poke_shared_mem(struct task *task, struct mem *mem) {
    // A NULL `task` is a caller with no `current` -- kswapd, and anything else
    // that takes the address-space barrier from a host thread rather than a
    // guest one. It is NOT a reason to do nothing, and returning here made the
    // barrier silently useless for exactly those callers: nobody is poked, the
    // trylockw spin in mem_write_lock_with_pokes burns its 1024 attempts
    // against siblings that never get told to yield, and it falls through to a
    // blocking write_lock that waits for guest threads which hold the read lock
    // for as long as they execute guest code.
    //
    // `task` is only ever used below to skip the caller's own thread, and
    // `other == task` already reduces to `other == NULL` -- which the line
    // above it excludes -- so a NULL caller correctly means "poke every thread
    // of this mem, there is no self to skip".
    if (mem == NULL)
        return;

    atomic_fetch_add_explicit(&quiesce_poke_calls, 1, memory_order_relaxed);
    if (trylock(&pids_lock) != 0) {
        atomic_fetch_add_explicit(&quiesce_poke_noop, 1, memory_order_relaxed);
        return;
    }
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *other = pid_entry->task;
        if (other == NULL || other == task)
            continue;
        if (other->mem != mem)
            continue;
        if (other->zombie || other->exiting)
            continue;
        // Only readers executing guest code hold the mem read lock and must be
        // evicted. A sibling parked in a blocking syscall (io_block) holds no
        // read lock, so poking it is pure waste -- the SIGUSR1 just bounces it
        // out of poll/futex for nothing (the real git/daemon storm: most
        // siblings sit in poll). Skip it. The race where it leaves io_block and
        // enters JIT right after this check is covered by mem_write_lock_with_
        // pokes re-poking every 64 attempts: by then io_block is clear and the
        // trylockw it now blocks forces another poke round that catches it.
        if (other->io_block && !poke_blocked_task_for_test(other)) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        // Same reasoning for a sibling parked in task_wait_for_mem_quiesce
        // (quiesce_parked, task.h): it holds no read lock, and under a
        // barrier storm (one mprotect per pthread_create in the thread
        // benchmark) re-signalling every parked sibling each poke round is
        // exactly the SIGUSR1 flood that melted the host scheduler. Same
        // stale-read recovery contract as io_block above.
        if (atomic_load_explicit(&other->quiesce_parked, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        // Already poked and hasn't consumed it: the sticky flag is still up,
        // so the sibling either hasn't reached a block boundary yet (the
        // flag, not the signal, is what evicts a JIT runner) or has already
        // exited guest code and is blocked on one of OUR locks. Re-signalling
        // it does nothing for the barrier — and a SIGUSR1 storm against a
        // thread parked in __psynch_rw_wrlock/rdlock is exactly the
        // repeated-EINTR pattern that wedged Darwin's psynch rwlock in the
        // mprotect-storm stress (writers asleep forever on a FREE lock).
        // cpu_take_poke clears the flag only when the task re-enters its run
        // loop, so this can't suppress a needed eviction.
        if (other->cpu.poked_ptr != NULL &&
                __atomic_load_n(other->cpu.poked_ptr, __ATOMIC_SEQ_CST)) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        pthread_kill(other->thread, SIGUSR1);
        atomic_fetch_add_explicit(&quiesce_pokes_sent, 1, memory_order_relaxed);
        if (other->cpu.poked_ptr == NULL)
            continue;
        cpu_poke(&other->cpu);
    }
    unlock(&pids_lock);
}

static void task_wait_for_mem_quiesce(struct task *task) {
    struct mem *mem = task != NULL ? task->mem : NULL;
    if (mem == NULL ||
            atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) == 0)
        return;
    // No mem read lock is held in here, so poking us can't help the barrier
    // writer — flag ourselves skippable (quiesce_parked, task.h). Cleared
    // before returning: the caller takes the read lock right after.
    atomic_store_explicit(&task->quiesce_parked, true, memory_order_relaxed);
    int spins = 0;
    while (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0)
        mem_quiesce_wait(mem, &spins);
    atomic_store_explicit(&task->quiesce_parked, false, memory_order_relaxed);
}

void task_run_current(void) {
    // Every host thread that runs guest work reaches here exactly once, so this
    // is the one place that catches them all -- task_thread and timer_thread do
    // it for themselves, but the thread that runs init does not go through
    // either (the CLI's main thread, and whichever thread the app boots on).
    // Idempotent, and it must happen before a wake poke can be the first thing
    // on this thread to touch the storage the handlers read. See
    // signal_thread_locals_init() in util/sync.c.
    signal_thread_locals_init();

    // A task whose image is a natively-implemented program never enters the
    // emulator at all: it is dispatched here instead, and does not return. The
    // execve entry points handle the ordinary case of an already-running task
    // exec'ing one; this covers a task whose FIRST image is native, which is
    // reached without any execve syscall returning -- the CLI's top-level
    // command and kernel/init.c's boot-command launcher both land here.
    native_exec_run_pending();

    struct task* save = current; // Because I kinda suspect that current gets messed up sometimes
    struct cpu_state *cpu = &save->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &save->mem->mmu);
    task_pthread_canary_note_tlb(&tlb, sizeof(tlb));
    
    while (true) {
        // The one place in the guest where a task is at a clean boundary: no
        // address-space lock held, not inside a syscall, and struct cpu_state
        // naming the NEXT instruction rather than the one being executed. A
        // checkpoint asked for from a syscall is taken here, one pass later,
        // which is what makes a restore continue instead of re-running the
        // request. See kernel/checkpoint.c.
        checkpoint_park_if_frozen();
        checkpoint_run_pending();
        task_wait_for_mem_quiesce(save);
        read_lock(&save->mem->lock);

        // ISH_PTHREAD_CANARY only, and a no-op otherwise: bracket the two
        // halves of the loop so a self-inflicted store into this thread's host
        // struct _pthread is attributed to one of them.
        task_pthread_canary_check_self_at(
                "at the top of task_run_current's loop, where the cleanup list must be empty",
                true);
        if (unlikely(guestprof_on))
            guestprof_state(GUESTPROF_GUEST);
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        if (unlikely(guestprof_on))
            guestprof_state(GUESTPROF_KERNEL);
        task_pthread_canary_check_self("after guest execution");

        read_unlock(&save->mem->lock);
        jit_cleanup_jetsam_after_interrupt(cpu);
 
        handle_interrupt(interrupt);
        task_pthread_canary_check_self("after handle_interrupt");
    }
}

static void task_pthread_canary_register(void);
void task_pthread_canary_check_self(const char *where);
void task_pthread_canary_check_self_at(const char *where, bool must_be_empty);
void task_pthread_canary_note_tlb(const void *tlb, unsigned long size);
void task_pthread_canary_note_unwind(void);

static void *task_thread(void *task) {
    current = task;

    // The wake signals are blocked on entry (task_start created us that way).
    // Instantiate the thread-local storage sigusr1_handler relies on -- on this
    // normal call stack, where malloc is safe -- before unblocking them. The assignment
    // above instantiates `current`; this covers should_unwind / unwind_buf /
    // should_mark_wait_interrupted as well.
    signal_thread_locals_init();

    sigset_t wake_sigs;
    sigemptyset(&wake_sigs);
    sigaddset(&wake_sigs, SIGUSR1);
    sigaddset(&wake_sigs, SIGUSR2); // the backup poke, see util/sync.c
    pthread_sigmask(SIG_UNBLOCK, &wake_sigs, NULL);

    update_thread_name();
    task_pthread_canary_register();

    // A child its parent's tracer attached to at clone time owes that tracer
    // its first stop (sys_clone_common_): a PTRACE_EVENT_STOP if the tracer
    // seized, taken first and whatever the mask, and otherwise a queued
    // SIGSTOP. Linux takes it on the child's way out of ret_from_fork, before
    // the child runs an instruction of its own; the loop in task_run_current
    // looks for signals only after the first interrupt, by which time a child
    // that exits at once has already exited.
    if (current->ptrace.traced) {
        ptrace_trap_stop_if_pending();
        lock(&current->sighand->lock, 0);
        bool pending = ((current->pending | task_group_pending(current)) &
                ~current->blocked) != 0;
        unlock(&current->sighand->lock);
        if (pending)
            receive_signals();
        // Whether or not a signal stopped it: a process already job-control
        // stopped reports that as its first stop, and the trap looked for above
        // is left to it (ptrace_trap_stop_if_pending).
        group_stop_wait();
        ptrace_trap_stop_if_pending();
    }

    task_run_current();
    die("task_thread returned"); // above function call should never return
    return NULL;
}

// A guest task's host stack has to be big enough for a NATIVE program, which
// runs as an ordinary C function on this thread rather than inside the
// emulator. Emulated code keeps its own recursion on the guest's stack and
// barely touches this one, so the 512 KB Darwin gives a non-main thread was
// never noticed -- until zsh and bash started running natively.
//
// Measured with native zsh: shell-function recursion costs ~3.5 KB of this
// stack per level, so 512 KB ran out at about 150 nested calls. zsh's own
// guard, FUNCNEST, defaults to 500 and is what makes `r() { r }; r` print
// "maximum nested function level reached" on a real zsh; here the C stack was
// exhausted first, and a native program is a function call on a thread of the
// APP, so the resulting SIGBUS took the whole app down (host exit 138) rather
// than one shell. `builtin() { builtin print x }; builtin` did the same thing
// in one line.
//
// So the stack is sized to fit the guard rather than the guard shrunk to fit
// the stack: 4 MB was measured to hold between 1250 and 1300 levels, i.e. two
// and a half times FUNCNEST's default, which leaves zsh's own guard to stop
// first and say so in zsh's own words. Shrinking FUNCNEST instead would have
// been a divergence from what the same script does off-device, and it would
// have fixed only zsh -- bash and every future native program share this
// thread.
//
// Re-measured once both shells had the guard, since the depth that matters is
// what is USABLE, not what the stack holds raw: native zsh refuses at 1186
// levels and native bash at 1455 (binary search, this rootfs, 4 MB). zsh's own
// FUNCNEST default of 500 therefore still fires first, which is the intent --
// an ordinary script that recurses too far gets zsh's message from zsh's guard,
// exactly as it does off-device.
//
// bash is the one that actually depends on this number, and it was not part of
// the original reasoning: bash's FUNCNEST is UNSET by default, so there is no
// first limit to fire and the stack guard is the only thing between a runaway
// recursive function and the end of the stack. Halving this to 2 MB would put
// zsh at roughly 550 usable levels -- close enough to FUNCNEST's 500 that the
// two guards would start racing -- and 1 MB would put it below, so ours would
// fire on scripts that are legal everywhere else. 4 MB is the smallest size
// with real margin, not a round number.
//
// The cost is address space rather than memory: the pages are committed on
// demand, so a thread that never recurses still touches only a few KB of it.
// Measured with 60 concurrent guest tasks live: RSS stayed in the low
// megabytes, i.e. the 8x reservation is not an 8x footprint.
// That is what makes this affordable to give to every guest task rather than
// only to the ones running native programs -- which is just as well, because
// at creation time we do not yet know which those are.
#define TASK_THREAD_STACK_SIZE (4 * 1024 * 1024)

// ISH_PTHREAD_CANARY=1: catch whoever corrupts a task thread's host
// `struct _pthread`.
//
// The pread_stack_thread_race SIGSEGV (docs/TODO.md) faults inside Darwin's
// _pthread_exit reading `self->__cleanup_stack`, which holds 0x100000000 --
// not a pointer. The bill is paid on the victim, in a frame that has nothing
// of ours on it, so the crash report names the victim and says nothing at all
// about who put that value there.
//
// So register every task thread's `self` and spin a watcher over that word.
// What it must NOT be is the point: `__cleanup_stack` heads a list of
// pthread_cleanup_push records, and libpthread pushes one itself inside every
// pthread_cond_wait -- so nonzero is normal, and the first version of this
// canary fired on all twelve runs catching AOK parked in mem_quiesce_park.
// A *valid* head is either NULL or the address of a record on this thread's
// own stack, i.e. somewhere in [self - 4 MB, self). Anything else is the
// crash. The instant one appears, suspend every other thread with the mach
// APIs and dump its registers and frame-pointer backtrace, so the writer is
// caught in the act rather than inferred. Addresses are raw and the report
// prints the main image's slide, so `atos -o build/ish -l <load address>`
// symbolises them.
#if defined(__APPLE__)

#define CANARY_SLOTS 256

struct canary_slot {
    _Atomic(uintptr_t) self;
    uint64_t sig; // snapshot of self[0] taken at registration
    // What __cleanup_stack held the last time it was valid, and the scan pass
    // that saw it. 0 -> 0x100000000 means a store into an empty field; a live
    // record pointer -> 0x100000000 means the value arrived through a cleanup
    // pop, i.e. out of the record's __next on the stack.
    uint64_t last_cleanup;
    uint64_t last_cleanup_pass;
    _Atomic int leaving; // the owner has entered do_exit's unregister
    // task_run_current's `struct tlb` local: 24 KB of stack that dominates
    // this thread's frame layout, so a bad stack address is worth reporting
    // relative to it before guessing at what else lives there.
    _Atomic(uintptr_t) tlb_base;
    _Atomic(uintptr_t) tlb_end;
    // The head record and its __next as of the previous scan pass. If __next
    // goes bad while the head is UNCHANGED, something stored into this
    // thread's live stack frame. If the head changed too, the bad value
    // arrived with the push -- i.e. it was already in __cleanup_stack, and the
    // record is only carrying it forward.
    uintptr_t last_head;
    uint64_t last_next;
    uint64_t birth_cleanup; // __cleanup_stack as found at registration
};

static struct canary_slot canary_slots[CANARY_SLOTS];
static _Atomic uint64_t canary_epoch;
static __thread struct canary_slot *canary_my_slot;

static bool task_pthread_canary_enabled(void) {
    static _Atomic int enabled = -1;
    int e = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (e < 0) {
        const char *v = getenv("ISH_PTHREAD_CANARY");
        e = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
        atomic_store_explicit(&enabled, e, memory_order_relaxed);
    }
    return e == 1;
}

// Everything below runs with the rest of the process frozen, so it allocates
// nothing and calls nothing that could take a lock a suspended thread holds.
static char *canary_put(char *p, const char *s) {
    while (*s != '\0')
        *p++ = *s++;
    return p;
}

static char *canary_hex(char *p, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    *p++ = '0';
    *p++ = 'x';
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int d = (int) ((v >> shift) & 0xf);
        if (d != 0 || started || shift == 0) {
            started = true;
            *p++ = digits[d];
        }
    }
    return p;
}

static char *canary_dec(char *p, uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char) ('0' + (int) (v % 10));
        v /= 10;
    } while (v != 0);
    while (n-- > 0)
        *p++ = tmp[n];
    return p;
}

// Frame-pointer unwind. Only ever called on a suspended thread whose sp we
// know, so the range check below is what keeps a garbage fp from faulting.
static char *canary_backtrace(char *p, uint64_t fp, uint64_t sp) {
    uint64_t prev = 0;
    for (int depth = 0; depth < 32; depth++) {
        if (fp == 0 || (fp & 0xf) != 0 || fp <= prev || fp < sp || fp - sp > (32u << 20))
            break;
        uint64_t next = ((const uint64_t *) fp)[0];
        uint64_t lr = ((const uint64_t *) fp)[1] & 0x0000ffffffffffffULL; // strip any PAC
        p = canary_put(p, " ");
        p = canary_hex(p, lr);
        prev = fp;
        fp = next;
    }
    return p;
}

// Defined with the watchpoint block below; 1 if `addr` is currently under a
// hardware watchpoint, 0 if not, -1 if watchpoints are off entirely. A canary
// catch on a WATCHED word that produced no trap is a real finding: the store
// was the owning thread's own.
static int canary_watch_covers(uintptr_t addr);

// Where the bad value was found. The distinction is the whole question: a bad
// list HEAD is a store into the host struct _pthread, while a bad `__next`
// inside a record is a store into the owning thread's own STACK, which
// libpthread then copies into the head on the next cleanup pop -- a completely
// different writer to go looking for.
static uintptr_t canary_bad_record;

static struct canary_slot *canary_bad_slot;
static uint64_t canary_pass;
static __thread const char *canary_self_where;
static const char *canary_report_where;
static _Atomic long canary_unwinds_total;
static _Atomic long canary_unwinds_with_record;
static uintptr_t canary_prev_head;
static uint64_t canary_prev_next;
static bool canary_have_prev_next;

static void canary_report(uintptr_t self, unsigned offset, uint64_t expect, uint64_t got) {
    // Freeze the rest of the process before doing anything else: the writer is
    // nanoseconds ahead of us and every instruction spent formatting is one
    // more it gets to run away in.
    mach_port_t me = mach_thread_self();
    thread_act_array_t acts = NULL;
    mach_msg_type_number_t nacts = 0;
    kern_return_t kr = task_threads(mach_task_self(), &acts, &nacts);
    if (kr == KERN_SUCCESS)
        for (mach_msg_type_number_t i = 0; i < nacts; i++)
            if (acts[i] != me)
                thread_suspend(acts[i]);

    static char buf[256 * 1024];
    char *p = buf;
    p = canary_put(p, "\n*** ISH_PTHREAD_CANARY: host struct _pthread corrupted ***\n victim self=");
    p = canary_hex(p, self);
    p = canary_put(p, " offset=+");
    p = canary_dec(p, offset);
    p = canary_put(p, " expected=");
    p = canary_hex(p, expect);
    p = canary_put(p, " found=");
    p = canary_hex(p, got);
    if (canary_bad_record != 0) {
        p = canary_put(p, "\n  ...found in the __next field of the cleanup record at ");
        p = canary_hex(p, canary_bad_record);
        p = canary_put(p, " (self-");
        p = canary_hex(p, (uint64_t) (self - canary_bad_record));
        p = canary_put(p, "): a store into this thread's own STACK, not into its struct _pthread");
        p = canary_put(p, "\n  the same record's __next one scan pass earlier: ");
        if (!canary_have_prev_next) {
            p = canary_put(p, "NOT SEEN -- the head was ");
            p = canary_hex(p, canary_prev_head);
            p = canary_put(p, " last pass, so this record is NEW and the bad value came in WITH THE PUSH (it was already in __cleanup_stack)");
        } else {
            p = canary_hex(p, canary_prev_next);
            p = canary_put(p, " -- the record was already live, so this is a STORE INTO THE LIVE STACK FRAME");
        }
        p = canary_put(p, "\n  the record's three words (__routine __arg __next):");
        for (int k = 0; k < 3; k++) {
            p = canary_put(p, " ");
            p = canary_hex(p, ((const uint64_t *) canary_bad_record)[k]);
        }
        uintptr_t bad = canary_bad_record + 16;
        uintptr_t tb = canary_bad_slot != NULL
                ? atomic_load_explicit(&canary_bad_slot->tlb_base, memory_order_relaxed) : 0;
        uintptr_t te = canary_bad_slot != NULL
                ? atomic_load_explicit(&canary_bad_slot->tlb_end, memory_order_relaxed) : 0;
        if (tb != 0) {
            p = canary_put(p, "\n  the bad word is at ");
            p = canary_hex(p, bad);
            p = canary_put(p, "; this thread's struct tlb spans ");
            p = canary_hex(p, tb);
            p = canary_put(p, "..");
            p = canary_hex(p, te);
            p = canary_put(p, " -> the word is ");
            if (bad >= tb && bad < te) {
                p = canary_put(p, "INSIDE the tlb, at tlb+");
                p = canary_hex(p, (uint64_t) (bad - tb));
            } else if (bad < tb) {
                p = canary_put(p, "below the tlb by ");
                p = canary_hex(p, (uint64_t) (tb - bad));
                p = canary_put(p, " bytes");
            } else {
                p = canary_put(p, "above the tlb by ");
                p = canary_hex(p, (uint64_t) (bad - te));
                p = canary_put(p, " bytes");
            }
        }
    }
    if (canary_bad_slot != NULL) {
        p = canary_put(p, "\n previous value of this word=");
        p = canary_hex(p, canary_bad_slot->last_cleanup);
        p = canary_put(p, " seen ");
        p = canary_dec(p, canary_pass - canary_bad_slot->last_cleanup_pass);
        p = canary_put(p, " scan pass(es) ago; owner in do_exit unregister: ");
        p = canary_put(p, atomic_load_explicit(&canary_bad_slot->leaving, memory_order_relaxed) ? "YES" : "no");
        p = canary_put(p, "; __cleanup_stack at this thread's birth=");
        p = canary_hex(p, canary_bad_slot->birth_cleanup);
        p = canary_put(p, "; slot still registered: ");
        p = canary_put(p, atomic_load_explicit(&canary_bad_slot->self, memory_order_relaxed) != 0 ? "yes" : "NO (raced with its own exit)");
    }
    if (canary_report_where != NULL) {
        p = canary_put(p, "\n caught by the victim's OWN thread at: ");
        p = canary_put(p, canary_report_where);
        p = canary_put(p, "; last seen good at: ");
        p = canary_put(p, canary_self_where != NULL ? canary_self_where : "(never checked)");
    }
    p = canary_put(p, "\n siglongjmps out of sigusr1_handler so far: ");
    p = canary_dec(p, (uint64_t) atomic_load_explicit(&canary_unwinds_total, memory_order_relaxed));
    p = canary_put(p, ", of which with a live cleanup record: ");
    p = canary_dec(p, (uint64_t) atomic_load_explicit(&canary_unwinds_with_record, memory_order_relaxed));
    p = canary_put(p, "\n image slide=");
    p = canary_hex(p, (uint64_t) _dyld_get_image_vmaddr_slide(0));
    p = canary_put(p, "\n victim words:");
    for (int i = 0; i < 16; i++) {
        p = canary_put(p, i % 8 == 0 ? "\n  +" : " ");
        if (i % 8 == 0) {
            p = canary_dec(p, (uint64_t) (i * 8));
            p = canary_put(p, ":");
        }
        p = canary_hex(p, ((const uint64_t *) self)[i]);
    }
    int covered = canary_watch_covers(self + offset);
    p = canary_put(p, "\n hardware watchpoint on this word: ");
    p = canary_put(p, covered < 0 ? "off (ISH_PTHREAD_WATCH unset)"
                   : covered ? "YES -- so no other thread stored it; the owner did"
                             : "no (this thread was not one of the four watched)");
    p = canary_put(p, "\n registered task threads:");
    for (int i = 0; i < CANARY_SLOTS; i++) {
        uintptr_t s = atomic_load_explicit(&canary_slots[i].self, memory_order_relaxed);
        if (s > 1) {
            p = canary_put(p, " ");
            p = canary_hex(p, s);
        }
    }
    p = canary_put(p, "\n");

    if (kr == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nacts; i++) {
            // Each thread costs about a kilobyte of registers and backtrace,
            // and CANARY_SLOTS allows 256 of them. Stop before the end of the
            // buffer rather than running off it -- a debugging aid that
            // corrupts the process it is debugging is worse than useless.
            if (p > buf + sizeof(buf) - 4096) {
                p = canary_put(p, " ...report truncated, too many threads\n");
                break;
            }
            p = canary_put(p, " thread port=");
            p = canary_dec(p, (uint64_t) acts[i]);
            if (acts[i] == me) {
                p = canary_put(p, " (canary watcher)\n");
                continue;
            }
#if defined(__arm64__) || defined(__aarch64__)
            arm_thread_state64_t st;
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(acts[i], ARM_THREAD_STATE64,
                                 (thread_state_t) &st, &count) != KERN_SUCCESS) {
                p = canary_put(p, " <no state>\n");
                continue;
            }
            uint64_t pc = (uint64_t) arm_thread_state64_get_pc(st);
            uint64_t lr = (uint64_t) arm_thread_state64_get_lr(st) & 0x0000ffffffffffffULL;
            uint64_t sp = (uint64_t) arm_thread_state64_get_sp(st);
            uint64_t fp = (uint64_t) arm_thread_state64_get_fp(st);
            // Task stacks are 4 MB with the struct _pthread on top, so an sp
            // just under `self` identifies the victim's own thread.
            if (sp < self && self - sp < TASK_THREAD_STACK_SIZE)
                p = canary_put(p, " (VICTIM)");
            p = canary_put(p, " pc=");
            p = canary_hex(p, pc);
            p = canary_put(p, " lr=");
            p = canary_hex(p, lr);
            p = canary_put(p, " sp=");
            p = canary_hex(p, sp);
            p = canary_put(p, " fp=");
            p = canary_hex(p, fp);
            p = canary_put(p, "\n  x:");
            for (int r = 0; r < 29; r++) {
                p = canary_put(p, " ");
                p = canary_hex(p, (uint64_t) st.__x[r]);
            }
            p = canary_put(p, "\n  bt:");
            p = canary_hex(p, pc);
            p = canary_put(p, " ");
            p = canary_hex(p, lr);
            p = canary_backtrace(p, fp, sp);
            p = canary_put(p, "\n");
#else
            p = canary_put(p, " <not arm64>\n");
#endif
        }
    }
    p = canary_put(p, "*** end ISH_PTHREAD_CANARY report ***\n");
    ssize_t unused = write(2, buf, (size_t) (p - buf));
    (void) unused;
    _exit(66);
}


// ISH_PTHREAD_WATCH=1 (implies the canary above): put an arm64 HARDWARE
// WATCHPOINT on `self + 8` for up to four registered task threads, armed on
// every OTHER thread in the process. The canary tells us the word went bad;
// this tells us which instruction did it, which is the whole question.
//
// Armed on every thread except the word's owner, deliberately: the owner
// writes it constantly and legitimately (libpthread's cleanup push/pop inside
// pthread_cond_wait), so a trap can only be a cross-thread write. If the
// canary keeps firing while this never does, the store is the owner's own --
// which is a result too, and points at the record on its stack rather than at
// the struct.
//
// Four is the hardware's limit, so slots rotate: each newly registered task
// thread takes the next one. With this test's thread churn that samples
// broadly rather than pinning the first four threads for the whole run.
#define CANARY_WATCHPOINTS 4

static _Atomic(uintptr_t) watch_addr[CANARY_WATCHPOINTS];
static _Atomic(uintptr_t) watch_owner[CANARY_WATCHPOINTS];
static _Atomic unsigned watch_mask[CANARY_WATCHPOINTS]; // 0 = one 8-byte word
static _Atomic unsigned watch_claim_counter;
static __thread int canary_my_watch_slot = -1;

// ISH_PTHREAD_WATCH=1     watch `self + 8` alone, eight bytes.
// ISH_PTHREAD_WATCH=stack watch the top 128 KB of the thread's stack as two
//                         masked 64 KB windows, which covers `self + 8` AND
//                         the band where libpthread's pthread_cond_wait
//                         cleanup record lives (measured at self-0x6600 to
//                         self-0x6900 on these call paths). The second mode
//                         exists because a bad `__cleanup_stack` can be
//                         written by the OWNER, out of libpthread's own
//                         cleanup pop, if the record's `__next` field was
//                         corrupted on the stack while the thread was parked
//                         -- a store the narrow watch cannot see. Costs two
//                         of the four hardware slots per thread, so half as
//                         many threads are covered.
#define CANARY_WATCH_OFF 0
#define CANARY_WATCH_WORD 1
#define CANARY_WATCH_STACK 2
#define CANARY_WATCH_RECORD 3
#define CANARY_WATCH_CENSUS 4
#define CANARY_WATCH_WINDOW_BITS 16

static int task_pthread_watch_mode(void) {
    static _Atomic int mode = -1;
    int m = atomic_load_explicit(&mode, memory_order_relaxed);
    if (m < 0) {
        const char *v = getenv("ISH_PTHREAD_WATCH");
        if (v == NULL || *v == '\0' || *v == '0')
            m = CANARY_WATCH_OFF;
        else if (v[0] == 's')
            m = CANARY_WATCH_STACK;
        else if (v[0] == 'r')
            m = CANARY_WATCH_RECORD;
        else if (v[0] == 'c')
            m = CANARY_WATCH_CENSUS;
        else
            m = CANARY_WATCH_WORD;
        atomic_store_explicit(&mode, m, memory_order_relaxed);
    }
    return m;
}

static bool task_pthread_watch_enabled(void) {
    return task_pthread_watch_mode() != CANARY_WATCH_OFF;
}

#if defined(__arm64__) || defined(__aarch64__)
// WCR: BAS = all eight bytes, LSC = store only, PAC = EL0, E = enable.
#define CANARY_WCR_STORE_8 ((0xffULL << 5) | (0x2ULL << 3) | (0x2ULL << 1) | 1ULL)

static void canary_arm_thread(mach_port_t thread, uintptr_t skip_owner) {
    arm_debug_state64_t ds;
    memset(&ds, 0, sizeof(ds));
    for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
        uintptr_t addr = atomic_load_explicit(&watch_addr[i], memory_order_acquire);
        uintptr_t owner = atomic_load_explicit(&watch_owner[i], memory_order_acquire);
        if (addr == 0 || owner == skip_owner)
            continue;
        unsigned mask = atomic_load_explicit(&watch_mask[i], memory_order_acquire);
        ds.__wvr[i] = (uint64_t) (mask != 0 ? (addr & ~((1ULL << mask) - 1)) : (addr & ~7ULL));
        ds.__wcr[i] = CANARY_WCR_STORE_8 | ((uint64_t) mask << 24);
    }
    thread_set_state(thread, ARM_DEBUG_STATE64, (thread_state_t) &ds,
                     ARM_DEBUG_STATE64_COUNT);
}

// A watchpoint exception on AArch64 reports the address of the instruction
// that made the access, and returning would just re-run it, so this never
// returns.
static void canary_disarm_thread(mach_port_t thread) {
    arm_debug_state64_t ds;
    memset(&ds, 0, sizeof(ds));
    thread_set_state(thread, ARM_DEBUG_STATE64, (thread_state_t) &ds,
                     ARM_DEBUG_STATE64_COUNT);
}

// In record mode the watched address is `head + 16` as of the last sweep, and
// the owner may have popped that record since -- at which point the bytes are
// ordinary stack again and somebody storing there is doing nothing wrong. The
// first version of this handler exited on any trap and reported ten of those
// as the writer. So: decide, and only stop for a store into a record that is
// STILL the owner's live head. A stale trap disarms this thread and returns,
// letting the store complete; the watcher's next sweep re-arms it.
static bool canary_watch_trap_is_live(uint64_t far) {
    if (task_pthread_watch_mode() != CANARY_WATCH_RECORD)
        return true;
    for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
        uintptr_t owner = atomic_load_explicit(&watch_owner[i], memory_order_relaxed);
        uintptr_t addr = atomic_load_explicit(&watch_addr[i], memory_order_relaxed);
        if (owner == 0 || addr == 0 || (far & ~7ULL) != (addr & ~7ULL))
            continue;
        return ((const volatile uint64_t *) owner)[1] + 16 == addr;
    }
    return false; // not one of ours any more
}

static _Atomic long canary_watch_stale_traps;

// ISH_PTHREAD_WATCH=census: do not stop at the first trap. Record the storing
// pc, disarm this thread, and carry on, so a run ends with a list of every
// distinct instruction that stored into another task thread's host stack. The
// answer is expected to be short -- writes into a sibling's stack are not a
// thing AOK should be doing at all -- and anything on it that is not already
// accounted for is a candidate for the corruption this file is chasing.
#define CANARY_CENSUS_MAX 64
static struct {
    _Atomic(uint64_t) pc;
    _Atomic long count;
} canary_census[CANARY_CENSUS_MAX];

static void canary_census_record(uint64_t pc) {
    for (int i = 0; i < CANARY_CENSUS_MAX; i++) {
        uint64_t seen = atomic_load_explicit(&canary_census[i].pc, memory_order_acquire);
        if (seen == pc) {
            atomic_fetch_add_explicit(&canary_census[i].count, 1, memory_order_relaxed);
            return;
        }
        if (seen == 0) {
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong(&canary_census[i].pc, &expected, pc)) {
                atomic_fetch_add_explicit(&canary_census[i].count, 1, memory_order_relaxed);
                return;
            }
            i--; // someone else took the slot; re-read it
        }
    }
}

static void canary_census_dump(void) {
    if (task_pthread_watch_mode() != CANARY_WATCH_CENSUS)
        return;
    static char buf[8192];
    char *p = buf;
    p = canary_put(p, "\n*** ISH_PTHREAD_WATCH census: stores into another task thread's stack ***\n image slide=");
    p = canary_hex(p, (uint64_t) _dyld_get_image_vmaddr_slide(0));
    p = canary_put(p, "\n");
    for (int i = 0; i < CANARY_CENSUS_MAX; i++) {
        uint64_t pc = atomic_load_explicit(&canary_census[i].pc, memory_order_relaxed);
        if (pc == 0)
            break;
        p = canary_put(p, "  ");
        p = canary_dec(p, (uint64_t) atomic_load_explicit(&canary_census[i].count, memory_order_relaxed));
        p = canary_put(p, " x pc=");
        p = canary_hex(p, pc);
        p = canary_put(p, "\n");
    }
    ssize_t unused = write(2, buf, (size_t) (p - buf));
    (void) unused;
}

static void canary_watch_trap(int UNUSED(sig), siginfo_t *UNUSED(info), void *ucontext) {
    ucontext_t *uc = (ucontext_t *) ucontext;
    if (task_pthread_watch_mode() == CANARY_WATCH_CENSUS) {
        // Filter by ADDRESS, not by window geometry. The masked windows are
        // power-of-two aligned and the struct sits at an arbitrary offset in
        // one, so the pair inevitably reaches some way ABOVE `self` -- into
        // whatever the host mapped next, which with ISH_MEM_QUARANTINE on is
        // often a guest page. Censusing by window put three JIT gadgets on the
        // list, storing to guest memory exactly as they should, and they read
        // like a spectacular finding. Only a store strictly below `self` and
        // within this thread's 4 MB stack is a store into another thread's
        // stack; the struct page above it is libpthread's own thread-list
        // traffic, which is legitimate and would drown everything else.
        uint64_t far = (uint64_t) uc->uc_mcontext->__es.__far;
        bool in_a_stack = false;
        for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
            uintptr_t owner = atomic_load_explicit(&watch_owner[i], memory_order_relaxed);
            if (owner != 0 && far < owner && owner - far <= TASK_THREAD_STACK_SIZE)
                in_a_stack = true;
        }
        if (in_a_stack)
            canary_census_record((uint64_t) uc->uc_mcontext->__ss.__pc);
        canary_disarm_thread(mach_thread_self());
        return;
    }
    if (!canary_watch_trap_is_live((uint64_t) uc->uc_mcontext->__es.__far)) {
        atomic_fetch_add_explicit(&canary_watch_stale_traps, 1, memory_order_relaxed);
        canary_disarm_thread(mach_thread_self());
        return;
    }
    static char buf[64 * 1024];
    char *p = buf;
    p = canary_put(p, "\n*** ISH_PTHREAD_WATCH: store into a LIVE watched word ***\n stale traps skipped so far: ");
    p = canary_dec(p, (uint64_t) atomic_load_explicit(&canary_watch_stale_traps, memory_order_relaxed));
    p = canary_put(p, "\n stored to=");
    p = canary_hex(p, (uint64_t) uc->uc_mcontext->__es.__far);
    p = canary_put(p, " by pc=");
    uint64_t pc = (uint64_t) uc->uc_mcontext->__ss.__pc;
    uint64_t lr = (uint64_t) uc->uc_mcontext->__ss.__lr & 0x0000ffffffffffffULL;
    uint64_t sp = (uint64_t) uc->uc_mcontext->__ss.__sp;
    uint64_t fp = (uint64_t) uc->uc_mcontext->__ss.__fp;
    p = canary_hex(p, pc);
    p = canary_put(p, "\n image slide=");
    p = canary_hex(p, (uint64_t) _dyld_get_image_vmaddr_slide(0));
    p = canary_put(p, "\n watched:");
    for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
        uintptr_t addr = atomic_load_explicit(&watch_addr[i], memory_order_relaxed);
        if (addr == 0)
            continue;
        p = canary_put(p, " ");
        p = canary_hex(p, addr);
    }
    // The one thing that decides whether this trap is the bug or the
    // instrument: in record mode the watched address is `head + 16` as of the
    // last sweep, and the owner may have popped that record since. If its
    // CURRENT head still names the record, the store landed on a live one --
    // the owner is parked in pthread_cond_wait and cannot have made it. If not,
    // the address went stale and this store is somebody's legitimate business.
    uint64_t far = (uint64_t) uc->uc_mcontext->__es.__far;
    for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
        uintptr_t owner = atomic_load_explicit(&watch_owner[i], memory_order_relaxed);
        uintptr_t addr = atomic_load_explicit(&watch_addr[i], memory_order_relaxed);
        if (owner == 0 || addr == 0 || (far & ~7ULL) != (addr & ~7ULL))
            continue;
        uint64_t head = ((const volatile uint64_t *) owner)[1];
        p = canary_put(p, "\n owner=");
        p = canary_hex(p, owner);
        p = canary_put(p, " its __cleanup_stack now=");
        p = canary_hex(p, head);
        p = canary_put(p, head + 16 == addr
                ? " -- STILL THE LIVE RECORD: this store is the corruption"
                : " -- record already popped: the watch address went stale, this store is legitimate");
    }
    p = canary_put(p, "\n storing thread sp=");
    p = canary_hex(p, sp);
    p = canary_put(p, " fp=");
    p = canary_hex(p, fp);
    p = canary_put(p, "\n  x:");
    for (int r = 0; r < 29; r++) {
        p = canary_put(p, " ");
        p = canary_hex(p, (uint64_t) uc->uc_mcontext->__ss.__x[r]);
    }
    p = canary_put(p, "\n  bt:");
    p = canary_hex(p, pc);
    p = canary_put(p, " ");
    p = canary_hex(p, lr);
    p = canary_backtrace(p, fp, sp);
    p = canary_put(p, "\n*** end ISH_PTHREAD_WATCH report ***\n");
    ssize_t unused = write(2, buf, (size_t) (p - buf));
    (void) unused;
    _exit(67);
}

static void canary_watch_install_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = canary_watch_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL);
}

// Re-arm the whole process. New threads start with a clean debug state and
// this test creates them constantly, so this has to be periodic rather than
// one-shot. The owner of each watched word is identified by its stack pointer
// landing inside its own 4 MB stack, below the struct.
static void canary_rearm_all(void) {
    thread_act_array_t acts = NULL;
    mach_msg_type_number_t nacts = 0;
    if (task_threads(mach_task_self(), &acts, &nacts) != KERN_SUCCESS)
        return;
    mach_port_t me = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < nacts; i++) {
        if (acts[i] == me)
            continue;
        arm_thread_state64_t st;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        uintptr_t owner = 0;
        if (thread_get_state(acts[i], ARM_THREAD_STATE64,
                             (thread_state_t) &st, &count) == KERN_SUCCESS) {
            uint64_t sp = (uint64_t) arm_thread_state64_get_sp(st);
            for (int w = 0; w < CANARY_WATCHPOINTS; w++) {
                uintptr_t o = atomic_load_explicit(&watch_owner[w], memory_order_acquire);
                if (o != 0 && sp < o && o - sp < TASK_THREAD_STACK_SIZE) {
                    owner = o;
                    break;
                }
            }
        }
        canary_arm_thread(acts[i], owner);
        mach_port_deallocate(mach_task_self(), acts[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t) acts, nacts * sizeof(*acts));
}

// Positive control for the arming itself: a watchpoint that was never applied
// looks exactly like a watchpoint that was never hit. ISH_PTHREAD_WATCH_SELFTEST=1
// arms this thread too and then stores a watched word's own value back over
// itself -- harmless, and a watchpoint traps on the store whatever the value
// is. Reaching the line after the store means the arming is not working, and
// says so rather than reporting a quiet, meaningless pass.
static bool canary_watch_selftest(void) {
    uintptr_t addr = atomic_load_explicit(&watch_addr[0], memory_order_acquire);
    if (addr == 0)
        return false; // nothing watched yet -- try again on the next sweep
    canary_arm_thread(mach_thread_self(), 0);
    volatile uint64_t *w = (volatile uint64_t *) addr;
    *w = *w;
    static const char msg[] = "ISH_PTHREAD_WATCH selftest: NO TRAP -- the watchpoints are not armed\n";
    ssize_t unused = write(2, msg, sizeof(msg) - 1);
    (void) unused;
    _exit(68);
}

static int canary_watch_covers(uintptr_t addr) {
    if (!task_pthread_watch_enabled())
        return -1;
    for (int i = 0; i < CANARY_WATCHPOINTS; i++) {
        uintptr_t a = atomic_load_explicit(&watch_addr[i], memory_order_relaxed);
        if (a == 0)
            continue;
        unsigned mask = atomic_load_explicit(&watch_mask[i], memory_order_relaxed);
        if (mask == 0 ? a == addr
                      : (a >> mask) == (addr >> mask))
            return 1;
    }
    return 0;
}

// Where the owning thread was when the invariant last held. If the store is
// made by the victim's own thread -- which is what the hardware watchpoint's
// silence points at -- this says which half of the loop it happens in: guest
// execution under the JIT, or AOK's own interrupt handling. Three loads and a
// branch per guest syscall, only when the canary is on.
void task_pthread_canary_note_tlb(const void *tlb, unsigned long size) {
    if (!task_pthread_canary_enabled())
        return; // a plain static, not TLS -- see task_pthread_canary_note_unwind
    struct canary_slot *slot = canary_my_slot;
    if (slot == NULL)
        return;
    atomic_store_explicit(&slot->tlb_base, (uintptr_t) tlb, memory_order_release);
    atomic_store_explicit(&slot->tlb_end, (uintptr_t) tlb + size, memory_order_release);
}

// Called from sigusr1_handler immediately before it siglongjmps. A non-empty
// cleanup list at that moment means the jump is abandoning a record on a frame
// that is about to cease to exist -- libpthread pushes one inside every
// pthread_cond_wait, and nothing pops it on this path. That leaves
// __cleanup_stack aimed at dead stack, which later gets reused by ordinary
// calls, and pthread_exit walks it at the end of the thread's life.
// No longer called from sigusr1_handler, and it must not be again: reading
// canary_my_slot there is a __thread access, and the first one on a thread goes
// through dyld's _tlv_get_addr, which mallocs. A signal that lands while the
// interrupted code holds the malloc lock -- pthread_exit freeing its TSD is the
// case that actually happened -- then aborts the process in
// _os_unfair_lock_recursive_abort. Kept because the counter it feeds is what
// killed the leaked-cleanup-record theory, but it is for ordinary contexts now.
void task_pthread_canary_note_unwind(void) {
    if (!task_pthread_canary_enabled())
        return;
    if (canary_my_slot == NULL)
        return;
    atomic_fetch_add_explicit(&canary_unwinds_total, 1, memory_order_relaxed);
    uintptr_t self = atomic_load_explicit(&canary_my_slot->self, memory_order_relaxed);
    if (self == 0)
        return;
    uint64_t head = ((const volatile uint64_t *) self)[1];
    if (head == 0)
        return;
    atomic_fetch_add_explicit(&canary_unwinds_with_record, 1, memory_order_relaxed);
    canary_report_where = "in sigusr1_handler, about to siglongjmp out of a live cleanup record";
    canary_bad_slot = canary_my_slot;
    canary_bad_record = (uintptr_t) head;
    canary_report(self, 8, 0, head);
}

// `where` names the check site. `must_be_empty` is the strong form: at the top
// of task_run_current's loop this thread is inside no pthread_cond_wait at all,
// so its cleanup list must be EMPTY, not merely "a plausible stack pointer". A
// record still on the list there was leaked by some non-local exit -- its frame
// is gone, ordinary calls are about to reuse those bytes, and pthread_exit will
// walk the corpse at the end of this thread's life. That is the shape the crash
// has, so it is worth catching directly rather than waiting for the value.
void task_pthread_canary_check_self_at(const char *where, bool must_be_empty) {
    if (!task_pthread_canary_enabled())
        return; // knob first: with it off this must not even touch TLS
    if (canary_my_slot == NULL)
        return;
    uintptr_t self = atomic_load_explicit(&canary_my_slot->self, memory_order_relaxed);
    if (self == 0)
        return;
    uint64_t head = ((const volatile uint64_t *) self)[1];
    bool ok = must_be_empty
            ? head == 0
            : (head == 0 || (head < self && self - head <= TASK_THREAD_STACK_SIZE));
    if (ok) {
        canary_self_where = where;
        return;
    }
    canary_report_where = where;
    canary_bad_slot = canary_my_slot;
    if (must_be_empty && head != 0)
        canary_bad_record = (uintptr_t) head;
    canary_report(self, 8, 0, head);
}

void task_pthread_canary_check_self(const char *where) {
    task_pthread_canary_check_self_at(where, false);
}

// ISH_PTHREAD_WATCH=record: watch `__next` of the cleanup record each parked
// thread currently has on its stack, armed on every thread EXCEPT its owner.
// This is the word that was measured going from 0 to 0x100000000 while its
// owner was blocked inside __psynch_cvwait and could not have written it
// itself, and unlike the struct _pthread it carries no legitimate cross-thread
// traffic at all -- so a trap here is the store, with the pc that made it.
// The record moves as threads park and wake, so the addresses are recomputed
// on every re-arm sweep rather than claimed once.
static void canary_watch_refresh_records(void) {
    int w = 0;
    for (int i = 0; i < CANARY_SLOTS && w < CANARY_WATCHPOINTS; i++) {
        uintptr_t self = atomic_load_explicit(&canary_slots[i].self, memory_order_acquire);
        if (self <= 1)
            continue;
        uint64_t head = ((const volatile uint64_t *) self)[1];
        if (head == 0 || head >= self || self - head > TASK_THREAD_STACK_SIZE)
            continue;
        atomic_store_explicit(&watch_owner[w], self, memory_order_release);
        atomic_store_explicit(&watch_mask[w], 0, memory_order_release);
        atomic_store_explicit(&watch_addr[w], (uintptr_t) head + 16, memory_order_release);
        w++;
    }
    for (; w < CANARY_WATCHPOINTS; w++) {
        atomic_store_explicit(&watch_addr[w], 0, memory_order_release);
        atomic_store_explicit(&watch_owner[w], 0, memory_order_release);
    }
}

static void canary_watch_claim(uintptr_t self) {
    if (task_pthread_watch_mode() == CANARY_WATCH_RECORD)
        return; // the sweep picks the addresses; nothing to claim here
    unsigned n = atomic_fetch_add_explicit(&watch_claim_counter, 1, memory_order_relaxed);
    if (task_pthread_watch_mode() == CANARY_WATCH_STACK ||
            task_pthread_watch_mode() == CANARY_WATCH_CENSUS) {
        // Two adjacent 64 KB windows. The lower one is anchored on the window
        // holding self-1, the other sits directly below it, so their union
        // always contains [self - 64 KB, self] whatever self's alignment.
        int slot = (int) (n % 2) * 2;
        canary_my_watch_slot = slot;
        uintptr_t top = (self - 1) & ~((1UL << CANARY_WATCH_WINDOW_BITS) - 1);
        for (int k = 0; k < 2; k++) {
            atomic_store_explicit(&watch_owner[slot + k], self, memory_order_release);
            atomic_store_explicit(&watch_mask[slot + k], CANARY_WATCH_WINDOW_BITS, memory_order_release);
            atomic_store_explicit(&watch_addr[slot + k],
                    top - ((uintptr_t) k << CANARY_WATCH_WINDOW_BITS), memory_order_release);
        }
        canary_arm_thread(mach_thread_self(), self);
        return;
    }
    int slot = (int) (n % CANARY_WATCHPOINTS);
    canary_my_watch_slot = slot;
    atomic_store_explicit(&watch_owner[slot], self, memory_order_release);
    atomic_store_explicit(&watch_mask[slot], 0, memory_order_release);
    atomic_store_explicit(&watch_addr[slot], self + 8, memory_order_release);
    // Arm THIS thread now rather than leaving it to the watcher's next sweep.
    // Guest thread churn is the whole point of this test, so a thread that
    // lives less than one sweep is exactly the kind that would otherwise run
    // its entire life unwatched -- and a writer that is never armed looks
    // identical to no writer at all.
    canary_arm_thread(mach_thread_self(), self);
}

static void canary_watch_release(uintptr_t self) {
    int slot = canary_my_watch_slot;
    canary_my_watch_slot = -1;
    if (slot < 0)
        return;
    if (task_pthread_watch_mode() == CANARY_WATCH_RECORD)
        return;
    int mode_now = task_pthread_watch_mode();
    int slots = (mode_now == CANARY_WATCH_STACK || mode_now == CANARY_WATCH_CENSUS) ? 2 : 1;
    for (int k = 0; k < slots; k++) {
        uintptr_t expected = self;
        // Only clear a slot if it is still ours; a later thread may have taken it.
        if (atomic_compare_exchange_strong(&watch_owner[slot + k], &expected, (uintptr_t) 0))
            atomic_store_explicit(&watch_addr[slot + k], 0, memory_order_release);
    }
}
#else
static void canary_watch_install_handler(void) {}
static void canary_rearm_all(void) {}
static bool canary_watch_selftest(void) { return true; }
static void canary_watch_refresh_records(void) {}
static int canary_watch_covers(uintptr_t UNUSED(addr)) { return -1; }
static void canary_watch_claim(uintptr_t UNUSED(self)) {}
static void canary_watch_release(uintptr_t UNUSED(self)) {}
#endif

static void *canary_watcher(void *arg) {
    (void) arg;
    pthread_setname_np("ish-canary");
    bool watching = task_pthread_watch_enabled();
    bool records = task_pthread_watch_mode() == CANARY_WATCH_RECORD;
    bool census = task_pthread_watch_mode() == CANARY_WATCH_CENSUS;
    bool selftest = watching && getenv("ISH_PTHREAD_WATCH_SELFTEST") != NULL;
    for (;;) {
        for (int i = 0; i < CANARY_SLOTS; i++) {
            uintptr_t self = atomic_load_explicit(&canary_slots[i].self, memory_order_acquire);
            if (self <= 1)
                continue;
            const volatile uint64_t *w = (const volatile uint64_t *) self;
            uint64_t cleanup_stack = w[1];
            uint64_t sig = w[0];
            // A finding only counts if the slot STILL names this thread. The
            // owner clears it in do_exit and then runs on into pthread_exit,
            // where libpthread tears the struct down and hands it to the next
            // thread -- so a check that started before the clear can easily
            // finish on a struct that is no longer the one it was reading.
            // That race is not hypothetical: it produced four "sig changed"
            // reports, every one of them on a thread that had just left the
            // table, and they looked exactly like a real corruption enriched
            // on exiting threads.
            if (atomic_load_explicit(&canary_slots[i].self, memory_order_acquire) != self)
                continue;
            canary_bad_slot = &canary_slots[i];
            if (cleanup_stack != 0 &&
                    (cleanup_stack >= self || self - cleanup_stack > TASK_THREAD_STACK_SIZE))
                canary_report(self, 8, 0, cleanup_stack);
            canary_slots[i].last_cleanup = cleanup_stack;
            canary_slots[i].last_cleanup_pass = canary_pass;
            // `sig` at +0 is snapshotted for the dump but deliberately NOT
            // compared. Every report it ever produced was on a thread already
            // inside its own pthread_exit -- the struct is being torn down and
            // handed on there, so the field legitimately stops matching, and
            // re-checking the slot does not close the window. It is also not
            // the word the crash reads. Watching it bought noise only.
            (void) sig;
            // Walk the cleanup chain the exiting thread will walk. `__next`
            // sits at +16 of each record (the same +0x10 _pthread_exit loads),
            // and a bad value there becomes the bad head one pop later --
            // catching it here says the store landed on the stack instead.
            uintptr_t rec = (uintptr_t) cleanup_stack;
            uint64_t head_next = 0;
            bool have_head_next = false;
            for (int depth = 0; rec != 0 && depth < 8; depth++) {
                uint64_t next = ((const volatile uint64_t *) rec)[2];
                if (depth == 0) {
                    head_next = next;
                    have_head_next = true;
                }
                bool next_ok = next == 0 ||
                        (next < self && self - next <= TASK_THREAD_STACK_SIZE);
                // Re-read the head: if the thread popped while we were looking,
                // `rec` is a dead frame and whatever sits there is not a record
                // at all. Only a value that survives the re-read counts.
                if (!next_ok &&
                        ((const volatile uint64_t *) self)[1] == cleanup_stack) {
                    canary_bad_record = rec;
                    canary_prev_head = canary_slots[i].last_head;
                    canary_prev_next = canary_slots[i].last_next;
                    canary_have_prev_next = canary_slots[i].last_head == (uintptr_t) cleanup_stack;
                    canary_report(self, 8, 0, next);
                }
                if (!next_ok)
                    break;
                rec = (uintptr_t) next;
            }
            canary_slots[i].last_head = (uintptr_t) cleanup_stack;
            canary_slots[i].last_next = have_head_next ? head_next : 0;
        }
        uint64_t pass = atomic_fetch_add_explicit(&canary_epoch, 1, memory_order_release);
        canary_pass = pass;
        // Threads are created constantly here and start with a clean debug
        // state, so the watchpoints have to be re-applied, not just set once.
        if (census && (pass % 2000000) == 0 && pass != 0)
            canary_census_dump();
        if (watching && (pass % (records ? 512 : 8192)) == 0) {
            if (records)
                canary_watch_refresh_records();
            canary_rearm_all();
            if (selftest && canary_watch_selftest())
                selftest = false;
        }
    }
    return NULL;
}

static void canary_start_watcher(void) {
    if (task_pthread_watch_enabled())
        canary_watch_install_handler();
    // No atexit here: the guest's exit path leaves through _exit and never
    // runs handlers, so a census registered that way prints nothing. The
    // watcher dumps it periodically instead, and the last dump of a run is the
    // complete picture.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attr, canary_watcher, NULL) != 0)
        die("ISH_PTHREAD_CANARY: could not start the watcher thread");
    pthread_attr_destroy(&attr);
}

static void task_pthread_canary_register(void) {
    if (!task_pthread_canary_enabled())
        return;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, canary_start_watcher);

    uintptr_t self = (uintptr_t) pthread_self();
    for (int i = 0; i < CANARY_SLOTS; i++) {
        uintptr_t free_slot = 0;
        if (!atomic_compare_exchange_strong(&canary_slots[i].self, &free_slot, (uintptr_t) 1))
            continue;
        canary_slots[i].sig = ((const uint64_t *) self)[0];
        // A brand new thread must start with an empty cleanup list. If it does
        // not, the bad value was never stored by anything running -- libpthread
        // handed this thread a struct that already had it, and the hunt is for
        // whoever wrote that memory before it was a struct _pthread.
        canary_slots[i].birth_cleanup = ((const uint64_t *) self)[1];
        canary_slots[i].last_cleanup = 0;
        canary_slots[i].last_cleanup_pass = 0;
        atomic_store_explicit(&canary_slots[i].leaving, 0, memory_order_relaxed);
        atomic_store_explicit(&canary_slots[i].self, self, memory_order_release);
        canary_my_slot = &canary_slots[i];
        if (canary_slots[i].birth_cleanup != 0) {
            canary_bad_slot = &canary_slots[i];
            canary_report_where = "at thread registration -- the struct arrived with it";
            canary_report(self, 8, 0, canary_slots[i].birth_cleanup);
        }
        if (task_pthread_watch_enabled())
            canary_watch_claim(self);
        return;
    }
}

// Called from do_exit, immediately before pthread_exit: past this point the
// thread's stack (and the struct on top of it) can go away under the watcher,
// so drop out of the table and wait for two full scan passes to be sure no
// watcher is mid-dereference of our pointer.
void task_pthread_canary_unregister(void) {
    struct canary_slot *slot = canary_my_slot;
    if (slot == NULL)
        return;
    atomic_store_explicit(&slot->leaving, 1, memory_order_release);
    canary_watch_release(atomic_load_explicit(&slot->self, memory_order_relaxed));
    canary_my_slot = NULL;
    atomic_store_explicit(&slot->self, 0, memory_order_release);
    uint64_t start = atomic_load_explicit(&canary_epoch, memory_order_acquire);
    for (int spins = 0; spins < 200000; spins++) {
        if (atomic_load_explicit(&canary_epoch, memory_order_acquire) - start >= 2)
            return;
        sched_yield();
    }
}

#else
// Not Darwin: there is no struct _pthread to watch and no ARM_DEBUG_STATE64 to
// watch it with, so every entry point is a no-op. All of these are called
// unconditionally from task_run_current, do_exit and sigusr1_handler, so they
// have to exist here or the link fails -- which is exactly what the Linux CI
// jobs are for.
static void task_pthread_canary_register(void) {}
void task_pthread_canary_unregister(void) {}
void task_pthread_canary_check_self(const char *UNUSED(where)) {}
void task_pthread_canary_check_self_at(const char *UNUSED(where), bool UNUSED(must_be_empty)) {}
void task_pthread_canary_note_tlb(const void *UNUSED(tlb), unsigned long UNUSED(size)) {}
void task_pthread_canary_note_unwind(void) {}
#endif


static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&task_thread_attr, TASK_THREAD_STACK_SIZE);
#if defined(__APPLE__)
    // Run emulated guest threads one QoS band below the UI thread
    // (USER_INTERACTIVE). A multi-threaded guest workload spawns one OS thread
    // per emulated CPU (e.g. `go run`/`go build` with GOMAXPROCS = ncpu); left
    // at the default priority those threads are CPU-bound at the same band as
    // the main thread and starve the terminal/UI, making the app unresponsive
    // for the duration of the burst. USER_INITIATED still runs guest work
    // promptly on the performance cores but lets the UI preempt it.
    //
    // There is a second reason this exact band is load-bearing, and it is not
    // the reason the line was written: XNU derives a thread's disk I/O
    // THROTTLE TIER from its QoS as well as its CPU band. USER_INITIATED maps
    // to tier 0, which is unthrottled; UTILITY maps to tier 1 and BACKGROUND
    // to tier 2, and tiers 1 through 3 sleep I/O for 5, 15 and 25 ms per
    // throttle period (debug.lowpri_throttle_tier{1,2,3}_io_period_ssd_msecs).
    // Measured on macOS, same file and same reads, only the policy changed:
    // p90 119 us at IOPOL_IMPORTANT against 2888-2962 us at
    // IOPOL_STANDARD/UTILITY/THROTTLE. That is a 25x latency regression paid
    // by every guest read and write that reaches media on these threads, and
    // nothing in the test suite measures I/O latency, so it would ship. So do
    // not lower this band while "tidying", and do not move blocking guest I/O
    // onto a background or utility queue: the CPU-scheduling argument above
    // survives that move and the latency does not. Numbers and the XNU
    // mapping are in docs/simulated_swap_plan.md, section 2.7.
    pthread_attr_set_qos_class_np(&task_thread_attr, QOS_CLASS_USER_INITIATED, 0);
#endif
}

int task_start(struct task *task) {
    // Create the thread with SIGUSR1 blocked so it cannot run sigusr1_handler
    // before task_thread has instantiated its thread-local storage (see
    // signal_thread_locals_init). Otherwise a sibling's TLB-shootdown poke
    // (task_poke_shared_mem -> pthread_kill(.., SIGUSR1)) could be delivered
    // while the new thread is mid-malloc instantiating that storage, making the
    // handler re-enter malloc and abort on the malloc lock. The new thread
    // inherits this mask and unblocks them itself once it is safe.
    sigset_t wake_sigs, oldmask;
    sigemptyset(&wake_sigs);
    sigaddset(&wake_sigs, SIGUSR1);
    sigaddset(&wake_sigs, SIGUSR2); // same reasoning, see util/sync.c
    pthread_sigmask(SIG_BLOCK, &wake_sigs, &oldmask);
    // Test knob: ISH_TEST_FAIL_TASK_START_AFTER=N makes every create after
    // the Nth fail as if the host were at its thread limit, so the unwind
    // path below can be regression-tested without a 16k-thread storm.
    static _Atomic int test_fail_after = -2; // -2 unparsed, -1 disabled
    if (atomic_load_explicit(&test_fail_after, memory_order_relaxed) == -2) {
        const char *env = getenv("ISH_TEST_FAIL_TASK_START_AFTER");
        atomic_store_explicit(&test_fail_after, env != NULL ? atoi(env) : -1,
                              memory_order_relaxed);
    }
    if (atomic_load_explicit(&test_fail_after, memory_order_relaxed) >= 0) {
        static _Atomic int started = 0;
        if (atomic_fetch_add_explicit(&started, 1, memory_order_relaxed) >=
                atomic_load_explicit(&test_fail_after, memory_order_relaxed)) {
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            return _EAGAIN;
        }
    }
    // pthread_create returns a POSITIVE errno on failure (EAGAIN at the host
    // thread limit). The old `< 0` check could never fire, so a failed create
    // was silently ignored: the fully-linked task had no host thread behind
    // it — a ghost that leaked its pid forever and wedged its thread group's
    // exit. Found via the bmt 10k-thread storm benchmark.
    int err = pthread_create(&task->thread, &task_thread_attr, task_thread, task);
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    if (err != 0)
        return _EAGAIN; // matches Linux clone() at the thread/rlimit ceiling
    // Only now does task->thread refer to this task's own host thread rather
    // than the parent's (copied in task_create_); let the per-CPU accounting
    // walker sample it. See host_thread_started in task.h.
    task->host_thread_started = true;
    return 0;
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // Maximum length for thread names in many systems, including Linux
    int result;

#ifdef __APPLE__
    // Never rename the main thread. The iOS app creates the visible
    // terminal's session by running do_execve (which lands here) with
    // `current` set ON the main thread, which stamped UIKit's main thread
    // with a guest name like "login-459" — making every crash report look
    // like a guest-thread crash. (CLI main-thread task loses its cosmetic
    // name too; top shows the process name regardless.)
    if (pthread_main_np())
        return;
#endif

    // Ensure that the name buffer is always null-terminated
    memset(name, 0, sizeof(name));

    // Create the thread name with PID
    //result = snprintf(name, sizeof(name) - 1, "%s-%d", current->comm, current->pid);
    result = snprintf(name, sizeof(name) - 1, "%.7s-%d", current->comm, current->pid);

    // Check if the output was truncated
    if (result >= (int)sizeof(name)) {
        // Handle truncation (e.g., by logging, adjusting the name format, etc.)
        // For this example, we just log a warning
        printk("WARNING: Thread name truncated in update_thread_name(%s).\n", name);
    }

#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

inline void modify_locks_held_count(struct task *task, int value) { // value should only be -1 or 1.
    if ((task == NULL) && (current != NULL)) {
        task = current;
    } else if (task == NULL) {
        return;
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid locks_held delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    // Only the task's own thread ever modifies its own locks_held count — every
    // caller passes current (lock()/unlock() in util/ro_locks.h, jit_crash_fn).
    // So this is single-writer and needs no CAS retry loop; one relaxed RMW is
    // correct. Concurrent readers (exit_wait_needed / task-teardown gating) use
    // relaxed atomic loads and only care whether the count is nonzero. This runs
    // on every lock()/unlock() — several times per guest syscall — so collapsing
    // the old load+CAS-loop to a single atomic add (an LSE `ldadd` on Apple
    // Silicon) measurably trims per-syscall lock overhead.
    int new_count = __atomic_add_fetch(&task->locks_held.count, value, __ATOMIC_RELAXED);

    if (new_count < 0) {
        // Unbalanced unlock (a bug): clamp back to zero rather than underflow.
        __atomic_store_n(&task->locks_held.count, 0, __ATOMIC_RELAXED);
        if (task->pid > 9)
            printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
    }
}

bool current_is_valid(void) {
    if(current != NULL)
        return true;
    
    return false;
}

// Raw return addresses from another task's host thread.
//
// The freeze blame can name the syscall a stuck task is in by reading x8 -- but
// only if it IS in a syscall. A task spinning in guest code, or blocked on a
// lock inside the kernel, leaves x8 holding whatever its LAST syscall was, and
// the blame then points at an innocent path: chronyd on an M4 iPad read
// "blocked in arm64 syscall 72" while every pselect6 shape reproducible on the
// CLI parked correctly. These frames settle that kind of disagreement.
//
// Here rather than in kernel/checkpoint.c because this file can already see the
// Mach headers: including them there makes PAGE_SIZE a runtime variable, which
// silently turns a later `static const char zero[PAGE_SIZE]` into a VLA.
//
// Collected while the thread is suspended, which is only memory reads, and every
// read is bounded to that thread's own stack so a garbage frame pointer ends the
// walk instead of faulting the app. The caller symbolizes AFTER this returns and
// the thread is running again: a suspended thread may hold the malloc or dyld
// lock that dladdr needs, and waiting on it here would hang the caller.
// The frame walk itself, for any host thread that is known to be alive. The
// caller answers that; see task_host_backtrace for a task's.
unsigned host_thread_backtrace(pthread_t thread, uintptr_t *frames, unsigned max) {
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
    if (frames == NULL || max == 0 || pthread_equal(thread, pthread_self()))
        return 0;
    uintptr_t stack_top = (uintptr_t) pthread_get_stackaddr_np(thread);
    uintptr_t stack_bottom = stack_top - pthread_get_stacksize_np(thread);
    mach_port_t th = pthread_mach_thread_np(thread);
    if (th == MACH_PORT_NULL || thread_suspend(th) != KERN_SUCCESS)
        return 0;
    unsigned n = 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(th, ARM_THREAD_STATE64,
                         (thread_state_t) &st, &count) == KERN_SUCCESS) {
        // The same pointer-authentication strip the canary report uses.
        frames[n++] = (uintptr_t) arm_thread_state64_get_pc(st) & 0x0000ffffffffffffULL;
        if (n < max)
            frames[n++] = (uintptr_t) arm_thread_state64_get_lr(st) & 0x0000ffffffffffffULL;
        uintptr_t fp = (uintptr_t) arm_thread_state64_get_fp(st);
        while (n < max && fp >= stack_bottom && fp + 16 <= stack_top && (fp & 7) == 0) {
            const uintptr_t *rec = (const uintptr_t *) fp;
            uintptr_t next = rec[0];
            uintptr_t ret = rec[1] & 0x0000ffffffffffffULL;
            if (ret == 0)
                break;
            frames[n++] = ret;
            if (next <= fp)
                break;
            fp = next;
        }
    }
    thread_resume(th);
    return n;
#else
    (void) thread; (void) frames; (void) max;
    return 0;
#endif
}

unsigned task_host_backtrace(struct task *task, uintptr_t *frames, unsigned max) {
    if (task == NULL)
        return 0;
    // The liveness test task_wake_for_freeze makes, for its reason: a pthread_t
    // whose thread has exited is undefined to touch, not a no-op.
    if (!atomic_load_explicit(&task->host_thread_started, memory_order_acquire) ||
            task->zombie || task->exiting ||
            atomic_load_explicit(&task->exit_finished, memory_order_acquire))
        return 0;
    return host_thread_backtrace(task->thread, frames, max);
}
