#include <pthread.h>
#include <signal.h>
#include <string.h>
#include "emu/cpu.h"
#include "kernel/calls.h"
#include "kernel/acct.h"
#include "fs/sock.h"
#include "kernel/checkpoint.h"
#include "kernel/resource.h"
#include "kernel/mm.h"
#include "kernel/futex.h"
#include "kernel/ptrace.h"
#include "kernel/task.h"
#include "util/sync.h"
#include "fs/fd.h"
#include "fs/devices.h"
#include "fs/tty.h"

struct halt_target {
    struct task *task;
    struct sighand *sighand;
};
static struct halt_target *halt_system_collect_locked(size_t *count);
static void halt_system_kill(struct halt_target *targets, size_t count);

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

// Optional hook invoked when init (pid 1) exits, before the other tasks are
// killed (halt_system_collect_locked). Called with pids_lock and init's
// general_lock held, so it must not take either. The standalone CLI sets this to
// terminate the host process with init's exit status; it does not return. The
// app's records that the guest has halted and returns.
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
// The walk below is unconditional, which is deliberate: Linux gates its
// equivalent (find_new_reaper) on signal->has_child_subreaper, a hint that
// copy_signal propagates to children of a subreaper and that
// PR_SET_CHILD_SUBREAPER back-fills over the existing descendant tree with
// walk_process_tree(). All that buys is skipping the walk in the overwhelming
// case where no ancestor is a subreaper at all; when the hint is set it agrees
// with the plain walk by construction. AOK carries no such hint -- one fewer
// inheritable per-process flag to get wrong, which is exactly the bug this
// walk's input had -- and the walk is bounded by tree depth anyway.
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
        tracee->ptrace.seized = false;
        // A stop still owed goes with the tracer, as on the PTRACE_DETACH path:
        // a tracer that dies between interrupting a tracee and seeing the stop
        // must not leave it one to take with nobody to report it to.
        __atomic_store_n(&tracee->ptrace.trap_stop, false, __ATOMIC_RELEASE);
        if (tracee->ptrace.stopped) {
            tracee->ptrace.stopped = false;
            notify(&tracee->ptrace.cond);
        }
    }
    unlock(&tracee->ptrace.lock);

    list_remove_safe(&tracee->ptrace_siblings);
}

// ---- who hears about an exit -----------------------------------------------
//
// Linux reports a traced task's exit to its TRACER first ("A zombie ptracee is
// only visible to its ptracer", wait_consider_task). A traced thread's exit is
// the tracer's alone: it is reaped with wait and then released. A traced
// process whose parent is somewhere else is reaped by the tracer, untraced, and
// only then announced to its parent (wait_task_zombie's EXIT_TRACE), which
// reaps it as usual. A tracer that goes away holding such a zombie passes it on
// the same way (__ptrace_detach).
//
// AOK did none of it. A thread was destroyed the moment it exited and a process
// was announced to its parent alone, so a tracer never heard of an exit it was
// not the parent of. strace -f of anything that made a thread or forked ended
// with "wait4(__WALL): No child processes" and exit status 1, having printed
// "+++ exited" for the top process only. Worse, a tracer waiting on a traced
// process by pid reached reap_if_zombie and destroyed it, and the real parent's
// waitpid then failed.

// The task tracing `task`, or NULL. Caller holds pids_lock.
//
// Never one of its own threads. Linux refuses that attach, and so does AOK now,
// but PTRACE_TRACEME from a thread names the thread's parent -- which here is
// whichever thread created it, in the same process -- and a zombie held for a
// tracer inside its own process would never be collected, and neither would
// the process.
static struct task *tracer_of(const struct task *task) {
    if (!task->ptrace.traced)
        return NULL;
    struct task *tracer = task->ptrace.tracer != NULL ? task->ptrace.tracer : task->parent;
    if (tracer == NULL || tracer->group == task->group)
        return NULL;
    return tracer;
}

// A zombie its tracer reports: every traced thread, and a traced process whose
// tracer is not in its parent's group. A process traced by its own parent is
// reported once, to the parent, as Linux's !ptrace_reparented case is.
static bool zombie_is_tracers(const struct task *task) {
    struct task *tracer = tracer_of(task);
    if (tracer == NULL)
        return false;
    if (task->group->leader != task)
        return true;
    return task->parent == NULL || tracer->group != task->parent->group;
}

// What a zombie reports to wait. Once the process has exited as a whole the
// process's code wins, even for a thread that died earlier with a code of its
// own -- Linux's SIGNAL_GROUP_EXIT test in wait_task_zombie. Caller holds
// pids_lock and task->group->lock.
static dword_t zombie_status(const struct task *task) {
    return task->group->doing_group_exit ? task->group->group_exit_code : task->exit_code;
}

// What an exit, or a reap, owes to other tasks: signals, which cannot be sent
// under pids_lock because send_signal_to_process takes it, and released tasks,
// which cannot be freed while the lists they were on are still being walked.
// Collected under pids_lock and settled by exit_notes_settle after it.
struct exit_signal_note {
    struct task *to;            // holds a reference
    int sig;
    struct siginfo_ info;
};

struct exit_notes {
    struct exit_signal_note inline_signals[4];
    struct exit_signal_note *signals;
    size_t nsignals, cap;
    // Tasks already out of the pid table, linked through ptrace_siblings,
    // which task_unlink_locked has just emptied.
    struct list dead;
};

static void exit_notes_init(struct exit_notes *notes) {
    notes->signals = notes->inline_signals;
    notes->nsignals = 0;
    notes->cap = sizeof(notes->inline_signals) / sizeof(notes->inline_signals[0]);
    list_init(&notes->dead);
}

static void exit_notes_signal(struct exit_notes *notes, struct task *to, int sig,
        struct siginfo_ info) {
    if (to == NULL || sig == 0)
        return;
    if (notes->nsignals == notes->cap) {
        // Only a tracer dying with many zombies outstanding gets here. The
        // child_exit notify has gone out already, so a signal lost to a failed
        // allocation costs a sleeping sigsuspend, not a wait.
        size_t cap = notes->cap * 2;
        struct exit_signal_note *grown = malloc(sizeof(*grown) * cap);
        if (grown == NULL)
            return;
        memcpy(grown, notes->signals, sizeof(*grown) * notes->nsignals);
        if (notes->signals != notes->inline_signals)
            free(notes->signals);
        notes->signals = grown;
        notes->cap = cap;
    }
    task_ref_cnt_mod(to, 1);
    notes->signals[notes->nsignals++] = (struct exit_signal_note) {
        .to = to, .sig = sig, .info = info,
    };
}

// Called without pids_lock. Returns whether `self` was among the released
// tasks: do_exit's own struct is freed by do_exit itself, last.
static bool exit_notes_settle(struct exit_notes *notes, struct task *self) {
    for (size_t i = 0; i < notes->nsignals; i++) {
        struct exit_signal_note *note = &notes->signals[i];
        // Process-directed, as do_exit's own SIGCHLD is: whichever thread of
        // the waiter is watching for it may take it. Sent to the waiting
        // thread itself, whose mask alone decides whether an ignored SIGCHLD
        // is queued.
        send_signal_to_process(note->to, note->sig, note->info);
        task_ref_cnt_mod(note->to, -1);
    }
    if (notes->signals != notes->inline_signals)
        free(notes->signals);
    notes->signals = notes->inline_signals;
    notes->nsignals = 0;

    bool released_self = false;
    struct task *dead, *tmp;
    list_for_each_entry_safe(&notes->dead, dead, tmp, ptrace_siblings) {
        list_remove(&dead->ptrace_siblings);
        if (dead == self) {
            released_self = true;
            continue;
        }
        // Defers by itself while the zombie's own thread is still finishing
        // do_exit, or while anything holds a reference.
        task_destroy_unlinked(dead, 2);
    }
    return released_self;
}

