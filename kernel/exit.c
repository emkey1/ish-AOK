#include <pthread.h>
#include <signal.h>
#include <string.h>
#include "emu/cpu.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/futex.h"
#include "kernel/ptrace.h"
#include "kernel/task.h"
#include "util/sync.h"
#include "fs/fd.h"
#include "fs/devices.h"
#include "fs/tty.h"

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;
extern dword_t extra_lock_pid;
extern const char extra_lock_comm;

static void halt_system_locked(void);

static bool trace_session_exit_task(struct task *UNUSED(task)) {
    return false;
}

static void amd64_decode_wait_status_exit(int status, char *buf, size_t size) {
    if (size == 0)
        return;
    if ((status & 0xff) == 0x7f) {
        snprintf(buf, size, "stopped sig=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    if ((status & 0x7f) == 0) {
        snprintf(buf, size, "exited code=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    snprintf(buf, size, "signaled sig=%d core=%d status=%#x",
             status & 0x7f, (status & 0x80) != 0, status);
}

// Decode a wait(2)-encoded status word into the SIGCHLD/waitid representation:
// a CLD_* si_code and the *bare* si_status (exit code or signal number). Used at
// the siginfo-marshalling boundary; do_wait keeps child.status as the raw word
// for wait4's int *status, so this never mutates that path.
static void decode_wait_status(dword_t status, int *code_out, int *status_out) {
    if (status == 0xffff) {                     // WIFCONTINUED
        *code_out = CLD_CONTINUED_;
        *status_out = SIGCONT_;
    } else if ((status & 0xff) == 0x7f) {       // WIFSTOPPED (group- or ptrace-stop)
        *code_out = CLD_STOPPED_;
        *status_out = (status >> 8) & 0xff;
    } else if ((status & 0x7f) == 0) {          // WIFEXITED
        *code_out = CLD_EXITED_;
        *status_out = (status >> 8) & 0xff;
    } else {                                    // WIFSIGNALED
        *code_out = (status & 0x80) ? CLD_DUMPED_ : CLD_KILLED_;
        *status_out = status & 0x7f;
    }
}

static bool amd64_trace_task_or_parent_lineage(struct task *task) {
    if (task == NULL)
        return false;
    if (amd64_trace_is_lineage_tgid(task->tgid))
        return true;
    return task->parent != NULL && amd64_trace_is_lineage_tgid(task->parent->tgid);
}

// Removes a task from its thread group. The caller is responsible for ensuring
// the task is quiescent before taking pids_lock and calling this helper.
static bool exit_tgroup(struct task *task) {
    struct tgroup *group = task->group;
    list_remove(&task->group_links);
    bool group_dead = list_empty(&group->threads);
    if (group_dead) {
        // Apply this process's SysV SEM_UNDO adjustments (threads share one
        // undo list via CLONE_SYSVSEM, so it applies at group death).
        sysv_sem_exit(group);
        // don't need to lock the group since the only pointers to it come from:
        // - other threads' current->group, but there are none left thanks to that list_empty call
        // - locking pids_lock first, which do_exit did
        if (group->itimer)
            timer_free(group->itimer);
        if (group->itimer_vprof_sampler)
            timer_free(group->itimer_vprof_sampler);

        // timer_create() timers were never freed here, only the two itimers
        // above. Each one owns a host thread, so a process that created timers
        // and exited leaked them permanently -- host threads climbed 2 -> 2402
        // in the audit's probe at 16 timers per process and never came back,
        // burning CPU forever on a repeating interval, with the callbacks
        // firing into a tgroup that is about to be freed.
        //
        // Clear tgroup BEFORE freeing: posix_timer_callback's first act is to
        // bail when it is NULL, and timer_free does not wait for a callback
        // already in flight -- it flags the timer dead and signals its thread.
        for (int i = 0; i < TIMERS_MAX; i++) {
            struct posix_timer *pt = &group->posix_timers[i];
            if (pt->timer == NULL)
                continue;
            pt->tgroup = NULL;
            timer_free(pt->timer);
            pt->timer = NULL;
        }

        // The group will be removed from its group and session by reap_if_zombie,
        // because fish tries to set the pgid to that of an exited but not reaped
        // task.
        // https://github.com/Microsoft/WSL/issues/2786
    }
    return group_dead;
}

// A function pointer that can be assigned to a cleanup function to be called upon task exit.
void (*exit_hook)(struct task *task, int code) = NULL;

// Optional hook invoked when init (pid 1) exits, before halt_system_locked(). The
// standalone CLI sets this to terminate the host process with init's exit status;
// it does not return. NULL for the iOS app (keeps the existing teardown behavior).
void (*halt_hook)(int status) = NULL;

static inline bool exit_wait_needed(struct task *task) {
    // Refs held by pidfds are excluded: they keep the struct task allocated
    // but must not stall the exit itself -- their holders (systemd's PidRef
    // main-pid tracking, waitid(P_PIDFD) callers) only learn of the exit
    // via pidfd_notify_exit(), which runs AFTER these gates. Counting them
    // here deadlocked the exit against the holder's close-when-dead.
    int refs = task_ref_cnt_get(task, 0) - atomic_load(&task->pidfd_ref_count);
    return refs > 2 || locks_held_count(task);
}

// do_exit's exit_wait_needed() poll loops below used a fixed WAIT_SLEEP (2us)
// interval. Under heavy concurrent /proc readers or a burst of simultaneous
// task exits, that turns into a syscall storm -- hundreds of exiting tasks
// each calling nanosleep() every 2us, with cost dominated by nanosleep()
// itself and the resulting scheduling churn rather than actual contention
// (confirmed via a live sample of a stuck process: do_exit/task_ref_cnt_get/
// nanosleep were the hottest frames, system time far exceeding wall time,
// under `ktop -b` scanning /proc concurrently with a heavy fork/exit churn
// loop -- found chasing a device report of ktop and foot both segfaulting on
// corrupted pointers during a Wayland session's app-launch burst). Starts at
// the same interval used elsewhere for genuinely brief spins, doubling on
// each retry up to a cap so a longer-held reference doesn't turn into an
// unbounded number of syscalls. Each call site uses its own local copy --
// this must never mutate the shared lock_pause global, which other spin-wait
// sites (rw_locks.c, fork.c, task.c) rely on staying at its tuned default.
static void exit_wait_backoff(struct timespec *pause) {
    nanosleep(pause, NULL);
    long ns = pause->tv_nsec * 2;
    if (ns > 1000000) // 1ms cap
        ns = 1000000;
    pause->tv_nsec = ns;
}

// Finds a new parent for the children of a task that is exiting. If no suitable parent
// is found within the task's group, it returns the 'init' task.
// Who inherits `task`'s children. A surviving thread of its own group first --
// the process is not really gone while one of those is running -- then the
// nearest ancestor that asked to be a subreaper (PR_SET_CHILD_SUBREAPER),
// then init. Without the subreaper step every orphan went straight to init,
// so a service manager that had set the flag never saw its own descendants
// die and could not reap them.
//
// Caller holds pids_lock, which is what keeps the ancestor walk's pointers
// alive.
static struct task *find_new_parent(struct task *task) {
    struct task *new_parent;
    list_for_each_entry(&task->group->threads, new_parent, group_links) {
        if (!new_parent->exiting)
            return new_parent;
    }
    // Walk up from the dying process's own parent. A subreaper that is itself
    // exiting is no use, and the loop is bounded by the tree's depth -- plus a
    // hard cap, because a corrupted parent chain must not spin here forever.
    struct task *ancestor = task->group->leader != NULL ? task->group->leader->parent : NULL;
    for (int depth = 0; ancestor != NULL && depth < MAX_PID; depth++) {
        if (ancestor->group != NULL && ancestor->group->child_subreaper &&
                !ancestor->exiting && !ancestor->zombie)
            return ancestor;
        if (ancestor->group != NULL && ancestor->group->leader == ancestor &&
                ancestor->parent == ancestor)
            break;                      // self-parented: nothing above it
        ancestor = ancestor->parent;
    }
    return pid_get_task(1);
}

static bool session_has_other_live_groups(struct pid *sid_pid, struct tgroup *group) {
    struct tgroup *session_group;
    list_for_each_entry(&sid_pid->session, session_group, session) {
        if (session_group != group && !list_empty(&session_group->threads))
            return true;
    }
    return false;
}

static void ptrace_detach_from_tracer(struct task *tracer, struct task *tracee) {
    bool traced_by_tracer = tracee->ptrace.tracer == tracer ||
        (tracee->ptrace.traced && tracee->ptrace.tracer == NULL && tracee->parent == tracer);
    if (!traced_by_tracer)
        return;

    lock(&tracee->ptrace.lock, 0);
    traced_by_tracer = tracee->ptrace.tracer == tracer ||
        (tracee->ptrace.traced && tracee->ptrace.tracer == NULL && tracee->parent == tracer);
    if (traced_by_tracer) {
        tracee->ptrace.traced = false;
        tracee->ptrace.tracer = NULL;
        tracee->ptrace.stop_at_syscall = false;
        tracee->ptrace.syscall_stopped = false;
        tracee->ptrace.signal = 0;
        tracee->ptrace.trap_event = 0;
        tracee->ptrace.eventmsg = 0;
        if (tracee->ptrace.stopped) {
            tracee->ptrace.stopped = false;
            notify(&tracee->ptrace.cond);
        }
    }
    unlock(&tracee->ptrace.lock);

    list_remove_safe(&tracee->ptrace_siblings);
}

// Hang up the controlling terminal as soon as the session leader exits, but
// only once the session is otherwise empty. Waiting until the zombie is reaped
// is too late for PTY users such as script, but hanging up while sshd still
// has a child shell running tears down the remote login immediately.
static void exit_hangup_session_tty(struct task *leader) {
    struct tgroup *group = leader->group;
    lock(&group->lock, 0);
    struct tty *tty = group->tty;
    pid_t_ sid = group->sid;
    unlock(&group->lock);
    if (tty == NULL || sid != leader->pid)
        return;

    struct pid *sid_pid = pid_get(sid);
    if (sid_pid == NULL)
        return;
    if (session_has_other_live_groups(sid_pid, group))
        return;

    int tty_release_count = 0;
    struct tgroup *session_group;
    list_for_each_entry(&sid_pid->session, session_group, session) {
        lock(&session_group->lock, 0);
        if (session_group->tty == tty) {
            session_group->tty = NULL;
            tty_release_count++;
        }
        unlock(&session_group->lock);
    }

    lock(&ttys_lock, 0);
    lock(&tty->lock, 0);
    tty->session = 0;
    tty->fg_group = 0;
    // Return value deliberately dropped. This is the session leader giving up
    // its own controlling terminal on the way out, and tty->session and
    // tty->fg_group are cleared just above, so there is nobody left to signal.
    // Signalling from inside do_exit would also mean sending under the exit
    // path's own locks, which is not a thing to do untested.
    (void) tty_hangup(tty);
    unlock(&tty->lock);
    while (tty_release_count-- > 0)
        tty_release(tty);
    unlock(&ttys_lock);
}

// Handles the termination of the current task. It releases resources, notifies the parent,
// and re-parents any children. It ensures the task is not in a critical section and that
// all locks are released before proceeding.  At least in theory
noreturn void do_exit(struct task *task, int status) {
    if(task->reference.ready_to_be_freed) {
        // Already queued for deletion by someone else, but the struct is still
        // alive on that queue -- publish here too, or a de_thread waiting on
        // this task would spin out its whole timeout for nothing.
        atomic_store_explicit(&task->exit_finished, true, memory_order_release);
        goto EXIT;
    }
    bool was_already_exiting = task->exiting;
    task->exiting = true;

    // Charge this task's final thread CPU time to its per-virtual-CPU
    // accounting slot (/proc/stat cpuN) while the host thread still exists
    // to be queried. Only on first entry; re-entry would just re-check the
    // banked flag anyway.
    if (!was_already_exiting)
        task_bank_cpu_time(task);

    // Report PTRACE_EVENT_EXIT only on the FIRST entry to do_exit. If we re-enter
    // (ptrace_stop_common can see a pending SIGKILL during this very event-exit
    // stop and call do_exit_group again), reporting the event again recurses
    // do_exit -> ptrace_event_stop -> ptrace_stop_common -> do_exit_group ->
    // do_exit ... until the stack overflows and the whole app crashes.
    if (!was_already_exiting && task == current && task->ptrace.traced &&
            (task->ptrace.options & PTRACE_O_TRACEEXIT_)) {
        struct siginfo_ info = {
            .sig = SIGTRAP_,
            .code = SI_USER_,
            .kill.pid = task->pid,
            .kill.uid = task->uid,
        };
        ptrace_event_stop(SIGTRAP_, &info, PTRACE_EVENT_EXIT_, (qword_t) (dword_t) status);
    }

    if (trace_session_exit_task(task)) {
        printk("INFO: exit session pid=%d tgid=%d comm=%s status=%#x did_exec=%d parent=%d\n",
               task->pid, task->tgid, task->comm, status, task->did_exec,
               task->parent != NULL ? task->parent->pid : -1);
    }

    // Must run before the wait loops below: a self-referencing pidfd this
    // task never closed would otherwise inflate its own reference count
    // forever, since the fd table close that would drop it is itself gated
    // behind those same waits. See pidfd_close_self_refs for the full story.
    pidfd_close_self_refs(task);

    lock(&task->general_lock, 0);

    // has to happen before mm_release
    struct timespec exit_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // Wait for other references and locks, but ignore extra pending signals while exiting.
        exit_wait_backoff(&exit_wait_pause);
    }
    guest_addr_t clear_tid = task->clear_tid;
    if (clear_tid) {
        pid_t_ zero = 0;
        if (user_put(clear_tid, zero) == 0)
            futex_wake(clear_tid, 1);
    }
    // Drop any futex pinned across an SA_RESTART restart window (rare: exiting
    // mid-restart). Must run before mm_release, since a private futex holds a
    // ref keyed on this task's mem. No-op unless a wait was parked.
    futex_release_restart_park();

    // release all our resources
    // mm_release can walk fd/inode teardown and therefore take inodes_lock.
    // Do not hold pids_lock across it, because procfs open/stat takes
    // inodes_lock before pids_lock and that lock ordering otherwise deadlocks.
    struct timespec mm_wait_pause = lock_pause;
    do {
        exit_wait_backoff(&mm_wait_pause);
        exit_wait_backoff(&mm_wait_pause);
    } while (exit_wait_needed(task)); // Wait for now, task is in one or more critical
    mm_release(task->mm);
    task->mm = NULL;
    task->mem = NULL;
    task->cpu.mmu = NULL;

    struct timespec files_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        exit_wait_backoff(&files_wait_pause);
    }
    fdtable_release(task->files);
    task->files = NULL;

    struct timespec fs_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        exit_wait_backoff(&fs_wait_pause);
    }
    fs_info_release(task->fs);
    task->fs = NULL;
    uts_ns_release(task->uts_ns);
    task->uts_ns = NULL;
    // sighand must be released below so it can be protected by pids_lock
    // since it can be accessed by other threads

    struct timespec rusage_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // Wait for now, task is in one or more critical sections, and/or has locks.
        exit_wait_backoff(&rusage_wait_pause);
    }
    struct rusage_ rusage = rusage_get_current();
    lock(&task->group->lock, 0);
    rusage_add(&task->group->rusage, &rusage);
    struct rusage_ group_rusage = task->group->rusage;
    unlock(&task->group->lock);

    // release the sighand
    struct timespec sighand_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // We added one to the task reference count above, thus the check is 2, in case any other thread is accessing.
        exit_wait_backoff(&sighand_wait_pause);
    }

    struct task *signal_parent = NULL;
    struct siginfo_ signal_info = {};
    int signal_no = 0;
    struct sighand *old_sighand = NULL;
    bool destroy_unlinked_task = false;
    // Set when the parent has disclaimed this child (SIGCHLD ignored or
    // SA_NOCLDWAIT): no zombie is left behind for a wait() that will
    // never come.
    bool autoreap = false;

    complex_lockt(&pids_lock, 0);

    // Roll this thread's I/O counters into the group so the process totals in
    // /proc/<pid>/io survive thread exit. Zero the live counters in the same
    // pids_lock section so a concurrent reader summing threads + io_dead never
    // double-counts.
    task_io_counters_add(&task->group->io_dead, &task->io);
    memset(&task->io, 0, sizeof(task->io));

    // save things that our parent might be interested in
    task->exit_code = status;
    if (amd64_trace_task_or_parent_lineage(task)) {
        char decoded[64];
        amd64_decode_wait_status_exit(status, decoded, sizeof(decoded));
        printk("tracked exit: pid=%d tgid=%d abi=%d comm=%s parent=%d parent_tgid=%d did_exec=%d %s\n",
               task->pid, task->tgid, task->abi, task->comm,
               task->parent != NULL ? task->parent->pid : -1,
               task->parent != NULL ? task->parent->tgid : -1,
               task->did_exec, decoded);
    }
    old_sighand = task->sighand;
    task->sighand = NULL;
    
    struct task *leader = task->group->leader;

    // reparent children
    struct task *new_parent = find_new_parent(task);
    struct task *child, *tmp;
    
    list_for_each_entry_safe(&task->children, child, tmp, siblings) {
        ptrace_detach_from_tracer(task, child);
        child->parent = new_parent;
        list_remove(&child->siblings);
        list_add(&new_parent->children, &child->siblings);
    }
    list_for_each_entry_safe(&task->ptracees, child, tmp, ptrace_siblings)
        ptrace_detach_from_tracer(task, child);
    if (exit_tgroup(task)) {
        exit_hangup_session_tty(leader);
        // notify parent that we died
        struct task *parent = leader->parent;
        if (parent == NULL) {
            // init died. The CLI's halt_hook exits the host process with init's
            // status (and does not return), so the host exit code mirrors the guest
            // — including for multi-threaded init (e.g. the Go runtime), whose
            // lingering sibling pthreads would otherwise be host-SIGKILLed by
            // halt_system_locked(), killing the whole process with signal 9 (137).
            if (halt_hook != NULL)
                halt_hook(status);
            halt_system_locked();
        } else {
            task_ref_cnt_mod(parent, 1);
            signal_parent = parent;
            signal_no = leader->exit_signal;
            // POSIX/Linux autoreap: a parent that has SIGCHLD set to SIG_IGN,
            // or SA_NOCLDWAIT on its handler, has said it will never wait --
            // so no zombie is left for it and its wait() returns ECHILD. AOK
            // left the zombie regardless, so a parent using the idiom
            // accumulated one per child for the life of the process.
            //
            // Only when this task IS the leader: that is the ordinary "a
            // process exited" case. When the leader died first and a sibling
            // thread finishes last, the leader is a separate struct that this
            // path does not own, and leaving that zombie for wait() is the
            // safe answer rather than reaching across to free it.
            if (task == leader && signal_no == SIGCHLD_) {
                struct sighand *psighand = parent->sighand;
                if (psighand != NULL) {
                    lock(&psighand->lock, 0);
                    struct sigaction_ *pact = &psighand->action[SIGCHLD_];
                    // SIG_IGN outright, or a handler that asked for no zombie.
                    if (pact->handler == SIG_IGN_ || (pact->flags & SA_NOCLDWAIT_))
                        autoreap = true;
                    unlock(&psighand->lock);
                }
            }
            if (!autoreap)
                leader->zombie = true;
            notify(&parent->group->child_exit);
            pidfd_notify_exit(leader);
            // The SIGCHLD a handler/sigwaitinfo/signalfd sees must carry a CLD_*
            // si_code and a bare si_status, not SI_KERNEL + the wait-encoded word.
            int chld_code, chld_status;
            decode_wait_status(task->exit_code, &chld_code, &chld_status);
            signal_info = (struct siginfo_) {
                .code = chld_code,
                .child.pid = task->pid,
                .child.uid = task->uid,
                .child.status = chld_status,
                .child.utime = clock_from_timeval(group_rusage.utime),
                .child.stime = clock_from_timeval(group_rusage.stime),
            };
        }

        if (exit_hook != NULL)
            exit_hook(task, status);
    }

    vfork_notify(task);

    unlock(&task->general_lock);
    
    if(task != leader || autoreap) {
        task_unlink_locked(task);
        destroy_unlinked_task = true;
    }
    
    unlock(&pids_lock);

    if (old_sighand != NULL)
        sighand_release(old_sighand);

    struct sigqueue *sigqueue, *sigqueue_tmp;
    list_for_each_entry_safe(&task->queue, sigqueue, sigqueue_tmp, queue) {
        list_remove(&sigqueue->queue);
        free(sigqueue);
    }

    if (signal_parent != NULL) {
        // Process-directed: the parent may be multithreaded, and the thread
        // that happens to be `leader->parent` (whichever one called fork())
        // need not be the thread that's watching for SIGCHLD (e.g. a
        // dedicated signalfd reaper thread). Deliver to the whole group's
        // shared queue so any sibling can observe/dequeue it, matching Linux
        // CLONE_THREAD signal-sharing semantics.
        if (signal_no != 0)
            send_signal_to_group(signal_parent->group, signal_no, signal_info);
        task_ref_cnt_mod(signal_parent, -1);
    }

    // Published BEFORE the destroy below, not after: task_destroy_unlinked
    // can free(task) outright, and a store into the struct after that is a
    // use-after-free. It says "this struct is no longer being used by its own
    // thread", which is true from here on -- everything below is teardown that
    // does not read it. execve's de_thread waits on this before releasing a
    // group leader whose identity it is taking (kernel/exec.c).
    atomic_store_explicit(&task->exit_finished, true, memory_order_release);

    if (destroy_unlinked_task)
        task_destroy_unlinked(task, 1);
    
