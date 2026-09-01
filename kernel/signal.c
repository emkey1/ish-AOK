#include "debug.h"
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include "fs/poll.h"
#include "kernel/calls.h"
#include "kernel/futex.h"
#include <stdio.h>
#include "kernel/signal.h"
#include "kernel/time.h"
#include "kernel/task.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"
#include "emu/memory.h"
#include "util/sync.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(sigset_t_ set);
static bool is_on_altstack(guest_addr_t sp, struct task *task);
static void restore_altstack(guest_addr_t sp, guest_addr_t stack, guest_addr_t size, dword_t flags);
static dword_t current_altstack_flags(struct task *task);
static void altstack_to_i386_user(struct task *task, struct stack_t_ *user_stack);
static void signalfd_wakeup_task(struct task *task, int sig);
static struct fd_ops signalfd_ops;
static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info);

static bool should_trace_signal_task(struct task *UNUSED(task)) {
    return false;
}

struct sigaction_i386_marshaled {
    addr_t handler;
    dword_t flags;
    addr_t restorer;
    sigset_t_ mask;
} __attribute__((packed));

struct sigaction_amd64_marshaled {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    sigset_t_ mask;
} __attribute__((packed));

struct amd64_siginfo_ {
    int_t sig;
    int_t sig_errno;
    int_t code;
    int_t __pad0;
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
            int_t __pad0;
            qword_t utime;
            qword_t stime;
        } child;
        struct {
            guest_addr_t addr;
            word_t addr_lsb;
            byte_t __pad0[6];
            guest_addr_t lower;
            guest_addr_t upper;
        } fault;
        struct {
            int_t timer;
            int_t overrun;
            union sigval_ value;
            int_t _private;
            int_t __pad0;
        } timer;
        struct {
            guest_addr_t call_addr;
            int_t syscall;
            dword_t arch;
        } sigsys;
        byte_t __pad[112];
    };
} __attribute__((packed));

static_assert(sizeof(struct amd64_siginfo_) == 128, "amd64 siginfo layout mismatch");

enum amd64_greg_index {
    AMD64_GREG_R8 = 0,
    AMD64_GREG_R9,
    AMD64_GREG_R10,
    AMD64_GREG_R11,
    AMD64_GREG_R12,
    AMD64_GREG_R13,
    AMD64_GREG_R14,
    AMD64_GREG_R15,
    AMD64_GREG_RDI,
    AMD64_GREG_RSI,
    AMD64_GREG_RBP,
    AMD64_GREG_RBX,
    AMD64_GREG_RDX,
    AMD64_GREG_RAX,
    AMD64_GREG_RCX,
    AMD64_GREG_RSP,
    AMD64_GREG_RIP,
    AMD64_GREG_EFL,
    AMD64_GREG_CSGSFS,
    AMD64_GREG_ERR,
    AMD64_GREG_TRAPNO,
    AMD64_GREG_OLDMASK,
    AMD64_GREG_CR2,
    AMD64_GREG_COUNT,
};

enum {
    AMD64_USER_CS = 0x33,
    AMD64_UC_FP_XSTATE = 0x1,
    AMD64_UC_SIGCONTEXT_SS = 0x2,
    AMD64_UC_STRICT_RESTORE_SS = 0x4,
};

struct amd64_stack_t_marshaled {
    qword_t stack;
    dword_t flags;
    dword_t pad;
    qword_t size;
};

struct amd64_fpxreg_ {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct amd64_xmmreg_ {
    uint32_t element[4];
};

struct amd64_fpstate_ {
    word_t cwd;
    word_t swd;
    word_t twd;
    word_t fop;
    qword_t rip;
    qword_t rdp;
    dword_t mxcsr;
    dword_t mxcr_mask;
    struct amd64_fpxreg_ st[8];
    struct amd64_xmmreg_ xmm[16];
    dword_t reserved1[24];
};

static_assert(sizeof(struct amd64_fpstate_) == 512, "amd64 fpstate layout mismatch");

struct amd64_mcontext_ {
    qword_t gregs[AMD64_GREG_COUNT];
    guest_addr_t fpstate;
    qword_t reserved1[8];
};

struct amd64_ucontext_ {
    qword_t flags;
    guest_addr_t link;
    struct amd64_stack_t_marshaled stack;
    struct amd64_mcontext_ mcontext;
    sigset_t_ sigmask;
    struct amd64_fpstate_ fpregs_mem;
    qword_t ssp[4];
};

struct rt_sigframe_amd64 {
    guest_addr_t pretcode;
    struct amd64_ucontext_ uc;
    struct amd64_siginfo_ info;
    char retcode[8];
};

// ---- AArch64 signal frame (arch/arm64 rt_sigframe layout) ----------------
// siginfo and stack_t reuse the amd64 marshaled structs: the generic
// 64-bit siginfo layout and stack_t are byte-identical on aarch64.
// The mcontext's __reserved area carries a chain of context records; the
// kernel always writes an fpsimd_context first, so real userspace
// (setjmp-out-of-handler, unwinders) expects one — followed by a null
// terminator record.
#define ARM64_FPSIMD_MAGIC 0x46508001u

struct arm64_fpsimd_context_ {
    dword_t magic;
    dword_t size; // sizeof(struct arm64_fpsimd_context_) = 528
    dword_t fpsr;
    dword_t fpcr;
    union xmm_reg vregs[32];
};

static_assert(sizeof(struct arm64_fpsimd_context_) == 528, "arm64 fpsimd_context layout mismatch");

struct arm64_mcontext_ {
    qword_t fault_address;
    qword_t regs[31];
    qword_t sp;
    qword_t pc;
    qword_t pstate;
    char reserved[4096] __attribute__((aligned(16)));
};

struct arm64_ucontext_ {
    qword_t flags;
    qword_t link;
    struct amd64_stack_t_marshaled stack;
    sigset_t_ sigmask;
    // The kernel reserves 128 bytes for the sigmask area (uc_sigmask is
    // declared sigset_t but followed by __unused padding out to 128);
    // userspace's ucontext_t declares uc_sigmask as the full 128 bytes.
    char sigmask_pad[128 - sizeof(sigset_t_)];
    struct arm64_mcontext_ mcontext; // aligned(16) via the member type
};

struct rt_sigframe_arm64 {
    struct amd64_siginfo_ info;
    struct arm64_ucontext_ uc;
    // Not part of the kernel's frame: the sigreturn trampoline. Real
    // Linux points X30 at the vDSO's __kernel_rt_sigreturn; this port
    // has no arm64 vDSO yet, so the trampoline lives on the stack like
    // i386's retcode (the JIT reads guest code through the normal
    // readable-page path, so no PROT_EXEC concern under emulation).
    dword_t retcode[2]; // movz x8, #139 ; svc #0
};


// riscv64 signal frame (arch/riscv uapi): sigcontext is the gp regs
// (pc, then x1..x31 in order) followed by the 528-byte __riscv_fp_state
// union (sized by its q-extension member; only the d-extension view is
// populated here). ucontext has the same 128-byte sigmask reservation as
// arm64's.
struct riscv64_mcontext_ {
    qword_t pc;
    qword_t regs[31]; // x1..x31
    // union __riscv_fp_state, 528 bytes (sized by the q-extension view,
    // which carries aligned(16) in the kernel uapi — that alignment is
    // what pushes uc_mcontext to offset 176 in the ucontext). Only the
    // d-extension view is populated.
    qword_t f[32];
    dword_t fcsr;
    char fp_pad[528 - 32 * 8 - 4];
} __attribute__((aligned(16)));
static_assert(sizeof(struct riscv64_mcontext_) == 784, "riscv64 sigcontext size");

struct riscv64_ucontext_ {
    qword_t flags;
    qword_t link;
    struct amd64_stack_t_marshaled stack;
    sigset_t_ sigmask;
    char sigmask_pad[128 - sizeof(sigset_t_)];
    struct riscv64_mcontext_ mcontext; // aligned(16) via the member type
};
static_assert(sizeof(struct riscv64_ucontext_) == 960, "riscv64 ucontext size");

struct rt_sigframe_riscv64 {
    struct amd64_siginfo_ info; // generic 64-bit siginfo layout
    struct riscv64_ucontext_ uc;
    // Not part of the kernel frame: the sigreturn trampoline. Real Linux
    // riscv64 always returns via the vDSO's __vdso_rt_sigreturn; this
    // port has no riscv vDSO, so it lives on the stack like arm64's.
    dword_t retcode[2]; // li a7, 139 ; ecall
};
static_assert(offsetof(struct rt_sigframe_riscv64, uc) == 128, "riscv64 frame uc offset");

static int sigaction_from_user(struct task *task, guest_addr_t user_addr, struct sigaction_ *action) {
    // arm64 shares the amd64 marshaling: aarch64's struct sigaction is the
    // same {handler, flags, restorer, mask} qword layout (arm64 defines
    // SA_RESTORER, so the field is present). Routing arm64 through the
    // i386 branch here was why busybox sh's SIGCHLD handler registration
    // read garbage before the arm64 frame support landed.
    if (guest_abi_is_64bit(task->abi)) {
        struct sigaction_amd64_marshaled user_action;
        if (user_get(user_addr, user_action))
            return _EFAULT;
        *action = (struct sigaction_) {
            .handler = user_action.handler,
            .flags = user_action.flags,
            .restorer = user_action.restorer,
            .mask = user_action.mask,
        };
    } else {
        struct sigaction_i386_marshaled user_action;
        if (user_get(user_addr, user_action))
            return _EFAULT;
        *action = (struct sigaction_) {
            .handler = user_action.handler,
            .flags = user_action.flags,
            .restorer = user_action.restorer,
            .mask = user_action.mask,
        };
    }
    return 0;
}

static int sigaction_to_user(struct task *task, guest_addr_t user_addr, const struct sigaction_ *action) {
    if (guest_abi_is_64bit(task->abi)) { // arm64 shares the amd64 layout, see sigaction_from_user
        struct sigaction_amd64_marshaled user_action = {
            .handler = action->handler,
            .flags = action->flags,
            .restorer = action->restorer,
            .mask = action->mask,
        };
        if (user_put(user_addr, user_action))
            return _EFAULT;
    } else {
        struct sigaction_i386_marshaled user_action = {
            .handler = (addr_t) action->handler,
            .flags = (dword_t) action->flags,
            .restorer = (addr_t) action->restorer,
            .mask = action->mask,
        };
        if (user_put(user_addr, user_action))
            return _EFAULT;
    }
    return 0;
}

static bool wake_waiting_task(struct task *task) {
    pthread_mutex_lock(&task->waiting_cond_lock.m);
    task->waiting_cond_lock.owner = pthread_self();

    cond_t *waiting_cond = task->waiting_cond;
    lock_t *waiting_lock = task->waiting_lock;
    bool *waiting_interrupt_flag = task->waiting_interrupt_flag;
    bool interrupted_wait = waiting_interrupt_flag != NULL ||
        (waiting_cond != NULL && waiting_lock != NULL);
    if (waiting_interrupt_flag != NULL) {
        // Counts the bug in docs/TODO.md's pread_stack_thread_race entry
        // directly, instead of waiting for its ~1% fatal outcome: a stale
        // pointer here means this store lands in a frame that has already
        // returned, and one of those eventually lands on libpthread's live
        // cleanup record. With the wait_for fix in place this is never hit;
        // with ISH_WAITFLAG_LEAK=1 it fires constantly. It only counts -- the
        // store still happens, so the A/B's two arms differ in exactly one
        // thing.
        if (!futex_wait_flag_is_live(waiting_interrupt_flag)) {
            static _Atomic long stale;
            long n = atomic_fetch_add_explicit(&stale, 1, memory_order_relaxed);
            if (n < 3 || (n % 500) == 0)
                fprintf(stderr, "URGENT: wake_waiting_task storing through a STALE "
                        "waiting_interrupt_flag=%p (occurrence %ld)\n",
                        (void *) waiting_interrupt_flag, n + 1);
        }
        __atomic_store_n(waiting_interrupt_flag, true, __ATOMIC_RELEASE);
    }
    if (waiting_cond != NULL && waiting_lock != NULL) {
        bool have_wait_lock = false;
        bool acquired_wait_lock = false;
        bool using_existing_wait_lock = false;
        int wait_lock_status = pthread_mutex_trylock(&waiting_lock->m);
        if (wait_lock_status == 0) {
            have_wait_lock = true;
            acquired_wait_lock = true;
        } else if (wait_lock_status == EBUSY &&
                   pthread_equal(waiting_lock->owner, pthread_self())) {
            // The signal sender may already hold the mutex associated with the
            // waiter (for example pids_lock during kill/wait interactions).
            // In that case it is safe to notify directly while keeping the
            // existing lock ownership.
            have_wait_lock = true;
            using_existing_wait_lock = true;
        } else if (wait_lock_status == EBUSY) {
            // Do not block on waiting_lock while holding waiting_cond_lock.
            // The waiter clears waiting_cond/waiting_lock after
            // pthread_cond_wait() returns, while still holding waiting_lock and
            // then taking waiting_cond_lock. Holding waiting_cond_lock here and
            // then blocking on waiting_lock deadlocks with that path.
            memset(&task->waiting_cond_lock.owner, 0, sizeof(task->waiting_cond_lock.owner));
            pthread_mutex_unlock(&task->waiting_cond_lock.m);

            // Wait until the waiter either reaches pthread_cond_wait() and
            // releases waiting_lock, or finishes the wait path entirely.
            pthread_mutex_lock(&waiting_lock->m);
            have_wait_lock = true;
            acquired_wait_lock = true;

            pthread_mutex_lock(&task->waiting_cond_lock.m);
            task->waiting_cond_lock.owner = pthread_self();
            if (task->waiting_cond != waiting_cond || task->waiting_lock != waiting_lock)
                have_wait_lock = false;
        }

        if (have_wait_lock) {
            notify(waiting_cond);
            if (!using_existing_wait_lock)
                pthread_mutex_unlock(&waiting_lock->m);
        } else if (acquired_wait_lock && !using_existing_wait_lock) {
            pthread_mutex_unlock(&waiting_lock->m);
        }
    }

    memset(&task->waiting_cond_lock.owner, 0, sizeof(task->waiting_cond_lock.owner));
    pthread_mutex_unlock(&task->waiting_cond_lock.m);
    return interrupted_wait;
}

static guest_addr_t current_user_sp(struct task *task) {
    if (task->abi == GUEST_ABI_AMD64)
        return task->cpu.amd64_regs[amd64_rsp];
    if (task->abi == GUEST_ABI_ARM64)
        return task->cpu.arm64_sp;
    if (task->abi == GUEST_ABI_RISCV64)
        return task->cpu.riscv64_regs[riscv64_sp];
    return task->cpu.esp;
}

struct signalfd_state {
    sigset_t_ mask;
};

struct signalfd_siginfo_ {
    dword_t signo;
    sdword_t sig_errno;
    sdword_t code;
    dword_t pid;
    dword_t uid;
    sdword_t fd;
    dword_t tid;
    dword_t band;
    dword_t overrun;
    dword_t trapno;
    sdword_t status;
    sdword_t sig_int;
    qword_t sig_ptr;
    qword_t utime;
    qword_t stime;
    qword_t addr;
    word_t addr_lsb;
    word_t __pad2;
    sdword_t syscall;
    qword_t call_addr;
    dword_t arch;
    byte_t __pad[28];
} __attribute__((packed));

static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd siginfo layout mismatch");

static int signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

static bool signal_is_realtime(int sig) {
    return sig > SIGSYS_ && sig < NUM_SIGS;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

static bool signal_is_synchronous_trap(int sig) {
    switch (sig) {
        case SIGILL_:
        case SIGTRAP_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
        case SIGSYS_:
            return true;
        default:
            return false;
    }
}

// SIGNAL_* and this prototype live in signal.h: sys_clone_common's vfork wait
// needs to classify a pending signal too.
int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if(sig > 63)
            return SIGNAL_IGNORE;
        
        if (action->handler == SIG_IGN_)
            return SIGNAL_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_CALL_HANDLER;
    }

