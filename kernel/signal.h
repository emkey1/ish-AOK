#ifndef SIGNAL_H
#define SIGNAL_H

#include "misc.h"
#include "kernel/errno.h"   // _EINTR/_ERESTART for signal_restart_or_eintr
#include "util/list.h"
#include "util/sync.h"
#include <stdatomic.h>
struct task;

typedef qword_t sigset_t_;

#define SIG_ERR_ -1
#define SIG_DFL_ 0
#define SIG_IGN_ 1

#define SA_NOCLDSTOP_ 1
#define SA_NOCLDWAIT_ 2
#define SA_SIGINFO_ 4
#define SA_ONSTACK_ 0x08000000
#define SA_RESTART_ 0x10000000
#define SA_NODEFER_ 0x40000000
#define SA_RESETHAND_ 0x80000000

struct sigaction_ {
    guest_addr_t handler;
    qword_t flags;
    guest_addr_t restorer;
    sigset_t_ mask;
};

// One past the highest signal number, so a valid signal is 1 <= sig < NUM_SIGS
// -- which is how every bound in the tree spells it. Linux's highest is
// SIGRTMAX == 64, so this is 65: at 64, signal 64 itself did not exist and
// sigaction/kill/tgkill all returned EINVAL for it. sig_mask(64) is 1 << 63,
// which still fits sigset_t_ (uint64_t).
#define NUM_SIGS 65

#define	SIGHUP_    1
#define	SIGINT_    2
#define	SIGQUIT_   3
#define	SIGILL_    4
#define	SIGTRAP_   5
#define	SIGABRT_   6
#define	SIGIOT_    6
#define	SIGBUS_    7
#define	SIGFPE_    8
#define	SIGKILL_   9
#define	SIGUSR1_   10
#define	SIGSEGV_   11
#define	SIGUSR2_   12
#define	SIGPIPE_   13
#define	SIGALRM_   14
#define	SIGTERM_   15
#define	SIGSTKFLT_ 16
#define	SIGCHLD_   17
#define	SIGCONT_   18
#define	SIGSTOP_   19
#define	SIGTSTP_   20
#define	SIGTTIN_   21
#define	SIGTTOU_   22
#define	SIGURG_    23
#define	SIGXCPU_   24
#define	SIGXFSZ_   25
#define	SIGVTALRM_ 26
#define	SIGPROF_   27
#define	SIGWINCH_  28
#define	SIGIO_     29
#define	SIGPWR_    30
#define SIGSYS_    31

#define SI_USER_ 0
#define SI_QUEUE_ -1
#define SI_TIMER_ -2
#define SI_TKILL_ -6
#define SI_KERNEL_ 128

// SIGCHLD si_code values (CLD_*). Linux reports these to a SA_SIGINFO SIGCHLD
// handler and to waitid(2), with si_status carrying the *bare* exit code or
// signal number (not the wait(2)-encoded status word).
#define CLD_EXITED_    1
#define CLD_KILLED_    2
#define CLD_DUMPED_    3
#define CLD_TRAPPED_   4
#define CLD_STOPPED_   5
#define CLD_CONTINUED_ 6
#define TRAP_BRKPT_ 1
#define TRAP_TRACE_ 2
#define ILL_ILLOPC_ 1
#define FPE_INTDIV_ 1
#define SEGV_MAPERR_ 1
#define SEGV_ACCERR_ 2
#define BUS_ADRALN_ 1
#define BUS_ADRERR_ 2
#define BUS_OBJERR_ 3

union sigval_ {
    int_t sv_int;
    guest_addr_t sv_ptr;
};

union i386_sigval_ {
    int_t sv_int;
    addr_t sv_ptr;
};

struct siginfo_ {
    int_t sig;
    int_t sig_errno;
    int_t code;
    union {
        struct {
            pid_t_ pid;
            uid_t_ uid;
        } kill;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            union sigval_ value;
        } rt;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            int_t status;
            clock_t_ utime;
            clock_t_ stime;
        } child;
        struct {
            guest_addr_t addr;
        } fault;
        struct {
            guest_addr_t addr;
            int_t syscall;
            // AUDIT_ARCH_* of the call a seccomp filter trapped.
            dword_t arch;
        } sigsys;
        struct {
            int_t timer;
            int_t overrun;
            union sigval_ value;
            int_t _private;
        } timer;
    };
};