EXIT:
    // The crash this instruments is a fault inside the pthread_exit below, so
    // this is the one check with no race in it at all: the thread validates
    // its own struct on its own stack, an instant before handing it to
    // libpthread. ISH_PTHREAD_CANARY only; a no-op otherwise.
    task_pthread_canary_check_self("in do_exit, immediately before pthread_exit");
    task_pthread_canary_unregister();
    pthread_exit(NULL);
}

// Exits all tasks in the current task's thread group and then calls do_exit to terminate
// the current task itself.
noreturn void do_exit_group(int status) {
    struct tgroup *group = current->group;
    struct task *task;
    struct group_exit_target {
        struct task *task;
        struct sighand *sighand;
    };
    struct group_exit_target stack_targets[32];
    struct group_exit_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

    while (true) {
        complex_lockt(&pids_lock, 0);
        lock(&group->lock, 0);

        if (amd64_trace_is_lineage_tgid(current->tgid)) {
            printk("tracked exit_group begin: current=%d tgid=%d abi=%d status=%#x threads=%lu doing=%d\n",
                   current->pid, current->tgid, current->abi, status,
                   list_size(&group->threads), group->doing_group_exit);
        }
        if (!group->doing_group_exit) {
            group->doing_group_exit = true;
            group->group_exit_code = status;
        } else {
            status = group->group_exit_code;
        }

        size_t needed = 0;
        list_for_each_entry(&group->threads, task, group_links) {
            if (task != current)
                needed++;
        }
        if (needed > target_cap) {
            unlock(&group->lock);
            unlock(&pids_lock);

            if (targets != stack_targets)
                free(targets);
            targets = malloc(sizeof(*targets) * needed);
            if (targets == NULL)
                die("out of memory collecting exit-group targets");
            target_cap = needed;
            continue;
        }

        target_count = 0;
        task_ref_cnt_mod(current, 1);
        list_for_each_entry(&group->threads, task, group_links) {
            if (amd64_trace_is_lineage_tgid(current->tgid)) {
                printk("tracked exit_group member: current=%d target=%d tgid=%d exiting=%d zombie=%d io_block=%d pending=%#llx blocked=%#llx self=%d\n",
                       current->pid, task->pid, task->tgid, task->exiting, task->zombie,
                       task->io_block,
                       (unsigned long long) task->pending,
                       (unsigned long long) task->blocked,
                       task == current);
            }
            if (task == current)
                continue;
            task_ref_cnt_mod(task, 1);
            targets[target_count].task = task;
            targets[target_count].sighand = task->sighand;
            if (targets[target_count].sighand != NULL)
                sighand_retain(targets[target_count].sighand);
            target_count++;
        }
        group->stopped = false;
        unlock(&group->lock);
        unlock(&pids_lock);
        break;
    }

    notify(&group->stopped_cond);
    for (size_t i = 0; i < target_count; i++) {
        if (targets[i].sighand != NULL) {
            deliver_signal_with_sighand(targets[i].task, targets[i].sighand, SIGKILL_, SIGINFO_NIL);
            sighand_release(targets[i].sighand);
        }
        task_ref_cnt_mod(targets[i].task, -1);
    }
    if (targets != stack_targets)
        free(targets);

    if(current->pid <= MAX_PID)
        do_exit(current, status);
    
    task_ref_cnt_mod(current, -1);
    unlock(&pids_lock);  // Shouldn't get here
    pthread_exit(NULL);
}