    switch (sig) {
        // Linux defaults SIGURG to ignore; it arrives with out-of-band TCP
        // data, so defaulting it to kill terminates innocent network users.
        // Linux's default-ignore set is exactly SIGCHLD, SIGCONT, SIGURG and
        // SIGWINCH. SIGURG is here for the reason below; SIGIO is NOT --
        // Linux terminates on it, and treating it as ignored meant a process
        // that had asked for async I/O notification and then failed to handle
        // it kept running as if nothing had happened, where every Linux would
        // have killed it. Nothing in this kernel raises SIGIO on its own; it
        // only arrives when the guest set it up with F_SETOWN/FASYNC.
        case SIGCONT_: case SIGCHLD_: case SIGURG_:
        case SIGWINCH_:
            return SIGNAL_IGNORE;

        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_STOP;

        default:
            return SIGNAL_KILL;
    }
}

// Wake a sibling blocked in poll_wait through its notify pipe, in addition to
// the SIGUSR1 poke. SIGUSR1 is shared with the TLB/quiesce shootdown poke and
// does not queue, so under load the guest-signal SIGUSR1 can be coalesced with
// a poke or land in a window where it has no effect, leaving real_poll_wait to
// run to its timeout and return 0 instead of EINTR. A byte on the notify pipe
// is not lost: it makes the host wait return so the poll loop re-checks pending.
// fd is read under the target's sighand->lock (held by our caller); poll_wait
// clears it before closing the pipe under the same lock, so it is never stale.
// The pipe is O_NONBLOCK; a full pipe already has a pending wake, so EAGAIN is
// fine to drop.
static void poll_notify_poke(int fd) {
    if (fd < 0)
        return;
    ssize_t wrote;
    do {
        wrote = write(fd, "", 1);
    } while (wrote < 0 && errno == EINTR);
}

// Discard every pending instance of `sig` from a task -- both the bitmask bit
// and any queued sigqueue entries (a signal lives in both, see
// deliver_signal_unlocked_locked / signal_take_next_locked). Caller holds
// task->sighand->lock, which protects task->pending and task->queue.
static void signal_flush_pending(struct task *task, int sig) {
    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&task->queue, sigqueue, tmp, queue) {
        if (sigqueue->info.sig == sig) {
            list_remove(&sigqueue->queue);
            free(sigqueue);
        }
    }
    sigset_del(&task->pending, sig);
}

// Linux prepare_signal() semantics: generating a continue (SIGCONT) or a stop
// (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU) signal mutually cancels the other kind that
// is still pending on the target, at generation time. Without this a rapid
// SIGSTOP-then-SIGCONT pair races: the SIGCONT lifts group->stopped and wakes
// the group-stop wait (kernel/calls.c), but the still-queued SIGSTOP is then
// processed, sets group->stopped again, and the task blocks forever in the
// group-stop wait_for_ignore_signals() with no further SIGCONT coming. This
// intermittently wedged stress-ng --schedmix, which hammers its children with
// interleaved SIGSTOP/SIGCONT. Operates on the target task (single-threaded
// process, the common case and the observed failure); a multi-threaded group
// whose stop and continue land on different threads is not fully covered here.
// Caller holds task->sighand->lock.
static void signal_prepare_stop_cont(struct task *task, int sig) {
    switch (sig) {
        case SIGCONT_:
            signal_flush_pending(task, SIGSTOP_);
            signal_flush_pending(task, SIGTSTP_);
            signal_flush_pending(task, SIGTTIN_);
            signal_flush_pending(task, SIGTTOU_);
            break;
        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            signal_flush_pending(task, SIGCONT_);
            break;
    }
}

// Poke `task` so it re-checks pending signals: send SIGUSR1, poke its poll
// notify pipe, poke the JIT if it's spinning in guest code, and wake any
// pthread_cond it's parked in. Caller holds `sighand->lock` (either `task`'s
// own sighand, or -- for a process-directed signal -- the shared sighand of
// `task`'s whole thread group) and gets it back on return; the lock is only
// dropped around wake_waiting_task, which must not be called while holding it
// (see the AB-BA comment on signalfd_wakeup_task above).
static bool signal_wake_task(struct task *task, struct sighand *sighand, int sig) {
    if (task == current)
        return wake_waiting_task(task);

    int wake_err = pthread_kill(task->thread, SIGUSR1);
    // Second, independent poke. The SIGUSR1 above is not reliable: on Darwin it
    // is intermittently swallowed in a way that leaves SIGUSR1 blocked and
    // pending in the target thread's own mask with sigusr1_handler never
    // running, after which that thread is deaf to every later SIGUSR1 for the
    // rest of its life. A target parked in a host syscall then finishes the
    // syscall on its own schedule and never reaches the checkpoint where
    // receive_signals() would act -- which is how a `sleep 30` could ignore a
    // pending SIGKILL and exit normally 30 seconds later. SIGUSR2 is delivered
    // fine at the moment SIGUSR1 is swallowed (measured), and all it has to do
    // is EINTR the host call. See sigusr2_handler in util/sync.c.
    //
    // A DEBUGGER WILL HALT ON THIS unless told not to. Anyone who had already
    // silenced SIGUSR1 suddenly finds the app stopping on every wake instead,
    // which looks like a new hang rather than a new signal. ish-gdb.gdb and
    // ish-lldb.lldbinit in the repo root silence both.
    pthread_kill(task->thread, SIGUSR2);
    // Robustly wake a sibling parked in poll/select/epoll: the SIGUSR1 above
    // can be lost in TLB-poke noise, but the notify-pipe write cannot.
    poll_notify_poke(task->poll_notify_fd);
    if ((sig == SIGKILL_ || sig == SIGABRT_) &&
            (amd64_trace_is_lineage_tgid(task->tgid) ||
             (current != NULL && amd64_trace_is_lineage_tgid(current->tgid)))) {
        printk("tracked signal wake: sender=%d sender_tgid=%d sig=%d target=%d target_tgid=%d abi=%d wake_err=%d exiting=%d io_block=%d pending=%#llx blocked=%#llx\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->tgid : -1,
               sig, task->pid, task->tgid, task->abi, wake_err,
               task->exiting, task->io_block,
               (unsigned long long) task->pending,
               (unsigned long long) task->blocked);
    }
    if (task->cpu.poked_ptr)
        cpu_poke(&task->cpu);

    // Wake pthread condition waiters without keeping sighand->lock held.
    // If the waiter is between publishing waiting_cond and entering
    // pthread_cond_wait(), retry briefly for its lock handoff so the wake
    // is not lost. This avoids global timed polling in wait_for().
    //
    // sighand->lock is genuinely, fully unlocked for the duration of this
    // release -- deliver_signal_unlocked_locked/deliver_signal_to_group_locked
    // can now call this concurrently for the same sighand (a burst of
    // simultaneous exits each raising SIGCHLD to the same parent, each
    // wanting its own wake attempt -- see the "already_pending" comments on
    // those two functions), and two such calls both reaching this exact
    // unlock/relock dance at once would race the same non-recursive
    // pthread_mutex_t: undefined behavior, and able to corrupt sighand->lock
    // for good, wedging every future signal delivery to this whole group.
    // wake_lock (acquired only while sighand->lock is NOT held, so it can
    // never nest with it and reintroduce an ABBA hazard) serializes this
    // dance so at most one thread is ever mid-wake for a given sighand.
    memset(&sighand->lock.owner, 0, sizeof(sighand->lock.owner));
    pthread_mutex_unlock(&sighand->lock.m);
    lock(&sighand->wake_lock, 0);
    bool interrupted_wait = wake_waiting_task(task);
    unlock(&sighand->wake_lock);
    pthread_mutex_lock(&sighand->lock.m);
    sighand->lock.owner = pthread_self();
    return interrupted_wait;
}

static void signal_note_interrupted(struct task *task, struct sighand *sighand, int sig, bool interrupted_wait) {
    if (!interrupted_wait)
        return;
    // A job-control stop is not an interruption. Linux parks the task inside
    // the wait and resumes it on SIGCONT, so the syscall never returns EINTR
    // to the guest -- and because no handler runs, that holds even for the
    // interfaces SA_RESTART cannot rescue (poll, select, epoll_wait). Only a
    // handler actually running can turn a wait into a guest-visible EINTR.
    int action = signal_action(sighand, sig);
    bool stops = action == SIGNAL_STOP;
    bool restart = stops || (action == SIGNAL_CALL_HANDLER &&
        !!(sighand->action[sig].flags & SA_RESTART_));
    __atomic_store_n(&task->restart_interrupted_syscall, restart, __ATOMIC_RELEASE);
    __atomic_store_n(&task->restart_interrupted_syscall_nohand, stops, __ATOMIC_RELEASE);
    __atomic_store_n(&task->wait_interrupted, true, __ATOMIC_RELEASE);
}

static void deliver_signal_unlocked_locked(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    if (task->exiting)
        return;

    // Standard (non-realtime) signals don't queue a second instance while one
    // is already pending -- that much matches Linux. But sigset_has(pending)
    // here and the clearing of that bit in receive_signals() are not atomic
    // with each other: a waiter can dequeue/clear the first occurrence and
    // re-enter its wait (e.g. sigsuspend()/wait4() looping to reap a burst of
    // exiting children, each raising SIGCHLD) before this second occurrence
    // arrives. Returning here unconditionally used to skip the wake too, so
    // that second occurrence was silently dropped with nobody left to notice
    // it -- a permanent hang, not just a redundant signal. Only skip the
    // requeue; always still attempt the wake below (redundant wakes of an
    // already-running thread are harmless).
    bool already_pending = !signal_is_realtime(sig) && sigset_has(task->pending, sig);
    if (!already_pending) {
        sigset_add(&task->pending, sig);
        struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
        sigqueue->info = info;
        sigqueue->info.sig = sig;
        list_add_tail(&task->queue, &sigqueue->queue);
    }
    // signalfd_wakeup_task is a best-effort, idempotent poke (see its own
    // comment: a signalfd's readiness is re-checked on the next scan/timeout
    // regardless), so -- like signal_wake_task below -- it must run even when
    // this occurrence was already pending, for the same reason.
    signalfd_wakeup_task(task, sig);

    // Synchronous fault signals must be delivered even when the task has them
    // masked. libc abort paths rely on this by blocking signals before
    // executing a crash instruction like `hlt`.
    if (sigset_has(task_wake_blocked(task) & ~task->waiting, sig) &&
            signal_is_blockable(sig) && !signal_is_synchronous_trap(sig))
        return;

    bool interrupted_wait = signal_wake_task(task, sighand, sig);
    signal_note_interrupted(task, sighand, sig, interrupted_wait);
}

// Deliver a process-directed signal into the thread group's shared queue
// (Linux's shared_pending): any sibling thread with `sig` unblocked -- not
// just whichever task object the sender happened to address -- can observe
// and dequeue it via receive_signals/signalfd/sigwaitinfo. Contrast
// deliver_signal_unlocked_locked, which targets one specific task's own
// per-thread queue. `members`/`count` is a pre-collected, ref-counted
// snapshot of the group's live threads (see send_signal_to_group): walking
// tgroup->threads needs pids_lock, and this runs under sighand->lock, so the
// snapshot has to happen first (pids_lock -> sighand->lock, never the
// reverse). Caller holds `sighand->lock`.
static void deliver_signal_to_group_locked(struct sighand *sighand, int sig, struct siginfo_ info,
        struct task **members, size_t count) {
    // Unlike send_signal_with_sighand's single-task path, this used to queue
    // and wake unconditionally, with no check of signal_action() at all. For
    // a group-default-ignored signal with no handler installed -- SIGCHLD is
    // the common case, sent here by every child exit -- that let it sit in
    // sighand->pending regardless of whether anyone was actually going to
    // consume it. is_signal_pending()/wait_interrupted_by_signal() (used by
    // wait_for's EINTR check for wait4/futex/poll) then see it as "pending"
    // and report EINTR, even to a wait4() waiting on a *different*, still-
    // running child that simply hasn't exited yet. Real Linux drops a
    // default-ignored signal instead of queuing it (sig_ignored()) unless a
    // consumer is synchronously waiting for it (blocked via sigprocmask for
    // signalfd, or in sigtimedwait/rt_sigsuspend's wait set) -- mirror that
    // here across the whole group before touching sighand->pending at all.
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE;
    if (ignored) {
        bool synchronously_consumed = false;
        for (size_t i = 0; i < count; i++) {
            if (sigset_has(members[i]->blocked | members[i]->waiting, sig)) {
                synchronously_consumed = true;
                break;
            }
        }
        if (!synchronously_consumed)
            return;
    }

    // See the matching comment in deliver_signal_unlocked_locked: skipping the
    // requeue for an already-pending standard signal is correct (Linux
    // doesn't queue multiple instances either), but skipping the wake too --
    // as this used to do via an unconditional early return -- can strand
    // every member of the group. This is the SIGCHLD path for a burst of
    // near-simultaneous child exits (send_signal_to_group), which is exactly
    // where the race is easy to hit: one child's SIGCHLD sets the pending bit
    // and wakes the parent, the parent's sigsuspend()/wait4() loop reaps that
    // child and goes back to sleep, and a second child exits and delivers
    // SIGCHLD while the first occurrence's pending bit hasn't been cleared by
    // receive_signals() yet -- that second, distinct occurrence must still
    // wake the parent even though it doesn't get its own queue entry.
    bool already_pending = !signal_is_realtime(sig) && sigset_has(sighand->pending, sig);
    if (!already_pending) {
        sigset_add(&sighand->pending, sig);
        struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
        sigqueue->info = info;
        sigqueue->info.sig = sig;
        list_add_tail(&sighand->queue, &sigqueue->queue);
    }

    for (size_t i = 0; i < count; i++) {
        struct task *task = members[i];
        signalfd_wakeup_task(task, sig);
        bool interrupted_wait = signal_wake_task(task, sighand, sig);
        signal_note_interrupted(task, sighand, sig, interrupted_wait);
    }
}