// The SIGCHLD (or other exit signal) payload describing `task`'s exit.
static struct siginfo_ exit_signal_info(struct task *task, dword_t status,
        const struct rusage_ *usage) {
    int code, bare_status;
    decode_wait_status(status, &code, &bare_status);
    return (struct siginfo_) {
        .code = code,
        .child.pid = task->pid,
        .child.uid = task->uid,
        .child.status = bare_status,
        .child.utime = usage != NULL ? clock_from_timeval(usage->utime) : 0,
        .child.stime = usage != NULL ? clock_from_timeval(usage->stime) : 0,
    };
}

// Take a reaped process out of the process table. Its group leaves its session
// and process group here, as a reaping wait always did -- and as the SIGCHLD
// autoreap path did not: it freed the group with both still linked into the
// pid's lists. The struct itself goes on `notes`.
static void release_process_locked(struct task *leader, struct exit_notes *notes) {
    lock(&leader->group->lock, 0);
    task_leave_session(leader);
    list_remove(&leader->group->pgroup);
    unlock(&leader->group->lock);
    task_unlink_locked(leader);
    list_add(&notes->dead, &leader->ptrace_siblings);
}

// Tell whoever waits for this process that it has exited: its tracer, when a
// tracer from outside its parent's group holds it, and otherwise its parent.
// Called once its last thread is gone and no thread zombie of it is left --
// from do_exit, or from whoever released that last zombie -- and again for a
// zombie that an exiting parent hands to another process. Linux's
// do_notify_parent, reached from exit_notify, release_task or reparent_leader.
//
// The signal names the process and carries the leader's own exit code, as
// Linux's does: when the leader went first, the last thread's tid and code
// described a task the parent has never heard of. A parent that disclaimed
// SIGCHLD gets no zombie -- unless the process is traced, which is never
// autoreaped. Caller holds pids_lock; leader->parent is not NULL.
static void exit_notify_process_locked(struct task *leader, struct exit_notes *notes) {
    struct tgroup *group = leader->group;
    group->exit_notify_deferred = false;

    struct task *parent = leader->parent;
    struct task *tracer = tracer_of(leader);
    bool to_tracer = zombie_is_tracers(leader);
    struct task *to = to_tracer ? tracer : parent;
    int sig = to_tracer ? SIGCHLD_ : leader->exit_signal;

    bool autoreap = false;
    if (tracer == NULL && sig == SIGCHLD_ && parent->sighand != NULL) {
        lock(&parent->sighand->lock, 0);
        struct sigaction_ *action = &parent->sighand->action[SIGCHLD_];
        if (action->handler == SIG_IGN_ || (action->flags & SA_NOCLDWAIT_))
            autoreap = true;
        // SIG_IGN sends nothing at all, as Linux's do_notify_parent does: not
        // even a SIGCHLD queued because the parent blocks it, which is what a
        // parent that blocked it was given. SA_NOCLDWAIT alone still sends it.
        if (action->handler == SIG_IGN_)
            sig = 0;
        unlock(&parent->sighand->lock);
    }

    lock(&group->lock, 0);
    struct rusage_ usage = group->rusage;
    unlock(&group->lock);
    struct siginfo_ info = exit_signal_info(leader, leader->exit_code, &usage);

    leader->zombie = true;
    notify(&to->group->child_exit);
    pidfd_notify_exit(leader);
    exit_notes_signal(notes, to, sig, info);
    if (autoreap)
        release_process_locked(leader, notes);
}

// A thread zombie reaped by its tracer, or let go by it: gone for good. The
// last one may be what its process's announcement was waiting for.
static void release_thread_locked(struct task *thread, struct exit_notes *notes) {
    struct tgroup *group = thread->group;
    task_unlink_locked(thread);
    list_add(&notes->dead, &thread->ptrace_siblings);
    if (--group->traced_zombies == 0 && group->exit_notify_deferred)
        exit_notify_process_locked(group->leader, notes);
}

// `tracer` is letting go of `zombie` without reaping it: the tracer is exiting.
// A thread is released. A process goes on to its parent, which has not heard of
// it yet -- unless the parent was the one told, being in the tracer's own
// group, or unless the announcement is still waiting on a thread zombie, in
// which case it will go to the parent when it is made. Caller holds pids_lock
// and has already detached `zombie`.
static void zombie_untraced_locked(struct task *tracer, struct task *zombie,
        struct exit_notes *notes) {
    if (zombie->group->leader != zombie) {
        release_thread_locked(zombie, notes);
        return;
    }
    if (zombie->group->exit_notify_deferred)
        return;
    if (zombie->parent == NULL || zombie->parent->group == tracer->group)
        return;
    exit_notify_process_locked(zombie, notes);
}

// POSIX: when a process's exit leaves a process group ORPHANED and that group
// still holds a stopped member, the group is sent SIGHUP and then SIGCONT.
//
// A group is orphaned when no member has a parent that is in a different
// group of the SAME session -- i.e. nobody outside the group is left who
// could resume it. Its stopped members would otherwise stay stopped for ever
// with no one able to continue them: the shell that stopped them is gone.
// That is the whole point of the rule, and AOK did not implement it, so a
// ^Z'd job whose shell then exited sat in state T until the guest was
// rebooted.
//
// Both helpers run under pids_lock. The signals are sent afterwards, from the
// same place the controlling-terminal SIGHUP is (see do_exit), because
// sending under pids_lock would take sighand->lock inside it.
static bool pgrp_is_orphaned_locked(pid_t_ pgid, struct task *ignore, pid_t_ sid) {
    struct pid *entry;
    list_for_each_entry(&alive_pids_list, entry, alive) {
        struct task *t = entry->task;
        if (t == NULL || t == ignore || t->exiting || t->zombie)
            continue;
        if (!task_is_leader(t) || t->group == NULL)
            continue;
        if (t->group->pgid != pgid || t->group->sid != sid)
            continue;
        struct task *parent = t->parent;
        if (parent == NULL || parent == ignore || parent->group == NULL)
            continue;
        // A member init has adopted is no way back, as in Linux, which skips
        // any whose parent is_global_init: init resumes nobody's stopped
        // jobs. Counted -- and in the CLI init shares the session with
        // everything it starts -- it kept a group whose member had outlived
        // its own parent from ever being orphaned. And do_exit asks both of
        // its questions after handing its children on, usually to init, so
        // no exit could have orphaned anything at all.
        if (parent->tgid == 1)
            continue;
        // Someone outside this group, but inside the session, can still
        // resume it -- so it is not orphaned.
        if (parent->group->sid == sid && parent->group->pgid != pgid)
            return false;
    }
    return true;
}

static bool pgrp_has_stopped_member_locked(pid_t_ pgid, pid_t_ sid) {
    struct pid *entry;
    list_for_each_entry(&alive_pids_list, entry, alive) {
        struct task *t = entry->task;
        if (t == NULL || t->group == NULL || !task_is_leader(t))
            continue;
        if (t->group->pgid == pgid && t->group->sid == sid && t->group->stopped)
            return true;
    }
    return false;
}