// always called from init process. Intended to be called when the init process exits.
static void halt_system_locked(void) {
    // brutally murder everything
    // which will leave everything in an inconsistent state. I will solve this problem later.
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task != NULL)
            pthread_kill(task->thread, SIGKILL);
    }

    // unmount all filesystems
    lock(&mounts_lock, 0);
    struct mount *mount, *tmp;
    list_for_each_entry_safe(&mounts, mount, tmp, mounts) {
        mount_remove(mount);
    }
    unlock(&mounts_lock);
}

dword_t sys_exit(dword_t status) {
    STRACE("exit(%d)\n", status);
    do_exit(current, status << 8);
}

dword_t sys_exit_group(dword_t status) {
    STRACE("exit_group(%d)\n", status);
    do_exit_group(status << 8);
}

#define WNOHANG_ (1 << 0)
#define WUNTRACED_ (1 << 1)
#define WEXITED_ (1 << 2)
#define WCONTINUED_ (1 << 3)
#define WNOWAIT_ (1 << 24)
#define __WALL_ (1 << 30)

#define P_ALL_ 0
#define P_PID_ 1
#define P_PGID_ 2

// returns false if the task cannot be reaped and true if the task was reaped
static bool reap_if_zombie(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out, int options) {
    if (!task->zombie)
        return false;
    lock(&task->group->lock, 0);

    // A thread-group leader must remain until the rest of the group exits.
    // Reaping it early leaves live threads pointing at freed group state and
    // corrupts /proc consumers that still dereference task->group.
    if (!list_empty(&task->group->threads)) {
        unlock(&task->group->lock);
        return false;
    }

    dword_t exit_code = task->exit_code;
    if (task->group->doing_group_exit)
        exit_code = task->group->group_exit_code;
    info_out->child.status = exit_code;

    struct rusage_ rusage = task->group->rusage;
    if (!(options & WNOWAIT_)) {
        lock(&current->group->lock, 0);
        rusage_add(&current->group->children_rusage, &rusage);
        unlock(&current->group->lock);
    }
    if (rusage_out != NULL)
        *rusage_out = rusage;

    unlock(&task->group->lock);

    // WNOWAIT means don't destroy the child, instead leave it so it could be waited for again.
    if (options & WNOWAIT_)
        return true;

    // Detach the group from global session/pgroup membership now so wait/reap
    // semantics stay Linux-like, but defer freeing the group object itself
    // until the task object is actually destroyed. Procfs and other refcounted
    // task readers can still legitimately dereference task->group after this.
    lock(&task->group->lock, 0);
    task_leave_session(task);
    list_remove(&task->group->pgroup);
    unlock(&task->group->lock);

    task_destroy(task, 2);
    return true;
}


