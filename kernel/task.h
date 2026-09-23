#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include <stdint.h>
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
#include "kernel/guestprof.h"

// The highest capability number this kernel defines. /proc/sys/kernel/cap_last_cap
// reports it, and it is chosen to match the 4.20 release AOK advertises.
//
// The "full" set is bits 0..CAP_LAST_CAP_ and deliberately NOT all ones. Linux
// never reports a mask wider than the capabilities it actually has -- a 6.12 box
// with cap_last_cap 40 reports CapBnd 000001ffffffffff -- and systemd uses an
// all-ones mask as its own CAP_MASK_UNSET sentinel. pidref_get_capability()
// rejects the WHOLE of /proc/PID/status with EBADMSG the moment a field parses
// to it, so with all-ones AOK every ConditionCapability= in the unit set came
// back "Couldn't determine result for ConditionCapability=CAP_SYS_ADMIN,
// assuming failed: Bad message" and the unit was skipped -- systemd-sysext.socket,
// dev-hugepages.mount, dev-mqueue.mount, sys-kernel-debug.mount and
// sys-kernel-tracing.mount, every boot, in an openSUSE guest.
//
// Nothing gains or loses a capability by this: every capability AOK checks is
// well below 37, so the bits being dropped name capabilities that do not exist.
#define CAP_LAST_CAP_ 37
#define CAP_FULL_LOW_ 0xffffffffu
#define CAP_FULL_HIGH_ ((1u << (CAP_LAST_CAP_ - 31)) - 1)

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
struct native_program;   // kernel/native.h

struct task {
    enum guest_abi abi;
    // Ticks since boot when this task was created -- /proc/<pid>/stat field 22
    // (starttime), which was hardcoded 0. It is the only source for a
    // process's age: ps -o etime, ps -o lstart and top's uptime column are all
    // derived from it, and with 0 every process looked as old as the kernel.
    uint64_t start_time_ticks;
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
    // The same argv flattened into the NUL-separated block /proc/<pid>/cmdline
    // is defined to be. A native program has no guest argv region for procfs to
    // read -- its address space holds no image and no stack, only the scratch
    // the shim marshals through -- so without this every native program shows
    // an empty command line to ps, top and anything else that asks. Owned by
    // the task; written under general_lock because procfs reads it there.
    char *native_cmdline;
    size_t native_cmdline_len;
    // The native program's signal dispositions, owned by kernel/native_libc.c
    // (struct nlibc_sigtable). Here rather than in that file's thread-local
    // storage because a native program's THREADS share one task, and the
    // thread that installs a handler is not always the one that delivers it.
    void *native_sigtable;
    // The native program entry this task is running, or NULL. Read by
    // kernel/checkpoint.c to find its ckpt_dump; see struct native_program.
    const struct native_program *native_running;
    // What that program said about itself when the checkpoint froze it,
    // produced on its own thread and consumed by the writer on another. Owned
    // by the task until the writer takes it.
    char *ckpt_native_state;

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

    // Of the held set, the ones whose handler the program installed with
    // SA_RESTART. The kernel cannot read that off sighand->action: a native
    // program's disposition THERE is the SIG_DFL placeholder the shim leaves
    // behind (nlibc_set_disposition), flags and all, so every native program's
    // SA_RESTART was silently a no-op -- signal_should_restart_syscall() had
    // nothing true to find and answered "do not restart" every time, turning
    // an interruption Linux hides into a guest-visible EINTR. Maintained
    // beside native_held, by the same nlibc_update_held_signals().
    sigset_t_ native_restart;

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

    // ---- the checkpoint freezer (kernel/checkpoint.c) --------------------
    //
    // `wanted` says this task must reach a syscall boundary and park; `frozen`
    // says it has. Two flags rather than one because the freezer has to WAIT
    // for the answer, and "I asked" and "it happened" are different facts --
    // conflating them is how a checkpoint ends up describing a task that was
    // still running.
    //
    // Atomic and nothing else: set by the freezing thread, read and answered
    // by the task's own thread, with no lock between them. A task that is
    // exiting simply never answers, which the freezer's timeout covers.
    _Atomic bool ckpt_freeze_wanted;
    _Atomic bool ckpt_frozen;
    // This task came back from an image, and how many of its syscalls have
    // been traced. Diagnostics only (ISH_CHECKPOINT_DEBUG): a restored guest
    // that loads cleanly and then quietly falls over is this feature's usual
    // failure, and the first few calls each task makes are what name it.
    bool ckpt_restored;
    unsigned ckpt_syscalls_traced;

    struct tgroup *group; // immutable
    struct list group_links;
    pid_t_ pid, tgid; // immutable
    uid_t_ uid, gid;
    uid_t_ euid, egid;
    uid_t_ suid, sgid;
    uid_t_ fsuid, fsgid;