// The children's groups an exit leaves orphaned with a stopped member, each
// owed SIGHUP and SIGCONT once pids_lock is dropped. A few fit inline -- a
// shell's stopped jobs, one group each -- and past that the list grows; a
// failed allocation costs that group its hangup, which is what every such
// group got before the rule was here.
struct orphaned_pgrps {
    pid_t_ inline_pgids[4];
    pid_t_ *pgids;
    size_t count, cap;
};

static void orphaned_pgrps_init(struct orphaned_pgrps *orphans) {
    orphans->pgids = orphans->inline_pgids;
    orphans->count = 0;
    orphans->cap = sizeof(orphans->inline_pgids) / sizeof(orphans->inline_pgids[0]);
}

static void orphaned_pgrps_add(struct orphaned_pgrps *orphans, pid_t_ pgid) {
    for (size_t i = 0; i < orphans->count; i++) {
        if (orphans->pgids[i] == pgid)
            return;
    }
    if (orphans->count == orphans->cap) {
        size_t cap = orphans->cap * 2;
        pid_t_ *grown = malloc(sizeof(*grown) * cap);
        if (grown == NULL)
            return;
        memcpy(grown, orphans->pgids, sizeof(*grown) * orphans->count);
        if (orphans->pgids != orphans->inline_pgids)
            free(orphans->pgids);
        orphans->pgids = grown;
        orphans->cap = cap;
    }
    orphans->pgids[orphans->count++] = pgid;
}

// Called without pids_lock. SIGHUP tells the stopped members the world they
// were stopped in is gone, and SIGCONT is what lets them run far enough to
// act on it. Order matters -- SIGCONT first would resume them into a session
// with no one to talk to.
static void orphaned_pgrp_hang_up(pid_t_ pgid) {
    send_group_signal(pgid, SIGHUP_, SIGINFO_NIL);
    send_group_signal(pgid, SIGCONT_, SIGINFO_NIL);
}

static void orphaned_pgrps_hang_up(struct orphaned_pgrps *orphans) {
    for (size_t i = 0; i < orphans->count; i++)
        orphaned_pgrp_hang_up(orphans->pgids[i]);
    if (orphans->pgids != orphans->inline_pgids)
        free(orphans->pgids);
    orphaned_pgrps_init(orphans);
}

// ---- the parent-death signal -------------------------------------------------
//
// PR_SET_PDEATHSIG. Linux's forget_original_parent sends it for every thread
// of every child process a dying THREAD hands on, to whoever takes the child:
//
//     for_each_thread(p, t) {
//         ...
//         if (t->pdeath_signal)
//             group_send_sig_info(t->pdeath_signal, SEND_SIG_NOINFO, t, PIDTYPE_TGID);
//     }
//
// So a child forked by a worker thread gets it when that worker exits, though
// the process that forked it lives on -- the known gotcha, and a real one, since
// Go forks from whichever thread its runtime is on. And again at each later
// reparent: a subreaper that took the child and then exits sends it too.
//
// AOK stored the setting and never sent it, so a helper that asked to go with
// the program that started it -- Go's SysProcAttr.Pdeathsig, systemd's
// FORK_DEATHSIG -- outlived it.
//
// Collected in do_exit's reparenting loop, under pids_lock, and sent once that
// is dropped, like every signal an exit sends. Kept by id rather than by
// reference, as the orphaned groups are: nothing holds the child's process in
// between, and one that has gone meanwhile is simply not found.
struct pdeath_note {
    pid_t_ tid, tgid;       // the thread that asked for it, and its process
    int sig;
};

struct pdeath_notes {
    struct pdeath_note inline_notes[4];
    struct pdeath_note *notes;
    size_t count, cap;
};

static void pdeath_notes_init(struct pdeath_notes *pd) {
    pd->notes = pd->inline_notes;
    pd->count = 0;
    pd->cap = sizeof(pd->inline_notes) / sizeof(pd->inline_notes[0]);
}

// Caller holds pids_lock, and is the exiting task.
static void pdeath_note_add_locked(struct pdeath_notes *pd, struct task *thread) {
    int sig = (int) thread->pdeath_signal;
    if (sig == 0 || thread->zombie)
        return;
    // Linux's group_send_sig_info asks check_kill_permission of the exiting
    // thread, as kill() would: a parent that has since become a user who may
    // not signal the child sends nothing. Asked here, while both are certainly
    // still there.
    if (!may_signal_task(thread, (dword_t) sig))
        return;
    if (pd->count == pd->cap) {
        size_t cap = pd->cap * 2;
        struct pdeath_note *grown = malloc(sizeof(*grown) * cap);
        if (grown == NULL)
            return;
        memcpy(grown, pd->notes, sizeof(*grown) * pd->count);
        if (pd->notes != pd->inline_notes)
            free(pd->notes);
        pd->notes = grown;
        pd->cap = cap;
    }
    pd->notes[pd->count++] = (struct pdeath_note) {
        .tid = thread->pid, .tgid = thread->tgid, .sig = sig,
    };
}

// Every thread of the child process led by `leader` that asked for a signal
// when its parent dies. Caller holds pids_lock.
//
// The leader is asked even once it has exited: it stays its process's leader,
// and one of Linux's for_each_thread, until the last thread goes -- so a
// process whose main thread asked and then left with pthread_exit still gets
// it.
static void pdeath_collect_locked(struct pdeath_notes *pd, struct task *leader) {
    bool leader_listed = false;
    struct task *thread;
    list_for_each_entry(&leader->group->threads, thread, group_links) {
        if (thread == leader)
            leader_listed = true;
        pdeath_note_add_locked(pd, thread);
    }
    if (!leader_listed)
        pdeath_note_add_locked(pd, leader);
}

// Called without pids_lock. `info` names the exiting process as the sender,
// as SEND_SIG_NOINFO does: SI_USER, its tgid and its real uid.
static void pdeath_notes_send(struct pdeath_notes *pd, struct siginfo_ info) {
    for (size_t i = 0; i < pd->count; i++) {
        const struct pdeath_note *note = &pd->notes[i];
        // Process-directed, sent to the thread that asked: it takes it if it
        // can, as Linux's complete_signal prefers it, and another thread of its
        // process otherwise -- the only choice once it has exited.
        complex_lockt(&pids_lock, 0);
        struct task *thread = pid_get_task(note->tid);
        if (thread != NULL && thread->tgid == note->tgid)
            task_ref_cnt_mod(thread, 1);
        else
            thread = NULL;
        unlock(&pids_lock);
        if (thread != NULL) {
            send_signal_to_process(thread, note->sig, info);
            task_ref_cnt_mod(thread, -1);
        }
    }
    if (pd->notes != pd->inline_notes)
        free(pd->notes);
    pdeath_notes_init(pd);
}