static bool notify_if_stopped(struct task *task, struct siginfo_ *info_out) {
    complex_lockt(&task->group->lock, 0);
    bool stopped = task->group->stopped;
    dword_t exit_code = task->group->group_exit_code;
    if (stopped && exit_code != 0)
        task->group->group_exit_code = 0;
    unlock(&task->group->lock);
    if (!stopped || exit_code == 0)
        return false;
    info_out->child.status = exit_code;
    return true;
}

// Report a pending WCONTINUED notification once, then clear it. The status word
// 0xffff is the WIFCONTINUED sentinel that decode_wait_status / __WIFCONTINUED
// recognize (CLD_CONTINUED, si_status SIGCONT for the waitid path).
static bool notify_if_continued(struct task *task, struct siginfo_ *info_out) {
    complex_lockt(&task->group->lock, 0);
    bool cont = task->group->continued;
    task->group->continued = false;
    unlock(&task->group->lock);
    if (!cont)
        return false;
    info_out->child.status = 0xffff;
    return true;
}

static bool notify_if_ptrace_stopped(struct task *task, struct siginfo_ *info_out) {
    lock(&task->ptrace.lock, 0);
    if (task->ptrace.stopped && task->ptrace.signal) {
        info_out->child.status = task->ptrace.trap_event << 16 | task->ptrace.signal << 8 | 0x7f;
        task->ptrace.signal = 0;
        task->ptrace.trap_event = 0;
        task->ptrace.eventmsg = 0;
        unlock(&task->ptrace.lock);
        return true;
    }
    unlock(&task->ptrace.lock);
    return false;
}