    // Scheduling, which AOK does not act on but must report back faithfully:
    // a process that sets SCHED_IDLE or a nice level and then reads back
    // something else concludes the call failed. Both are per-thread on Linux.
    // nice is the Linux nice value (-20..19); getpriority reports 20 - nice.
    int_t nice;
    // Minor faults and voluntary context switches, for getrusage. Every field
    // but utime/stime was a hard 0 -- a value Linux never produces for a
    // process that has run at all -- so `time -v`, Python's resource module
    // and every wait4 supervisor reported a process that had touched no memory
    // and never blocked. Written only by the owning thread; read by others
    // under pids_lock via rusage_get_task.
    unsigned long minflt;
    // Set by the swap-in path when a slot could not be read, consumed by the
    // page-fault handler to deliver SIGBUS instead of SIGSEGV. Per-task because
    // the fault is; see task_note_swap_io_fault below.
    bool swap_io_fault;
    // Set by tlb_handle_miss when the guest is committing memory and the host
    // is low on headroom; consumed by handle_timer_interrupt, which is the
    // first point after that where this thread holds no mem lock. Written and
    // read only by this task's own thread.
    bool mem_throttle_wanted;

    unsigned long nvcsw;
    // Peak resident size in KB, latched here as well as on the mm: do_exit
    // releases the address space before it snapshots the task's final usage,
    // so a maxrss read only from the mm is 0 by then -- which is exactly the
    // value wait4 and getrusage(RUSAGE_CHILDREN) report, and exactly the
    // consumer (`time -v prog`) that cares most.
    unsigned long maxrss_kb;
    // Policy with the SCHED_RESET_ON_FORK flag still in it, which is exactly
    // what sched_getscheduler returns.
    int_t sched_policy;

    // What the credentials WILL be after the exec currently in progress, and
    // whether it is a privileged (setuid/setgid) one. __do_execve fills these
    // from the executable's stat immediately before the image is loaded, and
    // elf_exec reads them when it builds the aux vector -- which is built
    // before the real credential change below it, so it cannot just read the
    // fields above. AT_SECURE is what tells libc to drop LD_PRELOAD and
    // friends; hardcoding it to 0 made every setuid-root binary loadable with
    // an attacker's shared object.
    bool exec_secure;
    uid_t_ exec_auxv_uid, exec_auxv_euid, exec_auxv_gid, exec_auxv_egid;
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
// Linux's NGROUPS_MAX. The array is heap-allocated because an inline one of
// this size would be 256KB in every task struct; it is NULL when ngroups is 0.
// OWNED, and `*task = *parent` in task_create_ is a shallow copy, so it is
// duplicated there and freed in task_free_final -- see the long comment beside
// native_env there for what aliasing an owned pointer across fork costs.
#define MAX_GROUPS 65536
    unsigned ngroups;
    uid_t_ *groups;
    char comm[16] __strncpy_safe; // locked by general_lock
    bool did_exec; // for that one annoying setsid edge case
    // Bumped by every execve. Linux replaces the whole cred object there
    // (prepare_exec_creds), so a descriptor opened before an exec is no longer
    // one the caller opened "under the credentials it still holds" even though
    // every uid and gid is unchanged -- and that is the only thing separating
    // the two. Recorded on each descriptor; see struct fd's open_creds.
    unsigned exec_gen;

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
        // a job-control group-stop is reported to the tracer, which is the one
        // thing the two attach styles disagree about: see ptrace_group_stop.
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
        // A PTRACE_EVENT_STOP this task owes its tracer: Linux's
        // JOBCTL_TRAP_STOP. PTRACE_INTERRUPT sets it, and so does the clone
        // that hands a seizing tracer a new child, whose first stop it is. The
        // task takes it at its next signal checkpoint, before any signal and
        // whatever its mask says (ptrace_trap_stop_if_pending). Any stop at all
        // answers it (ptrace_stop_common), and a detach drops it.
        //
        // Written under `lock`. Read without it, as task_trap_stop_pending, by
        // every wait that has to end so the task can stop -- the same waits
        // that ask checkpoint_freeze_pending, for the same reason.
        //
        // Until 2026-09-17 this was a queued SIGTRAP, and everything a signal
        // does differently from a flag was a bug. A tracee with SIGTRAP blocked
        // was never stopped, and one ignoring it dropped the interrupt. A new
        // child could not be given one at all: glibc blocks every signal around
        // pthread_create's and posix_spawn's clone, so the child got a SIGSTOP
        // instead, which strace -f injected as a real signal. And an interrupt
        // still queued when the tracer detached was an ordinary SIGTRAP from
        // then on, which is how every process `strace -p` attached to used to
        // die. See tests/manual/ptrace_seize_trap_stop.c and
        // ptrace_detach_survives.c.
        bool trap_stop;
        struct siginfo_ info;
        // Whether this stop has a siginfo at all -- Linux's last_siginfo, which
        // ptrace_stop() sets from the info it is handed and do_jobctl_trap
        // passes as NULL for an UNSEIZED tracee's group-stop. PTRACE_GETSIGINFO
        // fails with EINVAL when it is clear.
        //
        // This is the only thing that tells a classic tracee's group-stop from
        // a signal-delivery-stop of the very same SIGSTOP: both report status
        // 0x137f, and only the group-stop refuses GETSIGINFO. Measured on Linux
        // 6.12.101; it is how gdb and strace tell the two apart, and AOK
        // answered both, so every group-stop read as a signal to re-inject.
        bool has_siginfo;