struct i386_siginfo_ {
    int_t sig;
    int_t sig_errno;
    int_t code;
    union {
        struct {
            pid_t_ pid;
            uid_t_ uid;
        } kill;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            union i386_sigval_ value;
        } rt;
        struct {
            pid_t_ pid;
            uid_t_ uid;
            int_t status;
            clock_t_ utime;
            clock_t_ stime;
        } child;
        struct {
            addr_t addr;
        } fault;
        struct {
            addr_t addr;
            int_t syscall;
            dword_t arch;
        } sigsys;
        struct {
            int_t timer;
            int_t overrun;
            union i386_sigval_ value;
            int_t _private;
        } timer;
    };
};

// a reasonable default siginfo
static const struct siginfo_ SIGINFO_NIL = {
    .code = SI_KERNEL_,
};

// See kernel/signal.c. Counts `expirations` repeat POSIX-timer expirations
// onto the already-queued signal instead of queueing another; -1 if none is
// queued.
int signal_timer_count_overrun(struct task *task, int sig, int timer_id, uint64_t expirations);
// A POSIX timer's expiry: queue `sig` with `info` as the timer's own signal
// (struct sigqueue's from_timer), to `task` alone with `to_thread`
// (SIGEV_THREAD_ID), else to its process, where any thread that can takes it.
void send_timer_signal(struct task *task, bool to_thread, int sig, struct siginfo_ info);
struct tgroup;
// Timer `timer_id` of `group` was deleted: its signal, if still queued, is no
// longer its own. See kernel/signal.c.
void signal_timer_disown(struct tgroup *group, int timer_id);

struct sigqueue {
    struct list queue;
    struct siginfo_ info;
    // Queued by a POSIX timer's expiry (send_timer_signal), not sent: Linux's
    // preallocated sigqueue of the timer. Queued even behind a pending
    // instance of the same standard signal, and the only entry a later expiry
    // of that timer counts an overrun onto (signal_timer_count_overrun) or
    // that latches timer_getoverrun as it is taken (signal_timer_taken).
    // si_code SI_TIMER does not say so: rt_sigqueueinfo can claim it, and
    // Linux treats such a signal like any other. Cleared when the timer is
    // deleted (signal_timer_disown). Carried by a checkpoint.
    bool from_timer;
};

struct sigevent_ {
    union sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
};

// send a signal
// you better make sure the task isn't gonna get freed under me (pids_lock or current)
void send_signal(struct task *task, int sig, struct siginfo_ info);
// Whether current may send `sig` to `task`: Linux's check_kill_permission,
// credentials plus the SIGCONT-within-the-session exception. Asked by kill()
// and its family, and by an exit sending a child its parent-death signal.
bool may_signal_task(struct task *task, dword_t sig);
// send a signal without regard for whether the signal is blocked or ignored
void deliver_signal(struct task *task, int sig, struct siginfo_ info);
// Queue a signal on a task that task_start has not started yet, waking nothing.
// send_signal's wake pokes task->thread, which until task_start is still the
// PARENT's pthread, copied by task_create_. The task takes the signal before
// its first instruction only if its start path looks for one (task_thread).
void signal_queue_before_start(struct task *task, int sig, struct siginfo_ info);
// true when the syscall a signal interrupted restarts: the next deliverable
// signal runs a handler with SA_RESTART, or runs none at all (a stop, or only
// ignored signals)
bool signal_should_restart_syscall(void);
// Forget what an earlier syscall's interruption left for a restart: the answer
// a signal recorded when it interrupted a wait (restart_interrupted_syscall),
// and a pending rewind (restart_nohand_pending, restart_sys_pending). Called by
// both syscall dispatchers before every syscall; see the definition.
void signal_restart_state_clear(void);
// Wake a task out of whatever it is blocked in, with no signal involved --
// kernel/checkpoint.c's freezer. See the definition.
void task_wake_for_freeze(struct task *task);
// The same, for a task that now owes its tracer a PTRACE_EVENT_STOP
// (ptrace.trap_stop, kernel/task.h). The caller holds a reference on `task` and
// on `sighand`, which is `task`'s, and no lock. See the definition.
struct sighand;
void task_wake_for_ptrace_trap(struct task *task, struct sighand *sighand);
bool signal_should_restart_syscall_nohand(void);