static bool reap_if_needed(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out, int options) {
    assert(task_is_leader(task));
    if ((options & WUNTRACED_ && notify_if_stopped(task, info_out)) ||
        (options & WEXITED_ && reap_if_zombie(task, info_out, rusage_out, options)) ||
        (options & WCONTINUED_ && notify_if_continued(task, info_out))) {
        info_out->sig = SIGCHLD_;
        return true;
    }
    if (notify_if_ptrace_stopped(task, info_out))
        return true;
    return false;
}

// wait_for() can return _EINTR for a bare host-side SIGUSR1 poke that carries no
// guest signal: mem-quiesce TLB invalidation (task_poke_shared_mem) when a
// sibling thread mmaps/munmaps, a timer wakeup, etc. On Darwin pthread_cond_wait
// returns spuriously on signal delivery, so such a poke pops do_wait's cond-wait
// and wait_for reports EINTR even though nothing is deliverable. Treat the wait
// as interrupted only when an unblocked guest signal is actually pending, exactly
// as poll_wait() and the FUTEX_WAIT loop (futex_wait_has_pending_signal) already
// do. Otherwise an SA_RESTART waitpid that has already restarted past its real
// signal would be aborted with a spurious EINTR by an invisible poke.
static bool wait_interrupted_by_signal(void) {
    if (current == NULL)
        return false;
    __atomic_exchange_n(&current->wait_interrupted, false, __ATOMIC_ACQ_REL);
    lock(&current->sighand->lock, 0);
    // See kernel/signal.h: a shim-held signal must end this wait too.
    bool pending = !!((current->pending | current->sighand->pending) &
            ~task_wake_blocked(current));
    unlock(&current->sighand->lock);
    return pending;
}

