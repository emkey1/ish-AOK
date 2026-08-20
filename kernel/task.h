#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include <stdatomic.h>
#include "emu/cpu.h"
#include "kernel/abi.h"
#include "kernel/mm.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/resource.h"
#include "kernel/uts.h"
#include "fs/sockrestart.h"
#include "util/list.h"
#include "util/timer.h"
#include "util/sync.h"

extern void task_ref_cnt_mod(struct task *task, int value);

// Define a structure for the pending deletion queue
struct task_pending_deletion {
    struct task *task;
    time_t added_time; // Timestamp when the task was added to the queue
    struct list list; // For linking in the pending deletion list
};

// Global list of tasks pending deletion
extern struct list tasks_pending_deletion_queue;
extern pthread_mutex_t tasks_pending_deletion_lock;

// Per-task I/O accounting, surfaced via /proc/<pid>/io (and eventually
// taskstats). Written only by the owning task's thread from the syscall
// read/write paths; read cross-thread by procfs, so the fields are relaxed
// atomics (free on 64-bit hosts) rather than plain integers.
// rchar/wchar/syscr/syscw count all read/write traffic; read_bytes/
// write_bytes only bytes moved to/from file-backed fds (realfs/fakefs),
// approximating Linux's "hit the storage layer" semantics.
struct task_io_counters {
    _Atomic qword_t rchar;
    _Atomic qword_t wchar;
    _Atomic qword_t syscr;
    _Atomic qword_t syscw;
    _Atomic qword_t read_bytes;
    _Atomic qword_t write_bytes;
    _Atomic qword_t cancelled_write_bytes;
    // Block-I/O delay accounting for taskstats (iotop's IO> column): wall
    // time this task spent inside file-backed read/write ops, and how many
    // such ops. Not printed in /proc/<pid>/io (Linux doesn't either).
    _Atomic qword_t blkio_count;
    _Atomic qword_t blkio_delay_ns;
};