// A session leader's death takes the controlling terminal away from the whole
// session. Linux (disassociate_ctty) does this unconditionally, and does two
// separate things that AOK had run together:
//
//   the SESSION is disassociated -- every member loses its tty pointer and the
//   terminal forgets its session -- always, for every kind of terminal;
//
//   the DEVICE is hung up (reads and writes start failing) only for a real
//   terminal. A pty is left working; its foreground group just gets a SIGHUP.
//
// AOK hung the device up in both cases, and then guarded the whole thing on
// the session being otherwise empty to stop that from tearing down a remote
// login while sshd still had a shell running. The guard cured the symptom and
// left the terminal permanently unusable: tty->session stayed set with nobody
// able to clear it, so every later TIOCSCTTY on that pts got EPERM -- for the
// life of the emulator, since the other release path only runs at reap time
// and only for the last member of the session.
//
// Splitting the two puts the guard out of a job. Measured on Linux 6.12: after
// the leader exits, a surviving session member can still write to the pts and
// sees no POLLHUP, but /dev/tty stops resolving for it and a fresh session can
// claim the terminal.
//
// The SIGHUP is not sent from here -- do_exit is holding pids_lock. Like the
// parent's SIGCHLD beside it, the target goes back to the caller, which sends
// it once the lock is released. Linux sends no SIGCONT on this path (it passes
// on_exit=1, which suppresses it), so neither do we.
static void exit_hangup_session_tty(struct task *leader, struct tty_hangup_targets *hup) {
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
    hup->fg_group = tty->fg_group;
    hup->session = 0;
    tty->session = 0;
    tty->fg_group = 0;
    // Only a real terminal is hung up as a device. Doing it to a pty is what
    // made this dangerous enough to need the guard that bricked it.
    if (tty->type != TTY_PSEUDO_MASTER_MAJOR && tty->type != TTY_PSEUDO_SLAVE_MAJOR)
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
    if (unlikely(guestprof_on))
        guestprof_slot_release(task);
    if(task->reference.ready_to_be_freed) {
        // Already queued for deletion by someone else, but the struct is still
        // alive on that queue -- publish here too, or a de_thread waiting on
        // this task would spin out its whole timeout for nothing.
        atomic_store_explicit(&task->exit_finished, true, memory_order_release);
        goto EXIT;
    }
    bool was_already_exiting = task->exiting;
    task->exiting = true;
    if (!was_already_exiting)
        checkpoint_trace_exit(task->pid, task->comm, status);
    // A signal queued for the whole process that this thread was told to take
    // goes to a thread that is staying, or it would wait there unseen: no
    // other thread looks at the process's queue until it is told to.
    signal_exit_handoff(task);

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
    // Release the robust mutexes this thread still holds, before its address
    // space goes away. Linux runs exit_robust_list here for the same reason:
    // the lock words live in guest memory, and a waiter needs FUTEX_OWNER_DIED
    // written into them or it blocks for good.
    futex_exit_robust_list(task);

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
    // Last chance to read the address space: the exit-time usage snapshot
    // below runs after this, and a peak RSS read from a released mm is 0.
    task_maxrss_kb(task);
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
    lock(&task->group->lock, 0);
    // Snapshot, roll-up and flag all inside one group->lock section, because
    // this thread stays on group->threads until exit_tgroup() far below and
    // rusage_get_group_of() walks that list under the same lock. A reader is
    // then on exactly one side of the handover: it live-samples this thread
    // and finds nothing of it in group->rusage, or it finds the snapshot and
    // skips the thread. Neither half may leak out of the section --
    //
    //   * without the flag, the reader did BOTH for the whole ~140 lines
    //     until the unlink, so getrusage(RUSAGE_SELF) reported a process
    //     total that later fell by exactly one joined thread's CPU;
    //   * with the snapshot taken before the lock (as it was), a reader that
    //     won the lock first sampled this thread at a LATER instant than the
    //     snapshot froze, and the published figure was the smaller of the two
    //     -- the same backwards step, narrowed to this function's own lock
    //     wait, which measured up to 2ms.
    //
    // Cheap to hold the lock across: task->mm is already released above, so
    // the maxrss sample inside rusage_get_current() has no page table to walk,
    // and do_exit already owns task->general_lock, so it takes no new lock.
    //
    // Guarded, too, because do_exit can be re-entered for the same task (see
    // was_already_exiting at the top); a second roll-up would leave this
    // thread's CPU in the group total twice for good.
    if (!task->exit_rusage_counted) {
        struct rusage_ rusage = rusage_get_current();
        rusage_add(&task->group->rusage, &rusage);
        task->exit_utime = rusage.utime;    // see rusage_get_thread_cpu
        task->exit_stime = rusage.stime;
        task->exit_rusage_counted = true;
    }
    struct rusage_ group_rusage = task->group->rusage;
    unlock(&task->group->lock);

    // release the sighand
    struct timespec sighand_wait_pause = lock_pause;
    while (exit_wait_needed(task)) { // We added one to the task reference count above, thus the check is 2, in case any other thread is accessing.
        exit_wait_backoff(&sighand_wait_pause);
    }

    // Filled in when this task's exit takes a controlling terminal away from
    // its session; the SIGHUP goes out below, once pids_lock is released.
    struct tty_hangup_targets tty_hup = { .fg_group = 0, .session = 0 };
    struct sighand *old_sighand = NULL;
    bool destroy_unlinked_task = false;
    // Everyone this exit has to tell, and every task it releases -- this one
    // included, when a parent that disclaimed SIGCHLD leaves no zombie, and a
    // zombie child whose new parent disclaimed it. See "who hears about an
    // exit".
    struct exit_notes notes;
    exit_notes_init(&notes);

    // Zombies handed to a sibling thread; see the reparenting loop below.
    int reparented_zombies = 0;
    struct halt_target *halt_targets = NULL;
    size_t halt_target_count = 0;
    // The groups of children this exit orphans; hung up after the locks.
    struct orphaned_pgrps orphaned;
    orphaned_pgrps_init(&orphaned);
    // The parent-death signals the children's threads asked for; sent after
    // the locks too, from this process (Linux's SEND_SIG_NOINFO).
    struct pdeath_notes pdeath;
    pdeath_notes_init(&pdeath);
    struct siginfo_ pdeath_info = {
        .code = SI_USER_,
        .kill.pid = task->tgid,
        .kill.uid = task->uid,
    };

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

    // Whether this exit ends the process: what exit_tgroup says below, known
    // already, because every change to the thread list is made under
    // pids_lock and it is held from here to there.
    bool last_thread = task->group->threads.next == &task->group_links &&
        task->group->threads.prev == &task->group_links;

    // reparent children
    struct task *new_parent = find_new_parent(task);
    struct task *child, *tmp;
    // Handed to another thread of this same process, a child has not changed
    // parents in any way a program can see: wait finds it from every thread
    // before and after, and its exit is announced to the process whichever
    // thread created it. Linux's forget_original_parent calls reparent_leader,
    // which resets exit_signal and announces a zombie, only when the new
    // parent is in another thread group: "If this is a threaded reparent
    // there is no need to notify anyone anything has happened."
    bool to_another_process = new_parent != NULL && new_parent->group != task->group;

    list_for_each_entry_safe(&task->children, child, tmp, siblings) {
        ptrace_detach_from_tracer(task, child);
        child->parent = new_parent;
        list_remove(&child->siblings);
        list_add(&new_parent->children, &child->siblings);
        // The parent-death signal, for a child PROCESS, whoever takes it -- a
        // thread of this same process included, which is the one case every
        // step below skips. Not for a thread of our own process, which AOK
        // lists here only because it makes a thread the child of the thread
        // that created it: Linux lists no thread as anyone's child, and a
        // thread's setting is for its process's parent, sent when that one
        // exits. Nor when init itself is going and takes everything with it.
        if (child->group != NULL && child->group != task->group &&
                child->group->leader == child && new_parent != task)
            pdeath_collect_locked(&pdeath, child);
        // As Linux's reparent_leader does, and for the reason its comment
        // gives ("we don't want people slaying init"): a child cloned with
        // some other exit_signal must not get to send that signal to whoever
        // inherits it. A sibling thread is not "whoever": a child cloned with
        // SIGUSR1 by a worker that has since exited announces itself with
        // SIGUSR1, as it would have while the worker lived.
        //
        // Leaders only. In AOK a thread is a child of whichever task created
        // it, not of the group leader's parent as on Linux, so this list can
        // hold threads too -- and a thread's exit_signal is not the process's.
        // Nothing reads a non-leader's exit_signal today (every reader goes
        // through leader->exit_signal), so this is precision rather than a
        // fix, but it keeps the line saying what it means.
        if (to_another_process && child->group != NULL && child->group->leader == child)
            child->exit_signal = SIGCHLD_;
        // Moving the child is not enough when it is already dead. Its exit was
        // reported to US, and we are about to stop existing; unless the new
        // parent is told, nothing ever wakes it to reap, and the zombie stays
        // on the process table for the life of the system.
        //
        // That was the observed bug: with a real SysV init as pid 1 -- asleep
        // in pselect, reaping only on SIGCHLD -- every orphaned zombie
        // accumulated. `sudo anything` from a shell that forks-by-relaunch
        // leaves exactly this shape, and a device had collected 16 of them.
        // A manual `kill -CHLD 1` reaped all 16 at once, which is what named
        // the missing piece.
        //
        // Announced as its own exit was, by the same routine: Linux's
        // reparent_leader calls do_notify_parent, which exit_notify_process_locked
        // is. So a new parent whose SIGCHLD is SIG_IGN or has SA_NOCLDWAIT has
        // the zombie released here and now, and SIG_IGN is sent nothing -- not
        // even a SIGCHLD queued because it blocks the signal. AOK used to send
        // one SIGCHLD whatever the disposition and leave the zombie for a
        // wait that a parent which disclaimed SIGCHLD never makes. One signal
        // per zombie, as Linux sends them; they coalesce into the first
        // still-pending copy, so it is the first one's siginfo that a new
        // parent blocking SIGCHLD sees. That is the youngest here and the
        // oldest on Linux, whose children lists are oldest-first
        // (docs/TODO.md).
        //
        // Only a zombie that was announced to us as a process. A thread's
        // zombie is its tracer's; a process a tracer still holds is announced
        // by the tracer when it lets go; and one whose announcement is still
        // waiting on a thread zombie goes to whoever is its parent by then.
        // And only to another process: a sibling thread was told when the
        // zombie died -- the SIGCHLD went to the process -- so a second one is
        // news of nothing, and Linux does not call reparent_leader then.
        if (child->zombie && child->group->leader == child && tracer_of(child) == NULL &&
                !child->group->exit_notify_deferred) {
            if (to_another_process && new_parent->group != NULL)
                exit_notify_process_locked(child, &notes);
            else
                reparented_zombies++;
        }
        // The orphaned-group rule again, for the CHILD's group: Linux's
        // reparent_leader asks it of every child handed to another process
        // (kill_orphaned_pgrp(p, father)), as exit_notify asks it of the
        // exiting process's own group below. A shell puts each job in a group
        // of its own, so a shell that exits with a job stopped orphans the
        // JOB's group and never its own; asked only about its own, AOK left
        // the job in state T for ever.
        //
        // Asked once the child has moved, as Linux asks it. Our children in
        // that group still to be moved name us as their parent, and we are a
        // way back into the session, so the group is found orphaned only at
        // the last of them. A thread of this same process that takes them
        // instead is the way back that we were, which is why Linux does not
        // ask at all when the reparent is a threaded one.
        if (to_another_process && child->group != NULL && child->group->leader == child) {
            pid_t_ pgid = child->group->pgid;
            pid_t_ sid = child->group->sid;
            if (pgid != task->group->pgid && sid == task->group->sid &&
                    pgrp_is_orphaned_locked(pgid, NULL, sid) &&
                    pgrp_has_stopped_member_locked(pgid, sid))
                orphaned_pgrps_add(&orphaned, pgid);
        }
    }

    // A zombie handed to a sibling thread is announced to nobody, but the
    // condition is woken all the same. It is harmless: it is the one every
    // thread of this process waits on anyway.
    if (reparented_zombies > 0 && new_parent != NULL && new_parent->group != NULL)
        notify(&new_parent->group->child_exit);

    // Does this exit orphan our own process group and leave stopped members
    // in it? Computed here, while the group still describes the pre-exit
    // state and pids_lock is held; the signals go out below, after the locks.
    //
    // Only when the process goes, as Linux asks it only when group_dead. The
    // test below ignores the whole process, so asked at any thread's exit it
    // answered for a process that was not leaving: a worker thread's exit
    // sent SIGHUP and SIGCONT to its own group, and so killed the process
    // itself along with the stopped child that made the group qualify.
    //
    // And only once the children have moved, as Linux's exit_notify asks it
    // after forget_original_parent. Our children in our group name their new
    // parent now: init, which is no way back, or a subreaper -- which is one
    // when it is in the session and outside the group. Asked before the move,
    // they named us and were passed over, and a group a subreaper could still
    // resume was hung up under it.
    pid_t_ orphan_pgid = 0;
    if (last_thread && leader != NULL && leader->group != NULL) {
        pid_t_ pgid = leader->group->pgid;
        pid_t_ sid = leader->group->sid;
        struct task *parent = leader->parent;
        // Only if we were the one holding it together: a parent already in
        // the group cannot have been the outside link.
        if (parent != NULL && parent->group != NULL &&
                parent->group->pgid != pgid && parent->group->sid == sid &&
                pgrp_is_orphaned_locked(pgid, leader, sid) &&
                pgrp_has_stopped_member_locked(pgid, sid))
            orphan_pgid = pgid;
    }

    // Let go of everything this task traces. Linux's exit_ptrace: each tracee
    // is detached, and a zombie one goes where it would have gone untraced
    // (__ptrace_detach). Taken from the head until the list is empty, and
    // unlinked whatever the detach decides, so no entry can be walked twice.
    while (!list_empty(&task->ptracees)) {
        child = list_first_entry(&task->ptracees, struct task, ptrace_siblings);
        bool ours = tracer_of(child) == task;
        ptrace_detach_from_tracer(task, child);
        list_remove_safe(&child->ptrace_siblings);
        if (ours && child->zombie)
            zombie_untraced_locked(task, child, &notes);
    }

    bool group_dead = exit_tgroup(task);

    // Process accounting is captured here, under the locks, and WRITTEN far
    // below once they are gone: the record has to be taken while the task is
    // certainly still alive, and the file write cannot happen while a pid or
    // task lock is held.
    struct acct_record acct_rec;
    bool acct_pending = false;
    bool taskstats_pending = false;

    // A stop this task never reported is not reported now: what its tracer
    // hears about is the exit. Left set, a zombie's stale stop was reported
    // ahead of its exit, and PTRACE_CONT "resumed" a dead task.
    lock(&task->ptrace.lock, 0);
    task->ptrace.stopped = false;
    task->ptrace.signal = 0;
    unlock(&task->ptrace.lock);

    struct task *tracer = tracer_of(task);
    if (tracer != NULL && task != leader) {
        // A traced thread's exit is its tracer's to collect, with wait and
        // __WALL, and until then it stays in the pid table as a zombie. Its
        // process can be neither reaped nor announced before that.
        task->zombie = true;
        task->group->traced_zombies++;
        notify(&tracer->group->child_exit);
        exit_notes_signal(&notes, tracer, SIGCHLD_, exit_signal_info(task, status, &group_rusage));
    } else if (tracer != NULL && !group_dead) {
        // A traced leader whose other threads still run cannot be reaped yet,
        // but Linux tells its tracer that it has exited all the same.
        notify(&tracer->group->child_exit);
        exit_notes_signal(&notes, tracer, SIGCHLD_, exit_signal_info(task, status, &group_rusage));
    }

    if (group_dead) {
        // Once per PROCESS, not once per thread -- the same place Linux calls
        // acct_process(). Costs one relaxed load when accounting is off.
        acct_pending = acct_collect(leader, &group_rusage, status, &acct_rec);
        // The same record, for anything that registered a taskstats cpumask
        // instead of turning on BSD accounting. Also collected here and sent
        // below, and for the same two reasons.
        taskstats_pending = netlink_taskstats_exit_collect(leader, &group_rusage, status);
        exit_hangup_session_tty(leader, &tty_hup);
        // With no exit_group to name one, a process's exit code is the code of
        // its last thread to exit -- Linux's synchronize_group_exit since 6.0.
        // A leader that left early with a code of its own used to be what wait
        // reported, where Linux reports the thread that actually ended the
        // process.
        lock(&task->group->lock, 0);
        if (!task->group->doing_group_exit) {
            task->group->doing_group_exit = true;
            task->group->group_exit_code = status;
        }
        unlock(&task->group->lock);
        struct task *parent = leader->parent;
        if (parent == NULL) {
            // init died. The CLI's halt_hook exits the host process with init's
            // status (and does not return), so the host exit code mirrors the guest
            // — including for multi-threaded init (e.g. the Go runtime), whose
            // lingering sibling threads would otherwise still be running.
            if (halt_hook != NULL)
                halt_hook(status);
            halt_targets = halt_system_collect_locked(&halt_target_count);
        } else if (task->group->traced_zombies > 0) {
            // A zombie thread is still waiting for its tracer, this one or an
            // earlier one. The process is dead -- /proc shows Z, and nothing
            // can attach to it -- but it is announced, and becomes reapable,
            // only when the last of them is released.
            leader->zombie = true;
            task->group->exit_notify_deferred = true;
        } else {
            // POSIX/Linux autoreap happens in here too: a parent that has
            // SIGCHLD set to SIG_IGN, or SA_NOCLDWAIT on its handler, has said
            // it will never wait, so no zombie is left for it. AOK once left
            // the zombie regardless, and a parent using the idiom accumulated
            // one per child for the life of the process. That includes the
            // case where the leader died first and a sibling thread finishes
            // last, which used to keep its zombie because freeing the leader
            // from here was not safe; task_destroy_unlinked now waits for a
            // zombie's own do_exit to finish, and task_free_final no longer
            // reads a group through a thread.
            exit_notify_process_locked(leader, &notes);
        }

        if (exit_hook != NULL)
            exit_hook(task, status);
    }

    vfork_notify(task);

    unlock(&task->general_lock);
    
    if (task != leader && !task->zombie) {
        task_unlink_locked(task);
        destroy_unlinked_task = true;
    }
    
    unlock(&pids_lock);

    // The first point past the locked region, so the append cannot deadlock
    // against anything do_exit was holding.
    if (acct_pending)
        acct_write(&acct_rec);
    if (taskstats_pending)
        netlink_taskstats_exit_broadcast();

    if (old_sighand != NULL)
        sighand_release(old_sighand);

    struct sigqueue *sigqueue, *sigqueue_tmp;
    list_for_each_entry_safe(&task->queue, sigqueue, sigqueue_tmp, queue) {
        list_remove(&sigqueue->queue);
        free(sigqueue);
    }

    if (tty_hup.fg_group != 0)
        send_group_signal(tty_hup.fg_group, SIGHUP_, SIGINFO_NIL);

    // Each child's parent-death signal, then the orphaned-group rule for the
    // children's groups, then for our own: the order Linux's exit_notify
    // reaches them in.
    pdeath_notes_send(&pdeath, pdeath_info);
    orphaned_pgrps_hang_up(&orphaned);
    if (orphan_pgid != 0)
        orphaned_pgrp_hang_up(orphan_pgid);

    // Process-directed signals: the parent may be multithreaded, and the
    // thread that happens to be `leader->parent` (whichever one called fork())
    // need not be the thread that's watching for SIGCHLD (e.g. a dedicated
    // signalfd reaper thread). Delivered to the whole group's shared queue so
    // any sibling can observe/dequeue it, matching Linux CLONE_THREAD
    // signal-sharing semantics. This task, if it was released, is freed last,
    // below.
    if (exit_notes_settle(&notes, task))
        destroy_unlinked_task = true;

    if (halt_targets != NULL)
        halt_system_kill(halt_targets, halt_target_count);

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

// Init has exited and the halt_hook, if any, returned. Only the app gets here:
// the CLI's hook exits the process. What is left is to stop every other task.
//
// This used to pthread_kill(task->thread, SIGKILL) each one. A host SIGKILL is
// not aimed at a thread -- it ends the whole process -- so in the app, running
// `reboot` killed iSH-AOK outright whenever any task outlived init, and one
// always did: busybox init's kill(-1, SIGKILL) takes the terminal's shell, the
// terminal starts a new one, and init exits two seconds later (#587). It also
// sent that signal to the saved pthread_t of leaders whose threads were gone.
//
// A guest SIGKILL does what was meant. Collected here under pids_lock, with a
// reference and the sighand held, and delivered by do_exit after it has let go
// of its locks, the way do_exit_group delivers its own. Every task, not just
// leaders: a process whose leader already exited lives on in its other threads.
//
// Filesystems stay mounted. Tearing them down here freed mounts that the tasks
// just told to die were still unwinding through; the host process outlives the
// guest and releases them when it exits.
static struct halt_target *halt_system_collect_locked(size_t *count) {
    size_t cap = 0;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task != NULL && !task->zombie && task->sighand != NULL)
            cap++;
    }
    *count = 0;
    if (cap == 0)
        return NULL;
    struct halt_target *targets = malloc(sizeof(*targets) * cap);
    if (targets == NULL)
        return NULL;
    for (int i = 2; i < MAX_PID && *count < cap; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task->zombie || task->sighand == NULL)
            continue;
        task_ref_cnt_mod(task, 1);
        sighand_retain(task->sighand);
        targets[*count] = (struct halt_target) {.task = task, .sighand = task->sighand};
        (*count)++;
    }
    return targets;
}