int do_wait(int idtype, pid_t_ id, struct siginfo_ *info, struct rusage_ *rusage, int options) {
    if (idtype != P_ALL_ && idtype != P_PID_ && idtype != P_PGID_)
        return _EINVAL;
    if (options & ~(WNOHANG_|WUNTRACED_|WEXITED_|WCONTINUED_|WNOWAIT_|__WALL_))
        return _EINVAL;

    complex_lockt(&pids_lock, 0);
    int err;
    bool got_signal = false;

retry:
        if (idtype != P_PID_) {
            // look for a zombie child
            bool no_children = true;
            struct task *parent;
            list_for_each_entry(&current->group->threads, parent, group_links) {
                struct task *task;
                list_for_each_entry(&parent->children, task, siblings) {
                    if (!task_is_leader(task))
                        continue;
                    if (idtype == P_PGID_) {
                        lock(&task->group->lock, 0);
                        bool pgid_match = task->group->pgid == id;
                        unlock(&task->group->lock);
                        if (!pgid_match)
                            continue;
                    }
                    no_children = false;
                    info->child.pid = task->pid;
                    if (reap_if_needed(task, info, rusage, options))
                        goto found_something;
                }
                list_for_each_entry(&parent->ptracees, task, ptrace_siblings) {
                    // A tracer can wait on traced threads (non-leaders), not
                    // just process leaders, when __WALL is set -- strace -f
                    // relies on this to follow CLONE_THREAD workers. Without it
                    // a stopped thread's ptrace-stop is never reported, so the
                    // tracer blocks in wait4 forever and never resumes the
                    // thread, freezing the whole traced process (e.g. syslog-ng
                    // under strace -f, or any pthread program).
                    if (!task_is_leader(task) && !(options & __WALL_))
                        continue;
                    no_children = false;
                    info->child.pid = task->pid;
                    if (notify_if_ptrace_stopped(task, info)) {
                        info->sig = SIGCHLD_;
                        goto found_something;
                    }
                }
            }
        err = _ECHILD;
        if (no_children)
            goto error;
    } else {
        // check if this child is a zombie
        struct task *task = pid_get_task_zombie(id);
        err = _ECHILD;
        if (task == NULL)
            goto error;
        task = task->group->leader;
        info->child.pid = id;
        bool is_child = task->parent != NULL && task->parent->group == current->group;
        bool is_ptrace_child = task->ptrace.tracer != NULL && task->ptrace.tracer->group == current->group;
        if (!is_child && !is_ptrace_child)
            goto error;
        if (is_ptrace_child && notify_if_ptrace_stopped(task, info)) {
            info->sig = SIGCHLD_;
            goto found_something;
        }
        if (reap_if_needed(task, info, rusage, options))
            goto found_something;
    }

    // WNOHANG leaves the info in an implementation-defined state. set the pid
    // to 0 so wait4 can pass that along correctly.
    info->child.pid = 0;
    if (options & WNOHANG_) {
        info->sig = SIGCHLD_;
        goto found_something;
    }

    err = _EINTR;
    if (got_signal)
        goto error;

    // no matching zombie found, wait for one
    if (wait_for(&current->group->child_exit, &pids_lock, NULL)) {
        // Woke from the wait. Only latch an interruption when a real guest
        // signal is pending; a bare host-side poke (TLB invalidation, timer)
        // must not surface as EINTR -- see wait_interrupted_by_signal(). On the
        // next pass a newly exited child is still reaped first, so a real
        // SIGCHLD-driven wake is handled by the zombie scan above.
        if (wait_interrupted_by_signal())
            got_signal = true;
        goto retry;
    }
    goto retry;

    info->sig = SIGCHLD_;
found_something:
    if (amd64_trace_is_lineage_tgid(current->tgid)) {
        char decoded[64];
        amd64_decode_wait_status_exit(info->child.status, decoded, sizeof(decoded));
        printk("amd64 tracked reap: pid=%d tgid=%d abi=%d comm=%s child=%d %s options=%#x\n",
               current->pid, current->tgid, current->abi, current->comm,
               info->child.pid, decoded, options);
    }
    unlock(&pids_lock);
    return 0;

error:
    unlock(&pids_lock);
    return err;
}