// Turn a wait's _EINTR into _ERESTART when the handler that interrupted it was
// installed with SA_RESTART, so the dispatcher re-executes the syscall and the
// guest never sees the interruption. Call this from the syscall entry points
// Linux restarts -- read/write family, ioctl, open, the blocking socket calls,
// flock and F_SETLKW, wait.
//
// Do NOT call it from anything in signal(7)'s never-restarted list: poll,
// select, epoll_wait, nanosleep, sigsuspend and pause, the sigwait family,
// System V IPC (msgrcv, msgsnd, semop), io_getevents, or a socket call with
// SO_RCVTIMEO/SO_SNDTIMEO set (that one is signal_eintr_no_restart, below).
// Those must keep returning _EINTR; restarting them hangs a guest that relies
// on the interruption to make progress. Of those, poll, select, nanosleep,
// sigsuspend, pause, msgrcv and msgsnd take the _nohand form below, because
// Linux resumes them across a job-control stop. epoll_wait, rt_sigtimedwait
// and semop are never restarted at all: Linux fails them with EINTR once the
// process is continued.
static inline int_t signal_restart_or_eintr(int_t res) {
    if (res == _EINTR && signal_should_restart_syscall())
        return _ERESTART;
    return res;
}

// The ERESTARTNOHAND form, for signal(7)'s never-restarted interfaces: a
// running handler still gives the guest its EINTR, but a job-control stop
// resumes the syscall transparently, exactly as Linux does.
static inline int_t signal_restart_or_eintr_nohand(int_t res) {
    if (res == _EINTR && signal_should_restart_syscall_nohand())
        return _ERESTART_NOHAND;
    return res;
}

// The never-restarted form, for a socket wait with SO_RCVTIMEO/SO_SNDTIMEO
// armed: Linux ends it with sock_intr_errno(timeo), which is -EINTR however the
// interruption came -- an SA_RESTART handler, or a stop and a SIGCONT with no
// handler at all.
//
// It still consumes the interruption's recorded restart answer, as the two
// forms above do. A signal that interrupts a wait parked on a cond_t records
// whether that wait may be restarted (wake_waiting_task), and the next
// restart decision reads the record first -- so a timed wait that answered
// EINTR without reading it left the answer for the NEXT interrupted syscall.
// Measured: after a timed netlink recv was interrupted by an SA_RESTART
// handler, a pipe read interrupted by a handler WITHOUT SA_RESTART restarted,
// and returned data that arrived later instead of EINTR. Every syscall entry
// now forgets the record as well (signal_restart_state_clear), so this is no
// longer the only thing that stops the answer reaching the next syscall.
int_t signal_eintr_no_restart(int_t res);

// send a signal to current if it's not blocked or ignored, return whether that worked
// exists specifically for sending SIGTTIN/SIGTTOU
bool signal_is_ignored_or_blocked(int sig);
// send a signal to all processes in a group, could return ESRCH
int send_group_signal(dword_t pgid, int sig, struct siginfo_ info);
// check for and deliver pending signals on current
// must be called without pids_lock, current->group->lock, or current->sighand->lock
void receive_signals(void);
// Block for the duration of a job-control group-stop, reporting it to a tracer
// if the task is traced. Shared by the two execution models -- handle_interrupt
// for translated code, native_checkpoint for a native program -- because they
// used to hold separate copies and the native one silently lacked every bit of
// the ptrace handling. Call with no lock held, from the task's own context.
void group_stop_wait(void);
// The blocked set as far as WAKING a task is concerned, which is not the same
// as the blocked set as far as delivering to it is concerned: a native program's
// handler is held by the shim with the signal blocked (kernel/native_libc.c),
// and such a task must still wake up, because the handler runs at its next
// syscall checkpoint. Delivery decisions keep using ->blocked directly.
#define task_wake_blocked(task) ((task)->blocked & ~(task)->native_held)

// What of the process's shared queue (sighand->pending) is `task`'s to act on:
// all of it once the task has been told to take a signal there
// (group_sigpending in kernel/task.h, Linux's TIF_SIGPENDING), and nothing
// before. ORed with ->pending, it is the set a wait ends for and a syscall's
// way out looks at. Counting the whole shared queue for every thread made a
// signal sent to the process end every thread's wait: a SIGCHLD handler ran in
// whichever sibling got there first, and the rest failed with EINTR, where
// Linux interrupts only the thread it picked. Readable without sighand->lock,
// as the fast paths that use it read ->pending.
#define task_group_pending(task) \
    (__atomic_load_n(&(task)->group_sigpending, __ATOMIC_ACQUIRE) ? \
        __atomic_load_n(&(task)->sighand->pending, __ATOMIC_ACQUIRE) : (sigset_t_) 0)