        // Whether a signal the tracer injects to resume THIS stop is discarded
        // rather than delivered. Linux splits the stops in exactly two: a
        // signal-delivery-stop (ptrace_signal) and a syscall stop
        // (ptrace_report_syscall, which re-sends it explicitly) pass the
        // injected signal on, while do_jobctl_trap and ptrace_event -- every
        // group-stop, PTRACE_INTERRUPT stop and PTRACE_EVENT_* stop -- throw
        // away what ptrace_stop() returns. Measured on Linux 6.12.101:
        // PTRACE_CONT(SIGUSR1) from a group-stop leaves the tracee running and
        // unkilled, while the same injection at a syscall stop kills it.
        //
        // A flag of its own because it has to outlive the tracer's wait: the
        // trap_event this could otherwise be read from is cleared by wait4 as
        // it builds the status word (kernel/exit.c), long before the resume.
        bool stop_discards_signal;

        // PTRACE_LISTEN: the tracer has been shown this tracee's group-stop and
        // asked for it to stay in it rather than be resumed. Linux's
        // JOBCTL_LISTENING. The tracee waits the job-control stop out in
        // group_stop_wait's listening branch without reporting it again, which
        // is what keeps `kill -STOP` working on a process strace is attached
        // to. Two things end a listen: a SIGCONT, reported as a
        // PTRACE_EVENT_STOP carrying SIGTRAP, and a PTRACE_INTERRUPT, which
        // re-reports the group-stop.
        //
        // Written under `lock` by the tracer and by the tracee itself; read
        // without it by group_stop_wait, so accessed atomically like trap_stop.
        bool listening;
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
    // Set while this task is tearing an address space down -- exit, or the
    // execve that replaces one mm with another. A filesystem ->close reached
    // from that teardown must not wait indefinitely for a guest process: the
    // mm is dead either way, and anything that would have answered from a
    // thread of this process has already been reaped. `exiting` covers the
    // exit case on its own; this covers execve, where the task is very much
    // not exiting. See fs/fuse.c's bounded wait.
    bool mm_teardown;
    bool io_block;
    // This task's guestprof slot, or -1. On the task, not in thread-local
    // storage, because the release hook runs on whichever thread reaps the
    // struct. NOT inherited: task_create_ resets it explicitly, for the same
    // reason it resets cpu.poked_ptr -- the whole-struct copy from the parent
    // would otherwise hand a child its parent's slot to publish into.
    int prof_slot;
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
    // Set once do_exit has rolled this thread's final usage into
    // group->rusage. Written and read under group->lock, which is what makes
    // the handover atomic for rusage_get_group_of: from there this thread is
    // either still live-sampled and absent from group->rusage, or present in
    // group->rusage and skipped -- never counted in both. It stays on
    // group->threads for a good while after the roll-up (do_exit unlinks it in
    // exit_tgroup, ~140 lines and a pids_lock acquisition later), and without
    // this flag that whole window double-counted its CPU: measured as
    // getrusage(RUSAGE_SELF) reporting a process total that later went DOWN by
    // exactly one joined thread's worth. The io_dead half of the same handover
    // is made atomic by moving the counters under pids_lock instead; CPU time
    // cannot be moved, because it lives in the host thread.
    bool exit_rusage_counted;
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
    // (whether the wait is interruptible: waiting_interruptible, at the end)
    cond_t *waiting_cond;
    lock_t *waiting_lock;
    bool *waiting_interrupt_flag;
    lock_t waiting_cond_lock;
    bool wait_interrupted;
    // What the signal that interrupted this syscall's wait says about
    // restarting it (wake_waiting_task). It belongs to that one syscall:
    // the dispatchers clear it before each syscall runs, so a syscall that
    // never asks cannot leave it for the next (signal_restart_state_clear).
    bool restart_interrupted_syscall;
    // Same, but under ERESTARTNOHAND rules: set only when the interrupting
    // signal runs no handler. poll/select/epoll consult this one.
    bool restart_interrupted_syscall_nohand;
    // The syscall whose PC has just been rewound was an _ERESTART_NOHAND one,
    // so a handler about to run must cancel the restart. Set by the dispatcher
    // at rewind time, consumed by receive_signal, and cleared as the next
    // syscall starts -- the re-execution, after which there is no restart
    // left to cancel (signal_restart_state_clear).
    bool restart_nohand_pending;