void send_signal_to_group(struct tgroup *group, int sig, struct siginfo_ info) {
    if (sig == 0)
        return;

    struct task *stack_members[32];
    struct task **members = stack_members;
    size_t member_cap = sizeof(stack_members) / sizeof(stack_members[0]);
    size_t member_count = 0;
    struct sighand *sighand = NULL;
    struct task *task;

    complex_lockt(&pids_lock, 0);
    for (;;) {
        size_t needed = 0;
        list_for_each_entry(&group->threads, task, group_links)
            needed++;
        if (needed <= member_cap)
            break;
        unlock(&pids_lock);
        if (members != stack_members)
            free(members);
        members = malloc(sizeof(*members) * needed);
        if (members == NULL)
            return;
        member_cap = needed;
        complex_lockt(&pids_lock, 0);
    }

    list_for_each_entry(&group->threads, task, group_links) {
        if (task->zombie || task->exiting || task->sighand == NULL)
            continue;
        if (sighand == NULL) {
            sighand = task->sighand;
            sighand_retain(sighand);
        }
        task_ref_cnt_mod(task, 1);
        members[member_count++] = task;
    }
    unlock(&pids_lock);

    if (sighand != NULL) {
        lock(&sighand->lock, 0);
        deliver_signal_to_group_locked(sighand, sig, info, members, member_count);
        unlock(&sighand->lock);
        sighand_release(sighand);
    }

    for (size_t i = 0; i < member_count; i++)
        task_ref_cnt_mod(members[i], -1);
    if (members != stack_members)
        free(members);
}

void deliver_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    lock(&sighand->lock, 0);
    // deliver_signal is the forced path (faults, not kill()). Match Linux
    // force_sig_info_to_task semantics for synchronous traps: if the signal
    // is ignored, or blocked -- with ANY disposition, including a custom
    // handler ("we do not want to have a signal handler that was blocked be
    // invoked when user space had explicitly blocked it", kernel/signal.c)
    // -- reset to SIG_DFL and unblock it so the task dies. Without this the
    // faulting instruction re-executes forever: handle_interrupt only calls
    // receive_signals for unblocked pending signals, and receive_signals
    // skips blocked ones anyway. The blocked+custom-handler case was the
    // observable wedge: a guest that blocks SIGSEGV around a region that
    // then faults (e.g. node/V8 crash paths under npm) spun at 100% CPU,
    // unkillable from inside the guest, instead of dying like on Linux.
    // User-sent signals go through send_signal and are not affected.
    if (signal_is_synchronous_trap(sig) && signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_ || sigset_has(task->blocked, sig)) {
            *action = (struct sigaction_) {.handler = SIG_DFL_};
            sigset_del(&task->blocked, sig);
        }
    }
    deliver_signal_unlocked_locked(task, sighand, sig, info);
    unlock(&sighand->lock);
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    deliver_signal_with_sighand(task, sighand, sig, info);
}

static bool signal_list_still_has_locked(struct list *queue, int sig) {
    struct sigqueue *sigqueue;
    list_for_each_entry(queue, sigqueue, queue) {
        if (sigqueue->info.sig == sig)
            return true;
    }
    return false;
}

static bool signal_still_pending_locked(struct task *task, int sig) {
    return signal_list_still_has_locked(&task->queue, sig);
}

// Scans both `task`'s own (thread-directed) queue and, if present, its
// sighand's shared (process-directed) queue -- a signalfd/sigwaitinfo/
// receive_signals caller must see process-directed signals (e.g. SIGCHLD to a
// possibly-multithreaded parent, see send_signal_to_group) regardless of
// which sibling thread they were delivered through.
static bool signal_take_next_locked(struct task *task, sigset_t_ mask, struct siginfo_ *info_out) {
    // POSIX/signal(7): when several signals are pending, the lowest-numbered is
    // delivered first; multiple instances of the same (real-time) signal are
    // delivered FIFO. Each queue is in FIFO insertion order, so scan for the
    // lowest signal number and, using a strict <, keep the first (oldest)
    // entry of that number -- ties between the two queues favor whichever is
    // scanned first (the thread's own queue). Matters for
    // sigtimedwait/sigwaitinfo/signalfd.
    struct sighand *sighand = task->sighand;
    struct sigqueue *sigqueue;
    struct sigqueue *best = NULL;
    bool best_is_group = false;
    list_for_each_entry(&task->queue, sigqueue, queue) {
        if (!sigset_has(mask, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    if (sighand != NULL) {
        list_for_each_entry(&sighand->queue, sigqueue, queue) {
            if (!sigset_has(mask, sigqueue->info.sig))
                continue;
            if (best == NULL || sigqueue->info.sig < best->info.sig) {
                best = sigqueue;
                best_is_group = true;
            }
        }
    }
    if (best == NULL)
        return false;
    *info_out = best->info;
    int sig = best->info.sig;
    list_remove(&best->queue);
    if (best_is_group) {
        if (!signal_list_still_has_locked(&sighand->queue, sig))
            sigset_del(&sighand->pending, sig);
    } else if (!signal_still_pending_locked(task, sig)) {
        sigset_del(&task->pending, sig);
    }
    free(best);
    return true;
}

static void siginfo_to_i386_user(struct i386_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->child.pid = info->child.pid;
            out->child.uid = info->child.uid;
            out->child.status = info->child.status;
            out->child.utime = info->child.utime;
            out->child.stime = info->child.stime;
            break;
        case SIGILL_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
            out->fault.addr = (addr_t) info->fault.addr;
            break;
        case SIGTRAP_:
            if (info->code == SIGTRAP_ || info->code == (SIGTRAP_ | 0x80)) {
                // Ptrace syscall-stop: Linux reports only si_signo/si_code.
            } else if ((info->code >> 8) != 0) {
                // Ptrace event-stop: Linux populates si_pid/si_uid, not si_addr.
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else if (info->code <= 0 || info->code == SI_KERNEL_) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else {
                out->fault.addr = (addr_t) info->fault.addr;
            }
            break;
        case SIGSYS_:
            out->sigsys.addr = (addr_t) info->sigsys.addr;
            out->sigsys.syscall = info->sigsys.syscall;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                memcpy(&out->timer.value, &info->timer.value, sizeof(out->timer.value));
                out->timer._private = info->timer._private;
            } else {
                if (info->code == SI_QUEUE_) {
                    out->rt.pid = info->rt.pid;
                    out->rt.uid = info->rt.uid;
                    memcpy(&out->rt.value, &info->rt.value, sizeof(out->rt.value));
                } else {
                    out->kill.pid = info->kill.pid;
                    out->kill.uid = info->kill.uid;
                }
            }
            break;
    }
}

static void siginfo_to_amd64_user(struct amd64_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->child.pid = info->child.pid;
            out->child.uid = info->child.uid;
            out->child.status = info->child.status;
            out->child.utime = info->child.utime;
            out->child.stime = info->child.stime;
            break;
        case SIGILL_:
        case SIGBUS_:
        case SIGFPE_:
        case SIGSEGV_:
            out->fault.addr = info->fault.addr;
            break;
        case SIGTRAP_:
            if (info->code == SIGTRAP_ || info->code == (SIGTRAP_ | 0x80)) {
                // Ptrace syscall-stop: Linux reports only si_signo/si_code.
            } else if ((info->code >> 8) != 0) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else if (info->code <= 0 || info->code == SI_KERNEL_) {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            } else {
                out->fault.addr = info->fault.addr;
            }
            break;
        case SIGSYS_:
            out->sigsys.call_addr = info->sigsys.addr;
            out->sigsys.syscall = info->sigsys.syscall;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                out->timer.value = info->timer.value;
                out->timer._private = info->timer._private;
            } else {
                if (info->code == SI_QUEUE_) {
                    out->rt.pid = info->rt.pid;
                    out->rt.uid = info->rt.uid;
                    out->rt.value = info->rt.value;
                } else {
                    out->kill.pid = info->kill.pid;
                    out->kill.uid = info->kill.uid;
                }
            }
            break;
    }
}

static int siginfo_from_user(struct task *task, guest_addr_t user_addr, struct siginfo_ *info) {
    memset(info, 0, sizeof(*info));
    // Both 64-bit ABIs share the generic siginfo layout (arm64 was
    // falling into the i386 branch and reading garbage fields).
    if (guest_abi_is_64bit(task->abi)) {
        struct amd64_siginfo_ user_info;
        if (user_get(user_addr, user_info))
            return _EFAULT;
        info->sig = user_info.sig;
        info->sig_errno = user_info.sig_errno;
        info->code = user_info.code;
        if (info->code == SI_TIMER_) {
            info->timer.timer = user_info.timer.timer;
            info->timer.overrun = user_info.timer.overrun;
            info->timer.value = user_info.timer.value;
            info->timer._private = user_info.timer._private;
        } else if (info->code == SI_QUEUE_) {
            info->rt.pid = user_info.rt.pid;
            info->rt.uid = user_info.rt.uid;
            info->rt.value = user_info.rt.value;
        } else {
            info->kill.pid = user_info.kill.pid;
            info->kill.uid = user_info.kill.uid;
        }
    } else {
        struct i386_siginfo_ user_info;
        if (user_get(user_addr, user_info))
            return _EFAULT;
        info->sig = user_info.sig;
        info->sig_errno = user_info.sig_errno;
        info->code = user_info.code;
        if (info->code == SI_TIMER_) {
            info->timer.timer = user_info.timer.timer;
            info->timer.overrun = user_info.timer.overrun;
            memcpy(&info->timer.value, &user_info.timer.value, sizeof(info->timer.value));
            info->timer._private = user_info.timer._private;
        } else if (info->code == SI_QUEUE_) {
            info->rt.pid = user_info.rt.pid;
            info->rt.uid = user_info.rt.uid;
            memcpy(&info->rt.value, &user_info.rt.value, sizeof(info->rt.value));
        } else {
            info->kill.pid = user_info.kill.pid;
            info->kill.uid = user_info.kill.uid;
        }
    }
    return 0;
}

int siginfo_to_user(struct task *task, guest_addr_t user_addr, const struct siginfo_ *info) {
    if (guest_abi_is_64bit(task->abi)) {
        struct amd64_siginfo_ user_info;
        siginfo_to_amd64_user(&user_info, info);
        if (user_put(user_addr, user_info))
            return _EFAULT;
    } else {
        struct i386_siginfo_ user_info;
        siginfo_to_i386_user(&user_info, info);
        if (user_put(user_addr, user_info))
            return _EFAULT;
    }
    return 0;
}

// siginfo is a UNION: only the arm matching the signal's layout holds anything
// real, and reading the others back out hands the caller whatever bytes that
// arm happened to store. Every member was copied for every signal, so a
// sigqueue'd SIGUSR1 arrived with ssi_status, ssi_addr and ssi_tid carrying
// pieces of its own sigval, and ssi_fd was the constant -1 -- a value Linux
// only ever produces for a real SIGPOLL fd, and never a negative one.
//
// Linux's signalfd_copyinfo switches on siginfo_layout(sig, si_code) and
// leaves everything else at the memset zero. Same here.
static void signalfd_info_from_siginfo(struct signalfd_siginfo_ *out, struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->signo = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;

    // A negative code below SI_TKILL_ is one of the kernel's own queued
    // sources (SI_ASYNCIO, SI_MESGQ, ...); Linux gives them the same RT layout
    // as SI_QUEUE.
    bool rt_layout = info->code == SI_QUEUE_ || info->code <= SI_TKILL_;

    if (info->code == SI_TIMER_) {
        out->tid = info->timer.timer;
        out->overrun = info->timer.overrun;
        out->sig_int = info->timer.value.sv_int;
        out->sig_ptr = info->timer.value.sv_ptr;
    } else if (info->sig == SIGCHLD_ && info->code > 0) {
        out->pid = info->child.pid;
        out->uid = info->child.uid;
        out->status = info->child.status;
        out->utime = info->child.utime;
        out->stime = info->child.stime;
    } else if (info->sig == SIGSYS_ && info->code > 0) {
        out->call_addr = info->sigsys.addr;
        out->syscall = info->sigsys.syscall;
    } else if (info->code > 0 &&
               (info->sig == SIGILL_ || info->sig == SIGFPE_ ||
                info->sig == SIGSEGV_ || info->sig == SIGBUS_ ||
                info->sig == SIGTRAP_)) {
        // A fault code (SEGV_MAPERR and friends) is the only thing that makes
        // ssi_addr meaningful; a SIGSEGV someone merely kill()ed you with has
        // no address.
        out->addr = info->fault.addr;
    } else if (rt_layout) {
        out->pid = info->rt.pid;
        out->uid = info->rt.uid;
        out->sig_int = info->rt.value.sv_int;
        out->sig_ptr = info->rt.value.sv_ptr;
    } else {
        // SI_USER, SI_KERNEL and the rest: sender identity only.
        out->pid = info->kill.pid;
        out->uid = info->kill.uid;
    }
}