// Hand a sibling whatever current was told to take off the shared queue and
// has since blocked (group_handoff in kernel/task.h). Call with no lock held.
void signal_group_handoff(void);
// The same for everything `task` was told to take, as it starts to exit; after
// task->exiting is set, so that nothing hands it another. For do_exit.
void signal_exit_handoff(struct task *task);

// Whether `task` owes its tracer a PTRACE_EVENT_STOP (ptrace.trap_stop in
// kernel/task.h). It is not a signal and nothing blocks it, so every wait that
// ends for a deliverable signal has to end for it as well, and a syscall it cut
// short restarts: the stop runs no handler.
#define task_trap_stop_pending(task) __atomic_load_n(&(task)->ptrace.trap_stop, __ATOMIC_ACQUIRE)

// Replace the blocked mask outright, the way SIG_SETMASK does, for code acting
// on a task's behalf with no guest syscall to carry the set: native_spawn_opts
// gives a spawned child the mask a forked child would have restored for itself.
void sigmask_set_blocked(sigset_t_ set);
// Set the signal mask for the rest of a call like sigsuspend. The next
// receive_signals chooses against it and then puts the old mask back: through
// the first handler's frame, at that handler's sigreturn, or at once if it runs
// no handler.
void sigmask_set_temp(sigset_t_ mask);
// restore a temporary signal mask immediately
void sigmask_clear_temp(void);

struct sighand {
    atomic_uint refcount;
    struct sigaction_ action[NUM_SIGS];
    lock_t lock;
    // Process-directed signal queue, shared by every thread in the CLONE_SIGHAND
    // group (Linux's signal_struct->shared_pending). A signal landing here (as
    // opposed to one specific task's own `pending`/`queue`) can be dequeued by
    // ANY sibling thread with it unblocked -- not just whichever task object
    // the sender happened to address -- and signalfd, sigwaitinfo and
    // sigpending see it from every thread. But only a thread TOLD to take it
    // (task->group_sigpending) is woken for it or has a wait ended by it, as
    // on Linux. Locked by `lock`, same as every task's own `pending`/`queue`
    // (sighand is already shared across the group, so this is the same lock
    // instance for every sibling).
    struct list queue;
    sigset_t_ pending;
    // Serializes signal_wake_task's temporary release of `lock` (kernel/signal.c):
    // that function has to give up `lock` before calling wake_waiting_task, since
    // wake_waiting_task can itself block on an unrelated lock (see its own
    // comments) and holding `lock` across that risks an ABBA deadlock against
    // code elsewhere that must take locks in the other order (e.g. pids_lock ->
    // `lock`, never the reverse -- see send_process_signal). But that leaves a
    // real window where `lock` is genuinely, fully unlocked despite the "caller
    // holds sighand->lock" contract -- a second, concurrent deliver_signal_*
    // call for the same sighand (e.g. two children exiting at once, each
    // delivering SIGCHLD to the same parent) can legitimately acquire `lock`
    // during that window and reach its own signal_wake_task call, which then
    // races the first call's manual unlock/relock of the exact same mutex --
    // undefined behavior for a plain, non-recursive pthread_mutex_t, and able to
    // corrupt `lock` for good. wake_lock is acquired only while `lock` is NOT
    // held (after releasing it, before reacquiring it) so it can never nest with
    // `lock` in a way that reintroduces an ABBA hazard; it just ensures at most
    // one thread is ever mid-wake for a given sighand at a time.
    lock_t wake_lock;
};
struct sighand *sighand_new(void);
struct sighand *sighand_copy(struct sighand *sighand);
void sighand_retain(struct sighand *sighand);
void sighand_release(struct sighand *sighand);