    // Set on the OTHER threads of a group by execve, which must leave exactly
    // one thread standing (Linux's de_thread). It cannot be expressed with an
    // ordinary SIGKILL: receive_signal routes SIGKILL to do_exit_group, which
    // would take down the exec'ing thread too. So the signal is still what
    // wakes and reaches the thread -- all of that machinery is reused -- and
    // this flag only changes the disposition, from "kill the group" to "exit
    // just me". Read in kernel/signal.c's SIGNAL_KILL case.
    bool exit_requested;
    // Set by do_exit as its very last act, so another thread can tell "this
    // task has left the group list" (which happens partway through do_exit)
    // from "this task is finished with its own struct". execve's de_thread
    // needs the second before it can release the old group leader.
    _Atomic bool exit_finished;
    // Linux's restart_block, in the two places AOK needs it: a timed wait
    // restarted after a job-control stop or a checkpoint freeze --
    // poll/select/epoll_wait, or nanosleep -- must resume the deadline it
    // already had, not start its relative timeout over. Set only when such a
    // wait returns _ERESTART_NOHAND, which is how both of those report
    // themselves, and consumed by the re-executed syscall, which is
    // necessarily the very next one this task makes: _ERESTART_NOHAND
    // re-executes the same instruction, and a handler running in between
    // cancels the restart outright (see restart_nohand_pending). Host
    // CLOCK_MONOTONIC; a checkpoint image carries them on the guest's clocks.
    struct timespec poll_restart_deadline;
    bool poll_restart_valid;
    struct timespec sleep_restart_deadline;
    bool sleep_restart_valid;

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

    // The child this task's exec stand-in is waiting on (kernel/native_libc.c,
    // nlibc_exec_standin), or 0. A stand-in is ONLY that wait -- the program it
    // "became" is running as that child -- so a checkpoint restores it as the
    // wait and never by running the command that exec'd a second time.
    //
    // At the END of the struct on purpose: app code reads task fields, and the
    // Xcode build does not reliably recompile it when this header changes, so a
    // field added in the middle shifts every offset under a stale object.
    dword_t native_standin_child;

    // restart_nohand_pending's twin for an _ERESTART rewind: a handler about to
    // run without SA_RESTART cancels the restart (Linux's ERESTARTSYS). Set by
    // the dispatcher at rewind time, consumed by receive_signal, and cleared as
    // the next syscall starts. At the end for the reason given just above.
    bool restart_sys_pending;

    // The ptrace-stop this task is in is a signal-delivery-stop: ptrace.info is
    // the siginfo of a signal it was about to take, which is what a tracer
    // that resumes it with that signal must see delivered (Linux's
    // ptrace_signal). Locked by ptrace.lock. At the end for the reason given
    // above native_standin_child.
    bool ptrace_delivery_stop;

    // This thread's own final CPU time, recorded by do_exit in the same step
    // that rolls it into group->rusage and sets exit_rusage_counted; valid only
    // once that flag is set, and locked by group->lock like it. What
    // /proc/<pid>/task/<tid>/stat reports for a thread whose host thread is
    // gone (rusage_get_thread_cpu). At the end for the reason given above
    // native_standin_child.
    struct timeval_ exit_utime, exit_stime;

    // Whether the wait in waiting_cond is a wait_for, which a signal ends, or a
    // wait_for_ignore_signals, which consumes no interruption: wake_waiting_task
    // records one only for the first. Published and read under
    // waiting_cond_lock. At the end for the reason given above
    // native_standin_child.
    bool waiting_interruptible;

    // The guest clock the sleep behind sleep_restart_deadline was on. Within
    // one process the deadline is simply kept, but a checkpoint image has to
    // carry it on the clock Linux would count it on -- a relative sleep on
    // CLOCK_BOOTTIME counts the time the machine was stopped, one on
    // MONOTONIC or REALTIME does not (kernel/timer_ckpt.h). Set with
    // sleep_restart_valid. At the end for the reason given above
    // native_standin_child.
    uint_t sleep_restart_clock;

    // Linux's restart_block for a timed FUTEX_WAIT: the deadline the parked
    // wait had (futex_restart_futex), and the relative timeout it was given,
    // so that the restarted call -- the same futex, the same timeout --
    // waits out the same deadline instead of its whole timeout again. Only
    // for a restart nothing ran in front of: a handler clears it
    // (receive_signal). Owned by the task itself. At the end for the reason
    // given above native_standin_child.
    struct timespec futex_restart_deadline;
    struct timespec futex_restart_timeout;
    bool futex_restart_timed;