static struct fdtable *signalfd_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    // trylock, not lock: this is called from the signal-delivery path while
    // the sender holds a reference on `task` (see do_kill's pid_get_task_ref)
    // and possibly sighand->lock/pids_lock. A task mid-exit holds its own
    // general_lock for the entire do_exit() nanosleep-retry loop, which is
    // itself waiting for the sender's held reference to be dropped -- a
    // blocking lock() here deadlocks the two permanently (observed: a
    // SIGKILL-proof hang, kill()'s caller stuck here while its target spun
    // in do_exit forever). Same reasoning as the files->lock trylock below:
    // if general_lock isn't free, skip the signalfd wakeup optimization --
    // the signal is already recorded in task->pending either way.
    if (trylock(&task->general_lock) != 0)
        return NULL;
    if (!task->exiting && task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static void signalfd_wakeup_task(struct task *task, int sig) {
    if (task == NULL)
        return;

    struct fdtable *files = signalfd_task_files_retain(task);
    if (files == NULL)
        return;

    // Use trylock to avoid a deadlock: this function is called while
    // sighand->lock (and often pids_lock) is held.  f_close holds
    // files->lock during fdtable_close and may transitively need sighand or
    // pids.  If the files table is currently locked, skip the wakeup — the
    // signal is already pending in task->pending, so the task will find it
    // when it next checks for signals.
    if (trylock(&files->lock) != 0) {
        fdtable_release(files);
        return;
    }
    for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
        struct fd *fd = fdtable_get(files, fd_no);
        if (fd == NULL || fd->ops != &signalfd_ops || fd->data == NULL)
            continue;
        struct signalfd_state *state = fd->data;
        if (!sigset_has(state->mask, sig))
            continue;
        notify(&fd->cond);
        // Not poll_wakeup(): signalfd_poll (this fd's fd_ops.poll) takes
        // current->sighand->lock, and poll_wait holds poll->lock across its
        // call to fd->ops->poll() (poll->lock -> sighand->lock order). We get
        // here with sighand->lock already held, so a blocking poll_wakeup()
        // (fd->poll_lock -> poll->lock) would be the reverse order -- an
        // AB-BA deadlock against a thread mid-epoll_wait on this same
        // signalfd. See the comment on poll_wakeup_trylock() in fs/poll.c.
        poll_wakeup_trylock(fd, POLL_READ);
    }
    unlock(&files->lock);
    fdtable_release(files);
}

// Is anything this signalfd watches already queued? Both queues: a
// process-directed signal (e.g. SIGCHLD via send_signal_to_group) lives in the
// shared one, not this thread's own, and a signalfd on any sibling thread must
// still see it. Caller must NOT hold sighand->lock.
static bool signalfd_has_pending(sigset_t_ mask) {
    bool found = false;
    struct sigqueue *sigqueue;
    lock(&current->sighand->lock, 0);
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(mask, sigqueue->info.sig)) {
            found = true;
            goto out;
        }
    }
    list_for_each_entry(&current->sighand->queue, sigqueue, queue) {
        if (sigset_has(mask, sigqueue->info.sig)) {
            found = true;
            goto out;
        }
    }
out:
    unlock(&current->sighand->lock);
    return found;
}

static int signalfd_poll(struct fd *fd) {
    struct signalfd_state *state = fd->data;
    if (state == NULL)
        return POLL_ERR;
    return signalfd_has_pending(state->mask) ? POLL_READ : 0;
}

static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    struct signalfd_state *state = fd->data;
    if (state == NULL)
        return _EINVAL;
    if (bufsize < sizeof(struct signalfd_siginfo_))
        return _EINVAL;

    size_t max_infos = bufsize / sizeof(struct signalfd_siginfo_);
    size_t count = 0;
    lock(&fd->lock, 0);
    while (count == 0) {
        lock(&current->sighand->lock, 0);
        while (count < max_infos) {
            struct siginfo_ info;
            if (!signal_take_next_locked(current, state->mask, &info))
                break;
            signalfd_info_from_siginfo(&((struct signalfd_siginfo_ *) buf)[count], &info);
            count++;
        }
        unlock(&current->sighand->lock);
        if (count != 0)
            break;
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err != 0) {
            // A signal this fd watches is BLOCKED in the caller -- that is what
            // makes signalfd work at all -- so its arrival is the event being
            // waited for, not an interruption. wait_for reports ANY pending
            // signal as _EINTR without asking which, so a read that was
            // already blocking when the signal landed came back EINTR and the
            // record stayed queued: the ordinary signal-driven event loop
            // (block, signalfd, read) failed exactly when it was doing its job,
            // and only a read issued after the signal had already arrived
            // worked. Re-check before believing the interruption.
            if (!signalfd_has_pending(state->mask)) {
                unlock(&fd->lock);
                return err;
            }
        }
    }
    unlock(&fd->lock);
    return count * sizeof(struct signalfd_siginfo_);
}

static int signalfd_close(struct fd *fd) {
    free(fd->data);
    fd->data = NULL;
    return 0;
}

static struct fd_ops signalfd_ops = {
    .anon_inode_class = "signalfd",
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};

int_t sys_signalfd4(int_t fd_no, addr_t mask_addr, dword_t sigsetsize, int_t flags) {
    return sys_signalfd4_guest(fd_no, mask_addr, sigsetsize, flags);
}

int_t sys_signalfd4_guest(int_t fd_no, guest_addr_t mask_addr, dword_t sigsetsize, int_t flags) {
    if (sigsetsize != sizeof(sigset_t_))
        return _EINVAL;
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_))
        return _EINVAL;

    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    mask &= ~UNBLOCKABLE_MASK;

    if (fd_no != -1) {
        struct fd *fd = f_get(fd_no);
        if (fd == NULL || fd->ops != &signalfd_ops || fd->data == NULL)
            return _EINVAL;
        ((struct signalfd_state *) fd->data)->mask = mask;
        return fd_no;
    }

    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct signalfd_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    *state = (struct signalfd_state) {.mask = mask};
    fd->data = state;
    return f_install(fd, flags);
}

int_t sys_signalfd(int_t fd, addr_t mask_addr, dword_t sigsetsize) {
    return sys_signalfd4_guest(fd, mask_addr, sigsetsize, 0);
}

int_t sys_signalfd_guest(int_t fd, guest_addr_t mask_addr, dword_t sigsetsize) {
    return sys_signalfd4_guest(fd, mask_addr, sigsetsize, 0);
}

// A POSIX timer never has more than one signal outstanding. When it expires
// again while its last signal is still queued, Linux does not queue a second
// one -- it counts the missed expiration on the queued siginfo's si_overrun,
// which is the whole reason that field exists: a periodic timer whose signal
// is blocked, or whose handler is slow, tells the program how many periods it
// missed rather than burying it in a signal storm.
//
// AOK queued one signal per expiration. A 5ms timer left blocked for a second
// queued two hundred, and si_overrun was hardcoded 0, so a program could
// neither find out how far behind it was nor survive catching up.
//
// Returns the new overrun count if an entry for this timer was found and
// counted, or -1 if there was none and the caller should queue a signal.
int signal_timer_count_overrun(struct task *task, int sig, int timer_id) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return -1;
    int overrun = -1;
    lock(&sighand->lock, 0);
    struct sigqueue *sigqueue;
    // Thread-directed (send_signal) first, then the shared process queue:
    // a timer's signal goes to one or the other depending on how it was set
    // up, and either way there is at most one.
    list_for_each_entry(&task->queue, sigqueue, queue) {
        if (sigqueue->info.sig == sig && sigqueue->info.code == SI_TIMER_ &&
                sigqueue->info.timer.timer == timer_id) {
            overrun = ++sigqueue->info.timer.overrun;
            goto out;
        }
    }
    list_for_each_entry(&sighand->queue, sigqueue, queue) {
        if (sigqueue->info.sig == sig && sigqueue->info.code == SI_TIMER_ &&
                sigqueue->info.timer.timer == timer_id) {
            overrun = ++sigqueue->info.timer.overrun;
            goto out;
        }
    }
out:
    unlock(&sighand->lock);
    return overrun;
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    send_signal_with_sighand(task, sighand, sig, info);
}

static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;
    lock(&sighand->lock, 0);
    // Mutually cancel a pending stop/continue before queueing this one, matching
    // Linux prepare_signal(). Done unconditionally (before the ignored check) so
    // it still runs for a default-disposition SIGCONT, which skips the deliver
    // path below but must still flush any queued stop signal.
    signal_prepare_stop_cont(task, sig);
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE;
    bool synchronously_consumed = sigset_has(task->blocked | task->waiting, sig);
    if (should_trace_signal_task(task)) {
        printk("tracked signal send: target=%d tgid=%d comm=%s sig=%d ignored=%d sync=%d blocked=%#x waiting=%#x pending=%#x sender=%d/%s\n",
               task->pid, task->tgid, task->comm, sig, ignored, synchronously_consumed,
               task->blocked, task->waiting, task->pending,
               current != NULL ? current->pid : 0, current != NULL ? current->comm : "?");
    }
    if ((!ignored || synchronously_consumed) && (task->pid <= MAX_PID)) {
        deliver_signal_unlocked_locked(task, sighand, sig, info);
    }
    unlock(&sighand->lock);

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock, 0);
        // A SIGCONT that actually resumes a stopped group is a reportable
        // "continued" event for a WCONTINUED waiter (man wait). SIGKILL also
        // clears the stop but is not a continue. The parent is woken from the
        // resumed task's own context (the group-stop loop), never from here, to
        // avoid notifying across the signal-sender's locks.
        if (sig == SIGCONT_ && task->group->stopped)
            task->group->continued = true;
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

// Both predicates consume both flags: a syscall asks exactly one of them, and
// leaving the other set would leak this interruption's answer into the next
// syscall's decision.
static bool restart_flags_take(bool nohand_only) {
    bool restart = __atomic_exchange_n(&current->restart_interrupted_syscall, false, __ATOMIC_ACQ_REL);
    bool nohand = __atomic_exchange_n(&current->restart_interrupted_syscall_nohand, false, __ATOMIC_ACQ_REL);
    return nohand_only ? nohand : restart;
}