static void halt_system_kill(struct halt_target *targets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        deliver_signal_with_sighand(targets[i].task, targets[i].sighand, SIGKILL_, SIGINFO_NIL);
        sighand_release(targets[i].sighand);
        task_ref_cnt_mod(targets[i].task, -1);
    }
    free(targets);
}

// Only the low byte of an exit code survives, as in Linux: exit(0x1ff) leaves a
// wait status of 0xff00, and so does the PTRACE_EVENT_EXIT message built from
// it. Shifting the whole value handed a tracer 0x1ff00.
dword_t sys_exit(dword_t status) {
    STRACE("exit(%d)\n", status);
    do_exit(current, (status & 0xff) << 8);
}

dword_t sys_exit_group(dword_t status) {
    STRACE("exit_group(%d)\n", status);
    do_exit_group((status & 0xff) << 8);
}

#define WNOHANG_ (1 << 0)
#define WUNTRACED_ (1 << 1)
#define WEXITED_ (1 << 2)
#define WCONTINUED_ (1 << 3)
#define WNOWAIT_ (1 << 24)
#define __WNOTHREAD_ (1 << 29)
#define __WALL_ (1 << 30)
#define __WCLONE_ 0x80000000

#define P_ALL_ 0
#define P_PID_ 1
#define P_PGID_ 2