    // Linux's TIF_SIGPENDING, for the process's shared queue (sighand->queue)
    // alone: this thread has been told to take what is queued there. A signal
    // sent to the process tells ONE thread, as Linux's complete_signal does
    // (deliver_signal_to_group_locked), and every other thread leaves the
    // queue alone -- it ends none of their waits and they do not go looking
    // in it (task_group_pending in kernel/signal.h) -- so the signal
    // interrupts the thread that takes it and nobody else. Also set when a
    // sibling hands a signal on (group_signal_retarget) and when this thread's
    // own mask lets a queued one through (group_pending_mask_changed_locked);
    // cleared by the thread itself once nothing there is left for it. Set
    // and cleared under sighand->lock, read without it too. At the end for the
    // reason given above native_standin_child.
    bool group_sigpending;
    // Shared-queue signals this thread was told to take and has since
    // blocked, which it hands on to a sibling once it holds no lock
    // (signal_group_handoff). The thread's own. At the end for the reason
    // given above native_standin_child.
    sigset_t_ group_handoff;
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
// The same, but claiming a SPECIFIC pid rather than the next free one. For
// kernel/checkpoint.c: a restored process has to answer to the pid it had, or
// every $$, getppid() and recorded child pid in the guest is wrong. Returns
// NULL if that pid is already taken or reserved.
struct task *task_create_with_pid(struct task *parent, pid_t_ want);

// Synthetic kernel threads (kernel/task.c). They have no task and no thread --
// just enough of /proc for ps to render them bracketed, which is what programs
// testing "am I in a container" actually look for. pid_kthread_at walks them by
// index for /proc's readdir; it returns 0 past the end.
bool pid_is_kthread(dword_t pid, const char **name_out);
dword_t pid_kthread_at(size_t index);
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
// Put a restored process back in the session and process group the image gives
// it -- see kernel/group.c. Only kernel/checkpoint.c has any business here.
void tgroup_restore_ids(struct task *task, pid_t_ sid, pid_t_ pgid);
void task_leave_session(struct task *task);
// True when no member of this process group has a parent elsewhere in the same
// session -- so nothing outside it could continue it after a stop. Requires
// pids_lock.
bool pgroup_is_orphaned(pid_t_ pgid, pid_t_ sid);

struct posix_timer {
    // For a CLOCK_THREAD_CPUTIME_ID timer: the thread whose CPU clock it
    // counts against. See posix_timer_thread_cpu_now in kernel/time.c.
    pid_t_ cpu_clock_pid;
    // The guest clockid it was created on. timer->clockid is only the host
    // clock under it, which on Darwin is the same for MONOTONIC and BOOTTIME;
    // an absolute arming needs to know which one the deadline is on. (Here,
    // in what was padding, so struct tgroup keeps its size.)
    uint_t clock;
    struct timer *timer;
    int_t timer_id;
    // Armed with TIMER_ABSTIME. An absolute arming on a wall clock is an
    // instant, which the time the machine spends stopped across a checkpoint
    // brings nearer, where a relative one is kept on MONOTONIC and does not
    // (kernel/timer_ckpt.h). Padding again.
    bool abstime;
    struct tgroup *tgroup;
    pid_t_ thread_pid;
    int_t signal;
    union sigval_ sig_value;
    // Overruns counted onto the signal currently queued for this timer, which
    // is what timer_getoverrun reports. Reset when a fresh signal is queued,
    // so a handler that reads it sees the count for the expiration it was
    // just woken for -- the call's only real use. (Linux latches the value at
    // delivery rather than at queueing, so the two differ only for a caller
    // that asks while its own timer signal is still blocked and pending.)
    int_t last_overrun;
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
// POSIX timers per process. Linux has no small fixed cap -- the bound is
// RLIMIT_SIGPENDING, tens of thousands -- and 16 was low enough that an event
// framework holding one timer per source ran out. This is still a fixed array
// (its entries are handed to the timer thread as callback pointers, so it
// cannot be reallocated) but at a size no real program reaches; exhaustion
// reports EAGAIN, which is what Linux gives when it hits its own limit and
// what callers check for.
#define TIMERS_MAX 128
    struct posix_timer posix_timers[TIMERS_MAX];

    struct rlimit_ limits[RLIMIT_NLIMITS_];

    // https://twitter.com/tblodt/status/957706819236904960
    // TODO locking
    bool doing_group_exit;
    // PR_SET_CHILD_SUBREAPER: this process collects the orphans of its whole
    // descendant tree instead of letting them go to init. Consulted by
    // find_new_parent (kernel/exit.c). Strictly per-process: NOT inherited by
    // fork or clone (tgroup_copy clears it), because a subreaper whose
    // children were all subreapers too would only ever be handed the orphans
    // of its immediate children. Preserved across execve, like Linux.
    bool child_subreaper;
    // membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) has been called for
    // this process. Linux refuses PRIVATE_EXPEDITED with EPERM until it has,
    // and a runtime uses that EPERM to learn it must register. Lock:
    // group->lock. Deliberately left INHERITED by tgroup_copy, unlike the
    // per-process flags around it: measured on Linux 6.12, a fork of a
    // registered process gets PRIVATE_EXPEDITED honored rather than the EPERM
    // an unregistered one gets. (Linux keeps the state in the mm, not the
    // signal struct, which is why it comes along.) Cleared on execve, which
    // kernel/exec.c does, matching membarrier_exec_mmap().
    bool membarrier_private_expedited;
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