// ERESTARTNOHAND: restart only if the interrupting signal ran no handler. This
// is what poll/select/epoll_wait get -- SA_RESTART never rescues them, but a
// job-control stop still must not surface as EINTR.
bool signal_should_restart_syscall_nohand(void) {
    if (current == NULL)
        return false;

    if (restart_flags_take(true))
        return true;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    struct sigqueue *sigqueue;
    struct sigqueue *best = NULL;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(current->blocked, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    list_for_each_entry(&sighand->queue, sigqueue, queue) {
        if (sigset_has(current->blocked, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    bool stops = best != NULL && signal_action(sighand, best->info.sig) == SIGNAL_STOP;
    unlock(&sighand->lock);
    return stops;
}

bool signal_should_restart_syscall(void) {
    if (current == NULL)
        return false;

    if (restart_flags_take(false))
        return true;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    // The signal that actually interrupted the syscall (and is about to be
    // delivered) can be sitting on either queue -- current->queue for a
    // thread-targeted signal (deliver_signal_unlocked_locked, e.g. SIGWINCH
    // via send_signal) or sighand->queue for a process/group-targeted one
    // (deliver_signal_to_group_locked, e.g. SIGCHLD via send_signal_to_group).
    // This used to only scan current->queue, so any group-directed signal
    // fell through to the "no restart" default even when its handler had
    // SA_RESTART_ set -- turning what should be a transparent kernel-level
    // restart into a real EINTR surfacing all the way into the guest.
    // Mirrors signal_take_next_locked's selection (both queues, lowest
    // signal number wins, ties favor the thread's own queue since it's
    // scanned first).
    struct sigqueue *sigqueue;
    struct sigqueue *best = NULL;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(current->blocked, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    list_for_each_entry(&sighand->queue, sigqueue, queue) {
        if (sigset_has(current->blocked, sigqueue->info.sig))
            continue;
        if (best == NULL || sigqueue->info.sig < best->info.sig)
            best = sigqueue;
    }
    if (best == NULL) {
        unlock(&sighand->lock);
        return false;
    }
    int sig = best->info.sig;
    int action = signal_action(sighand, sig);
    if (action != SIGNAL_CALL_HANDLER) {
        // A stop resumes the syscall transparently; anything else with no
        // handler either kills the task or should not have woken it.
        bool stops = action == SIGNAL_STOP;
        unlock(&sighand->lock);
        return stops;
    }
    bool restart = !!(sighand->action[sig].flags & SA_RESTART_);
    unlock(&sighand->lock);
    return restart;
}

// Whether a signal would go nowhere if sent to us right now. The terminal
// job-control checks need to know this WITHOUT sending anything: Linux treats
// an ignored or blocked SIGTTOU as permission to proceed, and only turns an
// ignored SIGTTIN into EIO.
//
// This replaced a try_self_signal() that decided and delivered in one step,
// and delivered only to the calling task. Linux signals the whole process
// group (kill_pgrp), so a background job stops entirely rather than losing one
// thread -- the caller now does that, once it is holding no locks.
bool signal_is_ignored_or_blocked(int sig) {
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE ||
        sigset_has(current->blocked, sig);
    unlock(&sighand->lock);
    return ignored;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    struct group_signal_target {
        struct task *task;
        struct sighand *sighand;
    };
    struct group_signal_target stack_targets[32];
    struct group_signal_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

    complex_lockt(&pids_lock, 0);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }

    size_t needed = 0;
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup)
        needed++;
    if (needed > target_cap) {
        unlock(&pids_lock);
        targets = malloc(sizeof(*targets) * needed);
        if (targets == NULL)
            return _ENOMEM;
        target_cap = needed;

        complex_lockt(&pids_lock, 0);
        pid = pid_get(pgid);
        if (pid == NULL) {
            unlock(&pids_lock);
            free(targets);
            return _ESRCH;
        }
    }

    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        struct task *task = tgroup->leader;
        if (task == NULL || task->zombie || task->exiting || task->sighand == NULL)
            continue;
        task_ref_cnt_mod(task, 1);
        sighand_retain(task->sighand);
        targets[target_count++] = (struct group_signal_target) {
            .task = task,
            .sighand = task->sighand,
        };
    }
    unlock(&pids_lock);

    for (size_t i = 0; i < target_count; i++) {
        send_signal_with_sighand(targets[i].task, targets[i].sighand, sig, info);
        sighand_release(targets[i].sighand);
        task_ref_cnt_mod(targets[i].task, -1);
    }
    if (targets != stack_targets)
        free(targets);
    return 0;
}

static guest_addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

static guest_addr_t signal_restorer(const struct sigaction_ *action, bool rt) {
    if (current->abi == GUEST_ABI_AMD64)
        return action->restorer;
    return sigreturn_trampoline(rt ? "__kernel_rt_sigreturn" : "__kernel_sigreturn");
}

static bool signal_should_capture_trap_state(int sig) {
    return signal_is_synchronous_trap(sig);
}

static qword_t signal_trap_error(struct cpu_state *cpu) {
    switch (cpu->trapno) {
        case INT_PF: {
            qword_t err = 0x4; // user-mode fault
            if (cpu->segfault_was_write)
                err |= 0x2;
            mem_read_lock_quiesce_aware(current->mem);
            if (mem_segv_reason(current->mem, cpu->segfault_addr) == SEGV_ACCERR_)
                err |= 0x1;
            mem_read_unlock_quiesce_aware(current->mem);
            return err;
        }
        default:
            return 0;
    }
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu, int sig) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = signal_should_capture_trap_state(sig) ? cpu->trapno : 0;
    sc->err = signal_should_capture_trap_state(sig) ? (dword_t) signal_trap_error(cpu) : 0;
    if (sc->trapno == INT_PF)
        sc->cr2 = cpu->segfault_addr;
    else
        sc->cr2 = 0;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = (addr_t) signal_restorer(&current->sighand->action[info->sig], false);
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu, info->sig);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = (addr_t) signal_restorer(&current->sighand->action[info->sig], true);
    frame->sig = info->sig;
    siginfo_to_i386_user(&frame->info, info);
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_i386_user(current, &frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu, info->sig);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void setup_amd64_mcontext(struct amd64_mcontext_ *mcontext, struct cpu_state *cpu) {
    memset(mcontext, 0, sizeof(*mcontext));
    mcontext->gregs[AMD64_GREG_R8] = cpu->amd64_regs[amd64_r8];
    mcontext->gregs[AMD64_GREG_R9] = cpu->amd64_regs[amd64_r9];
    mcontext->gregs[AMD64_GREG_R10] = cpu->amd64_regs[amd64_r10];
    mcontext->gregs[AMD64_GREG_R11] = cpu->amd64_regs[amd64_r11];
    mcontext->gregs[AMD64_GREG_R12] = cpu->amd64_regs[amd64_r12];
    mcontext->gregs[AMD64_GREG_R13] = cpu->amd64_regs[amd64_r13];
    mcontext->gregs[AMD64_GREG_R14] = cpu->amd64_regs[amd64_r14];
    mcontext->gregs[AMD64_GREG_R15] = cpu->amd64_regs[amd64_r15];
    mcontext->gregs[AMD64_GREG_RDI] = cpu->amd64_regs[amd64_rdi];
    mcontext->gregs[AMD64_GREG_RSI] = cpu->amd64_regs[amd64_rsi];
    mcontext->gregs[AMD64_GREG_RBP] = cpu->amd64_regs[amd64_rbp];
    mcontext->gregs[AMD64_GREG_RBX] = cpu->amd64_regs[amd64_rbx];
    mcontext->gregs[AMD64_GREG_RDX] = cpu->amd64_regs[amd64_rdx];
    mcontext->gregs[AMD64_GREG_RAX] = cpu->amd64_regs[amd64_rax];
    mcontext->gregs[AMD64_GREG_RCX] = cpu->amd64_regs[amd64_rcx];
    mcontext->gregs[AMD64_GREG_RSP] = cpu->amd64_regs[amd64_rsp];
    mcontext->gregs[AMD64_GREG_RIP] = cpu->amd64_rip;
    collapse_flags(cpu);
    mcontext->gregs[AMD64_GREG_EFL] = cpu->eflags;
    // Linux x86_64 REG_CSGSFS packs CS, GS, FS, and a zero pad word.
    mcontext->gregs[AMD64_GREG_CSGSFS] =
        AMD64_USER_CS |
        ((qword_t) cpu->gs << 16);
    mcontext->gregs[AMD64_GREG_ERR] = signal_trap_error(cpu);
    mcontext->gregs[AMD64_GREG_TRAPNO] = 0;
    mcontext->gregs[AMD64_GREG_OLDMASK] = current->blocked;
}

static void setup_amd64_fpstate(struct amd64_fpstate_ *fpstate, struct cpu_state *cpu) {
    memset(fpstate, 0, sizeof(*fpstate));
    fpstate->cwd = cpu->fcw;
    fpstate->swd = cpu->fsw;
    fpstate->mxcsr = 0x1f80;

    for (int i = 0; i < 8; i++) {
        const float80 value = cpu->fp[i];
        for (int j = 0; j < 4; j++)
            fpstate->st[i].significand[j] = (word_t) (value.signif >> (j * 16));
        fpstate->st[i].exponent = value.signExp;
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            fpstate->xmm[i].element[j] = cpu->xmm[i].u32[j];
}

static void setup_rt_sigframe_amd64(struct siginfo_ *info, struct rt_sigframe_amd64 *frame) {
    memset(frame, 0, sizeof(*frame));
    frame->uc.flags = AMD64_UC_FP_XSTATE;
    frame->uc.link = 0;
    frame->uc.stack = (struct amd64_stack_t_marshaled) {
        .stack = current->altstack,
        .flags = current_altstack_flags(current),
        .size = current->altstack_size,
    };
    setup_amd64_mcontext(&frame->uc.mcontext, &current->cpu);
    setup_amd64_fpstate(&frame->uc.fpregs_mem, &current->cpu);
    if (signal_should_capture_trap_state(info->sig)) {
        frame->uc.mcontext.gregs[AMD64_GREG_TRAPNO] = current->cpu.trapno;
        if (current->cpu.trapno == INT_PF)
            frame->uc.mcontext.gregs[AMD64_GREG_CR2] = current->cpu.segfault_addr;
    }
    frame->uc.sigmask = current->blocked;
    siginfo_to_amd64_user(&frame->info, info);

    static const struct {
        uint8_t mov_rax_imm32;
        uint32_t nr_rt_sigreturn;
        uint16_t syscall;
    } __attribute__((packed)) rt_retcode = {
        .mov_rax_imm32 = 0xb8,
        .nr_rt_sigreturn = 15,
        .syscall = 0x050f,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void setup_rt_sigframe_arm64(struct siginfo_ *info, struct rt_sigframe_arm64 *frame) {
    struct cpu_state *cpu = &current->cpu;
    memset(frame, 0, sizeof(*frame));
    siginfo_to_amd64_user(&frame->info, info); // generic 64-bit siginfo layout, same on arm64
    frame->uc.flags = 0;
    frame->uc.link = 0;
    frame->uc.stack = (struct amd64_stack_t_marshaled) {
        .stack = current->altstack,
        .flags = current_altstack_flags(current),
        .size = current->altstack_size,
    };
    frame->uc.sigmask = current->blocked;

    struct arm64_mcontext_ *mc = &frame->uc.mcontext;
    mc->fault_address = info->sig == SIGSEGV_ || info->sig == SIGBUS_ ? info->fault.addr : 0;
    for (int i = 0; i < 31; i++)
        mc->regs[i] = cpu->arm64_regs[i];
    mc->sp = cpu->arm64_sp;
    mc->pc = cpu->arm64_pc;
    mc->pstate = cpu->arm64_nzcv; // NZCV in bits 31:28, the only PSTATE this port models

    // Context-record chain in __reserved: fpsimd_context, then a null
    // terminator (the kernel always writes fpsimd first; unwinders and
    // sigsetjmp paths expect to find it).
    struct arm64_fpsimd_context_ fpsimd = {
        .magic = ARM64_FPSIMD_MAGIC,
        .size = sizeof(fpsimd),
        .fpsr = cpu->arm64_fpsr,
        .fpcr = cpu->arm64_fpcr,
    };
    memcpy(fpsimd.vregs, cpu->arm64_v, sizeof(fpsimd.vregs));
    memcpy(mc->reserved, &fpsimd, sizeof(fpsimd));
    // terminator: magic 0, size 0 — already zero from the memset

    // Trampoline: movz x8, #__NR_rt_sigreturn (139) ; svc #0
    frame->retcode[0] = 0xd2800008u | (139u << 5);
    frame->retcode[1] = 0xd4000001u;
}

static void restore_arm64_mcontext(struct rt_sigframe_arm64 *frame, struct cpu_state *cpu) {
    struct arm64_mcontext_ *mc = &frame->uc.mcontext;
    for (int i = 0; i < 31; i++)
        cpu->arm64_regs[i] = mc->regs[i];
    cpu->arm64_sp = mc->sp;
    cpu->arm64_pc = mc->pc;
    cpu->arm64_nzcv = (dword_t) mc->pstate & 0xf0000000u;

    // Restore FP state if the handler's frame still carries the fpsimd
    // record (it might have been overwritten by a longjmp-mangled frame;
    // treat a missing record as "leave FP state alone", like the kernel's
    // optional-record parsing).
    struct arm64_fpsimd_context_ fpsimd;
    memcpy(&fpsimd, mc->reserved, sizeof(fpsimd));
    if (fpsimd.magic == ARM64_FPSIMD_MAGIC && fpsimd.size == sizeof(fpsimd)) {
        cpu->arm64_fpsr = fpsimd.fpsr;
        cpu->arm64_fpcr = fpsimd.fpcr;
        memcpy(cpu->arm64_v, fpsimd.vregs, sizeof(fpsimd.vregs));
    }
}

qword_t sys_rt_sigreturn_arm64(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_arm64 frame;
    // At handler entry SP = &frame; the handler's return through the
    // trampoline restores SP to exactly that point before the SVC.
    guest_addr_t frame_addr = cpu->arm64_sp;
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }

    restore_arm64_mcontext(&frame, cpu);

    lock(&current->sighand->lock, 0);
    restore_altstack(frame_addr, frame.uc.stack.stack,
            frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->arm64_regs[arm64_x0];
}


static void setup_rt_sigframe_riscv64(struct siginfo_ *info, struct rt_sigframe_riscv64 *frame) {
    struct cpu_state *cpu = &current->cpu;
    memset(frame, 0, sizeof(*frame));
    siginfo_to_amd64_user(&frame->info, info);
    frame->uc.flags = 0;
    frame->uc.link = 0;
    frame->uc.stack = (struct amd64_stack_t_marshaled) {
        .stack = current->altstack,
        .flags = current_altstack_flags(current),
        .size = current->altstack_size,
    };
    frame->uc.sigmask = current->blocked;

    struct riscv64_mcontext_ *mc = &frame->uc.mcontext;
    mc->pc = cpu->riscv64_pc;
    for (int i = 1; i < 32; i++)
        mc->regs[i - 1] = cpu->riscv64_regs[i];
    for (int i = 0; i < 32; i++)
        mc->f[i] = cpu->riscv64_f[i];
    mc->fcsr = cpu->riscv64_fcsr;

    // Trampoline: li a7, 139 (addi a7, x0, 139) ; ecall
    frame->retcode[0] = 0x08b00893u;
    frame->retcode[1] = 0x00000073u;
}

static void restore_riscv64_mcontext(struct rt_sigframe_riscv64 *frame, struct cpu_state *cpu) {
    struct riscv64_mcontext_ *mc = &frame->uc.mcontext;
    cpu->riscv64_pc = mc->pc;
    for (int i = 1; i < 32; i++)
        cpu->riscv64_regs[i] = mc->regs[i - 1];
    for (int i = 0; i < 32; i++)
        cpu->riscv64_f[i] = mc->f[i];
    cpu->riscv64_fcsr = mc->fcsr;
}

qword_t sys_rt_sigreturn_riscv64(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_riscv64 frame;
    // At handler entry SP = &frame; the trampoline ecall happens with SP
    // restored to exactly that point (same contract as arm64).
    guest_addr_t frame_addr = cpu->riscv64_regs[riscv64_sp];
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }

    restore_riscv64_mcontext(&frame, cpu);

    lock(&current->sighand->lock, 0);
    restore_altstack(frame_addr, frame.uc.stack.stack,
            frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->riscv64_regs[riscv64_a0];
}

static void receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);
    if (should_trace_signal_task(current)) {
        printk("tracked signal receive: pid=%d tgid=%d comm=%s sig=%d action=%d blocked=%#x pending=%#x waiting=%#x\n",
               current->pid, current->tgid, current->comm, sig,
               signal_action(sighand, sig), current->blocked, current->pending, current->waiting);
    }

    switch (signal_action(sighand, sig)) {
        case SIGNAL_IGNORE:
            return;

        case SIGNAL_STOP:
            lock(&current->group->lock,0);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
            // execve asked for THIS thread to go, not the whole group -- see
            // exit_requested in kernel/task.h. do_exit takes a non-leader
            // thread off the group list and destroys it without touching the
            // other threads or notifying the parent.
            if (__atomic_load_n(&current->exit_requested, __ATOMIC_ACQUIRE))
                do_exit(current, sig);
            do_exit_group(sig);
    }

    // A handler is about to run. If the syscall it interrupted asked for an
    // ERESTARTNOHAND restart -- poll/select/epoll_wait, which resume across a
    // job-control stop but not across a handler -- the restart is cancelled
    // here and the guest gets EINTR, exactly as Linux's handle_signal does.
    if (current->restart_nohand_pending) {
        current->restart_nohand_pending = false;
        current->poll_restart_valid = false;
        current->sleep_restart_valid = false;
        cancel_syscall_restart();
    }

    struct sigaction_ *action = &sighand->action[info->sig];
    bool need_siginfo = action->flags & SA_SIGINFO_;

    guest_addr_t sp = current_user_sp(current);
    if (guest_abi_is_64bit(current->abi)) {
        // amd64 and arm64: architected behavior — the altstack is used
        // only when the action asks for it.
        if ((action->flags & SA_ONSTACK_) && current->altstack && !is_on_altstack(sp, current))
            sp = current->altstack + current->altstack_size;
    } else {
        // Preserve longstanding i386 behavior. Existing 32-bit userspace in
        // this tree has historically run all handlers on the altstack when
        // one is configured, regardless of SA_ONSTACK.
        if (current->altstack && !is_on_altstack(sp, current))
            sp = current->altstack + current->altstack_size;
    }

    if (current->abi == GUEST_ABI_ARM64) {
        struct rt_sigframe_arm64 frame;
        setup_rt_sigframe_arm64(info, &frame);

        sp -= sizeof(frame);
        sp &= ~0xfull; // AAPCS64: SP 16-byte aligned at all public interfaces

        current->cpu.arm64_sp = sp;
        current->cpu.arm64_pc = action->handler;
        // arm64 has only rt signals: x1/x2 always point at info/ucontext
        // regardless of SA_SIGINFO (the flag only changes the handler's
        // declared signature, not the frame), matching the kernel.
        current->cpu.arm64_regs[arm64_x0] = info->sig;
        current->cpu.arm64_regs[arm64_x1] = sp + offsetof(struct rt_sigframe_arm64, info);
        current->cpu.arm64_regs[arm64_x2] = sp + offsetof(struct rt_sigframe_arm64, uc);
        // 0x04000000 = SA_RESTORER (arm64 defines it; the kernel honors an
        // explicit restorer and otherwise uses the vDSO trampoline — here,
        // the on-stack retcode, since there's no arm64 vDSO yet). Don't
        // read action->restorer without the flag: musl leaves the field
        // unset on aarch64.
        guest_addr_t restorer = action->flags & 0x04000000u ? action->restorer : 0;
        if (restorer == 0)
            restorer = sp + offsetof(struct rt_sigframe_arm64, retcode);
        current->cpu.arm64_regs[arm64_x30] = restorer;

        if (!(action->flags & SA_NODEFER_))
            sigset_add(&current->blocked, info->sig);
        current->blocked |= action->mask;

        if (user_write(sp, &frame, sizeof(frame))) {
            // See the amd64 path below: kill like Linux force_sigsegv
            // instead of self-deadlocking through deliver_signal.
            printk("WARNING: failed to install arm64 frame for %d at %#llx, killing\n",
                   info->sig, (unsigned long long) sp);
            unlock(&sighand->lock);
            do_exit_group(SIGSEGV_);
        }

        if (action->flags & SA_RESETHAND_)
            *action = (struct sigaction_) {.handler = SIG_DFL_};
        return;
    }

    if (current->abi == GUEST_ABI_RISCV64) {
        struct rt_sigframe_riscv64 frame;
        setup_rt_sigframe_riscv64(info, &frame);
        sp -= sizeof(frame);
        sp &= ~0xfull; // RISC-V psABI: SP 16-byte aligned

        current->cpu.riscv64_regs[riscv64_sp] = sp;
        current->cpu.riscv64_pc = action->handler;
        // Like arm64, riscv64 has only rt signals: a1/a2 always carry
        // info/ucontext regardless of SA_SIGINFO.
        current->cpu.riscv64_regs[riscv64_a0] = info->sig;
        current->cpu.riscv64_regs[riscv64_a1] = sp + offsetof(struct rt_sigframe_riscv64, info);
        current->cpu.riscv64_regs[riscv64_a2] = sp + offsetof(struct rt_sigframe_riscv64, uc);
        // riscv64 defines no SA_RESTORER (the real kernel always uses the
        // vDSO trampoline); this port always uses the on-stack retcode.
        current->cpu.riscv64_regs[riscv64_ra] =
            sp + offsetof(struct rt_sigframe_riscv64, retcode);

        if (!(action->flags & SA_NODEFER_))
            sigset_add(&current->blocked, info->sig);
        current->blocked |= action->mask;

        if (user_write(sp, &frame, sizeof(frame))) {
            printk("WARNING: failed to install riscv64 frame for %d at %#llx, killing\n",
                   info->sig, (unsigned long long) sp);
            unlock(&sighand->lock);
            do_exit_group(SIGSEGV_);
        }

        if (action->flags & SA_RESETHAND_)
            *action = (struct sigaction_) {.handler = SIG_DFL_};
        return;
    }

    if (current->abi == GUEST_ABI_AMD64) {
        struct rt_sigframe_amd64 frame;
        size_t frame_size = sizeof(frame);
        setup_rt_sigframe_amd64(info, &frame);

        if (sp > 128)
            sp -= 128;
        if (xsave_extra) {
            sp -= xsave_extra;
            sp &= ~0x3full;
            sp -= fxsave_extra;
        }
        sp -= frame_size;
        sp = (sp & ~0xfull) - 8;

        guest_addr_t restorer = action->restorer;
        if (restorer == 0)
            restorer = sp + offsetof(struct rt_sigframe_amd64, retcode);
        frame.pretcode = restorer;
        frame.uc.mcontext.fpstate = sp + offsetof(struct rt_sigframe_amd64, uc.fpregs_mem);

        current->cpu.amd64_regs[amd64_rsp] = sp;
        current->cpu.esp = (dword_t) sp;
        current->cpu.amd64_rip = action->handler;
        current->cpu.eip = (dword_t) action->handler;
        current->cpu.amd64_regs[amd64_rdi] = info->sig;
        current->cpu.amd64_regs[amd64_rsi] = need_siginfo ? sp + offsetof(struct rt_sigframe_amd64, info) : 0;
        current->cpu.amd64_regs[amd64_rdx] = need_siginfo ? sp + offsetof(struct rt_sigframe_amd64, uc) : 0;
        current->cpu.edi = (dword_t) current->cpu.amd64_regs[amd64_rdi];
        current->cpu.esi = (dword_t) current->cpu.amd64_regs[amd64_rsi];
        current->cpu.edx = (dword_t) current->cpu.amd64_regs[amd64_rdx];

        if (!(action->flags & SA_NODEFER_))
            sigset_add(&current->blocked, info->sig);
        current->blocked |= action->mask;

        if (user_write(sp, &frame, frame_size)) {
            // The handler can't run (the stack is unwritable or gone). Linux
            // force_sigsegv kills with SIG_DFL here. Calling deliver_signal
            // would self-deadlock: receive_signals already holds
            // sighand->lock and deliver_signal takes it again.
            printk("WARNING: failed to install amd64 frame for %d at %#llx, killing\n",
                   info->sig, (unsigned long long) sp);
            unlock(&sighand->lock);
            do_exit_group(SIGSEGV_);
        }

        if (action->flags & SA_RESETHAND_)
            *action = (struct sigaction_) {.handler = SIG_DFL_};
        return;
    }

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    // set up registers for signal handler
    current->cpu.eax = info->sig;
    current->cpu.eip = action->handler;

    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
    sp -= frame_size;
    // align sp + 4 on a 16-byte boundary because that's what the abi says
    sp = ((sp + 4) & ~0xf) - 4;
    current->cpu.esp = sp;

    // Update the mask. By default the signal will be blocked while in the
    // handler, but sigaction is allowed to customize this.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        // See the amd64 path above: kill like Linux force_sigsegv instead of
        // re-taking sighand->lock via deliver_signal and self-deadlocking.
        printk("WARNING: failed to install frame for %d at %#x, killing\n", info->sig, sp);
        unlock(&sighand->lock);
        do_exit_group(SIGSEGV_);
    }

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
}

void signal_delivery_stop(int sig, struct siginfo_ *info) {
    unlock(&current->sighand->lock);
    ptrace_signal_stop(sig, info);
    lock(&current->sighand->lock, 0);
}

// Park here for the duration of a job-control group-stop (^Z, SIGSTOP,
// SIGTTIN, SIGTTOU), and report it to a tracer if there is one.
//
// Both execution models need this and they used to have separate copies:
// handle_interrupt (kernel/calls.c) for translated guest code, and
// native_checkpoint (kernel/native.c) for a native program, which runs as host
// code on the guest task's thread and so is never dispatched an instruction at
// all. The copies drifted -- the native one had no ptrace handling whatsoever,
// so `strace` on a native program that got ^Z'd hung the tracer's wait4
// forever. One function, called from both, is what stops that recurring.
//
// Call it with no lock held, from the task's OWN context.
//
// `traced` is re-checked on every pass, not just on entry, and that is
// load-bearing in BOTH directions: the tracer may detach us while we are
// stopped, and it may also ATTACH to us while we are stopped. PTRACE_SEIZE of
// an already-group-stopped tracee sets `traced` from the tracer's own thread
// and notifies stopped_cond to bring us back around here (kernel/ptrace.c).
// Testing it once, outside the wait, is what a tracee that raise(SIGSTOP)'d
// before its tracer seized it used to do: it parked in the plain job-control
// wait below, never noticed it had become traced, never reported the stop, and
// the tracer's wait4 hung forever. Linux handles the same race from the other
// side -- ptrace_attach() wakes a __TASK_STOPPED tracee so it can re-enter the
// trap and report.
//
// ptrace_group_stop() takes group->lock itself, so it must NOT be called with
// that lock held -- hence the branch above the lock rather than inside it.
void group_stop_wait(void) {
    struct tgroup *group = current->group;
    // Fast path: group->stopped is almost always false. Read it locklessly
    // (it is _Atomic) and only take group->lock to actually wait when stopped.
    // Missing a just-set transition here is harmless: a SIGSTOP'd thread is
    // poked and comes back through its caller, catching the stop on the next
    // pass.
    if (!group->stopped)
        return;

    while (group->stopped) {
        if (current->ptrace.traced) {
            ptrace_group_stop();
            continue;
        }
        lock(&group->lock, 0);
        if (group->stopped && !current->ptrace.traced)
            wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
        unlock(&group->lock);
    }

    // We were stopped and have just been resumed. If SIGCONT flagged a
    // reportable continue, wake a parent blocked in wait4/waitid(WCONTINUED)
    // (the flag itself is consumed by the parent's notify_if_continued).
    // Done from our own context -- never the signal sender's -- so taking
    // pids_lock here respects the pids_lock -> group->lock ordering.
    if (group->continued) {
        struct task *parent = NULL;
        int signal_no = 0;
        complex_lockt(&pids_lock, 0);
        parent = current->group->leader->parent;
        if (parent != NULL) {
            task_ref_cnt_mod(parent, 1);
            signal_no = current->group->leader->exit_signal;
            notify(&parent->group->child_exit);
        }
        unlock(&pids_lock);
        // A resume is a reportable event in its own right, and the SIGCHLD
        // that carries it is how a shell learns a job it backgrounded is
        // running again -- without it the parent's handler never fires and
        // only a WCONTINUED wait ever notices. The stop side of this already
        // existed (see receive_signals); the continue side did not, so
        // si_code was never CLD_CONTINUED.
        //
        // SA_NOCLDSTOP suppresses it, exactly as it does the stop: the flag
        // is about stop AND continue notifications, not stops alone.
        if (parent != NULL) {
            if (signal_no == SIGCHLD_) {
                struct sighand *psighand = parent->sighand;
                if (psighand != NULL) {
                    lock(&psighand->lock, 0);
                    if (psighand->action[SIGCHLD_].flags & SA_NOCLDSTOP_)
                        signal_no = 0;
                    unlock(&psighand->lock);
                }
            }
            if (signal_no != 0) {
                struct siginfo_ info = {
                    .code = CLD_CONTINUED_,
                    .child.pid = current->group->leader->pid,
                    .child.uid = current->uid,
                    .child.status = SIGCONT_,
                };
                send_signal(parent, signal_no, info);
            }
            task_ref_cnt_mod(parent, -1);
        }
    }
}

void receive_signals(void) {  
    lock(&current->group->lock, 0);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);

    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Only ignore a signal right now if
    // it was both blocked during the call and should still be blocked after
    // the call.
    sigset_t_ blocked = current->blocked;
    if (current->has_saved_mask) {
        blocked &= current->saved_mask;
        current->has_saved_mask = false;
        current->blocked = current->saved_mask;
    }

    // Deliver pending unblocked signals LOWEST-NUMBERED-FIRST, matching Linux's
    // dequeue order (next_signal). When several are deliverable at once (e.g. a
    // sigprocmask that unblocks a whole set), each receive_signal stacks a frame
    // on top of the previous one, so the handlers RUN highest-first (LIFO) and
    // the per-frame saved mask is captured incrementally — bit-for-bit what real
    // Linux does. Previously this drained the queue in FIFO insertion order, so
    // a scrambled-order send ran the handlers in the wrong order.
    for (;;) {
        struct sigqueue *best = NULL;
        struct sigqueue *sigqueue;
        bool best_is_group = false;
        list_for_each_entry(&current->queue, sigqueue, queue) {
            if (sigset_has(blocked, sigqueue->info.sig))
                continue;
            if (best == NULL || sigqueue->info.sig < best->info.sig)
                best = sigqueue;
        }
        // Also drain the shared (process-directed) queue -- e.g. a SIGCHLD
        // delivered via send_signal_to_group to a sibling thread of this
        // process, see kernel/exit.c.
        list_for_each_entry(&sighand->queue, sigqueue, queue) {
            if (sigset_has(blocked, sigqueue->info.sig))
                continue;
            if (best == NULL || sigqueue->info.sig < best->info.sig) {
                best = sigqueue;
                best_is_group = true;
            }
        }
        if (best == NULL)
            break;

        int sig = best->info.sig;
        struct siginfo_ info = best->info;
        list_remove(&best->queue);
        if (best_is_group) {
            if (!signal_list_still_has_locked(&sighand->queue, sig))
                sigset_del(&sighand->pending, sig);
        } else if (!signal_still_pending_locked(current, sig)) {
            sigset_del(&current->pending, sig);
        }
        free(best);

        if (current->ptrace.traced && sig != SIGKILL_ &&
                sig != current->ptrace.deliver_sig) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_delivery_stop(sig, &info);
        } else {
            // A signal the tracer asked us to deliver (PTRACE_CONT with a
            // signal) is consumed and actually delivered exactly once, rather
            // than re-trapped through signal_delivery_stop (which would make the
            // tracer re-inject it forever).
            if (sig == current->ptrace.deliver_sig)
                current->ptrace.deliver_sig = 0;
            receive_signal(sighand, &info);
        }
    }

    unlock(&sighand->lock);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock, 0);
        bool now_stopped = current->group->stopped;
        // group_exit_code is (stop_sig << 8 | 0x7f) while stopped; recover the
        // bare stop signal for the SIGCHLD si_status.
        int stop_sig = (current->group->group_exit_code >> 8) & 0xff;
        unlock(&current->group->lock);
        if (now_stopped) {
            struct task *parent = NULL;
            int signal_no = 0;
            complex_lockt(&pids_lock, 0);
            parent = current->parent;
            if (parent != NULL) {
                task_ref_cnt_mod(parent, 1);
                signal_no = current->group->leader->exit_signal;
                notify(&parent->group->child_exit);
            }
            unlock(&pids_lock);
            // SA_NOCLDSTOP: the parent asked NOT to be told when a child
            // merely stops or continues. Only the stop notification is
            // suppressed -- the child's eventual exit still raises SIGCHLD --
            // and wait(WUNTRACED) still reports the stop, because the flag is
            // about the signal, not about waitability.
            if (parent != NULL && signal_no == SIGCHLD_) {
                struct sighand *psighand = parent->sighand;
                if (psighand != NULL) {
                    lock(&psighand->lock, 0);
                    if (psighand->action[SIGCHLD_].flags & SA_NOCLDSTOP_)
                        signal_no = 0;
                    unlock(&psighand->lock);
                }
            }
            if (parent != NULL) {
                // The stop SIGCHLD must carry CLD_STOPPED + the stop signal and
                // the child's pid/uid, not SIGINFO_NIL (which a SA_SIGINFO
                // handler / sigwaitinfo would read as SI_KERNEL with no child).
                struct siginfo_ info = {
                    .code = CLD_STOPPED_,
                    .child.pid = current->pid,
                    .child.uid = current->uid,
                    .child.status = stop_sig,
                };
                if (signal_no != 0)
                    send_signal(parent, signal_no, info);
                task_ref_cnt_mod(parent, -1);
            }
        }
    }
}