// A ptrace-stop is reported to the tracer and to nobody else. Linux's
// wait_task_stopped shows a waiter that is not the tracer only a group-stop,
// and only for WUNTRACED. reap_if_needed asked here on behalf of any parent,
// so a parent whose child someone else traced could take that child's stop:
// waitpid(child, 0) came back with 0x137f while the child sat stopped, and the
// tracer, never seeing it, never resumed it. A tracer that resumes the parent
// at each fork event, as strace -f does, lost the first stop of 17 to 37 of 200
// forks. A hung `strace -f sh -c '... | cat | wc -l'` had wc parked in a
// syscall-stop that strace's wait4 never reported.
static bool waiter_is_tracer(const struct task *task) {
    const struct task *tracer = tracer_of(task);
    return tracer != NULL && tracer->group == current->group;
}

// The same, for one wait: with __WNOTHREAD only the waiting thread's own
// tracees count, as Linux's ptrace pass walks only that thread's list.
static bool wait_traces(const struct task *task, bool this_thread_only) {
    return waiter_is_tracer(task) && (!this_thread_only || tracer_of(task) == current);
}

// A process cannot be reaped while any of its threads is still around: live
// ones, or zombies a tracer has yet to collect. Reaping it early leaves those
// threads pointing at freed group state -- the group is freed with the leader
// -- and corrupts /proc consumers that still dereference task->group. Caller
// holds task->group->lock.
static bool process_has_threads_locked(struct task *task) {
    return !list_empty(&task->group->threads) || task->group->traced_zombies > 0;
}