dword_t sys_waitid(int_t idtype, pid_t_ id, addr_t info_addr, int_t options) {
    return sys_waitid_guest(idtype, id, info_addr, options);
}

#define P_PIDFD_ 3

dword_t sys_waitid_guest(int_t idtype, pid_t_ id, guest_addr_t info_addr, int_t options) {
    STRACE("waitid(%d, %d, %#x, %#x)", idtype, id, info_addr, options);
    // waitid(P_PIDFD, pidfd, ...): wait on the process the pidfd references.
    // systemd >= 260 waits for every child (generators, executor forks) this
    // way; without it each wait failed EINVAL ("Failed to wait for ...").
    if (idtype == P_PIDFD_) {
        int_t pid = pidfd_get_pid(id);
        if (pid < 0)
            return pid;
        idtype = P_PID_;
        id = pid;
    }
    struct siginfo_ info = {};
    int_t res = 0;
    TASK_MAY_BLOCK {
        res = do_wait(idtype, id, &info, NULL, options);
    }
    if (res == _EINTR && signal_should_restart_syscall())
        return _ERESTART;
    if (res < 0 || (res == 0 && info.child.pid == 0))
        return res;
    // do_wait fills child.status with the raw wait(2)-encoded word (for wait4);
    // waitid's siginfo wants a CLD_* si_code and a bare si_status instead.
    int cld_code, cld_status;
    decode_wait_status(info.child.status, &cld_code, &cld_status);
    info.code = cld_code;
    info.child.status = cld_status;
    if (info_addr != 0 && siginfo_to_user(current, info_addr, &info))
        return _EFAULT;
    return 0;
}