static inline void task_io_counters_add(struct task_io_counters *dst,
        struct task_io_counters *src) {
    atomic_fetch_add_explicit(&dst->rchar, atomic_load_explicit(&src->rchar, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->wchar, atomic_load_explicit(&src->wchar, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->syscr, atomic_load_explicit(&src->syscr, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->syscw, atomic_load_explicit(&src->syscw, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->read_bytes, atomic_load_explicit(&src->read_bytes, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->write_bytes, atomic_load_explicit(&src->write_bytes, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->cancelled_write_bytes, atomic_load_explicit(&src->cancelled_write_bytes, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->blkio_count, atomic_load_explicit(&src->blkio_count, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&dst->blkio_delay_ns, atomic_load_explicit(&src->blkio_delay_ns, memory_order_relaxed), memory_order_relaxed);
}
struct futex; // opaque; defined in kernel/futex.c (see futex_restart_futex below)
struct native_exec_pending; // opaque; defined in kernel/native.c

struct task {
    enum guest_abi abi;
    struct cpu_state cpu;
    bool force_single_step;
    bool force_no_jit_cache;
    struct mm *mm; // locked by general_lock
    struct mem *mem; // pointer to mm.mem, for convenience
    pthread_t thread;
    uint64_t threadid;

    // Set by execve when the program resolves to one implemented natively
    // inside iSH-AOK (kernel/native.h), and consumed where this task would
    // otherwise start executing the loaded image. Hung off the task rather
    // than kept thread-local because a task can be exec'd by a thread that is
    // only impersonating it (kernel/init.c's boot-command launcher does
    // exactly that, then hands the task to its own thread).
    struct native_exec_pending *native_exec;
    // The environment that native program sees, seeded from execve's envp.
    // Here rather than in a global because two native programs really can run
    // at once, one per guest task (kernel/native.h).
    char **native_env;
    // argv as the running native program received it. Kept because Darwin's
    // libc answers _NSGetArgv() about the host process, and a foreign runtime
    // that reads its arguments that way (Rust's std::env::args does) would
    // otherwise get the iSH app's command line.
    char **native_argv;
    int native_argc;

    // Signals a native program has a handler for that the SHIM is blocking on
    // its behalf, and which the program itself has not asked to block.
    //
    // A native program cannot give the kernel a handler -- that would jump the
    // guest CPU into host code -- so the shim blocks the signal and runs the
    // handler at the next syscall checkpoint instead. Blocked means "do not
    // wake this task" everywhere else in the kernel, which is exactly wrong
    // here: the task must wake, so that its next checkpoint can run the
    // handler. Without this, ^C during `sleep 30` under a native bash did
    // nothing until the NEXT keystroke, which the interrupted read then ate.
    //
    // Kept apart from what the program blocked for itself, because that half
    // must go on meaning what it says. kernel/native_libc.c maintains both.
    sigset_t_ native_prog_blocked;
    sigset_t_ native_held;

    struct {
        atomic_int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;

    // How many of reference.count are held by live pidfds (kernel/pidfd.c).
    // A pidfd must keep the struct task allocated (its ref does that via the
    // deferred-free path), but must NEVER gate do_exit's progress: the
    // holder typically learns of the exit by POLLING the pidfd, which only
    // turns readable after do_exit runs. Counting these refs in
    // exit_wait_needed() deadlocked every systemd service whose main
    // process PID 1 tracks by pidfd (v255+ PidRef): the task couldn't exit
    // while the pidfd was open, and PID 1 wouldn't close it until the exit
    // -- each user@ start hung for the full job timeout.
    // exit_wait_needed() subtracts this count.
    atomic_int pidfd_ref_count;
    
    struct {
        pthread_mutex_t lock;
        int count; // Count of locks held by the current task.
    } locks_held;
    
    int stuck_count;

    struct tgroup *group; // immutable
    struct list group_links;
    pid_t_ pid, tgid; // immutable
    uid_t_ uid, gid;
    uid_t_ euid, egid;
    uid_t_ suid, sgid;
    uid_t_ fsuid, fsgid;
    dword_t cap_effective[2];
    dword_t cap_permitted[2];
    dword_t cap_inheritable[2];
    // Ambient set (prctl PR_CAP_AMBIENT): capabilities that survive a
    // root-to-nonroot uid transition into the permitted+effective sets --
    // how systemd's AmbientCapabilities= keeps caps across enforce_user's
    // setuid (and re-asserts them with capset afterwards, which our
    // permitted-subset check would otherwise EPERM). Inherited across fork
    // via the struct copy, like the other sets.
    dword_t cap_ambient[2];
    bool keepcaps;
#define MAX_GROUPS 32
    unsigned ngroups;
    uid_t_ groups[MAX_GROUPS];
    char comm[16] __strncpy_safe; // locked by general_lock
    bool did_exec; // for that one annoying setsid edge case

    struct task_io_counters io;

    struct fdtable *files;
    struct fs_info *fs;
    // Shared with the parent unless CLONE_NEWUTS/unshare(CLONE_NEWUTS) asked
    // for a private one. Never NULL on a live task.
    struct uts_namespace *uts_ns;

    // locked by sighand->lock
    struct sighand *sighand;
    sigset_t_ blocked;
    sigset_t_ pending;
    sigset_t_ waiting; // if nonzero, an ongoing call to sigtimedwait is waiting on these
    struct list queue;
    cond_t pause; // please don't signal this
    // per-thread alternate signal stack (not shared with CLONE_SIGHAND threads)
    guest_addr_t altstack;
    guest_addr_t altstack_size;
    // private
    sigset_t_ saved_mask;
    bool has_saved_mask;

    struct {
        // Locks all ptrace-related things
        lock_t lock;
        cond_t cond;

        bool traced;
        bool stopped;
        // Attached via PTRACE_SEIZE (vs classic TRACEME/ATTACH). Determines how
        // a job-control group-stop is reported to the tracer: a seized tracee
        // gets a PTRACE_EVENT_STOP event-stop (which strace -f recognizes as a
        // group-stop and resumes with PTRACE_CONT(0)); a classic tracee gets a
        // plain signal-stop carrying the stop signal.
        bool seized;
        bool sysgood;
        bool stop_at_syscall;
        bool syscall_stopped;
        dword_t options;
        int signal;
        // A signal the tracer injected via PTRACE_CONT/SYSCALL/etc. It must be
        // delivered (run its action) on the next receive, not re-trapped through
        // signal_delivery_stop — otherwise an injected signal loops forever.
        int deliver_sig;
        struct siginfo_ info;
        int trap_event;
        qword_t eventmsg;
        int syscall;
        struct task *tracer;
    } ptrace;

    // locked by pids_lock
    struct task *parent;
    struct list children;
    struct list siblings;
    struct list ptracees;
    struct list ptrace_siblings;
    // Every open pidfd (kernel/pidfd.c) referencing this task, so the exit
    // path can wake their pollers when this task becomes a zombie.
    struct list pidfds;

    guest_addr_t clear_tid;
    guest_addr_t robust_list;
    dword_t pdeath_signal;
    // /proc/<pid>/oom_score_adj (Linux range -1000..1000, default 0).
    // Inherited across fork via task_create_'s struct copy, matching Linux.
    // We don't model a real OOM killer, so this is stored purely so
    // ExecStart's mandatory oom_score_adj write/verify (systemd-executor
    // calls exit(EXIT_OOM_ADJUST) if this file is missing or rejects a
    // valid value) succeeds.
    int oom_score_adj;

    // locked by pids_lock
    dword_t exit_code;
    bool zombie;
    bool exiting;
    bool io_block;
    // Set once do_exit has banked this task's final thread CPU time into its
    // per-virtual-CPU accounting slot (task_bank_cpu_time); tells the
    // /proc/stat walker to stop live-sampling a thread that may be gone.
    _Atomic bool cpu_time_banked;
    // Set by task_start once this task's own host pthread exists. Until then
    // task->thread still holds the PARENT's pthread (task_create_ copies the
    // whole struct), so the /proc/stat walker sampling a just-forked task
    // would charge the parent's entire accumulated CPU time to the CHILD's
    // virtual-CPU slot -- and that contribution then vanishes once the child's
    // real thread starts, making the slot's counters go backward.
    _Atomic bool host_thread_started;
    // Set while this task sits in task_wait_for_mem_quiesce (no mem read lock
    // held), cleared before it can re-take one. Lets task_poke_shared_mem skip
    // the SIGUSR1 the same way it skips io_block tasks: a parked sibling holds
    // no read lock, so poking it can't help the barrier writer. Relaxed
    // atomics; a stale read is recovered by the writer's every-64-attempts
    // re-poke, same recovery contract as the io_block skip.
    _Atomic bool quiesce_parked;

    // Heap-allocated and refcounted, one reference for each side. It used to
    // live on the stack of the parent's clone() call, which is only safe while
    // the parent is guaranteed to outlive the child's use of it -- and it is
    // not: a fatal signal stops the parent waiting and it returns while the
    // child is still running and still due to touch this struct at its next
    // exec or exit. The last side to release frees it (vfork_info_release).
    //
    // Atomic because vfork_notify() claims it with an exchange rather than
    // under a lock: do_exit() calls in holding task->general_lock, so this
    // cannot be a lock-protected field. See vfork_notify().
    struct vfork_info {
        atomic_int refcount;
        bool done;
        cond_t cond;
        lock_t lock;
    } *_Atomic vfork;
    int exit_signal;

    // lock for anything that needs locking but is not covered by some other lock
    // specifically: comm, mm
    lock_t general_lock;

    struct task_sockrestart sockrestart;

    // current condition/lock, so it can be notified in case of a signal
    cond_t *waiting_cond;
    lock_t *waiting_lock;
    bool *waiting_interrupt_flag;
    lock_t waiting_cond_lock;
    bool wait_interrupted;
    bool restart_interrupted_syscall;

    // SA_RESTART futex lost-wake fix (kernel/futex.c): when a FUTEX_WAIT is
    // interrupted by a signal whose handler restarts the syscall, the waiter
    // dequeues but PINS the futex here (the held ref keeps the object -- and
    // its wake_seq counter -- alive) and snapshots wake_seq. If a FUTEX_WAKE
    // bumps wake_seq while the waiter is off-queue during the handler + SVC
    // restart, the restarted wait honors it as a wake instead of losing it.
    // NULL when nothing is parked. Manipulated only under futex_lock.
    struct futex *futex_restart_futex;
    guest_addr_t futex_restart_uaddr;
    uint64_t futex_restart_wake_seq;

    // Write-end of the notify pipe of the poll the task is currently blocked in
    // (poll_wait), or -1. A thread blocked in real_poll_wait (kevent/epoll_wait)
    // can only be torn out of its host wait by a host signal, and SIGUSR1 is
    // shared with TLB/quiesce pokes, so a guest-signal SIGUSR1 can be coalesced
    // away or consumed in a window where it has no effect -- letting the host
    // wait run to its timeout and return 0 instead of EINTR. Guest-signal
    // delivery writes a byte here in addition to SIGUSR1 so the poll wakes
    // through its (non-lossy) notify pipe and re-checks pending. Guarded by
    // sighand->lock: set/cleared by the waiter in poll_wait, read by the signal
    // sender in deliver_signal_unlocked_locked.
    int poll_notify_fd;
};

// current will always give the process that is currently executing
// if I have to stop using __thread, current will become a macro
extern __thread struct task *current;

static inline void task_set_mm(struct task *task, struct mm *mm) {
    task->mm = mm;
    task->mem = &task->mm->mem;
    task->cpu.mmu = &task->mem->mmu;
}

static inline struct guest_abi_desc task_abi_desc(const struct task *task) {
    return guest_abi_desc(task->abi);
}

static inline bool task_is_64bit(const struct task *task) {
    return guest_abi_is_64bit(task->abi);
}

// Creates a new process, initializes most fields from the parent. Specify
// parent as NULL to create the init process. Returns NULL if out of memory.
// Ends with an underscore because there's a mach function by the same name
struct task *task_create_(struct task *parent);
// A child of current with fork semantics, for a caller that execs into it
// immediately. See kernel/fork.c. NULL on failure, already cleaned up.
struct task *task_fork_for_exec(void);
// Removes the process from the process table and frees it. Must be called with pids_lock.
void task_destroy(struct task *task, int UNUSED(caller));
// Removes the process from the process table. Must be called with pids_lock.
void task_unlink_locked(struct task *task);
// Frees an already-unlinked task, or defers it if references remain.
void task_destroy_unlinked(struct task *task, int UNUSED(caller));
// Full teardown for a task that was created (and possibly exec'd) but whose
// host thread never started (task_start failure); see kernel/fork.c.
void task_never_ran_destroy(struct task *task);

// misc
void vfork_notify(struct task *task);
pid_t_ task_setsid(struct task *task);
void task_leave_session(struct task *task);

struct posix_timer {
    struct timer *timer;
    int_t timer_id;
    struct tgroup *tgroup;
    pid_t_ thread_pid;
    int_t signal;
    union sigval_ sig_value;
};

// struct thread_group is way too long to type comfortably
struct tgroup {
    struct list threads; // locked by pids_lock
    struct task *leader; // immutable
    struct rusage_ rusage;

    // cgroup2 membership path relative to the hierarchy root ("/foo/bar"),
    // recorded when this process's pid is written to a cgroup.procs file on
    // a cgroup2 mount (fs/tmp.c); NULL means the root cgroup. Reported by
    // /proc/<pid>/cgroup. systemd --user derives its own delegated subtree
    // from /proc/self/cgroup -- the hardcoded "0::/" made it try to create
    // init.scope at the HIERARCHY root (EACCES for uid != 0), killing every
    // user@ start with "Failed to allocate manager object". Heap-allocated;
    // tgroup_copy strdups it, task_free_final frees it. Locked by
    // group->lock.
    char *cgroup_path;
    // I/O counters of threads that already exited, rolled up in exit.c so a
    // process's /proc/<pid>/io totals survive its threads. Locked by pids_lock.
    struct task_io_counters io_dead;

    // Process-group/session membership lists are protected by pids_lock.
    // Group-local metadata (sid, pgid, tty) is protected by group->lock.
    pid_t_ sid, pgid;
    struct list session;
    struct list pgroup;

    // Read locklessly on every interrupt-return fast path in handle_interrupt;
    // _Atomic so that unlocked read is well-defined. All writes are still made
    // under group->lock, which also orders the stopped_cond wait/notify.
    _Atomic bool stopped;
    cond_t stopped_cond;

    struct tty *tty;
    struct timer *itimer;
    // ITIMER_VIRTUAL/PROF (kernel/time.c): neither has a native CPU-time
    // clock this codebase's timer subsystem supports (util/timer.h only
    // allows CLOCK_MONOTONIC/CLOCK_REALTIME), so a single periodic
    // CLOCK_MONOTONIC sampler timer drives both. Armed lazily on first use,
    // freed alongside itimer above.
    struct timer *itimer_vprof_sampler;
    struct cpu_itimer_state {
        bool armed;
        struct timespec deadline; // accumulated CPU time (rusage_get_group) at which to next fire
        struct timespec interval; // rearm interval in CPU-time units; zero = one-shot
    } itimer_virtual, itimer_prof;
#define TIMERS_MAX 16
    struct posix_timer posix_timers[TIMERS_MAX];

    struct rlimit_ limits[RLIMIT_NLIMITS_];

    // https://twitter.com/tblodt/status/957706819236904960
    // TODO locking
    bool doing_group_exit;
    dword_t group_exit_code;

    // Set under group->lock when SIGCONT resumes a stopped group; reported once
    // to a parent waiting with WCONTINUED (then cleared). Enables the wait4/
    // waitid pull path for the continue notification (the async SIGCHLD push for
    // CLD_CONTINUED is not modeled). Lock: group->lock.
    bool continued;

    struct rusage_ children_rusage;
    cond_t child_exit;

    dword_t personality;

    // for everything in this struct not locked by something else.
    // Lock ordering: pids_lock -> group->lock -> tty->lock.
    lock_t lock;
};

static inline bool task_is_leader(struct task *task) {
    return task->group->leader == task;
}

struct pid {
    dword_t id;
    struct task *task;
    struct list alive; // list of alive pids
    struct list session;
    struct list pgroup;
};

// @alive_pids_list is used as a head of all active pids.
// Scanning this list, you should start list_for_each from alive_pids_list,
// to avoid having this head element in your cycle.
extern struct list alive_pids_list;

struct task_snapshot {
    struct task **tasks;
    unsigned count;
};

// synchronizes obtaining a pointer to a task and freeing that task
extern lock_t pids_lock;
// these functions must be called with pids_lock
struct pid *pid_get(dword_t pid);
struct pid *pid_get_last_allocated(void);
struct task *pid_get_task(dword_t pid);
struct task *pid_get_task_ref(dword_t pid);
struct task *pid_get_task_zombie(dword_t id); // don't return null if the task exists as a zombie
struct task *pid_get_task_zombie_ref(dword_t id); // ...and take a reference, like pid_get_task_ref
int task_snapshot_collect(struct task_snapshot *snapshot, bool leaders_only);
void task_snapshot_release(struct task_snapshot *snapshot);

dword_t get_count_of_blocked_tasks(void);
dword_t get_count_of_alive_tasks(void);
void get_guest_loadavg(uint64_t out[3]);

// Live user/system CPU time of one task's host thread, in jiffies (USER_HZ =
// 100). Works cross-thread. Reports 0/0 if the thread is gone or the host
// won't say (on non-Mach hosts the user/system split isn't available and the
// total is reported as user time).
void task_thread_cpu_time(struct task *task, unsigned long *out_utime, unsigned long *out_stime);
// Charges the exiting task's final thread CPU time to its per-virtual-CPU
// accounting slot; called once from do_exit while the host thread still
// exists to be queried. Sets task->cpu_time_banked.
void task_bank_cpu_time(struct task *task);
// Per-emulated-CPU usage for /proc/stat's cpuN lines: each task's real thread
// CPU time charged to slot pid % ncpu (live tasks sampled, exited tasks from
// the banked totals). Returns 0 and a malloc'd get_cpu_count()-sized array,
// or _ENOMEM.
struct cpu_usage;
int get_emulated_per_cpu_usage(struct cpu_usage **cpus_usage);

#define MAX_PID (1 << 15) // oughta be enough

// Spawn the host thread that runs the task. Returns 0 on success or
// _EAGAIN if the host cannot create another thread (thread limit/memory);
// on failure the task has NOT started and the caller must unwind it.
int must_check task_start(struct task *task);
void task_run_current(void);
void task_poke_shared_mem(struct task *task, struct mem *mem);

extern void (*exit_hook)(struct task *task, int code);
// Called when the init process (pid 1) exits, before the brutal halt_system_locked()
// teardown. If set (the standalone CLI sets it), it is expected not to return — it
// terminates the host process with a status derived from init's exit code, so the
// host exit status mirrors the guest's instead of the process dying via the
// pthread_kill(SIGKILL) sweep. Left NULL by the iOS app, preserving its behavior.
extern void (*halt_hook)(int status);

#define superuser() (current != NULL && current->euid == 0)

// Update the thread name to match the current task, in the format "comm-pid".
// Will ensure that the -pid part always fits, then will fit as much of comm as possible.
void update_thread_name(void);

// To collect statics on which tasks are blocked we need to proccess areas
// of code which could block our task (e.g reads or writes). Before executing
// of functions which can block the task, we mark our task as blocked and
// unblock it after the function is executed.
__attribute__((always_inline)) inline int task_may_block_start(void) {
    current->io_block = 1;
    return 0;
}

__attribute__((always_inline)) inline int task_may_block_end(void) {
    current->io_block = 0;
    return 0;
}

#define TASK_MAY_BLOCK for (int i = task_may_block_start(); i < 1; task_may_block_end(), i++)

void init_pending_queues(void);
void cleanup_pending_deletions(void);


//
static inline unsigned task_ref_cnt_get(struct task *task, unsigned UNUSED(lock_if_zero)) {
    int tmp = atomic_load_explicit(&task->reference.count, memory_order_acquire);
    if(tmp < 0 || tmp > 1000)  // Work around brain damage.  Remove when said brain damage is fixed
        tmp = 0;
    return (unsigned) tmp;
}


static inline unsigned locks_held_count(struct task *task) {
    if(task->pid < 10)  // Bootstrap tasks are exempt from this accounting path.
        return 0;
    unsigned tmp = __atomic_load_n(&task->locks_held.count, __ATOMIC_RELAXED);

    // Exit/reap paths intentionally hold one bookkeeping lock while asking
    // whether any other locks are still outstanding.  Discount that slot here.
    if (tmp > 0)
        tmp--;

    return tmp;
}


bool current_is_valid(void);
// fun little utility function
static inline int current_pid(struct task *task) {
    if (task == NULL || task->exiting)
        return -1;
    return task->pid;
}

static inline int current_uid(struct task *task) {
    if (task == NULL || task->exiting)
        return -1;
    return task->uid;
}

static inline char * current_comm(struct task *task) {
    static char comm[16];
    if (task == NULL || task->exiting || task->comm[0] == '\0')
        return "";
    strncpy(comm, task->comm, sizeof(comm));
    comm[sizeof(comm) - 1] = '\0';
    return comm;
}

#endif