static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
}

static void sync_i386_shadows_from_amd64(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static void restore_amd64_fpstate(struct amd64_fpstate_ *fpstate, struct cpu_state *cpu) {
    cpu->fcw = fpstate->cwd;
    cpu->fsw = fpstate->swd;

    for (int i = 0; i < 8; i++) {
        uint64_t significand = 0;
        for (int j = 0; j < 4; j++)
            significand |= (uint64_t) fpstate->st[i].significand[j] << (j * 16);
        cpu->fp[i] = (float80) {
            .signif = significand,
            .signExp = fpstate->st[i].exponent,
        };
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            cpu->xmm[i].u32[j] = fpstate->xmm[i].element[j];
}

static void restore_amd64_mcontext(struct amd64_mcontext_ *mcontext, struct cpu_state *cpu) {
    cpu->amd64_regs[amd64_r8] = mcontext->gregs[AMD64_GREG_R8];
    cpu->amd64_regs[amd64_r9] = mcontext->gregs[AMD64_GREG_R9];
    cpu->amd64_regs[amd64_r10] = mcontext->gregs[AMD64_GREG_R10];
    cpu->amd64_regs[amd64_r11] = mcontext->gregs[AMD64_GREG_R11];
    cpu->amd64_regs[amd64_r12] = mcontext->gregs[AMD64_GREG_R12];
    cpu->amd64_regs[amd64_r13] = mcontext->gregs[AMD64_GREG_R13];
    cpu->amd64_regs[amd64_r14] = mcontext->gregs[AMD64_GREG_R14];
    cpu->amd64_regs[amd64_r15] = mcontext->gregs[AMD64_GREG_R15];
    cpu->amd64_regs[amd64_rdi] = mcontext->gregs[AMD64_GREG_RDI];
    cpu->amd64_regs[amd64_rsi] = mcontext->gregs[AMD64_GREG_RSI];
    cpu->amd64_regs[amd64_rbp] = mcontext->gregs[AMD64_GREG_RBP];
    cpu->amd64_regs[amd64_rbx] = mcontext->gregs[AMD64_GREG_RBX];
    cpu->amd64_regs[amd64_rdx] = mcontext->gregs[AMD64_GREG_RDX];
    cpu->amd64_regs[amd64_rax] = mcontext->gregs[AMD64_GREG_RAX];
    cpu->amd64_regs[amd64_rcx] = mcontext->gregs[AMD64_GREG_RCX];
    cpu->amd64_regs[amd64_rsp] = mcontext->gregs[AMD64_GREG_RSP];
    cpu->amd64_rip = mcontext->gregs[AMD64_GREG_RIP];
    cpu->trapno = (dword_t) mcontext->gregs[AMD64_GREG_TRAPNO];
    cpu->segfault_addr = mcontext->gregs[AMD64_GREG_CR2];

    cpu->eflags = (dword_t) mcontext->gregs[AMD64_GREG_EFL];
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;

    sync_i386_shadows_from_amd64(cpu);
}

dword_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    if (current->abi == GUEST_ABI_AMD64)
        return (dword_t) sys_rt_sigreturn_amd64();

    struct rt_sigframe_ frame;
    // esp points past the first field of the frame
    if (user_get(cpu->esp - offsetof(struct rt_sigframe_, sig), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);

    lock(&current->sighand->lock, 0);
    restore_altstack(cpu->esp, frame.uc.stack.stack, frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

qword_t sys_rt_sigreturn_amd64(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_amd64 frame;
    struct amd64_fpstate_ fpstate;
    guest_addr_t frame_addr = cpu->amd64_regs[amd64_rsp] - offsetof(struct rt_sigframe_amd64, uc);
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }

    restore_amd64_mcontext(&frame.uc.mcontext, cpu);
    if (frame.uc.mcontext.fpstate != 0) {
        if (user_get(frame.uc.mcontext.fpstate, fpstate)) {
            deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
            return _EFAULT;
        }
        restore_amd64_fpstate(&fpstate, cpu);
    }

    lock(&current->sighand->lock, 0);
    restore_altstack(cpu->amd64_regs[amd64_rsp], frame.uc.stack.stack,
            frame.uc.stack.size, frame.uc.stack.flags);
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->amd64_regs[amd64_rax];
}

dword_t sys_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
    // esp points past the first two fields of the frame
    if (user_get(cpu->esp - offsetof(struct sigframe_, sc), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);

    lock(&current->sighand->lock, 0);
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(oldmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock, "sighand_new\0");
    lock_init(&sighand->wake_lock, "sighand_wake\0");
    list_init(&sighand->queue);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    return new_sighand;
}

void sighand_retain(struct sighand *sighand) {
    atomic_fetch_add_explicit(&sighand->refcount, 1, memory_order_relaxed);
}

void sighand_release(struct sighand *sighand) {
    if (atomic_fetch_sub_explicit(&sighand->refcount, 1, memory_order_acq_rel) == 1) {
        free(sighand);
    }
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (!signal_is_blockable(sig))
        return _EINVAL;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    struct sigaction_ prev_action = sighand->action[sig];
    if (oldaction)
        *oldaction = prev_action;
    if (action)
        sighand->action[sig] = *action;
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    return sys_rt_sigaction_guest(signum, action_addr, oldaction_addr, sigset_size);
}

dword_t sys_rt_sigaction_guest(dword_t signum, guest_addr_t action_addr, guest_addr_t oldaction_addr, dword_t sigset_size) {
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;
    // Signal 0 is the "does this process exist" probe for kill(2); it has no
    // disposition to set or read. Accepting it here reported success for a
    // call that did nothing, so a caller checking whether a signal number is
    // usable by round-tripping it through sigaction was told 0 was.
    if (signum == 0)
        return _EINVAL;
    struct sigaction_ action = {};
    struct sigaction_ oldaction = {};
    if (action_addr != 0) {
        int err = sigaction_from_user(current, action_addr, &action);
        if (err < 0)
            return err;
    }
    STRACE("rt_sigaction(%d, %#llx {handler=%#llx, flags=%#llx, restorer=%#llx, mask=%#llx}, %#llx, %d)", signum,
            (unsigned long long) action_addr,
            (unsigned long long) action.handler,
            (unsigned long long) action.flags,
            (unsigned long long) action.restorer,
            (unsigned long long) action.mask,
            (unsigned long long) oldaction_addr, sigset_size);

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0) {
        err = sigaction_to_user(current, oldaction_addr, &oldaction);
        if (err < 0)
            return err;
    }
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = (set & ~UNBLOCKABLE_MASK);
}

void sigmask_set_blocked(sigset_t_ set) {
    sigmask_set(set);
}

static void sigmask_set_temp_unlocked(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

void sigmask_set_temp(sigset_t_ mask) {
    lock(&current->sighand->lock, 0);
    sigmask_set_temp_unlocked(mask);
    unlock(&current->sighand->lock);
}

void sigmask_clear_temp(void) {
    lock(&current->sighand->lock, 0);
    if (current->has_saved_mask) {
        current->blocked = current->saved_mask;
        current->has_saved_mask = false;
    }
    unlock(&current->sighand->lock);
}

static int do_sigprocmask(dword_t how, sigset_t_ set) {
    if (how == SIG_BLOCK_)
        sigmask_set(current->blocked | set);
    else if (how == SIG_UNBLOCK_)
        sigmask_set(current->blocked & ~set);
    else if (how == SIG_SETMASK_)
        sigmask_set(set);
    else
        return _EINVAL;
    return 0;
}

dword_t sys_sigprocmask_guest(dword_t how, guest_addr_t set_addr, guest_addr_t oldset_addr) {
    dword_t set32 = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set32))
            return _EFAULT;

    STRACE("sigprocmask(%s, %#llx, %#x)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (unsigned long long) set32 : ~0ull,
            oldset_addr);

    if (oldset_addr != 0) {
        dword_t oldset32 = (dword_t) current->blocked;
        if (user_put(oldset_addr, oldset32))
            return _EFAULT;
    }
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock, 0);
        int err = do_sigprocmask(how, (sigset_t_) set32);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr) {
    return sys_sigprocmask_guest(how, set_addr, oldset_addr);
}