// returns false if the task cannot be reaped and true if the task was reaped
static bool reap_if_zombie(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out,
        int options, struct exit_notes *notes) {
    if (!task->zombie)
        return false;
    // A zombie someone else traces is that tracer's to report first. Its
    // parent waits: this answers "not yet", which is neither the child nor
    // ECHILD.
    if (zombie_is_tracers(task) && !waiter_is_tracer(task))
        return false;
    lock(&task->group->lock, 0);

    if (process_has_threads_locked(task)) {
        unlock(&task->group->lock);
        return false;
    }

    info_out->child.status = zombie_status(task);

    // The child's own usage AND its reaped descendants', Linux's RUSAGE_BOTH:
    // that is what wait4 reports, and what a parent's RUSAGE_CHILDREN and
    // times() accumulate (wait_task_zombie: cutime += tgutime + sig->cutime).
    // Passing on only the child's own lost every grandchild -- a parent that
    // reaped a shell was charged nothing for the programs the shell ran.
    struct rusage_ rusage = task->group->rusage;
    rusage_add(&rusage, &task->group->children_rusage);
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
    release_process_locked(task, notes);
    return true;
}

// The tracer's side of a traced zombie (see "who hears about an exit" above).
// A thread is reported and released. A process whose parent is the tracer is
// an ordinary reap. Any other process is reported, untraced, and announced to
// its parent -- which alone is charged its usage when it reaps it, as Linux
// charges only the EXIT_DEAD reap.
static bool reap_traced_zombie(struct task *task, struct siginfo_ *info_out,
        struct rusage_ *rusage_out, int options, struct exit_notes *notes) {
    if (!task->zombie || !waiter_is_tracer(task))
        return false;
    bool thread = task->group->leader != task;
    if (!thread && !zombie_is_tracers(task))
        return reap_if_zombie(task, info_out, rusage_out, options, notes);

    lock(&task->group->lock, 0);
    if (!thread && process_has_threads_locked(task)) {
        unlock(&task->group->lock);
        return false;
    }
    info_out->child.status = zombie_status(task);
    if (rusage_out != NULL) {
        *rusage_out = task->group->rusage;      // RUSAGE_BOTH, as reap_if_zombie
        rusage_add(rusage_out, &task->group->children_rusage);
    }
    unlock(&task->group->lock);

    if (options & WNOWAIT_)
        return true;
    if (thread) {
        release_thread_locked(task, notes);
        return true;
    }
    ptrace_detach_from_tracer(tracer_of(task), task);
    exit_notify_process_locked(task, notes);
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
    if (task->ptrace.stopped && task->ptrace.signal && waiter_is_tracer(task)) {
        info_out->child.status = task->ptrace.trap_event << 16 | task->ptrace.signal << 8 | 0x7f;
        task->ptrace.signal = 0;
        task->ptrace.trap_event = 0;
        // Not ptrace.eventmsg: the message must outlive the wait that reports
        // its stop, because waiting is how a tracer finds out there is a
        // message to ask for. Clearing it here made PTRACE_GETEVENTMSG answer
        // 0 at every event, so gdb took 0 as a new thread's pid, called
        // waitpid(0, ...), got some other child back and died with "wait
        // returned unexpected PID". Linux never clears it; the next stop
        // overwrites it (ptrace_stop_common).
        unlock(&task->ptrace.lock);
        return true;
    }
    unlock(&task->ptrace.lock);
    return false;
}

static bool reap_if_needed(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out,
        int options, struct exit_notes *notes) {
    assert(task_is_leader(task));
    if ((options & WUNTRACED_ && notify_if_stopped(task, info_out)) ||
        (options & WEXITED_ && reap_if_zombie(task, info_out, rusage_out, options, notes)) ||
        (options & WCONTINUED_ && notify_if_continued(task, info_out))) {
        info_out->sig = SIGCHLD_;
        return true;
    }
    if (notify_if_ptrace_stopped(task, info_out))
        return true;
    return false;
}

// Everything a tracer can be told about one of its tracees: a stop, or its
// exit. Tracee is an ordinary child here or not.
static bool report_tracee(struct task *task, struct siginfo_ *info_out, struct rusage_ *rusage_out,
        int options, struct exit_notes *notes) {
    if (notify_if_ptrace_stopped(task, info_out) ||
            ((options & WEXITED_) && reap_traced_zombie(task, info_out, rusage_out, options, notes))) {
        info_out->sig = SIGCHLD_;
        return true;
    }
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
    // A CHECKPOINT FREEZE ends this wait too, and it is not a signal -- so it
    // has to be asked about here rather than found among the pending ones.
    // This function exists to IGNORE bare pokes, which is exactly right for a
    // TLB shootdown and exactly wrong for a freeze: the freeze needs the
    // syscall to return so the dispatcher can rewind the program counter over
    // it and the task can park. Without this a shell blocked in wait() never
    // reached a boundary and froze nothing.
    //
    // The EINTR it produces never reaches the guest: syscall_result_should_
    // restart turns it into a restart while the freeze is on, so the wait
    // re-enters on the far side of the checkpoint.
    if (checkpoint_freeze_pending())
        return true;
    // So does a PTRACE_EVENT_STOP the task owes its tracer, for the same
    // reason; the restart predicates restart the wait after the stop.
    if (task_trap_stop_pending(current))
        return true;
    lock(&current->sighand->lock, 0);
    // See kernel/signal.h: a shim-held signal must end this wait too.
    bool pending = !!((current->pending | task_group_pending(current)) &
            ~task_wake_blocked(current));
    unlock(&current->sighand->lock);
    return pending;
}