    // Threads of this process that have exited but are still zombies, because
    // a tracer has not reaped them yet (kernel/exit.c). A traced task's exit is
    // its tracer's to collect, so such a thread stays in the pid table after
    // it has left `threads`. Until the count is back to zero the process
    // cannot be reaped and its exit is not announced -- Linux's
    // delay_group_leader -- which is also what keeps this struct alive for
    // them: it is freed with the leader. Locked by pids_lock.
    //
    // At the end of the struct for the same reason native_standin_child is at
    // the end of struct task.
    int traced_zombies;
    // The last thread has gone while traced_zombies was not zero, so telling
    // the parent (or tracer) is owed to whoever releases the last of them.
    // Locked by pids_lock.
    bool exit_notify_deferred;
    // A SIGCONT resumed this stopped process and its parent has not been told
    // yet: Linux's SIGNAL_CLD_CONTINUED, which the first thread back from the
    // stop takes and reports (group_stop_wait). `continued` is the other half,
    // what a WCONTINUED wait reports, and only that wait clears it -- read in
    // this one's place, every thread coming back from the stop told the parent
    // again, and every stop after an unwaited continue was followed by another
    // CLD_CONTINUED however it ended. A new stop drops it, as Linux's
    // signal_set_stop_flags does. Lock: group->lock.
    bool continue_unannounced;
};

// Is this thread group the leader of its session? Linux keeps this as a
// per-thread-group flag (signal->leader), so it is a property of the PROCESS
// and not of the calling thread -- a thread of a session leader is inside a
// session leader. Comparing the sid against the CALLING task's pid gets that
// wrong for every thread but one; compare against the group leader's.
//
// Caller must hold tgroup->lock (sid) -- leader is immutable.
static inline bool tgroup_is_session_leader(struct tgroup *tgroup) {
    return tgroup->sid == tgroup->leader->pid;
}


static inline bool task_is_leader(struct task *task) {
    return task->group->leader == task;
}

// The parent of `task`'s PROCESS, which is the parent every thread of it
// reports -- getppid(), /proc/<pid>/task/<tid>/stat and status, taskstats --
// as on Linux, where the thread group shares one real_parent. A thread's own
// ->parent will not do: in AOK a thread is a child of the thread that created
// it (kernel/fork.c), so for every thread but the first it is a thread of the
// same process, which then reported itself as its own parent. The leader stays
// its process's leader after it exits, and moves with the process when it is
// reparented. Caller holds pids_lock.
static inline struct task *task_process_parent(struct task *task) {
    struct task *leader = task->group != NULL && task->group->leader != NULL ?
        task->group->leader : task;
    return leader->parent;
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

// Every task ever created, cumulative and monotonic -- /proc/stat's
// "processes" field. It reported the CURRENT live count instead, so it never
// grew and anything sampling it to derive a fork rate (vmstat, sar, monitoring
// agents) saw a flat zero forever.
extern _Atomic uint64_t total_forks;

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
int task_snapshot_collect_all(struct task_snapshot *snapshot);
void task_snapshot_release(struct task_snapshot *snapshot);

dword_t get_count_of_blocked_tasks(void);
// Live processes (thread group leaders) owned by one real uid, for
// RLIMIT_NPROC.
dword_t task_count_for_uid(uid_t_ uid);
// Count one minor fault against the running task, if there is one. Called from
// the emulator's fault paths, which can also run with no current task.
static inline void task_count_minflt(void) {
    if (current != NULL)
        current->minflt++;
}
// Note that THIS fault failed because a swap slot could not be read, so the
// fault handler can deliver SIGBUS rather than SIGSEGV.
//
// Linux distinguishes the two and so must AOK: a page whose contents could not
// be brought back from storage is BUS_ADRERR, "the address is valid but the
// hardware could not deliver it", while SIGSEGV says the mapping was never
// there. A program with a SIGBUS handler -- databases and JITs commonly have
// one -- takes an entirely different branch, and reporting the wrong one sends
// it down the wrong path with no way to tell.
//
// A flag rather than a return value because it has to cross emu/memory.c's
// mem_ptr_fault, which can only answer NULL, and it is consumed by the very
// next thing that runs on this thread. Cleared on read so a later ordinary
// SIGSEGV cannot inherit it.
static inline void task_note_swap_io_fault(void) {
    if (current != NULL)
        current->swap_io_fault = true;
}
static inline bool task_take_swap_io_fault(void) {
    if (current == NULL || !current->swap_io_fault)
        return false;
    current->swap_io_fault = false;
    return true;
}
dword_t get_count_of_alive_tasks(void);
// The guest's 1/5/15-minute load averages, scaled by 65536 as sysinfo(2)
// reports them. Sampled every 5 s by a timer thread, as on Linux, so the
// answer does not depend on who reads it or how often; see kernel/task.c.
void get_guest_loadavg(uint64_t out[3]);

// Time since the guest booted, on a monotonic host clock (the one the guest's
// CLOCK_BOOTTIME reads), with its zero placed at the whole second boot_time
// names. Never goes backward within a boot. The platform get_uptime()
// implementations report this, and so should anything that wants guest
// uptime at a finer grain than their ticks.
uint64_t guest_uptime_ns(void);
// The guest's reading of guest clock `clock` -- a guest clockid, CLOCK_*_ --
// which the host reads as `host_clock`, clockid_to_real's answer for it. For a
// boot-relative one (CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_MONOTONIC_RAW and
// their variants) that is the host's reading minus the guest's origin for that
// clock, so the guest sees time since ITS boot rather than the host's -- the
// host may have been up for weeks. For any other clock (CLOCK_REALTIME, the
// CPU-time clocks) it is the host's reading of host_clock unchanged.
//
// The GUEST clockid, because it is the guest's clock that has the origin, not
// the host clock under it: on Darwin the guest's MONOTONIC and BOOTTIME are
// both the host's CLOCK_MONOTONIC, and after a checkpoint restore they read
// different values (guest_clock_resume). A timer keeps the guest clockid it
// was created with for the same reason.
//
// Every place a guest-visible absolute time on one of these clocks is read or
// interpreted must go through this and not timespec_now(): clock_gettime, and
// the four sites that turn a guest's absolute deadline into an interval
// (clock_nanosleep/timer_settime/timerfd_settime with TIMER_ABSTIME, and
// futex FUTEX_WAIT_BITSET). Everything else in the tree -- the timer thread,
// poll, the socket and futex wait loops -- compares host readings only with
// other host readings and must keep using timespec_now().
struct timespec guest_clock_now(uint_t clock, clockid_t host_clock);
// The same rebasing applied to a reading the caller already took of the host
// clock under `clock`, for clock_gettime -- which must keep reporting the
// host's errno rather than silently substituting a fallback clock the way
// timespec_now does.
struct timespec guest_clock_from_host(uint_t clock, struct timespec host);

// The guest's boot-relative clocks read at one instant, with the host's wall
// clock at that instant: what a checkpoint image carries so that a restored
// machine's clocks go on from where they were instead of restarting at zero.
// All in ns except boot_time, which is kernel/task.c's boot_time -- the whole
// second uptime counts from.
struct guest_clock_reading {
    int64_t monotonic_ns;
    int64_t boottime_ns;    // which is also uptime
    int64_t raw_ns;
    int64_t realtime_ns;
    int64_t boot_time;
};
void guest_clock_read(struct guest_clock_reading *out);
// Put the guest's clocks back where `saved` left them, the way a resume from
// hibernation does on Linux: CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW go on
// from their saved values, and CLOCK_BOOTTIME (uptime) also counts the
// wall-clock time since `saved` was read. For a checkpoint restore, before
// any restored task runs; see kernel/task.c for why. Returns that time, the
// ns BOOTTIME was advanced by.
int64_t guest_clock_resume(const struct guest_clock_reading *saved);
// Start the guest's clocks again from `boot`, as a boot does. What a restore
// that failed after guest_clock_resume puts back, so the fresh boot that
// follows it does not inherit the image's clocks.
void guest_clock_restart(time_t boot);
// The same in 100 Hz ticks, the unit struct uptime_info carries -- for now
// rounded to whole tenths of a second, for the /proc/uptime format reason
// given at the definition.
uint64_t guest_uptime_ticks(void);
// /proc/stat's aggregate "cpu" line, from this process's cumulative user and
// system CPU time in ns: capacity is get_cpu_count() times guest uptime, and
// every field only ever grows. For the platform get_total_cpu_usage()s.
struct cpu_usage;
void guest_cpu_usage_total(uint64_t user_ns, uint64_t system_ns, struct cpu_usage *out);

// Live user/system CPU time of one task's host thread. Works cross-thread.
// Reports 0/0 if the thread is gone or the host won't say (on non-Mach hosts
// the user/system split isn't available and the total is reported as user
// time). The first is in jiffies (USER_HZ = 100); the second in nanoseconds,
// to the host's own precision (microseconds on Darwin).
void task_thread_cpu_time(struct task *task, unsigned long *out_utime, unsigned long *out_stime);
void task_thread_cpu_time_ns(struct task *task, uint64_t *user_ns, uint64_t *system_ns);
// Charges the exiting task's final thread CPU time to its per-virtual-CPU
// accounting slot; called once from do_exit while the host thread still
// exists to be queried. Sets task->cpu_time_banked.
void task_bank_cpu_time(struct task *task);
// Per-emulated-CPU usage for /proc/stat's cpuN lines: each task's real thread
// CPU time charged to slot pid % ncpu (live tasks sampled, exited tasks from
// the banked totals), against the guest's uptime as each slot's capacity. No
// field ever decreases between calls. Returns 0 and a malloc'd
// get_cpu_count()-sized array, or _ENOMEM.
int get_emulated_per_cpu_usage(struct cpu_usage **cpus_usage);

#define MAX_PID (1 << 15) // oughta be enough

// The wrap point for pid allocation, settable through
// /proc/sys/kernel/pid_max. Bounded by MAX_PID, which sizes the table.
dword_t task_pid_max(void);
int task_set_pid_max(dword_t value);

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
// Raw return addresses from another task's HOST thread, innermost first: where
// a task that will not park actually IS. Returns how many were collected, 0 if
// the thread cannot be read. See kernel/task.c.
unsigned task_host_backtrace(struct task *task, uintptr_t *frames, unsigned max);
// The same walk for any host thread the caller knows to be alive.
unsigned host_thread_backtrace(pthread_t thread, uintptr_t *frames, unsigned max);

extern void (*halt_hook)(int status);

#define superuser() (current != NULL && current->euid == 0)

// Linux capability numbers, for the gates below. Only the ones something
// actually checks are listed; add as needed rather than transcribing all 40.
#define CAP_DAC_READ_SEARCH_ 2
#define CAP_FSETID_      4
#define CAP_SETGID_      6
#define CAP_SETUID_      7
#define CAP_SYS_CHROOT_  18
#define CAP_SYS_PACCT_   20
#define CAP_SYS_PTRACE_  19
#define CAP_NET_BIND_SERVICE_ 10
#define CAP_SYS_ADMIN_   21
#define CAP_SYS_NICE_    23
#define CAP_SYS_RESOURCE_ 24
#define CAP_WAKE_ALARM_  35

// The equivalent of Linux's capable(): true if the caller holds `cap` in its
// effective set, or is root. Privileged syscalls gate on this rather than on
// superuser() alone, so a process given a capability without uid 0 behaves the
// way it would on Linux. Implemented in kernel/getset.c, next to the capset
// machinery that maintains the sets.
bool current_capable(unsigned cap);

// What Linux's commit_creds compares to decide that a change of the caller's
// own credentials forgets its parent-death signal (PR_SET_PDEATHSIG): the
// effective and filesystem ids, and the permitted capabilities. Taken with
// cred_change_begin before the change and handed to cred_change_commit after
// it. Implemented in kernel/getset.c.
struct cred_change {
    uid_t_ euid, egid, fsuid, fsgid;
    dword_t cap_permitted[2];
};
void cred_change_begin(struct cred_change *change);
void cred_change_commit(const struct cred_change *change);

// The equivalent of Linux's ptrace_may_access(PTRACE_MODE_ATTACH_FSCREDS):
// may the caller read or write `target`'s memory? Anything that exposes one
// task's address space to another -- /proc/<pid>/mem, process_vm_readv --
// must ask this, because being related to a process (or merely knowing its
// pid) is not permission to read its secrets.
bool current_may_access_task_mem(struct task *target);

// Update the thread name to match the current task, in the format "comm-pid".
// Will ensure that the -pid part always fits, then will fit as much of comm as possible.
void update_thread_name(void);

// ISH_PTHREAD_CANARY=1 only: leave the host-struct-_pthread watch table.
// Must be called on a task thread immediately before it calls pthread_exit --
// see the canary block in kernel/task.c.
void task_pthread_canary_unregister(void);

// ISH_PTHREAD_CANARY=1 only: validate this thread's own __cleanup_stack and
// name `where` if it has gone bad. A no-op with the knob off.
void task_pthread_canary_check_self(const char *where);
void task_pthread_canary_check_self_at(const char *where, bool must_be_empty);

// ISH_PTHREAD_CANARY=1 only: record where this thread's 24 KB `struct tlb`
// stack local lives, so a bad stack address can be reported relative to it.
void task_pthread_canary_note_tlb(const void *tlb, unsigned long size);

// ISH_PTHREAD_CANARY=1 only: report a siglongjmp that is about to abandon a
// live pthread cleanup record. See the definition in kernel/task.c.
void task_pthread_canary_note_unwind(void);

// To collect statics on which tasks are blocked we need to proccess areas
// of code which could block our task (e.g reads or writes). Before executing
// of functions which can block the task, we mark our task as blocked and
// unblock it after the function is executed.
__attribute__((always_inline)) inline int task_may_block_start(void) {
    current->io_block = 1;
    // The profiler's off-CPU bucket. Without it a workload that spends its wall
    // time waiting on a socket reports as "not in guest code", which reads like
    // emulator overhead rather than the network.
    if (unlikely(guestprof_on))
        guestprof_state(GUESTPROF_BLOCKED);
    return 0;
}

__attribute__((always_inline)) inline int task_may_block_end(void) {
    current->io_block = 0;
    if (unlikely(guestprof_on))
        guestprof_state(GUESTPROF_KERNEL);
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