dword_t sys_rt_sigprocmask_guest(dword_t how, guest_addr_t set_addr, guest_addr_t oldset_addr, dword_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ set = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set))
            return _EFAULT;
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    if (oldset_addr != 0)
        if (user_put(oldset_addr, current->blocked))
            return _EFAULT;
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock, 0);
        int err = do_sigprocmask(how, set);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    return sys_rt_sigprocmask_guest(how, set_addr, oldset_addr, size);
}

int_t sys_rt_sigpending(addr_t set_addr) {
    return sys_rt_sigpending_guest(set_addr);
}

int_t sys_rt_sigpending_guest(guest_addr_t set_addr) {
    STRACE("rt_sigpending(%#llx)", (unsigned long long) set_addr);
    // as defined by the standard. Includes the shared (process-directed)
    // queue: sigpending(2) is specified as the union of the thread's own and
    // the process's pending sets.
    sigset_t_ pending = (current->pending | current->sighand->pending) & current->blocked;
    if (user_put(set_addr, pending))
        return _EFAULT;
    return 0;
}

static bool is_on_altstack(guest_addr_t sp, struct task *task) {
    return sp > task->altstack && sp <= task->altstack + task->altstack_size;
}

static void restore_altstack(guest_addr_t sp, guest_addr_t stack, guest_addr_t size, dword_t flags) {
    if (is_on_altstack(sp, current))
        return;
    if (flags & SS_DISABLE_) {
        current->altstack = 0;
        current->altstack_size = 0;
        return;
    }
    if (size >= MINSIGSTKSZ_) {
        current->altstack = stack;
        current->altstack_size = size;
    }
}

static dword_t current_altstack_flags(struct task *task) {
    dword_t flags = 0;
    if (task->altstack == 0)
        flags |= SS_DISABLE_;
    if (is_on_altstack(current_user_sp(task), task))
        flags |= SS_ONSTACK_;
    return flags;
}

static void altstack_to_i386_user(struct task *task, struct stack_t_ *user_stack) {
    user_stack->stack = task->altstack;
    user_stack->flags = current_altstack_flags(task);
    user_stack->size = (dword_t) task->altstack_size;
}

static int altstack_to_user(struct task *task, guest_addr_t user_addr) {
    dword_t flags = current_altstack_flags(task);
    // arm64 stack_t == amd64 stack_t (generic 64-bit layout).
    if (guest_abi_is_64bit(task->abi)) {
        struct amd64_stack_t_marshaled user_stack = {
            .stack = task->altstack,
            .flags = flags,
            .size = task->altstack_size,
        };
        if (user_put(user_addr, user_stack))
            return _EFAULT;
    } else {
        struct stack_t_ user_stack = {
            .stack = task->altstack,
            .flags = flags,
            .size = (dword_t) task->altstack_size,
        };
        if (user_put(user_addr, user_stack))
            return _EFAULT;
    }
    return 0;
}

static int altstack_from_user(struct task *task, guest_addr_t user_addr, guest_addr_t *stack_out, guest_addr_t *size_out, dword_t *flags_out) {
    if (guest_abi_is_64bit(task->abi)) {
        struct amd64_stack_t_marshaled user_stack;
        if (user_get(user_addr, user_stack))
            return _EFAULT;
        *stack_out = user_stack.stack;
        *size_out = user_stack.size;
        *flags_out = user_stack.flags;
    } else {
        struct stack_t_ user_stack;
        if (user_get(user_addr, user_stack))
            return _EFAULT;
        *stack_out = user_stack.stack;
        *size_out = user_stack.size;
        *flags_out = user_stack.flags;
    }
    return 0;
}

dword_t sys_sigaltstack(guest_addr_t ss_addr, guest_addr_t old_ss_addr) {
    STRACE("sigaltstack(%#llx, %#llx)", (unsigned long long) ss_addr, (unsigned long long) old_ss_addr);
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    if (old_ss_addr != 0) {
        if (altstack_to_user(current, old_ss_addr)) {
            unlock(&sighand->lock);
            return _EFAULT;
        }
    }
    if (ss_addr != 0) {
        if (is_on_altstack(current_user_sp(current), current)) {
            unlock(&sighand->lock);
            return _EPERM;
        }
        guest_addr_t stack;
        guest_addr_t size;
        dword_t flags;
        int err = altstack_from_user(current, ss_addr, &stack, &size, &flags);
        if (err < 0) {
            unlock(&sighand->lock);
            return err;
        }
        // Only SS_DISABLE and SS_ONSTACK are defined; anything else is a
        // caller that got the struct wrong, and Linux says so rather than
        // installing a stack from a request it did not understand.
        if (flags & ~(dword_t) (SS_DISABLE_ | SS_ONSTACK_)) {
            unlock(&sighand->lock);
            return _EINVAL;
        }
        if (flags & SS_DISABLE_) {
            current->altstack = 0;
            current->altstack_size = 0;
        } else {
            if (size < MINSIGSTKSZ_) {
                unlock(&sighand->lock);
                return _ENOMEM;
            }
            current->altstack = stack;
            current->altstack_size = size;
        }
    }
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_sigaltstack_guest(guest_addr_t ss_addr, guest_addr_t old_ss_addr) {
    return sys_sigaltstack(ss_addr, old_ss_addr);
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    return sys_rt_sigsuspend_guest(mask_addr, size);
}

int_t sys_rt_sigsuspend_guest(guest_addr_t mask_addr, uint_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);
    lock(&current->sighand->lock, 0);
    sigmask_set_temp_unlocked(mask);
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    return _EINTR;
}

int_t sys_pause(void) {
    lock(&current->sighand->lock, 0);
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    return _EINTR;
}

static int_t sys_rt_sigtimedwait_common(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size,
        bool timeout_time64) {
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    struct timespec timeout;
    if (timeout_addr != 0) {
        // The amd64 ABI's native struct timespec is 64-bit (== timespec64_).
        // Reading it as the 32-bit i386 struct timespec_ pulled tv_nsec out of
        // the high half of tv_sec — always 0 for any sub-second timeout — so
        // the EINVAL range check never fired and sub-second waits collapsed to
        // an immediate EAGAIN. amd64 must always read the 64-bit layout.
        bool read64 = timeout_time64 || guest_abi_is_64bit(current->abi);
        if (read64) {
            struct timespec64_ fake_timeout;
            if (user_get(timeout_addr, fake_timeout))
                return _EFAULT;
            timeout.tv_sec = fake_timeout.sec;
            timeout.tv_nsec = fake_timeout.nsec;
        } else {
            struct timespec_ fake_timeout;
            if (user_get(timeout_addr, fake_timeout))
                return _EFAULT;
            timeout.tv_sec = fake_timeout.sec;
            timeout.tv_nsec = fake_timeout.nsec;
        }
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
            return _EINVAL;
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    lock(&current->sighand->lock, 0);
    struct siginfo_ info;
    if (signal_take_next_locked(current, set, &info)) {
        unlock(&current->sighand->lock);
        if (info_addr != 0)
            if (siginfo_to_user(current, info_addr, &info))
                return _EFAULT;
        STRACE("done sigtimedwait immediate = %d\n", info.sig);
        return info.sig;
    }
    assert(current->waiting == 0);
    current->waiting = set;
    int err = 0;
    TASK_MAY_BLOCK {
        do {
            err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
        } while (err == 0);
    }
    current->waiting = 0;
    if (err == _ETIMEDOUT) {
        unlock(&current->sighand->lock);
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }

    bool found = signal_take_next_locked(current, set, &info);
    unlock(&current->sighand->lock);
    if (!found)
        return _EINTR;
    if (info_addr != 0)
        if (siginfo_to_user(current, info_addr, &info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, false);
}

int_t sys_rt_sigtimedwait_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, false);
}