// What would happen if `sig` were delivered to a task using this sighand, given
// its current disposition. Callers must hold sighand->lock. Exported for
// sys_clone_common's vfork wait, which has to tell a signal that will terminate
// the task from one it could return from.
#define SIGNAL_IGNORE 0
#define SIGNAL_KILL 1
#define SIGNAL_CALL_HANDLER 2
#define SIGNAL_STOP 3
int signal_action(struct sighand *sighand, int sig);
// Whether receive_signals will stop for task's tracer before `sig` is
// delivered, which makes signal_action's answer a guess: the tracer may deliver
// it, suppress it, or deliver something else. (PTRACE_INTERRUPT is no signal at
// all: see ptrace.trap_stop and task_trap_stop_pending.)
bool signal_stops_for_tracer(struct task *task, int sig);
void deliver_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info);
struct tgroup;
// Deliver a process-directed signal to `task`'s thread group: enqueues into
// the shared sighand->queue (visible to any sibling thread's signalfd/
// sigwaitinfo/sigpending/receive_signals, matching Linux's shared_pending) and
// tells ONE thread that can take it -- `task` itself when it can -- to do so,
// as Linux's complete_signal does. For every signal addressed to "the
// process": kill() and the rest of its family, the terminal's, a child's exit,
// stop and continue, the timers', rather than to one specific thread
// (tkill/tgkill/synchronous traps stay on send_signal/deliver_signal). A
// SIGCONT or stop signal cancels the other kind on every queue of the process,
// and SIGCONT and SIGKILL lift a stop, as send_signal does for one thread.
//
// `task` is the thread it is sent to, and only its mask decides whether a
// signal the process ignores is queued at all, as on Linux: for a child's exit
// that is the thread that forked it. It may have exited; a task no longer in
// the pid table was reaped with its process, and is sent nothing. The caller
// holds a reference on `task`, and no pids_lock.
void send_signal_to_process(struct task *task, int sig, struct siginfo_ info);
// The same, for a caller that holds pids_lock.
void send_signal_to_process_pids_locked(struct task *task, int sig, struct siginfo_ info);
// The same, sent to the group's leader, which is where Linux sends an interval
// timer's signal (it_real_fn's leader_pid).
void send_signal_to_group(struct tgroup *group, int sig, struct siginfo_ info);
// Tell `parent` that a child stopped, continued or entered a ptrace stop, as
// Linux's do_notify_parent_cldstop does once it has picked the recipient: with
// SIGCHLD -- never the child's exit signal -- to the recipient's process, and
// not at all when its SIGCHLD is SIG_IGN or has SA_NOCLDSTOP. `info` carries
// the CLD_* code and the child's pid, uid and status. The caller holds a
// reference on `parent` and no lock.
void notify_parent_cldstop(struct task *parent, struct siginfo_ info);

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size);
dword_t sys_rt_sigaction_guest(dword_t signum, guest_addr_t action_addr, guest_addr_t oldaction_addr, dword_t sigset_size);
dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr);
dword_t sys_rt_sigreturn(void);
qword_t sys_rt_sigreturn_amd64(void);
qword_t sys_rt_sigreturn_arm64(void);
qword_t sys_rt_sigreturn_riscv64(void);
dword_t sys_sigreturn(void);

#define SIG_BLOCK_ 0
#define SIG_UNBLOCK_ 1
#define SIG_SETMASK_ 2
typedef uint64_t sigset_t_;
dword_t sys_rt_sigprocmask(dword_t how, addr_t set, addr_t oldset, dword_t size);
dword_t sys_rt_sigprocmask_guest(dword_t how, guest_addr_t set, guest_addr_t oldset, dword_t size);
dword_t sys_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr);
dword_t sys_sigprocmask_guest(dword_t how, guest_addr_t set_addr, guest_addr_t oldset_addr);
int_t sys_rt_sigpending(addr_t set_addr);
int_t sys_rt_sigpending_guest(guest_addr_t set_addr);

static inline sigset_t_ sig_mask(int sig) {
    assert(sig >= 1 && sig < NUM_SIGS);
    return 1l << (sig - 1);
}

static inline bool sigset_has(sigset_t_ set, int sig) {
    return !!(set & sig_mask(sig));
}
static inline void sigset_add(sigset_t_ *set, int sig) {
    *set |= sig_mask(sig);
}
static inline void sigset_del(sigset_t_ *set, int sig) {
    *set &= ~sig_mask(sig);
}