// Whether a wait is for this child at all, by how the child announces its exit
// -- Linux's eligible_child. A child whose exit signal is anything but SIGCHLD,
// 0 included, is a "clone child": only __WCLONE waits for those, __WCLONE waits
// for nothing else, and __WALL waits for both. A wait that finds only children
// it is not for fails with ECHILD, WNOHANG or not, since they are not counted.
//
// AOK asked nobody's exit signal, so a plain waitpid reaped a child cloned with
// SIGUSR1 -- on Linux, ECHILD -- and __WCLONE was EINVAL.
//
// `leader` is a process leader: its exit signal is the process's. Not asked for
// a child the waiter traces, which is waited for as if with __WALL -- Linux has
// assumed that since 4.7, and does not ask in its ptrace pass (wait_traces).
static bool wait_eligible(const struct task *leader, int options) {
    if (options & __WALL_)
        return true;
    bool clone_child = leader->exit_signal != SIGCHLD_;
    return clone_child == ((options & __WCLONE_) != 0);
}

int do_wait(int idtype, pid_t_ id, struct siginfo_ *info, struct rusage_ *rusage, int options) {
    if (idtype != P_ALL_ && idtype != P_PID_ && idtype != P_PGID_)
        return _EINVAL;
    // Linux's wait4 and waitid both take __WNOTHREAD, __WCLONE and __WALL;
    // wait4 refuses waitid's WEXITED and WNOWAIT itself.
    if (options & ~(WNOHANG_|WUNTRACED_|WEXITED_|WCONTINUED_|WNOWAIT_|
            __WNOTHREAD_|__WALL_|__WCLONE_))
        return _EINVAL;
    // __WNOTHREAD: this thread's own children and tracees, not every thread's.
    bool this_thread_only = (options & __WNOTHREAD_) != 0;

    struct exit_notes notes;
    exit_notes_init(&notes);
    complex_lockt(&pids_lock, 0);
    int err;
    bool got_signal = false;

retry:
        if (idtype != P_PID_) {
            // look for a zombie child
            bool no_children = true;
            struct task *parent;
            list_for_each_entry(&current->group->threads, parent, group_links) {
                if (this_thread_only && parent != current)
                    continue;
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
                    // Before it counts as a child: one this wait is not for
                    // leaves the answer ECHILD, not "nothing yet". Unless we
                    // trace it: a tracee is waited for as if with __WALL, and
                    // one that is also our child is on no ptracees list
                    // (ptrace.c), so this is the only place it is found.
                    if (!wait_traces(task, this_thread_only) && !wait_eligible(task, options))
                        continue;
                    no_children = false;
                    info->child.pid = task->pid;
                    if (reap_if_needed(task, info, rusage, options, &notes))
                        goto found_something;
                }
                list_for_each_entry(&parent->ptracees, task, ptrace_siblings) {
                    // Every tracee, thread or not and with or without __WALL:
                    // Linux has assumed __WALL for a traced child since 4.7
                    // ("wait/ptrace: assume __WALL if the child is traced"),
                    // and measured on 6.12, a tracer's plain waitpid(-1, 0)
                    // reports a traced thread's stops and its exit. strace -f
                    // passes __WALL anyway; without either, a stopped thread
                    // was never reported, the tracer never resumed it, and the
                    // whole traced process froze.
                    if (idtype == P_PGID_) {
                        lock(&task->group->lock, 0);
                        bool pgid_match = task->group->pgid == id;
                        unlock(&task->group->lock);
                        if (!pgid_match)
                            continue;
                    }
                    no_children = false;
                    info->child.pid = task->pid;
                    if (report_tracee(task, info, rusage, options, &notes))
                        goto found_something;
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
        // A thread id is not a waitable child. Linux's wait_task_zombie and
        // wait_task_stopped match only thread-group LEADERS for a non-tracer,
        // so waiting on a child's thread tid fails immediately with ECHILD.
        // Resolving the tid to its leader instead made the leader look like
        // the match: WNOHANG returned 0 as though the child were merely still
        // running, which tells a caller to keep polling a pid that will never
        // be reportable. A tracer may still wait on a thread it traces.
        //
        // A traced task's stop and exit belong to THAT TASK, so it is asked
        // about itself. Resolving a thread to its leader first and testing the
        // leader's ptrace state meant `waitpid(<tid>, ..., __WALL)` never
        // returned: the thread was stopped, the leader was not, and the wait
        // slept forever on child_exit. `waitpid(-1, ..., __WALL)` found the
        // same stop immediately, via the ptracees loop above -- so the tracer
        // that waits on -1 (strace) worked and the tracer that waits on the
        // pid it just attached to (gdb's linux_nat_post_attach_wait) hung.
        // Measured by tests/manual/ptrace_detach_survives.c, which failed
        // exactly the two wait-by-tid cases and passed the six others. The same
        // resolution let a tracer waiting on a thread's tid collect the whole
        // PROCESS under that tid, and a tracer waiting on a process it traced
        // but had not forked destroy it, so its real parent's waitpid failed.
        //
        // Linux's do_wait_pid asks the same two questions, as the parent and
        // as the tracer. Only the parent is asked whether the wait is for a
        // child of this kind: the tracer's is always.
        info->child.pid = id;
        bool as_parent = task_is_leader(task) && task->parent != NULL &&
            (this_thread_only ? task->parent == current
                              : task->parent->group == current->group) &&
            wait_eligible(task, options);
        bool as_tracer = wait_traces(task, this_thread_only);
        if (!as_parent && !as_tracer)
            goto error;
        if (as_tracer && report_tracee(task, info, rusage, options, &notes))
            goto found_something;
        if (as_parent && reap_if_needed(task, info, rusage, options, &notes))
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
    exit_notes_settle(&notes, NULL);
    return 0;

error:
    unlock(&pids_lock);
    exit_notes_settle(&notes, NULL);
    return err;
}

dword_t sys_waitid(int_t idtype, pid_t_ id, addr_t info_addr, int_t options) {
    return sys_waitid_guest(idtype, id, info_addr, options);
}

#define P_PIDFD_ 3

dword_t sys_waitid_guest(int_t idtype, pid_t_ id, guest_addr_t info_addr, int_t options) {
    STRACE("waitid(%d, %d, %#x, %#x)", idtype, id, info_addr, options);
    // waitid must be told WHICH state changes to wait for; unlike wait4 there
    // is no default. (WSTOPPED is waitid's name for the bit wait4 calls
    // WUNTRACED -- the same value.) With none of them the call can never report anything, so
    // Linux refuses it immediately rather than blocking forever or -- as here
    // -- returning ECHILD, which tells the caller it has no children when the
    // real problem is its own argument.
    if ((options & (WEXITED_ | WUNTRACED_ | WCONTINUED_)) == 0)
        return _EINVAL;
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
    // A negative pid other than -1 is a process group, as for wait4: a restored
    // shell waits for its foreground JOB, and a job is a group.
    int idtype = P_PID_;
    pid_t_ id = (pid_t_) pid;
    if (pid == (dword_t) -1) {
        idtype = P_ALL_;
    } else if ((sdword_t) pid < -1) {
        idtype = P_PGID_;
        id = (pid_t_) -(sdword_t) pid;
    }
    // Blocked, as wait4 marks it (sys_wait4_guest below). Without it a native
    // shell waiting on its foreground command -- or a restored one waiting on
    // its job, or an exec stand-in waiting on the program it started -- read
    // as RUNNING: `R` in ps, and one more runnable task in the load average for
    // every such wait. An idle iPad after a restore reported a load of 1.77.
    int err = 0;
    TASK_MAY_BLOCK {
        err = do_wait(idtype, id, &info, NULL, options | WEXITED_);
    }
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