// Wait for a child on behalf of a natively-implemented program
// (kernel/native_io.h). do_wait and the P_*/WEXITED_ constants are private to
// this file, so the wrapper lives here rather than exporting them; a native
// parent then blocks in exactly the place a translated one does.
//
// pid (dword_t)-1 means "any child", matching waitpid(-1, ...). Returns the
// reaped pid, or a negative errno.
int task_wait_child(dword_t pid, int *status_out, int options) {
    struct siginfo_ info = {};
    int idtype = (pid == (dword_t) -1) ? P_ALL_ : P_PID_;
    int err = do_wait(idtype, (pid_t_) pid, &info, NULL, options | WEXITED_);
    if (err < 0)
        return err;
    if (status_out != NULL)
        *status_out = info.child.status;
    return (int) info.child.pid;
}

dword_t sys_wait4(pid_t_ id, addr_t status_addr, dword_t options, addr_t rusage_addr) {
    return sys_wait4_guest(id, status_addr, options, rusage_addr);
}

dword_t sys_wait4_guest(pid_t_ id, guest_addr_t status_addr, dword_t options, guest_addr_t rusage_addr) {
    STRACE("wait4(%d, %#x, %#x, %#x)", id, status_addr, options, rusage_addr);
    // WEXITED and WNOWAIT are waitid(2)-only flags; wait4/waitpid reject them
    // with EINVAL (the kernel's wait4 allowed set excludes both). wait4 always
    // waits for exited children, so WEXITED is implied (OR'd in below).
    if (options & (WEXITED_ | WNOWAIT_))
        return _EINVAL;

    int idtype;
    if (id > 0)
        idtype = P_PID_;
    else if (id == -1)
        idtype = P_ALL_;
    else {
        idtype = P_PGID_;
        if (id == 0) {
            lock(&current->group->lock, 0);
            id = current->group->pgid;
            unlock(&current->group->lock);
        }
        else
            id = -id;
    }

    struct siginfo_ info = {.child.pid = 0xbaba};
    struct rusage_ rusage;
    int_t res = 0;
    TASK_MAY_BLOCK {
        res = do_wait(idtype, id, &info, &rusage, options | WEXITED_);
    }
    if (res == _EINTR && signal_should_restart_syscall())
        return _ERESTART;
    if (res < 0 || (res == 0 && info.child.pid == 0))
        return res;
    if (status_addr != 0 && user_put(status_addr, info.child.status))
        return _EFAULT;
    if (rusage_addr != 0 && write_guest_rusage_abi(current->abi, rusage_addr, &rusage))
        return _EFAULT;
    return info.child.pid;
}

// Host-side reap of a single child by pid, returning the raw wait(2) status word
// through status_out (no guest memory involved). Used by
// run_guest_command_capture() so the app can run a one-shot guest command and
// learn its exit code. Waits for the specific pid only, so it never reaps a
// child some other waiter (e.g. real init) is expecting. With nonblock set, uses
// WNOHANG and returns 0 immediately when the child has not exited yet.
int wait_child_status(pid_t_ pid, int *status_out, bool nonblock) {
    struct siginfo_ info = {.child.pid = 0};
    struct rusage_ rusage;
    int res = do_wait(P_PID_, pid, &info, &rusage, WEXITED_ | (nonblock ? WNOHANG_ : 0));
    if (res < 0)
        return res;
    if (status_out != NULL)
        *status_out = info.child.status;
    return info.child.pid; // 0 when WNOHANG and the child is still running
}

dword_t sys_waitpid(pid_t_ pid, addr_t status_addr, dword_t options) {
    return sys_wait4(pid, status_addr, options, 0);
}