struct stack_t_ {
    addr_t stack;
    dword_t flags;
    dword_t size;
};
#define SS_ONSTACK_ 1
#define SS_DISABLE_ 2
#define MINSIGSTKSZ_ 2048
dword_t sys_sigaltstack(guest_addr_t ss, guest_addr_t old_ss);
dword_t sys_sigaltstack_guest(guest_addr_t ss, guest_addr_t old_ss);

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size);
int_t sys_rt_sigsuspend_guest(guest_addr_t mask_addr, uint_t size);
int_t sys_pause(void);
int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size);
int_t sys_rt_sigtimedwait_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size);
int_t sys_rt_sigtimedwait_time64(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size);
int_t sys_rt_sigtimedwait_time64_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size);
dword_t sys_rt_sigqueueinfo(pid_t_ pid, dword_t sig, addr_t uinfo_addr);
dword_t sys_rt_sigqueueinfo_guest(pid_t_ pid, dword_t sig, guest_addr_t uinfo_addr);
dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, dword_t sig, addr_t uinfo_addr);
dword_t sys_rt_tgsigqueueinfo_guest(pid_t_ tgid, pid_t_ tid, dword_t sig, guest_addr_t uinfo_addr);
int_t sys_signalfd(int_t fd, addr_t mask_addr, dword_t sigsetsize);
int_t sys_signalfd4(int_t fd, addr_t mask_addr, dword_t sigsetsize, int_t flags);
int_t sys_signalfd_guest(int_t fd, guest_addr_t mask_addr, dword_t sigsetsize);
int_t sys_signalfd4_guest(int_t fd, guest_addr_t mask_addr, dword_t sigsetsize, int_t flags);

// kill(2)'s send to the process `task` belongs to, after its permission check.
int signal_kill_process(struct task *task, dword_t sig, int si_code);
dword_t sys_kill(pid_t_ pid, dword_t sig);
dword_t sys_tkill(pid_t_ tid, dword_t sig);
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig);
int siginfo_to_user(struct task *task, guest_addr_t user_addr, const struct siginfo_ *info);

// signal frame structs. There's a good chance this should go in its own header file

// thanks kernel for giving me something to copy/paste
struct sigcontext_ {
    word_t gs, __gsh;
    word_t fs, __fsh;
    word_t es, __esh;
    word_t ds, __dsh;
    dword_t di;
    dword_t si;
    dword_t bp;
    dword_t sp;
    dword_t bx;
    dword_t dx;
    dword_t cx;
    dword_t ax;
    dword_t trapno;
    dword_t err;
    dword_t ip;
    word_t cs, __csh;
    dword_t flags;
    dword_t sp_at_signal;
    word_t ss, __ssh;

    dword_t fpstate;
    dword_t oldmask;
    dword_t cr2;
};

struct ucontext_ {
    uint_t flags;
    uint_t link;
    struct stack_t_ stack;
    struct sigcontext_ mcontext;
    sigset_t_ sigmask;
} __attribute__((packed));

struct fpreg_ {
    word_t significand[4];
    word_t exponent;
};

struct fpxreg_ {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct xmmreg_ {
    uint32_t element[4];
};

struct fpstate_ {
    /* Regular FPU environment.  */
    dword_t cw;
    dword_t sw;
    dword_t tag;
    dword_t ipoff;
    dword_t cssel;
    dword_t dataoff;
    dword_t datasel;
    struct fpreg_ st[8];
    word_t status;
    word_t magic;

    /* FXSR FPU environment.  */
    dword_t _fxsr_env[6];
    dword_t mxcsr;
    dword_t reserved;
    struct fpxreg_ fxsr_st[8];
    struct xmmreg_ xmm[8];
    dword_t padding[56];
};

struct sigframe_ {
    addr_t restorer;
    dword_t sig;
    struct sigcontext_ sc;
    struct fpstate_ fpstate;
    dword_t extramask;
    char retcode[8];
};

struct rt_sigframe_ {
    addr_t restorer;
    int_t sig;
    addr_t pinfo;
    addr_t puc;
    union {
        struct i386_siginfo_ info;
        char __pad[128];
    };
    struct ucontext_ uc;
    char retcode[8];
};

// On a 64-bit system with 32-bit emulation, the fpu state is stored in extra
// space at the end of the frame, not in the frame itself. We store the fpu
// state in the frame where it should be, and ptraceomatic will set this. If
// they are set we'll add some padding to the bottom to the frame to make
// everything align.
extern int xsave_extra;
extern int fxsave_extra;

#endif