int_t sys_rt_sigtimedwait_time64(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, true);
}

int_t sys_rt_sigtimedwait_time64_guest(guest_addr_t set_addr, guest_addr_t info_addr, guest_addr_t timeout_addr, uint_t set_size) {
    return sys_rt_sigtimedwait_common(set_addr, info_addr, timeout_addr, set_size, true);
}

// Linux's check_kill_permission. The credential rule is the obvious part; the
// exception is not, and it was missing entirely: SIGCONT may be sent to ANY
// process in the same SESSION whatever its credentials.
//
// That exception is what job control is built on. A shell that started a
// privileged job -- `sudo something`, or any setuid program -- keeps the
// stopped process in its own session but not under its own uid, so without it
// `fg` could not resume anything privileged, and kill_group inherited the same
// refusal for the whole process group.
static bool may_signal_task(struct task *task, dword_t sig) {
    if (superuser())
        return true;
    // A thread signalling its own process never needs a credential check.
    if (task->tgid == current->tgid)
        return true;
    if (current->uid == task->uid || current->uid == task->suid ||
            current->euid == task->uid || current->euid == task->suid)
        return true;
    if (sig == SIGCONT_) {
        lock(&task->group->lock, 0);
        pid_t_ target_sid = task->group->sid;
        unlock(&task->group->lock);
        // A target with no session is reachable too, as in Linux.
        if (target_sid == 0 || target_sid == current->group->sid)
            return true;
    }
    return false;
}

int signal_kill_task(struct task *task, dword_t sig, int si_code) {
    // FIXME: Need to check references to kernel here to be sure they are zero
    if (!may_signal_task(task, sig))
        return _EPERM;
    // kill(2) reports SI_USER; tkill/tgkill(2) report SI_TKILL. A handler that
    // inspects si_code (or si_pid, which is meaningless for SI_TKILL) must see
    // the right one — glibc raise() routes through tgkill, so this is common.
    struct siginfo_ info = {
        .code = si_code,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };

    send_signal(task, sig, info);
    return 0;
}

static int queue_signal_task(struct task *task, dword_t sig, struct siginfo_ info) {
    if (!may_signal_task(task, sig))
        return _EPERM;

    send_signal(task, sig, info);
    return 0;
}

struct kill_target {
    struct task *task;
};

static int kill_group(pid_t_ pgid, dword_t sig, int si_code) {
    struct kill_target stack_targets[32];
    struct kill_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
retry:
    target_count = 0;
    size_t needed = 0;
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup)
        needed++;
    if (needed > target_cap) {
        unlock(&pids_lock);
        if (targets != stack_targets)
            free(targets);
        targets = malloc(sizeof(*targets) * needed);
        if (targets == NULL)
            return _ENOMEM;
        target_cap = needed;

        complex_lockt(&pids_lock, 0);
        pid = pid_get(pgid);
        if (pid == NULL) {
            unlock(&pids_lock);
            free(targets);
            return _ESRCH;
        }
        goto retry;
    }

    // Zombie/exiting members count as successfully signaled on Linux (the
    // signal is just discarded), same as do_kill's single-pid case. Without
    // this, kill(-pgid) on a group whose members all just exited returned
    // EPERM -- nix hits exactly that killing a finished builder's group,
    // and reported "killing process N: Operation not permitted" for every
    // channel unpack that won the race.
    size_t skipped = 0;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        struct task *task = tgroup->leader;
        if (task != NULL && (task->zombie || task->exiting || task->sighand == NULL)) {
            // The leader is a corpse but the process may well still be alive:
            // its other threads keep running, and the leader stays registered
            // until the last of them exits. Signalling the group must reach
            // those threads rather than counting the whole process as gone.
            struct task *live = NULL, *thread;
            list_for_each_entry(&tgroup->threads, thread, group_links) {
                if (!thread->exiting && !thread->zombie && thread->sighand != NULL) {
                    live = thread;
                    break;
                }
            }
            task = live;
        }
        if (task == NULL) {
            skipped++;
            continue;
        }
        task_ref_cnt_mod(task, 1);
        targets[target_count++] = (struct kill_target) {.task = task};
    }
    unlock(&pids_lock);

    if (target_count == 0 && skipped == 0) {
        // The pid exists but no process group hangs off it: no such pgroup.
        if (targets != stack_targets)
            free(targets);
        return _ESRCH;
    }

    int err = skipped > 0 ? 0 : _EPERM;
    for (size_t i = 0; i < target_count; i++) {
        int kill_err = signal_kill_task(targets[i].task, sig, si_code);
        task_ref_cnt_mod(targets[i].task, -1);
        if (err == _EPERM)
            err = kill_err;
    }
    if (targets != stack_targets)
        free(targets);
    return err;
}

static int kill_everything(dword_t sig, int si_code) {
    struct kill_target stack_targets[64];
    struct kill_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

retry:
    target_count = 0;
    if (targets == NULL)
        return _ENOMEM;

    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        if (target_count == target_cap) {
            unlock(&pids_lock);
            size_t new_cap = target_cap * 2;
            struct kill_target *new_targets = targets == stack_targets
                ? malloc(sizeof(*new_targets) * new_cap)
                : realloc(targets, sizeof(*new_targets) * new_cap);
            if (new_targets == NULL) {
                if (targets != stack_targets)
                    free(targets);
                return _ENOMEM;
            }
            if (targets == stack_targets)
                memcpy(new_targets, stack_targets, sizeof(stack_targets));
            targets = new_targets;
            target_cap = new_cap;
            complex_lockt(&pids_lock, 0);
            goto retry;
        }
        task_ref_cnt_mod(task, 1);
        targets[target_count++] = (struct kill_target) {.task = task};
    }
    unlock(&pids_lock);

    // Linux never reports EPERM for the broadcast form: kill(-1) returns 0
    // whenever at least one process was CONSIDERED -- even if every send was
    // denied -- and ESRCH only when nothing matched at all. Starting from
    // EPERM meant a caller with nothing to signal was told it lacked
    // permission, and one that legitimately could not signal a privileged
    // process was told the same thing about a broadcast that had worked.
    for (size_t i = 0; i < target_count; i++) {
        (void) signal_kill_task(targets[i].task, sig, si_code);
        task_ref_cnt_mod(targets[i].task, -1);
    }
    if (targets != stack_targets)
        free(targets);
    return target_count > 0 ? 0 : _ESRCH;
}

// si_code distinguishes the sender: SI_USER for kill(2), SI_TKILL for
// tkill/tgkill(2). Linux forces this on the receiving side, so we thread it
// down from the syscall entry point rather than letting kill_task assume SI_USER.
// kill(2) is PROCESS-directed: Linux puts it in the shared queue and
// complete_signal() hands it to a thread that can actually take it. AOK
// delivers into one task's private queue, which is right for tkill/tgkill and
// wrong for kill -- under the standard daemon shape (block these signals in
// every thread, one dedicated thread sigwait()s them) the signal lands on a
// thread that blocks it and that nobody will ever dequeue from, while the
// sigwait-ing thread sees nothing. mariadbd hit this trying to make its own
// signal thread exit and could not die; a hung mariadbd then wedged an entire
// Devuan boot. See tests/manual/sigwait_kill.c.
//
// Choosing the thread rather than re-routing kill through the group path is
// deliberate: the group path does not carry the stop/cont and default-ignore
// handling that send_signal() does, and using it for kill hung signal_restart,
// signal_stop_cont and process_conformance. This changes only the case that
// was already broken -- every target that could already receive the signal
// still receives it, on the same task as before.
//
// Caller holds pids_lock (the group thread list needs it).
static struct task *pick_process_directed_target(struct task *task, dword_t sig) {
    // kill(pid, 0) is the "does this process exist" probe and carries no
    // signal at all -- sig_mask(0) is out of range and asserts. It has no
    // target to choose, so it never gets here. (apt does this constantly; the
    // first version of this function skipped the check and killed apt on the
    // spot.)
    if (sig == 0)
        return task;
    // A task that has begun exiting cannot take anything: do_exit clears
    // sighand and sets exiting BEFORE the group is dead, and a thread-group
    // leader stays registered in the pid table until every sibling has gone.
    // So kill(pid) on a process whose leader exited first addressed a corpse,
    // and send_signal dropped the signal on the sighand==NULL and exiting
    // checks -- kill() returned 0 and nothing whatsoever happened, forever.
    bool addressed_usable = !task->exiting && !task->zombie && task->sighand != NULL;
    // The addressed task can take it: nothing to do. This is every
    // single-threaded case, and the common multithreaded one.
    if (addressed_usable &&
            (!sigset_has(__atomic_load_n(&task->blocked, __ATOMIC_ACQUIRE), sig) ||
             sigset_has(__atomic_load_n(&task->waiting, __ATOMIC_ACQUIRE), sig)))
        return task;
    if (task->group == NULL)
        return task;

    struct task *candidate = NULL;
    struct task *live = NULL;   // any live sibling, blocked or not
    struct task *thread;
    list_for_each_entry(&task->group->threads, thread, group_links) {
        if (thread->exiting || thread->zombie || thread->sighand == NULL)
            continue;
        if (live == NULL)
            live = thread;
        // A thread parked in sigwait() for this signal is the best target
        // there is -- it is asking for it by name.
        if (sigset_has(__atomic_load_n(&thread->waiting, __ATOMIC_ACQUIRE), sig))
            return thread;
        if (candidate == NULL &&
                !sigset_has(__atomic_load_n(&thread->blocked, __ATOMIC_ACQUIRE), sig))
            candidate = thread;
    }
    if (candidate != NULL)
        return candidate;
    // Every live thread has it blocked. Leave it pending on the addressed task
    // so it fires when that thread unblocks -- unless the addressed task is a
    // corpse, in which case parking it there means dropping it. Any live
    // sibling will do; the signal waits on its mask instead.
    if (!addressed_usable && live != NULL)
        return live;
    return task;
}

// thread_directed distinguishes tkill/tgkill (deliver to THIS thread's private
// queue, which is their entire purpose) from kill (deliver to the process).
static int do_kill_common(pid_t_ pid, dword_t sig, pid_t_ tgid, int si_code,
                          bool thread_directed) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig >= NUM_SIGS)
        return _EINVAL;
    int err;
    if (pid == 0) {
        // "Every process in MY process group." Encoding that as a negative pid
        // and re-dispatching collided with the pid == -1 broadcast whenever the
        // caller's pgid was 1 -- the default for the top-level shell and
        // everything started under it -- so an ordinary kill(0, sig) signalled
        // every task in the guest, across every session and process group.
        // Dispatch the group directly so the broadcast stays reachable only
        // from a literal -1.
        lock(&current->group->lock, 0);
        pid_t_ pgid = current->group->pgid;
        unlock(&current->group->lock);
        complex_lockt(&pids_lock, 0);
        err = kill_group(pgid, sig, si_code);
    } else if (pid == -1) {
        complex_lockt(&pids_lock, 0);
        err = kill_everything(sig, si_code);
    } else if (pid < 0) {
        complex_lockt(&pids_lock, 0);
        err = kill_group(-pid, sig, si_code);
    } else {
        complex_lockt(&pids_lock, 0);
        struct task *task = pid_get_task_zombie(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // An exited-but-unreaped (zombie) task still exists for kill() on
        // Linux: the signal is discarded but the call returns 0, not ESRCH.
        // stress-ng does kill(child, SIGKILL) right after the child exits,
        // before wait4() reaps it.
        if (task->zombie) {
            unlock(&pids_lock);
            return 0;
        }

        if (!thread_directed)
            task = pick_process_directed_target(task, sig);
        task_ref_cnt_mod(task, 1);
        unlock(&pids_lock);
        err = signal_kill_task(task, sig, si_code);
        task_ref_cnt_mod(task, -1);
    }
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill_common(pid, sig, 0, SI_USER_, false);
}
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill_common(tid, sig, tgid, SI_TKILL_, true);
}
dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill_common(tid, sig, 0, SI_TKILL_, true);
}

dword_t sys_rt_sigqueueinfo(pid_t_ pid, dword_t sig, addr_t uinfo_addr) {
    return sys_rt_sigqueueinfo_guest(pid, sig, uinfo_addr);
}

dword_t sys_rt_sigqueueinfo_guest(pid_t_ pid, dword_t sig, guest_addr_t uinfo_addr) {
    if (pid <= 0 || sig <= 0 || sig >= NUM_SIGS)
        return _EINVAL;

    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;

    info.sig = sig;
    info.sig_errno = 0;
    info.code = SI_QUEUE_;
    info.rt.pid = current->pid;
    info.rt.uid = current->uid;

    // Process-directed, exactly as kill(2) is: Linux routes rt_sigqueueinfo
    // through kill_proc_info/group_send_sig_info, so any thread of the target
    // that can take the signal is a legitimate destination. Queueing straight
    // into the resolved task's private queue meant a sibling already parked in
    // sigwait()/sigtimedwait() -- the whole reason a program uses sigqueue --
    // waited out its timeout while the signal sat undeliverable beside it.
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(pid);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    task = pick_process_directed_target(task, sig);
    task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);

    err = queue_signal_task(task, sig, info);
    task_ref_cnt_mod(task, -1);
    return err;
}

dword_t sys_rt_tgsigqueueinfo_guest(pid_t_ tgid, pid_t_ tid, dword_t sig, guest_addr_t uinfo_addr) {
    if (tgid <= 0 || tid <= 0 || sig <= 0 || sig >= NUM_SIGS)
        return _EINVAL;

    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;

    info.sig = sig;
    info.sig_errno = 0;

    struct task *task = pid_get_task_ref(tid);
    if (task == NULL) {
        return _ESRCH;
    }
    if (task->tgid != tgid) {
        task_ref_cnt_mod(task, -1);
        return _ESRCH;
    }

    err = queue_signal_task(task, sig, info);
    task_ref_cnt_mod(task, -1);
    return err;
}

dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, dword_t sig, addr_t uinfo_addr) {
    return sys_rt_tgsigqueueinfo_guest(tgid, tid, sig, uinfo_addr);
}
