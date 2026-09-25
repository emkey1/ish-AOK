#include "debug.h"
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include "fs/poll.h"
#include "kernel/rseq.h"
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
#include "kernel/anonfd_ckpt.h"

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
static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info,
        bool from_timer);

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

// riscv64 has no SA_RESTORER, so its struct sigaction has no restorer field
// (asm-generic's layout without __ARCH_HAS_SA_RESTORER). Read as the amd64
// layout, sa_mask came from the word after it -- in musl's k_sigaction an
// uninitialised pad -- and a handler ran with whatever the stack held there,
// typically the mask of the previous sigaction call.
struct sigaction_riscv64_marshaled {
    qword_t handler;
    qword_t flags;
    sigset_t_ mask;
} __attribute__((packed));
static_assert(sizeof(struct sigaction_riscv64_marshaled) == 24, "riscv64 sigaction layout mismatch");

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
    // read garbage before the arm64 frame support landed. riscv64 does not
    // define it, and has a layout of its own.
    if (task->abi == GUEST_ABI_RISCV64) {
        struct sigaction_riscv64_marshaled user_action;
        if (user_get(user_addr, user_action))
            return _EFAULT;
        *action = (struct sigaction_) {
            .handler = user_action.handler,
            .flags = user_action.flags,
            .mask = user_action.mask,
        };
    } else if (guest_abi_is_64bit(task->abi)) {
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
    if (task->abi == GUEST_ABI_RISCV64) { // no restorer field, see sigaction_from_user
        struct sigaction_riscv64_marshaled user_action = {
            .handler = action->handler,
            .flags = action->flags,
            .mask = action->mask,
        };
        if (user_put(user_addr, user_action))
            return _EFAULT;
    } else if (guest_abi_is_64bit(task->abi)) { // arm64 shares the amd64 layout, see sigaction_from_user
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

// What a signal interrupting a wait says about restarting the syscall the wait
// belongs to (signal_wait_interruption), for wake_waiting_task to record:
// whether an ERESTARTSYS call restarts, and whether even an ERESTARTNOHAND one
// does, because no handler runs.
struct wait_interruption {
    bool restart;
    bool nohand;
};

static bool wake_waiting_task(struct task *task, const struct wait_interruption *interruption) {
    pthread_mutex_lock(&task->waiting_cond_lock.m);
    task->waiting_cond_lock.owner = pthread_self();

    cond_t *waiting_cond = task->waiting_cond;
    lock_t *waiting_lock = task->waiting_lock;
    bool *waiting_interrupt_flag = task->waiting_interrupt_flag;
    bool interrupted_wait = waiting_interrupt_flag != NULL ||
        (waiting_cond != NULL && waiting_lock != NULL);
    // Record the interruption before anything below wakes the task, while
    // holding waiting_cond_lock proves it is still inside the wait: it cannot
    // leave without taking this lock to unpublish the wait, and wait_for looks
    // at wait_interrupted only after that.
    //
    // It used to be recorded after the wake, once the sender had its
    // sighand->lock back, and by then the woken task could be through its
    // restart decision, its handler and into its next syscall. The late
    // wait_interrupted then cut that syscall's wait short with no signal
    // pending, and only a restart record left over from the syscall before
    // hid it, by restarting the call. Once every syscall began by clearing
    // that record (signal_restart_state_clear), the EINTR reached the guest:
    // an eventfd read under 500 SA_RESTART signals a second failed with EINTR
    // 3 to 9 times in 5 seconds, where Linux never does. A counter on both
    // halves showed each late record followed by a wait that began with
    // wait_interrupted set and no signal pending.
    //
    // And only for a wait that will look at it. wait_for_ignore_signals --
    // a vfork parent's wait, a group stop, a ptrace stop -- never consumes
    // wait_interrupted, so the mark outlived it and ended the task's NEXT
    // wait instead. Measured: SIGUSR1, handled with SA_RESTART, arriving while
    // a vfork parent waited made the parent's next eventfd read fail with
    // EINTR at once, 10 times out of 10; Linux, never. The signal itself is
    // still pending when such a wait ends, so the next wait sees it anyway.
    bool records = waiting_interrupt_flag != NULL ||
        (waiting_cond != NULL && waiting_lock != NULL && task->waiting_interruptible);
    if (records && interruption != NULL) {
        __atomic_store_n(&task->restart_interrupted_syscall, interruption->restart, __ATOMIC_RELEASE);
        __atomic_store_n(&task->restart_interrupted_syscall_nohand, interruption->nohand, __ATOMIC_RELEASE);
        __atomic_store_n(&task->wait_interrupted, true, __ATOMIC_RELEASE);
    }
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

// Discard every pending instance of the signals in `mask` from one queue --
// both the bitmask bits and any queued sigqueue entries (a signal lives in
// both, see deliver_signal_unlocked_locked / signal_take_next_locked): a
// task's own queue and pending set, or the process's (sighand->queue and
// ->pending). Caller holds the sighand->lock that protects them.
static void signal_flush_queue_locked(struct list *queue, sigset_t_ *pending, sigset_t_ mask) {
    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(queue, sigqueue, tmp, queue) {
        if (sigset_has(mask, sigqueue->info.sig)) {
            list_remove(&sigqueue->queue);
            free(sigqueue);
        }
    }
    *pending &= ~mask;
}

#define SIGNAL_STOP_MASK (sig_mask(SIGSTOP_) | sig_mask(SIGTSTP_) | \
        sig_mask(SIGTTIN_) | sig_mask(SIGTTOU_))

// What generating `sig` cancels, if it is a stop or continue signal.
static sigset_t_ signal_stop_cont_cancels(int sig) {
    if (sig == SIGCONT_)
        return SIGNAL_STOP_MASK;
    if (sig > 0 && sig < NUM_SIGS && sigset_has(SIGNAL_STOP_MASK, sig))
        return sig_mask(SIGCONT_);
    return 0;
}

// Linux prepare_signal() semantics: generating a continue (SIGCONT) or a stop
// (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU) signal mutually cancels the other kind that
// is still pending, at generation time -- on the process's queue and on every
// thread's own, whichever of them it is sent to. Without this a rapid
// SIGSTOP-then-SIGCONT pair races: the SIGCONT lifts group->stopped and wakes
// the group-stop wait (kernel/calls.c), but the still-queued SIGSTOP is then
// processed, sets group->stopped again, and the task blocks forever in the
// group-stop wait_for_ignore_signals() with no further SIGCONT coming. This
// intermittently wedged stress-ng --schedmix, which hammers its children with
// interleaved SIGSTOP/SIGCONT.
//
// The process's queue and `task`'s own, and the own queues of `members`, the
// process's threads (group_snapshot_take). send_signal is called with
// pids_lock held and so has no snapshot to pass: a stop or continue signal it
// sends leaves the other kind on the queues of threads it was not sent to.
// Only the kernel sends one that way -- ptrace's SIGSTOP, a resume's signal --
// and tkill, tgkill and rt_tgsigqueueinfo reach every thread
// (signal_prepare_stop_cont_threads). This cancelled on the target's own queue
// alone, which since kill() queues on the process is rarely where the other
// kind waits. Caller holds sighand->lock.
static void signal_prepare_stop_cont(struct sighand *sighand, struct task *task, int sig,
        struct task **members, size_t count) {
    sigset_t_ cancels = signal_stop_cont_cancels(sig);
    if (cancels == 0)
        return;
    signal_flush_queue_locked(&sighand->queue, &sighand->pending, cancels);
    if (task != NULL)
        signal_flush_queue_locked(&task->queue, &task->pending, cancels);
    for (size_t i = 0; i < count; i++) {
        if (members[i] != task)
            signal_flush_queue_locked(&members[i]->queue, &members[i]->pending, cancels);
    }
}

// Poke `task` so it re-checks pending signals: send SIGUSR1, poke its poll
// notify pipe, poke the JIT if it's spinning in guest code, and wake any
// pthread_cond it's parked in. Caller holds `sighand->lock` (either `task`'s
// own sighand, or -- for a process-directed signal -- the shared sighand of
// `task`'s whole thread group) and gets it back on return; the lock is only
// dropped around wake_waiting_task, which must not be called while holding it
// (see the AB-BA comment on signalfd_wakeup_task above).
// Drop every wake poke to tasks whose comm starts with this, simulating a lost
// poke for a test, so the recovery paths that exist for one (fs/poll.c's cap
// and unwedge, kernel/time.c's sleep slices) can be watched working.
// tests/manual/wake_poke_lost.c is the reader. The real fault this stood in
// for -- another thread's siglongjmp blocking SIGUSR1 in every thread, see
// util/sync.h -- is fixed, and tests/manual/wake_mask_isolation.c provokes it
// directly.
//
// Deliberately narrow: a comm PREFIX, never all tasks. Set it to something
// broad and the guest stops responding to signals, which is the fault, not a
// test of it.
static bool wake_poke_dropped_for(const struct task *task) {
    static const char *prefix = NULL;
    static int looked_up = 0;
    if (!looked_up) {
        prefix = getenv("ISH_TEST_LOSE_WAKE_POKES");
        looked_up = 1;
    }
    if (prefix == NULL || *prefix == '\0' || task == NULL)
        return false;
    return strncmp(task->comm, prefix, strlen(prefix)) == 0;
}

static bool signal_restart_decided_at_delivery(struct task *task, int sig);

// Whether `sig` interrupting one of `task`'s syscalls restarts it even as an
// ERESTARTNOHAND call -- poll, select, nanosleep, sigsuspend -- which only a
// handler running may end. That is, whether its delivery runs no handler.
//
// A job-control stop is not an interruption: Linux parks the task inside the
// wait and resumes it on SIGCONT. Nor is a signal the task ignores, with
// SIG_IGN or by default like SIGCHLD: get_signal() dequeues it, discards it
// and finds nothing to deliver, and the call restarts. Until it is delivered,
// the same goes for any signal a tracer sees first: see
// signal_restart_decided_at_delivery.
//
// An ignored signal used to count as an interruption, and it is not rare: a
// child's SIGCHLD is queued whenever the thread that forked it blocks it, as
// musl's fork() and pthread_exit() block every signal while they run, and the
// queued signal wakes another thread of the process to discard it (every
// other thread, until deliver_signal_to_group_locked told just one). Each
// failed its wait with EINTR where Linux carries on -- a nanosleep, a poll, a
// futex, a wait4, sigsuspend.
//
// Callers keep out a signal the native shim holds a handler for: its
// disposition here is only a placeholder. Call with sighand->lock held.
static bool signal_restarts_nohand(struct task *task, struct sighand *sighand, int sig) {
    int action = signal_action(sighand, sig);
    return action == SIGNAL_STOP || action == SIGNAL_IGNORE ||
        signal_restart_decided_at_delivery(task, sig);
}

// What `sig` interrupting one of `task`'s waits says about restarting the
// syscall the wait belongs to. Call with sighand->lock held: it reads the
// disposition.
static struct wait_interruption signal_wait_interruption(struct task *task,
        struct sighand *sighand, int sig) {
    // Only a handler actually running can turn a wait into a guest-visible
    // EINTR, which is why even the interfaces SA_RESTART cannot rescue (poll,
    // select) resume when no handler runs: see signal_restarts_nohand.
    //
    // sighand->action is not the truth for a signal the native shim is holding
    // a handler for -- what sits there is the SIG_DFL placeholder
    // nlibc_set_disposition left behind, so a native program's own SIGTSTP
    // handler would read as SIGNAL_STOP here and park a poll() that Linux
    // interrupts. For those the shim's recorded flags are the answer, and a
    // handler is always what runs, so `nohand` is false by construction.
    bool held = sigset_has(__atomic_load_n(&task->native_held, __ATOMIC_ACQUIRE), sig);
    bool nohand = !held && signal_restarts_nohand(task, sighand, sig);
    bool restart = held
        ? sigset_has(__atomic_load_n(&task->native_restart, __ATOMIC_ACQUIRE), sig)
        : (nohand || (signal_action(sighand, sig) == SIGNAL_CALL_HANDLER &&
            !!(sighand->action[sig].flags & SA_RESTART_)));
    return (struct wait_interruption) {.restart = restart, .nohand = nohand};
}

static void signal_wake_task(struct task *task, struct sighand *sighand, int sig) {
    // Worked out now, while sighand->lock is held; the wake below drops it.
    struct wait_interruption interruption = signal_wait_interruption(task, sighand, sig);
    if (task == current) {
        wake_waiting_task(task, &interruption);
        return;
    }

    if (wake_poke_dropped_for(task))
        return;   // as if every poke below had been swallowed

    // Nothing to break out of yet, and nobody to poke. Until task_start a
    // task's `thread` is its PARENT's, copied by task_create_ -- or, for pid 1
    // in the app, nothing at all until the entry point starts it -- so the
    // pokes below would land on another thread, or hand pthread_kill a thread
    // that is not there, which is undefined. A checkpoint restore makes this
    // ordinary rather than rare: it re-arms a process's timers before the app
    // has started pid 1, and one already due fires at once. The signal is
    // queued; the task's first pass through the emulator takes it, and the
    // poke of its own cpu_state makes that pass come at once.
    if (!atomic_load_explicit(&task->host_thread_started, memory_order_acquire)) {
        if (task->cpu.poked_ptr)
            cpu_poke(&task->cpu);
        return;
    }

    int wake_err = pthread_kill(task->thread, SIGUSR1);
    // Second, independent poke. The SIGUSR1 above was not reliable: on Darwin it
    // was intermittently left blocked and pending in the target thread's own
    // mask with sigusr1_handler never running, after which that thread was deaf
    // to every later SIGUSR1. (The cause was another thread's siglongjmp, which
    // on Darwin sets every thread's mask -- fixed in util/sync.h; this stays as
    // the second way in.) A target parked in a host syscall then finishes the
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
    wake_waiting_task(task, &interruption);
    unlock(&sighand->wake_lock);
    pthread_mutex_lock(&sighand->lock.m);
    sighand->lock.owner = pthread_self();
}

// Wake a task out of whatever it is blocked in, with no signal involved.
//
// For kernel/checkpoint.c's freezer. It is signal_wake_task's body without the
// signal: the same two pokes, for the same reason -- pthread_kill(SIGUSR1) to
// break a HOST syscall, and wake_waiting_task to break a wait parked on a
// cond_t. Neither alone is enough (see the note above about Darwin swallowing
// SIGUSR1), and a freeze that misses one task is a freeze that does not
// happen.
//
// No sighand lock: this takes none of the group's locks and delivers nothing,
// so it cannot be the ABBA hazard signal_wake_task has to dance around.
void task_wake_for_freeze(struct task *task) {
    if (task == NULL || task == current)
        return;
    // A task with no live host thread must not be poked. pthread_kill on a
    // pthread_t whose thread has exited is UNDEFINED, not a no-op -- and a
    // freezer walks a snapshot, which holds the struct task alive but says
    // nothing about the thread behind it. A `sleep` that finished between the
    // snapshot and the wake took the whole app down here, silently and
    // instantly, which read as "the wake never returned".
    //
    // host_thread_started is the flag task_start sets once the thread really
    // exists; zombie and exiting cover the other end of its life.
    if (!atomic_load_explicit(&task->host_thread_started, memory_order_acquire) ||
            task->zombie || task->exiting ||
            atomic_load_explicit(&task->exit_finished, memory_order_acquire))
        return;
    // ISH_CHECKPOINT_LOSE_WAKES=1: every wake below is lost, and the freeze is
    // announced only by ckpt_freeze_wanted. On macOS these wakes land, so a
    // blocking path that relies on them freezes on the CLI and refuses on a
    // device, where they can be lost. With this set the CLI fails the same way,
    // so such a path can be found and its fix proven where it can be run.
    static int lose_wakes = -1;
    if (lose_wakes < 0) {
        const char *v = getenv("ISH_CHECKPOINT_LOSE_WAKES");
        lose_wakes = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    if (lose_wakes)
        return;
    __atomic_store_n(&task->wait_interrupted, true, __ATOMIC_RELEASE);
    pthread_kill(task->thread, SIGUSR1);
    cpu_poke(&task->cpu);
    if (task->sighand != NULL) {
        lock(&task->sighand->wake_lock, 0);
        wake_waiting_task(task, NULL);
        unlock(&task->sighand->wake_lock);
    }
}

// Wake a task that now owes its tracer a PTRACE_EVENT_STOP (ptrace.trap_stop),
// so that it reaches the checkpoint where it stops -- out of guest code, a host
// call or a cond_t wait, as a signal would get it out of them.
//
// The pokes signal_wake_task makes, and nothing that depends on a signal: no
// restart record and no wait_interrupted. The waits it breaks find
// task_trap_stop_pending where they look for a pending signal, and the restart
// predicates ask the flag directly -- a stop runs no handler, so the syscall
// restarts. Not signal_wake_task itself, whose work is decided by the signal it
// is given, and there is none.
//
// The caller holds a reference on `task` and on `sighand`, taken under
// pids_lock -- do_exit clears ->sighand under that lock and releases it after
// -- and holds no lock now, since the wake can block on the lock the task is
// waiting under.
void task_wake_for_ptrace_trap(struct task *task, struct sighand *sighand) {
    lock(&sighand->lock, 0);
    // Until task_start, task->thread is still the parent's pthread; a task
    // that has not run yet looks for the flag before its first instruction
    // (task_thread). One on its way out has nothing left to stop for, and
    // poking a thread that has exited is undefined.
    bool live = atomic_load_explicit(&task->host_thread_started, memory_order_acquire) &&
        !task->zombie && !task->exiting &&
        !atomic_load_explicit(&task->exit_finished, memory_order_acquire);
    if (live) {
        // A host call, both pokes: see signal_wake_task for why one is not
        // enough. The notify pipe's fd is read under sighand->lock.
        pthread_kill(task->thread, SIGUSR1);
        pthread_kill(task->thread, SIGUSR2);
        poll_notify_poke(task->poll_notify_fd);
        // Guest code.
        if (task->cpu.poked_ptr)
            cpu_poke(&task->cpu);
    }
    unlock(&sighand->lock);
    if (live) {
        // A cond_t wait. Without sighand->lock, as signal_wake_task does it.
        lock(&sighand->wake_lock, 0);
        wake_waiting_task(task, NULL);
        unlock(&sighand->wake_lock);
    }
}

// Whether a signal that interrupted a syscall can say yet if that syscall
// restarts. Its disposition normally does -- a handler with SA_RESTART restarts
// it, one without gives EINTR, a stop restarts it -- but not for a signal a
// tracer sees first.
//
// (PTRACE_INTERRUPT was the other kind while it queued a real SIGTRAP: read by
// SIGTRAP's disposition, terminate, it meant "no restart", and a read() blocked
// on a pipe failed with EINTR once the tracer resumed it, 10 runs out of 10,
// where Linux 6.12 restarted it 10 out of 10. The interrupt is now
// ptrace.trap_stop, a flag with no disposition at all, and the restart
// predicates answer for it with task_trap_stop_pending.)
//
// A signal a tracer sees first. receive_signals turns it into a
// signal-delivery-stop, and the tracer chooses what happens next. Resumed with
// no signal, nothing is delivered and Linux restarts the call -- that is gdb's
// ^C and `continue` on a program waiting for input, and AOK failed the read.
// Injected, its handler runs, and the call restarts only if that handler allows
// it. So both say "restart" here, and receive_signal cancels the restart if a
// handler that does not allow it runs first: Linux's rule, applied where Linux
// applies it, in handle_signal. SIGKILL is never stopped for, and neither is a
// signal the tracer already chose to deliver (ptrace.deliver_sig); their
// dispositions still decide.
//
// Measured before this, on Linux 6.12 and an arm64 Devuan guest, with the call
// blocked and then stopped by PTRACE_INTERRUPT or by a SIGUSR1 or SIGINT the
// tracer resumed without: read, recv, accept, open of a FIFO, a write to a full
// pipe, eventfd, waitpid, waitid, futex, F_SETLKW, poll, select, nanosleep and
// clock_nanosleep all carried on under Linux and all failed with EINTR under
// AOK. epoll_wait and sigtimedwait fail with EINTR on both, and still do here:
// they never restart.
//
// Callers exclude a signal the native shim holds a handler for. receive_signals
// never dequeues one, so no tracer ever sees it.
static bool signal_restart_decided_at_delivery(struct task *task, int sig) {
    return signal_stops_for_tracer(task, sig);
}

bool signal_stops_for_tracer(struct task *task, int sig) {
    return task->ptrace.traced && sig != SIGKILL_ && sig != task->ptrace.deliver_sig;
}

// Linux's sig_ignored: "Tracers may want to know about even ignored signal
// unless it is SIGKILL". A signal a traced task ignores -- SIG_IGN, or by
// default like SIGCHLD, SIGCONT, SIGWINCH and SIGURG -- is queued all the same,
// the tracer is shown it at a signal-delivery-stop, and it is discarded only if
// the tracer then delivers it. AOK dropped it as it was sent, so a tracer never
// heard of one: `strace -f sh -c 'ls'` printed no "--- SIGCHLD ---" line, and
// strace's own start-up, which sends the SIGCONT that lifts its tracee's stop,
// saw no SIGCONT where Linux 6.12 stops for it (status 0x127f) every time.
static bool signal_traced_not_ignored(struct task *task, int sig) {
    return task->ptrace.traced && sig != SIGKILL_;
}

// Put one signal on a queue, a POSIX timer's own or not (struct sigqueue).
// Caller holds sighand->lock.
static void signal_enqueue_locked(struct list *queue, sigset_t_ *pending, int sig,
        struct siginfo_ info, bool from_timer) {
    struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
    sigqueue->info = info;
    sigqueue->info.sig = sig;
    sigqueue->from_timer = from_timer;
    list_add_tail(queue, &sigqueue->queue);
    sigset_add(pending, sig);
}

// `from_timer`: a POSIX timer's expiry, queued whatever else of its number is
// pending (struct sigqueue in kernel/signal.h).
static void deliver_signal_unlocked_locked(struct task *task, struct sighand *sighand, int sig,
        struct siginfo_ info, bool from_timer) {
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
    //
    // A POSIX timer's own signal is the exception, as on Linux: send_sigqueue
    // queues the timer's preallocated sigqueue with no legacy_queue check,
    // behind a SIGALRM that kill() or an itimer left pending, and a second
    // timer on the same signal queues its own too. Measured on 6.12: an
    // itimer's SIGALRM pending, then a POSIX timer's, and sigtimedwait takes
    // two, si_code SI_KERNEL then SI_TIMER. Only the timer's OWN signal still
    // queued stops it, and that is an overrun (signal_timer_count_overrun).
    bool already_pending = !signal_is_realtime(sig) && sigset_has(task->pending, sig);
    if (!already_pending || from_timer)
        signal_enqueue_locked(&task->queue, &task->pending, sig, info, from_timer);
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

    signal_wake_task(task, sighand, sig);
}

// ---- which thread takes a signal sent to the whole process ------------------
//
// Linux queues such a signal once, for the process (shared_pending), and tells
// ONE thread about it: complete_signal picks the thread it was sent to if that
// one can take it and otherwise another that can, and wakes that thread alone.
// Every other thread carries on, its syscalls uninterrupted.
//
// AOK woke every thread that did not block the signal, and every one of them
// counted it as its own. They raced for it: a SIGCHLD handler ran in whichever
// sibling got there first rather than in the thread the child's exit was sent
// to, and the losers' waits had ended anyway and failed with EINTR. Measured
// with a SIGCHLD handler without SA_RESTART, the forking thread in waitpid and
// twelve siblings parked in clock_nanosleep, nanosleep, ppoll, pselect6 and
// pipe reads: on alpine-amd64-test the handler ran in a sibling and siblings'
// calls failed with EINTR in 20 rounds of 20. Linux 6.12 ran it in the forking
// thread and touched no sibling, 40 rounds of 40.
//
// Waking one thread was not enough by itself, because the others noticed the
// signal on their own: every wait, and every syscall's way out, asked whether
// the shared queue held anything unblocked, and a host sleep asks that every
// 50ms. So a thread now looks at the shared queue only once it has been told to
// (group_sigpending, Linux's TIF_SIGPENDING; task_group_pending in
// kernel/signal.h). And nothing can be left there unseen: a thread told to take
// a signal that then blocks it or exits hands it to one that can
// (group_signal_retarget), and a thread whose mask lets a queued one through
// tells itself (group_pending_mask_changed_locked).
//
// Every signal sent to a process comes this way (send_signal_to_process): a
// child's exit, stop and continue, the interval and POSIX timers, kill() of a
// pid, a process group or everything, sigqueue, pidfd_send_signal, the
// terminal's signals and SIGWINCH, and a tracer's SIGCHLD. Only the child's
// exit and the interval timers did at first; the rest were put on one
// thread's own queue (see signal_send_checked).

// Whether `task` can take `sig` off the shared queue now: it does not block it,
// or waits for it in sigtimedwait, and it is not on its way out. Linux's
// wants_signal, less its preference for a thread with nothing else pending,
// which only spreads the work around. Call with sighand->lock held.
//
// A thread that cannot take it must never be told. Telling wakes it, and the
// wake marks its wait interrupted -- wake_waiting_task stores through
// waiting_interrupt_flag and sets wait_interrupted -- which futex(FUTEX_WAIT*)
// and rt_sigtimedwait turn straight into a guest-visible EINTR without asking
// whether anything is deliverable to that thread. foot's render workers block
// every signal and park in an unchecked sem_wait(), and one child's SIGCHLD
// EINTR'd all of them into popping an empty work queue (SIGSEGV at 0x8). Same
// test as deliver_signal_unlocked_locked's.
static bool group_signal_takes_locked(struct task *task, int sig) {
    if (task->exiting || task->zombie || task->sighand == NULL)
        return false;
    return !(sigset_has(task_wake_blocked(task) & ~task->waiting, sig) &&
            signal_is_blockable(sig) && !signal_is_synchronous_trap(sig));
}

// The thread to tell about `sig`, just queued for the process: Linux's
// complete_signal. The one it was sent to if that one can take it -- for a
// child's exit the thread that forked it, for an interval timer the leader,
// for kill() the thread the pid names -- and otherwise the first that can. A
// thread held in a ptrace stop comes last, as Linux passes it over: only its
// tracer can let it go. NULL when no thread can take it now. It stays queued,
// and whichever thread unblocks it first tells itself.
static struct task *group_signal_pick_locked(struct task *target, int sig,
        struct task **members, size_t count) {
    for (int pass = 0; pass < 2; pass++) {
        bool stopped_too = pass == 1;
        if (group_signal_takes_locked(target, sig) &&
                (stopped_too || !target->ptrace.stopped))
            return target;
        for (size_t i = 0; i < count; i++) {
            struct task *task = members[i];
            if (task != target && group_signal_takes_locked(task, sig) &&
                    (stopped_too || !task->ptrace.stopped))
                return task;
        }
    }
    return NULL;
}

// A thread already told to take the shared queue that can take `sig`. A second
// instance of a standard signal is not queued, and Linux wakes nobody for it;
// here the thread told about the first is poked again (see already_pending
// below), rather than a second thread being told about the same signal.
static struct task *group_signal_told_locked(int sig, struct task **members, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct task *task = members[i];
        if (__atomic_load_n(&task->group_sigpending, __ATOMIC_ACQUIRE) &&
                group_signal_takes_locked(task, sig))
            return task;
    }
    return NULL;
}

// Whether `task` taking `sig` ends the process. Linux's complete_signal then
// starts the group exit at once and wakes every thread, so that none runs on
// while the one it picked gets there; here every thread that can take it is
// told, and the first to do so takes the group down (do_exit_group). Not a
// thread that blocks it -- a sigtimedwait waiter, asking for it by name -- nor
// a traced one, whose tracer decides.
static bool group_signal_fatal_locked(struct sighand *sighand, struct task *task, int sig) {
    return signal_action(sighand, sig) == SIGNAL_KILL &&
        !sigset_has(task->blocked, sig) && !task->ptrace.traced;
}

// Tell `task` to take what is on the shared queue, and wake it for `sig`.
// Drops and retakes sighand->lock (signal_wake_task).
static void group_signal_tell_locked(struct task *task, struct sighand *sighand, int sig) {
    // Before the wake, which the task answers by looking.
    __atomic_store_n(&task->group_sigpending, true, __ATOMIC_RELEASE);
    signal_wake_task(task, sighand, sig);
}

// current's recalc_sigpending, for the shared queue: told to take it exactly
// when something queued there is something it can take. For a thread that has
// just dequeued, or changed its mask. Call with sighand->lock held.
static void group_pending_recalc_locked(void) {
    bool takes = (current->sighand->pending & ~task_wake_blocked(current)) != 0;
    __atomic_store_n(&current->group_sigpending, takes, __ATOMIC_RELEASE);
}

// current's mask just changed; `old` is what task_wake_blocked said before.
// Linux's __set_task_blocked. A thread whose mask now lets a queued process
// signal through is told to take it, which is how a thread that UNBLOCKS such a
// signal finds it when nobody could take it as it was sent. And what the thread
// was told to take and now blocks is set aside for signal_group_handoff to give
// to a sibling, which a caller does once it holds no lock -- that needs
// pids_lock. Call with sighand->lock held.
static void group_pending_mask_changed_locked(sigset_t_ old) {
    sigset_t_ queued = current->sighand->pending;
    if (__atomic_load_n(&current->group_sigpending, __ATOMIC_ACQUIRE))
        current->group_handoff |= queued & task_wake_blocked(current) & ~old;
    group_pending_recalc_locked();
}

// Deliver a process-directed signal into the thread group's shared queue
// (Linux's shared_pending), and tell one thread that can take it to do so.
// Contrast deliver_signal_unlocked_locked, which targets one specific task's
// own per-thread queue. `target` is the thread the signal was sent to (Linux's
// `p` in __send_signal_locked), and `members`/`count` is a pre-collected,
// ref-counted snapshot of the group's live threads (group_snapshot_take):
// walking tgroup->threads needs pids_lock, and this runs under sighand->lock,
// so the snapshot has to happen first (pids_lock -> sighand->lock, never the
// reverse). Caller holds `sighand->lock`.
static void deliver_signal_to_group_locked(struct sighand *sighand, struct task *target,
        int sig, struct siginfo_ info, bool from_timer, struct task **members, size_t count) {
    // A signal whose disposition ignores it is dropped here, as Linux's
    // sig_ignored() drops it, unless the thread it was SENT TO blocks it or
    // waits for it in sigtimedwait: a handler may be installed by the time it
    // is unblocked. SIGCHLD at SIG_DFL, sent by every child exit, is the
    // common case, and a queued one is taken, and discarded, by a thread told
    // to take it.
    //
    // Only the target's mask counts, never the rest of the group's. For a
    // child's exit the target is the thread that forked it
    // (exit_notify_process_locked). This used to queue the signal if ANY
    // thread blocked it, and musl blocks every signal in a thread for the
    // whole of fork() and pthread_exit(), glibc around pthread_create's clone
    // -- so a child dying while some sibling forked or exited had its SIGCHLD
    // queued, and the woken threads failed their waits with EINTR. Measured,
    // with one thread blocking SIGCHLD: a sibling's 2 s clock_nanosleep failed
    // at 0.3 s, when a child exited, where Linux 6.12 sleeps the 2 s.
    //
    // The raw ->blocked, not task_wake_blocked(): a native program's handler
    // is held by the shim with the signal blocked and the disposition left at
    // SIG_DFL, and that blocked bit is what keeps its SIGCHLD from being
    // dropped as ignored.
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE &&
        !signal_traced_not_ignored(target, sig);
    if (ignored && !sigset_has(target->blocked | target->waiting, sig))
        return;

    // See the matching comment in deliver_signal_unlocked_locked: skipping the
    // requeue for an already-pending standard signal is correct (Linux
    // doesn't queue multiple instances either), but skipping the wake too --
    // as this used to do via an unconditional early return -- can strand
    // every member of the group. This is the SIGCHLD path for a burst of
    // near-simultaneous child exits (send_signal_to_process), which is exactly
    // where the race is easy to hit: one child's SIGCHLD sets the pending bit
    // and wakes the parent, the parent's sigsuspend()/wait4() loop reaps that
    // child and goes back to sleep, and a second child exits and delivers
    // SIGCHLD while the first occurrence's pending bit hasn't been cleared by
    // receive_signals() yet -- that second, distinct occurrence must still
    // wake the parent even though it doesn't get its own queue entry.
    //
    // A POSIX timer's own signal is queued regardless, as in
    // deliver_signal_unlocked_locked.
    bool already_pending = !signal_is_realtime(sig) && sigset_has(sighand->pending, sig);
    if (!already_pending || from_timer)
        signal_enqueue_locked(&sighand->queue, &sighand->pending, sig, info, from_timer);

    // Every signalfd watching for it, in whichever thread: Linux's
    // signalfd_notify wakes them all, whether or not a thread takes it.
    for (size_t i = 0; i < count; i++)
        signalfd_wakeup_task(members[i], sig);

    struct task *taker = already_pending ?
        group_signal_told_locked(sig, members, count) : NULL;
    if (taker == NULL)
        taker = group_signal_pick_locked(target, sig, members, count);
    if (taker == NULL)
        return;
    if (group_signal_fatal_locked(sighand, taker, sig)) {
        for (size_t i = 0; i < count; i++) {
            if (group_signal_takes_locked(members[i], sig))
                group_signal_tell_locked(members[i], sighand, sig);
        }
        return;
    }
    group_signal_tell_locked(taker, sighand, sig);
}

// The live threads of `group`, each with a reference, and their sighand, with
// one. Walking group->threads needs pids_lock, and what is done with the
// threads needs sighand->lock, which comes after it (pids_lock ->
// sighand->lock, never the reverse), so the list is taken first. With `target`,
// also a reference on *target, which is the group's leader if it was NULL. And
// one on the leader, which keeps `group` itself: a group goes with its leader
// (task_free_final). false, holding nothing, if the list could not be
// allocated. Caller holds pids_lock.
struct group_snapshot {
    struct task *stack[32];
    struct task **members;
    size_t count;
    struct sighand *sighand;
    struct task *leader;
};

static bool group_snapshot_take_locked(struct tgroup *group, struct task **target,
        struct group_snapshot *snap) {
    snap->members = snap->stack;
    snap->count = 0;
    snap->sighand = NULL;
    snap->leader = NULL;
    size_t needed = 0;
    struct task *task;
    list_for_each_entry(&group->threads, task, group_links)
        needed++;
    if (needed > sizeof(snap->stack) / sizeof(snap->stack[0])) {
        snap->members = malloc(sizeof(*snap->members) * needed);
        if (snap->members == NULL)
            return false;
    }

    list_for_each_entry(&group->threads, task, group_links) {
        if (task->zombie || task->exiting || task->sighand == NULL)
            continue;
        if (snap->sighand == NULL) {
            snap->sighand = task->sighand;
            sighand_retain(snap->sighand);
        }
        task_ref_cnt_mod(task, 1);
        snap->members[snap->count++] = task;
    }
    snap->leader = group->leader;
    if (snap->leader != NULL)
        task_ref_cnt_mod(snap->leader, 1);
    if (target != NULL) {
        // A leader that has exited stays the group's leader until the last
        // thread goes, so this is never NULL while there is a member to wake.
        if (*target == NULL)
            *target = group->leader;
        if (*target != NULL)
            task_ref_cnt_mod(*target, 1);
    }
    return true;
}

static bool group_snapshot_take(struct tgroup *group, struct task **target,
        struct group_snapshot *snap) {
    complex_lockt(&pids_lock, 0);
    bool taken = group_snapshot_take_locked(group, target, snap);
    unlock(&pids_lock);
    return taken;
}

static void group_snapshot_release(struct group_snapshot *snap) {
    if (snap->sighand != NULL)
        sighand_release(snap->sighand);
    for (size_t i = 0; i < snap->count; i++)
        task_ref_cnt_mod(snap->members[i], -1);
    if (snap->leader != NULL)
        task_ref_cnt_mod(snap->leader, -1);
    if (snap->members != snap->stack)
        free(snap->members);
}

// The snapshot of *target's process, or of `*group` when there is no target,
// which then becomes the group's leader (group_snapshot_take_locked). A target
// must still be in the pid table: that is what keeps its process's group,
// which goes with its leader, and a leader is reaped only after its last
// thread. A reference is all a sender holds, and one reaped since was reaped
// with its process. *group is set to the process's group. Caller holds
// pids_lock.
static bool process_snapshot_take_locked(struct tgroup **group, struct task **target,
        struct group_snapshot *snap) {
    if (*target != NULL) {
        if (pid_get_task_zombie((*target)->pid) != *target || (*target)->group == NULL)
            return false;
        *group = (*target)->group;
    }
    return group_snapshot_take_locked(*group, target, snap);
}

// A SIGCONT resumes a stopped process, and a SIGKILL ends its stop, whichever
// queue it went to and whether or not it is queued at all: a SIGCONT the
// process ignores, as it does by default, still continues it. Once the signal
// is sent, with sighand->lock dropped.
static void signal_resume_group(struct tgroup *group, int sig) {
    if (sig != SIGCONT_ && sig != SIGKILL_)
        return;
    lock(&group->lock, 0);
    // A SIGCONT that actually resumes a stopped group is a reportable
    // "continued" event for a WCONTINUED waiter (man wait). SIGKILL also
    // clears the stop but is not a continue. The parent is woken from the
    // resumed task's own context (the group-stop loop), never from here, to
    // avoid notifying across the signal-sender's locks.
    if (sig == SIGCONT_ && group->stopped) {
        group->continued = true;
        group->continue_unannounced = true;
    }
    group->stopped = false;
    notify(&group->stopped_cond);
    unlock(&group->lock);
}

// Linux's ptrace_trap_notify, for a SIGCONT: every SEIZED tracee among the
// process's threads is told, whether or not the process was stopped, and owes
// its tracer a PTRACE_EVENT_STOP for it (ptrace_trap_notify, kernel/task.h),
// taken before the SIGCONT itself is. Measured on 6.12: a seized tracee in
// nanosleep reports 0x80057f and then the SIGCONT; one at a syscall stop
// reports its next stop, then 0x80057f, then the SIGCONT; one whose group-stop
// the tracer has been shown but not yet LISTENed to reports 0x80057f as soon
// as it is.
//
// Marked BEFORE the SIGCONT is queued, as prepare_signal marks it: a tracee
// woken by the signal must find the flag already there, or it takes the
// SIGCONT's delivery-stop first. Marked after, it did, in 2 of 15 runs of
// tests/manual/ptrace_strace_startup.c. Call with no lock held.
static void signal_cont_mark_tracees(struct task **members, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct task *task = members[i];
        lock(&task->ptrace.lock, 0);
        if (task->ptrace.traced && task->ptrace.seized)
            __atomic_store_n(&task->ptrace_trap_notify, true, __ATOMIC_RELEASE);
        unlock(&task->ptrace.lock);
    }
}

// ...and woken to take it, as signal_wake_up wakes an interruptible sleeper:
// the SIGCONT wakes only the thread that takes it. Not out of a ptrace stop,
// which Linux leaves alone unless the tracee is listening; that one takes it
// once resumed, and a PTRACE_LISTEN finds it owed and re-traps at once. Nor a
// tracee that has taken it already. Call with no lock held.
static void signal_cont_wake_tracees(struct task **members, size_t count,
        struct sighand *sighand) {
    if (sighand == NULL)
        return;
    for (size_t i = 0; i < count; i++) {
        struct task *task = members[i];
        lock(&task->ptrace.lock, 0);
        bool wake = task->ptrace.traced && task->ptrace.seized && !task->ptrace.stopped &&
            __atomic_load_n(&task->ptrace_trap_notify, __ATOMIC_ACQUIRE);
        unlock(&task->ptrace.lock);
        if (wake)
            task_wake_for_ptrace_trap(task, sighand);
    }
}

// A process-directed signal to `group`, sent to `target`, or to the group's
// leader when that is NULL: what send_signal does for one thread, for the
// process's queue. Cancels a pending stop or continue on every queue of the
// process, queues the signal on the process unless it is ignored, tells one
// thread that can take it (deliver_signal_to_group_locked), and lifts a stop.
// With `pids_locked`, the caller holds pids_lock.
static void send_process_signal(struct tgroup *group, struct task *target, int sig,
        struct siginfo_ info, bool pids_locked, bool from_timer) {
    if (sig == 0)
        return;
    struct group_snapshot snap;
    if (!pids_locked)
        complex_lockt(&pids_lock, 0);
    bool taken = process_snapshot_take_locked(&group, &target, &snap);
    if (!pids_locked)
        unlock(&pids_lock);
    if (!taken)
        return;
    if (snap.sighand != NULL && target != NULL) {
        if (sig == SIGCONT_)
            signal_cont_mark_tracees(snap.members, snap.count);
        lock(&snap.sighand->lock, 0);
        // Before the signal can be dropped as ignored: a SIGCONT at SIG_DFL
        // still cancels a pending stop.
        signal_prepare_stop_cont(snap.sighand, NULL, sig, snap.members, snap.count);
        deliver_signal_to_group_locked(snap.sighand, target, sig, info, from_timer,
                snap.members, snap.count);
        unlock(&snap.sighand->lock);
        signal_resume_group(group, sig);
        if (sig == SIGCONT_)
            signal_cont_wake_tracees(snap.members, snap.count, snap.sighand);
    }
    if (target != NULL)
        task_ref_cnt_mod(target, -1);
    group_snapshot_release(&snap);
}

// Hand the queued signals in `set`, which `from` was told to take and can no
// longer -- it has blocked them, or it is exiting -- to threads that can, as
// Linux's retarget_shared_pending does. Left with `from` they would sit unseen,
// since no other thread looks at the shared queue until it is told to. A thread
// already told covers what it can take without being woken again. Call with no
// lock held.
static void group_signal_retarget(struct task *from, sigset_t_ set) {
    struct group_snapshot snap;
    if (!group_snapshot_take(from->group, NULL, &snap))
        return;
    struct sighand *sighand = snap.sighand;
    if (sighand != NULL) {
        lock(&sighand->lock, 0);
        set &= sighand->pending;
        for (size_t i = 0; i < snap.count && set != 0; i++) {
            struct task *task = snap.members[i];
            if (task == from || task->exiting || task->zombie || task->sighand == NULL)
                continue;
            sigset_t_ takes = set & ~(task_wake_blocked(task) & ~task->waiting);
            if (takes == 0)
                continue;
            set &= ~takes;
            if (!__atomic_load_n(&task->group_sigpending, __ATOMIC_ACQUIRE))
                group_signal_tell_locked(task, sighand, __builtin_ctzll(takes) + 1);
        }
        unlock(&sighand->lock);
    }
    group_snapshot_release(&snap);
}

void signal_group_handoff(void) {
    if (current == NULL || current->group_handoff == 0)
        return;
    sigset_t_ set = current->group_handoff;
    current->group_handoff = 0;
    group_signal_retarget(current, set);
}

// Linux's exit_signals. Under the lock, with task->exiting already set: a
// delivery before this told the task and is seen here, and one after it sees
// the task exiting and tells another (group_signal_takes_locked).
void signal_exit_handoff(struct task *task) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL || task->group == NULL)
        return;
    lock(&sighand->lock, 0);
    sigset_t_ set = task->group_handoff;
    task->group_handoff = 0;
    if (__atomic_exchange_n(&task->group_sigpending, false, __ATOMIC_ACQ_REL))
        set |= sighand->pending & ~task_wake_blocked(task);
    unlock(&sighand->lock);
    if (set != 0)
        group_signal_retarget(task, set);
}

void send_signal_to_process(struct task *task, int sig, struct siginfo_ info) {
    send_process_signal(NULL, task, sig, info, false, false);
}

void send_signal_to_process_pids_locked(struct task *task, int sig, struct siginfo_ info) {
    send_process_signal(NULL, task, sig, info, true, false);
}

void send_signal_to_group(struct tgroup *group, int sig, struct siginfo_ info) {
    send_process_signal(group, NULL, sig, info, false, false);
}

// tkill, tgkill and rt_tgsigqueueinfo send a stop or continue signal to one
// thread, and Linux's prepare_signal cancels the other kind on every thread of
// the process. send_signal, which kernel callers reach holding pids_lock,
// cancels it on that thread's queue and the process's; this does the rest,
// first. Call with no lock held.
static void signal_prepare_stop_cont_threads(struct task *task, int sig) {
    if (signal_stop_cont_cancels(sig) == 0)
        return;
    struct group_snapshot snap;
    struct tgroup *group = NULL;
    struct task *target = task;
    complex_lockt(&pids_lock, 0);
    bool taken = process_snapshot_take_locked(&group, &target, &snap);
    unlock(&pids_lock);
    if (!taken)
        return;
    if (snap.sighand != NULL) {
        lock(&snap.sighand->lock, 0);
        signal_prepare_stop_cont(snap.sighand, NULL, sig, snap.members, snap.count);
        unlock(&snap.sighand->lock);
        // A SIGCONT sent to one thread notifies every seized thread of its
        // process, as one sent to the process does: Linux's prepare_signal
        // walks the whole thread group either way. The caller queues it after
        // this returns.
        if (sig == SIGCONT_) {
            signal_cont_mark_tracees(snap.members, snap.count);
            signal_cont_wake_tracees(snap.members, snap.count, snap.sighand);
        }
    }
    task_ref_cnt_mod(target, -1);
    group_snapshot_release(&snap);
}

// Every notice of a stop, a continue or a ptrace stop comes here, as every one
// on Linux goes through do_notify_parent_cldstop. They were sent as the
// child's EXIT signal, from three copies of the same few lines: a child cloned
// with SIGUSR1 announced its stops and continues as SIGUSR1 -- past a parent's
// SA_NOCLDSTOP, which was asked only of SIGCHLD -- and one cloned with 0
// announced none of them, and a tracer's stop notice was that signal with an
// empty siginfo. Linux sends SIGCHLD whatever the exit signal (measured on
// 6.12: only the exit uses it), and SIG_IGN suppresses it too, even when it is
// blocked and would otherwise be queued. To the process, where any thread
// that can take it does, as a child's exit is: it was put on the one thread's
// own queue, so a sibling in sigwaitinfo never saw it.
void notify_parent_cldstop(struct task *parent, struct siginfo_ info) {
    struct sighand *sighand = parent->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock, 0);
    struct sigaction_ *action = &sighand->action[SIGCHLD_];
    bool wanted = action->handler != SIG_IGN_ && !(action->flags & SA_NOCLDSTOP_);
    unlock(&sighand->lock);
    if (wanted)
        send_signal_to_process(parent, SIGCHLD_, info);
}

// `task`'s tracer, or NULL. Caller holds pids_lock.
static struct task *cldstop_tracer_locked(struct task *task) {
    if (!task->ptrace.traced)
        return NULL;
    return task->ptrace.tracer != NULL ? task->ptrace.tracer : task->parent;
}

// The tracer of a process's leader, when it is not in the parent's process:
// Linux tells it of a continue as well as the parent (get_signal's
// ptrace_reparented(group_leader) case). One in the parent's own process would
// be told the same thing twice. Caller holds pids_lock.
static struct task *cldstop_leader_tracer_locked(struct task *leader, struct task *parent) {
    struct task *tracer = cldstop_tracer_locked(leader);
    if (tracer == NULL || tracer->group == leader->group)
        return NULL;
    if (parent != NULL && tracer->group == parent->group)
        return NULL;
    return tracer;
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
    deliver_signal_unlocked_locked(task, sighand, sig, info, false);
    unlock(&sighand->lock);
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    deliver_signal_with_sighand(task, sighand, sig, info);
}

void signal_queue_before_start(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock, 0);
    if (signal_is_realtime(sig) || !sigset_has(task->pending, sig)) {
        struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
        if (sigqueue != NULL) {
            sigset_add(&task->pending, sig);
            sigqueue->info = info;
            sigqueue->info.sig = sig;
            sigqueue->from_timer = false;
            list_add_tail(&task->queue, &sigqueue->queue);
        }
    }
    unlock(&sighand->lock);
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

// ---- which queued signal is taken next --------------------------------------
//
// Linux takes a thread's queued signals in a fixed order, and so does every
// dequeuer here: receive_signals, which runs handlers; signal_take_next_locked,
// for sigtimedwait and signalfd; and signal_deciding_locked, which answers for
// an interrupted syscall with the signal whose handler will be set up first.
//
//   1. For delivery only, a SIGKILL, which ends the process before anything
//      else is taken (signal_next_deliverable_locked); then a signal an
//      instruction raised, oldest first: a synchronous signal
//      (signal_is_synchronous_trap) on the thread's own queue with a positive
//      si_code, which nothing but the kernel -- or a thread queueing to itself
//      -- can send. get_signal's dequeue_synchronous_signal, so that the frame
//      holding the faulting PC is the first one built, before another signal's
//      handler becomes the PC.
//   2. The thread's own queue, then the process's shared one: dequeue_signal
//      takes from tsk->pending and only then from signal->shared_pending.
//   3. Within a queue, a synchronous signal before any other, then the
//      lowest-numbered, and of one signal its oldest entry: next_signal, then
//      collect_signal.
//
// It shows because frames stack. The first signal taken gets the first frame,
// and its handler runs LAST, once every handler stacked on it has returned; and
// the first handler set up decides whether the syscall it cut short restarts.
// AOK took the lowest-numbered signal across both queues. With both blocked, an
// interval timer's SIGALRM, which is the process's, and raise(SIGTERM), which
// is the thread's, then one sigprocmask unblocking both: Linux 6.12 (x86_64,
// and i386 under -m32) runs SIGALRM's handler and then SIGTERM's, since
// SIGTERM was taken first, and AOK ran them the other way round. A raised
// SIGINT and SIGSEGV the same: Linux takes SIGSEGV first and runs SIGINT's
// handler first, and sigwaitinfo returns SIGSEGV first.

// Linux's next_signal as a rank, the lowest taken first: a synchronous signal
// before any other, then the lower number.
static int signal_take_rank(int sig) {
    return signal_is_synchronous_trap(sig) ? sig : NUM_SIGS + sig;
}

static bool signal_passed_over(struct sighand *sighand, int sig);

// Whether a scan that asks for `passed_over` leaves `sig` where it is: a
// signal_passed_over, which delivery takes and discards, is not the one that
// decides a restart. Notes that there was one.
static bool signal_skip_passed_over(struct sighand *sighand, int sig, bool *passed_over) {
    if (passed_over == NULL || !signal_passed_over(sighand, sig))
        return false;
    *passed_over = true;
    return true;
}

// Step 1 above: the oldest entry on current's own queue that an instruction
// raised, of the signals in `set`.
static struct sigqueue *signal_next_fault_locked(struct sighand *sighand, sigset_t_ set,
        bool *passed_over) {
    struct sigqueue *sigqueue;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        int sig = sigqueue->info.sig;
        if (!sigset_has(set, sig) || !signal_is_synchronous_trap(sig) ||
                sigqueue->info.code <= SI_USER_)
            continue;
        if (!signal_skip_passed_over(sighand, sig, passed_over))
            return sigqueue;
    }
    return NULL;
}

// Step 3 above, on one queue: the entry to take of those whose signal is in
// `set`. Each queue is in the order it was sent, so a strict < keeps the oldest
// entry of a signal. With `passed_over`, see signal_skip_passed_over.
static struct sigqueue *signal_next_in_locked(struct list *queue, struct sighand *sighand,
        sigset_t_ set, bool *passed_over) {
    struct sigqueue *sigqueue;
    struct sigqueue *next = NULL;
    list_for_each_entry(queue, sigqueue, queue) {
        int sig = sigqueue->info.sig;
        if (!sigset_has(set, sig) || signal_skip_passed_over(sighand, sig, passed_over))
            continue;
        if (next == NULL || signal_take_rank(sig) < signal_take_rank(next->info.sig))
            next = sigqueue;
    }
    return next;
}

// The signal current's delivery takes next of those `deliverable` lets
// through, all three steps above, and whether it is on the process's queue,
// which is looked at only `with_shared`. For receive_signals and, with
// `passed_over`, signal_deciding_locked, which must agree on it.
//
// A SIGKILL before any of them, from either queue. Linux's complete_signal
// starts the group exit as it is sent, and get_signal takes the thread down
// before it dequeues anything else, so no handler is set up for, and no
// tracer shown, a signal that happens to be ahead of it. Here it is queued
// like any other, and kill(pid, SIGKILL) queues it on the process's queue,
// behind every signal on the thread's own.
static struct sigqueue *signal_next_deliverable_locked(struct sighand *sighand,
        sigset_t_ deliverable, bool with_shared, bool *passed_over, bool *shared) {
    *shared = false;
    sigset_t_ sigkill = deliverable & sig_mask(SIGKILL_);
    struct sigqueue *next = signal_next_in_locked(&current->queue, sighand, sigkill, NULL);
    if (next == NULL && with_shared) {
        next = signal_next_in_locked(&sighand->queue, sighand, sigkill, NULL);
        *shared = next != NULL;
    }
    if (next != NULL)
        return next;
    next = signal_next_fault_locked(sighand, deliverable, passed_over);
    if (next == NULL)
        next = signal_next_in_locked(&current->queue, sighand, deliverable, passed_over);
    if (next == NULL && with_shared) {
        next = signal_next_in_locked(&sighand->queue, sighand, deliverable, passed_over);
        *shared = next != NULL;
    }
    return next;
}

// A POSIX timer's signal has been taken -- run as a handler, accepted by
// sigtimedwait, read from a signalfd. That is when Linux latches the count
// timer_getoverrun reports (posixtimer_rearm, from dequeue_signal), and it
// reports that count until the next one is taken or the timer is set again,
// not the one building on the signal queued behind it. Measured on 6.12: a
// signal taken with si_overrun 19, the next expiry queued, and
// timer_getoverrun still said 19 -- where here it had become 0 and then
// counted up, so a handler that outlived a period, asking how many it had
// missed, was told about the next signal instead of its own.
//
// Caller holds the sighand->lock the signal came off. The slot is read without
// group->lock; nothing is followed through it. Only the timer's own signal
// counts (struct sigqueue's from_timer), and a deleted timer's is disowned
// (signal_timer_disown), so a timer made in its slot since is not given its
// count. What this cannot tell apart, and Linux can (it checks which arming a
// signal came from), is a signal queued before the timer was set again:
// taking one latches its old count, where Linux leaves 0.
static void signal_timer_taken(struct task *task, const struct sigqueue *taken) {
    const struct siginfo_ *info = &taken->info;
    // rt_sigqueueinfo can claim SI_TIMER and any id; only the timer's own
    // signal latches a count, as only Linux's preallocated one does.
    if (!taken->from_timer || task->group == NULL)
        return;
    int_t id = info->timer.timer;
    if (id < 0 || id >= TIMERS_MAX)
        return;
    struct posix_timer *pt = &task->group->posix_timers[id];
    if (pt->timer == NULL || pt->signal != info->sig)
        return;
    __atomic_store_n(&pt->last_overrun, info->timer.overrun, __ATOMIC_RELAXED);
}

// Take the next of the signals in `mask` off `task`'s queues, as sigtimedwait
// and signalfd do: steps 2 and 3 above, which is Linux's dequeue_signal. Step
// 1 is get_signal's alone. Both queues: a signalfd or sigwaitinfo caller must
// see a signal sent to the process (e.g. SIGCHLD to a possibly-multithreaded
// parent, see send_signal_to_process) whichever thread it was told to.
static bool signal_take_next_locked(struct task *task, sigset_t_ mask, struct siginfo_ *info_out) {
    struct sighand *sighand = task->sighand;
    struct sigqueue *best = signal_next_in_locked(&task->queue, sighand, mask, NULL);
    bool best_is_group = false;
    if (best == NULL && sighand != NULL) {
        best = signal_next_in_locked(&sighand->queue, sighand, mask, NULL);
        best_is_group = best != NULL;
    }
    if (best == NULL)
        return false;
    *info_out = best->info;
    signal_timer_taken(task, best);
    int sig = best->info.sig;
    list_remove(&best->queue);
    if (best_is_group) {
        if (!signal_list_still_has_locked(&sighand->queue, sig))
            sigset_del(&sighand->pending, sig);
        // Taken by name, or by a signalfd: whatever this thread was told to
        // take may be gone now.
        if (task == current)
            group_pending_recalc_locked();
    } else if (!signal_still_pending_locked(task, sig)) {
        sigset_del(&task->pending, sig);
    }
    free(best);
    return true;
}

// Linux's siginfo_layout for a negative si_code: the sender's pid, uid and
// queued value (SIL_RT), for every one of them -- SI_QUEUE, SI_MESGQ,
// SI_ASYNCIO, SI_TKILL and the rest -- but SI_TIMER and SI_SIGIO, which have
// layouts of their own, and whatever the signal is: a sigqueue'd SIGSEGV or
// SIGCHLD carries its value too. Only SI_QUEUE's value used to be copied out,
// so an mq_notify signal (SI_MESGQ) arrived with si_value 0.
static bool siginfo_code_is_rt(int_t code) {
    return code < 0 && code != SI_TIMER_ && code != SI_SIGIO_;
}

static void siginfo_to_i386_user(struct i386_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    if (siginfo_code_is_rt(info->code)) {
        out->rt.pid = info->rt.pid;
        out->rt.uid = info->rt.uid;
        memcpy(&out->rt.value, &info->rt.value, sizeof(out->rt.value));
        return;
    }
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
            out->sigsys.arch = info->sigsys.arch;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                memcpy(&out->timer.value, &info->timer.value, sizeof(out->timer.value));
                out->timer._private = info->timer._private;
            } else {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
            }
            break;
    }
}

static void siginfo_to_amd64_user(struct amd64_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->sig = info->sig;
    out->sig_errno = info->sig_errno;
    out->code = info->code;
    if (siginfo_code_is_rt(info->code)) {
        out->rt.pid = info->rt.pid;
        out->rt.uid = info->rt.uid;
        out->rt.value = info->rt.value;
        return;
    }
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
            out->sigsys.arch = info->sigsys.arch;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->timer.timer = info->timer.timer;
                out->timer.overrun = info->timer.overrun;
                out->timer.value = info->timer.value;
                out->timer._private = info->timer._private;
            } else if (info->code >= CLD_EXITED_ && info->code <= CLD_CONTINUED_) {
                // A CLD_* code under some other signal: a child announcing
                // itself with the exit signal it was cloned with. Every sender
                // of one fills the child arm, and a 64-bit Linux copies the
                // whole siginfo out, so a SIGUSR1 handler reads si_status 9
                // for a child that exited 9. This handed it si_pid and si_uid
                // alone. (The i386 layout is left alone: Linux's -m32 view on
                // x86_64 reads si_status 0 as well.)
                out->child.pid = info->child.pid;
                out->child.uid = info->child.uid;
                out->child.status = info->child.status;
                out->child.utime = info->child.utime;
                out->child.stime = info->child.stime;
            } else {
                out->kill.pid = info->kill.pid;
                out->kill.uid = info->kill.uid;
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
        } else if (siginfo_code_is_rt(info->code)) {
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
        } else if (siginfo_code_is_rt(info->code)) {
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

    // Every negative code but SI_TIMER and SI_SIGIO: see siginfo_code_is_rt.
    // (This asked for SI_QUEUE and the codes below SI_TKILL, which missed
    // SI_MESGQ and SI_ASYNCIO -- both lie between the two.)
    bool rt_layout = siginfo_code_is_rt(info->code);

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
        out->arch = info->sigsys.arch;
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
// process-directed signal (e.g. SIGCHLD via send_signal_to_process) lives in the
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
    .name = "signalfd",
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

// An overrun count plus `expirations`, saturating at INT_MAX (DELAYTIMER_MAX).
static int_t timer_overrun_add(int_t overrun, uint64_t expirations) {
    uint64_t sum = (uint64_t) (overrun > 0 ? overrun : 0) + expirations;
    return sum > INT_MAX ? INT_MAX : (int_t) sum;
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
// `expirations` is how many to count: more than one when the timer's thread
// fell behind and delivers every period it missed at once (util/timer.c).
// The count saturates at INT_MAX, as Linux's does (DELAYTIMER_MAX).
//
// Returns the new overrun count if an entry for this timer was found and
// counted, or -1 if there was none and the caller should queue a signal.
int signal_timer_count_overrun(struct task *task, int sig, int timer_id, uint64_t expirations) {
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
        if (sigqueue->from_timer && sigqueue->info.sig == sig &&
                sigqueue->info.timer.timer == timer_id) {
            overrun = sigqueue->info.timer.overrun =
                timer_overrun_add(sigqueue->info.timer.overrun, expirations);
            goto out;
        }
    }
    list_for_each_entry(&sighand->queue, sigqueue, queue) {
        if (sigqueue->from_timer && sigqueue->info.sig == sig &&
                sigqueue->info.timer.timer == timer_id) {
            overrun = sigqueue->info.timer.overrun =
                timer_overrun_add(sigqueue->info.timer.overrun, expirations);
            goto out;
        }
    }
out:
    unlock(&sighand->lock);
    return overrun;
}

// A deleted POSIX timer's signal, still queued, is from now on an ordinary
// queued signal: taken like any other, but no timer's to count an overrun
// onto (signal_timer_count_overrun) or to latch a count from
// (signal_timer_taken). The next timer made in its slot has the same id and
// is not the same timer -- Linux gives each timer a sigqueue of its own.
// Call with no lock held.
void signal_timer_disown(struct tgroup *group, int timer_id) {
    struct group_snapshot snap;
    if (group == NULL || !group_snapshot_take(group, NULL, &snap))
        return;
    struct sighand *sighand = snap.sighand;
    if (sighand != NULL) {
        lock(&sighand->lock, 0);
        struct sigqueue *sigqueue;
        list_for_each_entry(&sighand->queue, sigqueue, queue) {
            if (sigqueue->from_timer && sigqueue->info.timer.timer == timer_id)
                sigqueue->from_timer = false;
        }
        for (size_t i = 0; i < snap.count; i++) {
            list_for_each_entry(&snap.members[i]->queue, sigqueue, queue) {
                if (sigqueue->from_timer && sigqueue->info.timer.timer == timer_id)
                    sigqueue->from_timer = false;
            }
        }
        unlock(&sighand->lock);
    }
    group_snapshot_release(&snap);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    send_signal_with_sighand(task, sighand, sig, info, false);
}

void send_timer_signal(struct task *task, bool to_thread, int sig, struct siginfo_ info) {
    if (!to_thread) {
        send_process_signal(NULL, task, sig, info, false, true);
        return;
    }
    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    send_signal_with_sighand(task, sighand, sig, info, true);
}

static void send_signal_with_sighand(struct task *task, struct sighand *sighand, int sig, struct siginfo_ info,
        bool from_timer) {
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
    signal_prepare_stop_cont(sighand, task, sig, NULL, 0);
    // A traced task ignores nothing but SIGKILL: its tracer is shown the
    // signal at delivery and decides (signal_traced_not_ignored).
    bool ignored = signal_action(sighand, sig) == SIGNAL_IGNORE &&
        !signal_traced_not_ignored(task, sig);
    bool synchronously_consumed = sigset_has(task->blocked | task->waiting, sig);
    if (should_trace_signal_task(task)) {
        printk("tracked signal send: target=%d tgid=%d comm=%s sig=%d ignored=%d sync=%d blocked=%#x waiting=%#x pending=%#x sender=%d/%s\n",
               task->pid, task->tgid, task->comm, sig, ignored, synchronously_consumed,
               task->blocked, task->waiting, task->pending,
               current != NULL ? current->pid : 0, current != NULL ? current->comm : "?");
    }
    if ((!ignored || synchronously_consumed) && (task->pid <= MAX_PID)) {
        deliver_signal_unlocked_locked(task, sighand, sig, info, from_timer);
    }
    unlock(&sighand->lock);
    signal_resume_group(task->group, sig);
}

// Both predicates consume both flags: a syscall asks exactly one of them, and
// leaving the other set would leak this interruption's answer into the next
// syscall's decision.
static bool restart_flags_take(bool nohand_only) {
    bool restart = __atomic_exchange_n(&current->restart_interrupted_syscall, false, __ATOMIC_ACQ_REL);
    bool nohand = __atomic_exchange_n(&current->restart_interrupted_syscall_nohand, false, __ATOMIC_ACQ_REL);
    return nohand_only ? nohand : restart;
}

// What an interruption leaves behind for a restart belongs to one syscall, and
// both dispatchers clear it as the next syscall starts. Two things:
//
// The restart record. Consuming it at the decision was never enough, because
// plenty of syscalls never make one. sigsuspend, pause, rt_sigtimedwait,
// msgrcv, msgsnd and semop handed their EINTR straight back, and a wait that
// COMPLETES -- a sigtimedwait that takes the signal it was waiting for, a wait4
// that reaps the child whose SIGCHLD interrupted it -- asks nothing at all.
// Each left its answer set, and the next syscall to be cut short by a signal
// without parking on a cond_t (a pipe read, which blocks in the host) read it
// before looking at the signal: a handler with no SA_RESTART restarted the
// read, and the guest got data that arrived 400ms later instead of EINTR.
// Measured with an SA_RESTART SIGALRM ending each of those calls and a plain
// SIGUSR1 at a pipe read after.
//
// restart_nohand_pending and restart_sys_pending, which say the PC was just
// rewound over a syscall, so a handler that runs before it re-executes may
// cancel the restart. On amd64 nothing cleared them when the re-executed call
// ended some other way: that dispatcher's restart backstop only ever sets them.
// The next handler then "cancelled" a restart that was no longer there,
// stepping the PC forward two bytes into the middle of whatever followed, and
// the guest died. Measured on x86_64 Alpine: SIGSTOP and SIGCONT inside a
// nanosleep, then a handled SIGUSR1 to end it, killed the process with SIGILL or
// SIGSEGV where Linux returns EINTR.
//
// A syscall's entry is late enough for all of it: a handler that cancels a
// restart runs before the rewound syscall re-executes, and the re-execution is
// the next syscall to start. poll_restart_valid and sleep_restart_valid are
// left alone; they carry a deadline INTO the re-executed call, which reads them.
void signal_restart_state_clear(void) {
    if (current == NULL)
        return;
    __atomic_store_n(&current->restart_interrupted_syscall, false, __ATOMIC_RELEASE);
    __atomic_store_n(&current->restart_interrupted_syscall_nohand, false, __ATOMIC_RELEASE);
    current->restart_nohand_pending = false;
    current->restart_sys_pending = false;
}

// Is SIG one the shim is holding a native handler for? Its entry in
// sighand->action is then the SIG_DFL placeholder nlibc_set_disposition left,
// so nothing about the program's real disposition can be read from there --
// ask struct task's native_restart instead.
static bool signal_native_held(int sig) {
    return sigset_has(__atomic_load_n(&current->native_held, __ATOMIC_ACQUIRE), sig);
}

static bool signal_native_restarts(int sig) {
    return sigset_has(__atomic_load_n(&current->native_restart, __ATOMIC_ACQUIRE), sig);
}

// A deliverable signal that does nothing when it is delivered: one the task
// ignores, with SIG_IGN or by default like SIGCHLD. Never a signal the shim
// holds a handler for, which always runs one whatever the placeholder
// disposition says, nor one a tracer sees first, which stops for the tracer.
// Call with sighand->lock held.
static bool signal_passed_over(struct sighand *sighand, int sig) {
    return !signal_native_held(sig) && !signal_stops_for_tracer(current, sig) &&
        signal_action(sighand, sig) == SIGNAL_IGNORE;
}

// The signal whose delivery decides whether an interrupted syscall restarts,
// or NULL: the one receive_signals will take first, in the order Linux takes
// them (signal_next_deliverable_locked). Call with sighand->lock held.
//
// Except that a signal_passed_over is passed over, as Linux's get_signal()
// dequeues such a signal, discards it and goes on to the next: it runs no
// handler, so it cannot be what decides. *ignored_seen says whether one was --
// with nothing else deliverable, that is what cut the syscall short, and the
// syscall restarts. Taking the lowest signal whatever it was let an ignored
// SIGCHLD decide "no restart" for every thread it woke. And a handled signal
// numbered above an ignored one must still decide for a native program, which
// has no handler-time cancel (receive_signal) to take back a restart promised
// on the ignored signal's word.
//
// "Deliverable" is task_wake_blocked() rather than plain ->blocked, which is
// the same question every other pending-signal predicate in the kernel asks
// (fs/real.c, fs/poll.c, fs/sock.c, kernel/futex.c). Using the raw blocked set
// here made the two halves disagree for a native program: the shim blocks
// every signal it has a handler for and runs the handler at a checkpoint
// instead, so the wait side counted it as pending and cut the syscall short
// while this side counted it as blocked, found nothing, and refused to
// restart. Native SA_RESTART was dead on arrival, and the interruption
// surfaced as a guest-visible EINTR -- "echo: write error: Interrupted system
// call" out of a native bash. For a translated guest native_held is 0 and this
// is the blocked set exactly as before.
//
// The process's shared queue only for a thread told to take it
// (task_group_pending): nothing there can have cut another thread's syscall
// short, and that thread's way out does not go looking there. *shared says the
// signal found is on that queue.
static struct sigqueue *signal_deciding_locked(struct sighand *sighand, bool *ignored_seen,
        bool *shared) {
    bool told = __atomic_load_n(&current->group_sigpending, __ATOMIC_ACQUIRE);
    *ignored_seen = false;
    return signal_next_deliverable_locked(sighand, ~task_wake_blocked(current), told,
            ignored_seen, shared);
}

// Whether the deciding signal is one this thread may never deliver: it is on
// the process's queue, where any sibling that can take it may take it first --
// between this decision and this thread's own delivery, since the lock is
// dropped in between. Linux decides at delivery (handle_signal turns
// ERESTARTNOHAND or ERESTARTSYS into EINTR only when a handler is set up), and
// so must this: promise the restart, and receive_signal cancels it if the
// handler really runs here. Deciding "the handler will run, so EINTR" now was
// wrong whenever the sibling won: the call failed with EINTR and no handler
// ran in its thread. Measured on an M4 iPad with
// tests/manual/signal_process_wake_one.c -- "handed on" and "lost the race",
// a sigsuspend or a pipe read waking at the child's exit with EINTR, in 8 of
// 12 runs from a fresh boot, where Linux carries on waiting.
//
// Not for a signal the native shim holds a handler for: a native program has
// no handler-time cancel to take the promise back, so it keeps the prediction.
static bool signal_decided_at_delivery_shared(struct sigqueue *best, bool shared) {
    return shared && !signal_native_held(best->info.sig);
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
    bool ignored_seen, shared;
    struct sigqueue *best = signal_deciding_locked(sighand, &ignored_seen, &shared);
    // A shim-held signal always runs a handler, whatever the kernel's
    // placeholder disposition claims -- and ERESTARTNOHAND is cancelled by a
    // handler running. Without this the placeholder for, say, a native
    // program's own SIGTSTP handler would read as SIGNAL_STOP and restart a
    // poll() that Linux would have interrupted.
    //
    // A signal whose delivery decides (signal_restart_decided_at_delivery)
    // restarts too, like a stop; if a handler does run before the call
    // re-executes, receive_signal cancels the restart. With nothing to deliver
    // but ignored signals, no handler runs (signal_restarts_nohand). The same
    // with nothing to deliver at all, which is what a task told to take the
    // process's shared queue finds once another thread has taken what was
    // there -- Linux's get_signal finds nothing and the call restarts -- and
    // what a PTRACE_EVENT_STOP the task owes its tracer ends a wait with.
    // Neither runs a handler. Measured without the first: a sigsuspend whose
    // SIGCHLD a sibling calling sigprocmask took first returned EINTR with no
    // handler run in its thread, where Linux goes on waiting
    // (tests/manual/signal_process_wake_one.c, "lost the race").
    //
    // And a signal on the process's queue restarts, to be cancelled if its
    // handler runs here: a sibling may take it first
    // (signal_decided_at_delivery_shared).
    bool restart = best != NULL ?
        !signal_native_held(best->info.sig) &&
            (signal_decided_at_delivery_shared(best, shared) ||
             signal_restarts_nohand(current, sighand, best->info.sig)) :
        ignored_seen || __atomic_load_n(&current->group_sigpending, __ATOMIC_ACQUIRE) ||
            task_trap_stop_pending(current);
    unlock(&sighand->lock);
    return restart;
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
    // (deliver_signal_to_group_locked, e.g. SIGCHLD via send_signal_to_process).
    // This used to only scan current->queue, so any group-directed signal
    // fell through to the "no restart" default even when its handler had
    // SA_RESTART_ set -- turning what should be a transparent kernel-level
    // restart into a real EINTR surfacing all the way into the guest.
    bool ignored_seen, shared;
    struct sigqueue *best = signal_deciding_locked(sighand, &ignored_seen, &shared);
    if (best == NULL) {
        bool told = __atomic_load_n(&current->group_sigpending, __ATOMIC_ACQUIRE);
        unlock(&sighand->lock);
        // Nothing to deliver but ignored signals, which run no handler and so
        // restart the call (signal_restarts_nohand). Or nothing at all, and
        // the task was told to take the process's shared queue: another thread
        // took what was there first -- a handled SIGCHLD, taken by a sibling on
        // its own way out -- so nothing runs here, and the call restarts as it
        // does on Linux, whose get_signal finds nothing. The record that
        // signal left (restart_flags_take) is what ITS handler allows, and
        // that handler does not run in this thread. Or no signal at all: a
        // PTRACE_EVENT_STOP the task owes its tracer ends a wait without one,
        // and restarts the call like a stop does. Linux's read() interrupted
        // by PTRACE_INTERRUPT returns the data that arrives after PTRACE_CONT;
        // this returned EINTR.
        return ignored_seen || told || task_trap_stop_pending(current);
    }
    int sig = best->info.sig;
    // A native program's handler, which the kernel is only holding a
    // placeholder for. The shim recorded the real sa_flags.
    if (signal_native_held(sig)) {
        unlock(&sighand->lock);
        return signal_native_restarts(sig);
    }
    // A stop, which resumes the syscall transparently, or a signal a tracer
    // will see before anything is delivered: restart, and let receive_signal
    // cancel it if a handler without SA_RESTART runs first. See
    // signal_restart_decided_at_delivery. The same for a signal on the
    // process's queue, which a sibling may take before this thread does
    // (signal_decided_at_delivery_shared).
    if (signal_restarts_nohand(current, sighand, sig) ||
            signal_decided_at_delivery_shared(best, shared)) {
        unlock(&sighand->lock);
        return true;
    }
    if (signal_action(sighand, sig) != SIGNAL_CALL_HANDLER) {
        // No handler, and not a stop or ignored: it kills the task.
        unlock(&sighand->lock);
        return false;
    }
    bool restart = !!(sighand->action[sig].flags & SA_RESTART_);
    unlock(&sighand->lock);
    return restart;
}

int_t signal_eintr_no_restart(int_t res) {
    if (res == _EINTR && current != NULL)
        (void) restart_flags_take(false);
    return res;
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

// Whether any thread of `tgroup` can still take a signal: one not on its way
// out. Caller holds pids_lock.
static bool tgroup_live_locked(struct tgroup *tgroup) {
    struct task *task;
    list_for_each_entry(&tgroup->threads, task, group_links) {
        if (!task->exiting && !task->zombie && task->sighand != NULL)
            return true;
    }
    return false;
}

// Signal every process in process group PGID, as Linux's kill_pgrp does, for
// the kernel's own senders: the terminal's ^C, ^\ and ^Z and its background
// SIGTTIN and SIGTTOU, a hangup, the orphaned group's SIGHUP and SIGCONT, and
// the SIGKILL for a timed-out app command's group.
int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    struct task *stack_targets[32];
    struct task **targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;

    struct pid *pid;
    struct tgroup *tgroup;
    complex_lockt(&pids_lock, 0);
retry:
    pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        if (targets != stack_targets)
            free(targets);
        return _ESRCH;
    }

    // Counted again after every unlock: the group can gain members while the
    // array is allocated, and filling a count taken before that overran it.
    size_t needed = 0;
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
        goto retry;
    }

    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        // To each process, as kill(-pgid) does: the signal waits on the
        // process's queue and a thread that can take it is told, the leader
        // being only where the search starts. This gave it to the leader: a
        // process whose main thread had left with pthread_exit was passed
        // over entirely -- ^C, a hangup and the orphaned group's SIGHUP never
        // reached it -- and one whose main thread blocked the signal kept it
        // there while another thread would have taken it.
        if (tgroup->leader == NULL || !tgroup_live_locked(tgroup))
            continue;
        task_ref_cnt_mod(tgroup->leader, 1);
        targets[target_count++] = tgroup->leader;
    }
    unlock(&pids_lock);

    for (size_t i = 0; i < target_count; i++) {
        send_signal_to_process(targets[i], sig, info);
        task_ref_cnt_mod(targets[i], -1);
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
            // An instruction fetch from a mapped page that may not be
            // executed: X86_PF_INSTR, and the page is present.
            if (!cpu->segfault_was_write &&
                    !mmu_page_executable(&current->mem->mmu, PAGE(cpu->segfault_addr)))
                err |= 0x10 | 0x1;
            mem_read_unlock_quiesce_aware(current->mem);
            return err;
        }
        default:
            return 0;
    }
}

// The mask a signal frame keeps for sigreturn to put back: Linux's
// sigmask_to_save. The first handler after a sigsuspend-like call is set up
// with that call's temporary mask still in force (receive_signals), and it is
// the mask from before the call that its sigreturn restores.
static sigset_t_ sigmask_to_save(void) {
    return current->has_saved_mask ? current->saved_mask : current->blocked;
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
    sc->oldmask = sigmask_to_save() & 0xffffffff;
}

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = (addr_t) signal_restorer(&current->sighand->action[info->sig], false);
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu, info->sig);
    frame->extramask = sigmask_to_save() >> 32;

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
    frame->uc.sigmask = sigmask_to_save();

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
    mcontext->gregs[AMD64_GREG_OLDMASK] = sigmask_to_save();
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
    frame->uc.sigmask = sigmask_to_save();
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
    frame->uc.sigmask = sigmask_to_save();

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
    frame->uc.sigmask = sigmask_to_save();

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

// The mask a handler runs with, as Linux's signal_delivered sets it: the mask
// in force as it is set up -- a sigsuspend-like call's temporary one, for the
// first handler after such a call -- plus the handler's sa_mask, plus the
// signal itself unless SA_NODEFER. Call once the frame is built: the frame
// holds what sigreturn puts back (sigmask_to_save), so a saved mask is done
// with here.
//
// receive_signals chooses each next signal against this mask, so a signal the
// handler blocks waits for its sigreturn rather than being stacked on it.
static void signal_handler_mask_set(const struct sigaction_ *action, int sig) {
    current->has_saved_mask = false;
    sigset_t_ blocked = current->blocked | action->mask;
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&blocked, sig);
    current->blocked = blocked & ~UNBLOCKABLE_MASK;
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
            // A new stop supersedes a continue nobody has heard of yet, as
            // Linux's signal_set_stop_flags clears SIGNAL_STOP_CONTINUED and
            // SIGNAL_CLD_CONTINUED: neither the notice nor a WCONTINUED wait
            // reports it any more. The wait did, for a process that was by
            // then stopped again.
            current->group->continued = false;
            current->group->continue_unannounced = false;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
            // execve asked for THIS thread to go, not the whole group -- see
            // exit_requested in kernel/task.h. do_exit takes a non-leader
            // thread off the group list without touching the other threads.
            //
            // With status 0, as Linux's do_group_exit gives a thread that
            // de_thread zaps. Nothing saw the status while such a thread was
            // simply destroyed, but a traced one is a zombie its tracer reaps,
            // and strace -f reported "+++ killed by SIGKILL +++" for a sibling
            // Linux reports as "+++ exited with 0 +++".
            if (__atomic_load_n(&current->exit_requested, __ATOMIC_ACQUIRE))
                do_exit(current, 0);
            do_exit_group(sig);
    }

    struct sigaction_ *action = &sighand->action[info->sig];

    // A handler is about to run. If the syscall it interrupted was rewound to
    // restart, this is where Linux's handle_signal settles whether it still
    // does, and so does this. An ERESTARTNOHAND restart -- poll/select, which
    // resume across a job-control stop but not across a handler -- is always
    // cancelled, and the guest gets EINTR. An _ERESTART one is cancelled when
    // this handler lacks SA_RESTART (ERESTARTSYS).
    //
    // That second half is what lets a restart be promised before anyone knows
    // which handler will run. A signal a tracer stops for is resumed, injected
    // or replaced as the tracer likes, so its syscall is set to restart and
    // this decides (signal_restart_decided_at_delivery). It also settles a stop
    // followed by a SIGCONT handler without SA_RESTART: measured on Linux 6.12,
    // read, recv, waitpid, futex and the rest then fail with EINTR, where AOK
    // restarted them because the stop, not the handler, had decided.
    //
    // Only the first handler decides. One stacked on top of it finds the
    // syscall already settled, as it does on Linux.
    bool cancel = current->restart_nohand_pending ||
        (current->restart_sys_pending && !(action->flags & SA_RESTART_));
    current->restart_nohand_pending = false;
    current->restart_sys_pending = false;
    if (cancel) {
        current->poll_restart_valid = false;
        current->sleep_restart_valid = false;
        cancel_syscall_restart();
    }
    // A timed futex wait keeps its deadline only across a restart nothing ran
    // in front of (futex_restart_deadline). After a handler -- which Linux
    // answers with EINTR for a timed wait, and this with a restart -- the
    // restarted wait gets its whole timeout, and the handler's own futex calls
    // or a longjmp out of it cannot pick up a deadline meant for another call.
    current->futex_restart_timed = false;

    // A thread inside its rseq critical section takes the signal from the
    // section's abort handler, so that is where its handler returns to; a
    // descriptor that is not a valid one is SIGSEGV (Linux's force_sigsegv).
    if (!rseq_signal_deliver()) {
        printk("WARNING: pid %d: invalid rseq critical section at signal delivery, killing\n",
               current->pid);
        unlock(&sighand->lock);
        do_exit_group(SIGSEGV_);
    }

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
        // explicit restorer and otherwise uses the vDSO trampoline -- here,
        // the [sigpage] exec maps, kernel/exec.c map_sigpage). Don't read
        // action->restorer without the flag: musl leaves the field unset on
        // aarch64. The stack copy of the trampoline is only for an address
        // space with no sigpage, one exec did not build; the stack is not
        // executable, so returning there would fault.
        guest_addr_t restorer = action->flags & 0x04000000u ? action->restorer : 0;
        if (restorer == 0)
            restorer = current->mm->vdso != 0 ? current->mm->vdso
                    : sp + offsetof(struct rt_sigframe_arm64, retcode);
        current->cpu.arm64_regs[arm64_x30] = restorer;

        signal_handler_mask_set(action, info->sig);

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
        // vDSO trampoline); here that is the [sigpage] exec maps, with the
        // stack copy only for an address space that has none -- see arm64.
        current->cpu.riscv64_regs[riscv64_ra] = current->mm->vdso != 0 ? current->mm->vdso
                : sp + offsetof(struct rt_sigframe_riscv64, retcode);

        signal_handler_mask_set(action, info->sig);

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

        // Linux requires SA_RESTORER on x86_64, and every libc sets it. One
        // that did not used to return to a stack copy of the trampoline; now
        // it is the [sigpage] (kernel/exec.c), since the stack does not run.
        guest_addr_t restorer = action->restorer;
        if (restorer == 0)
            restorer = current->mm->vdso != 0 ? current->mm->vdso
                    : sp + offsetof(struct rt_sigframe_amd64, retcode);
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

        signal_handler_mask_set(action, info->sig);

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

    signal_handler_mask_set(action, info->sig);

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
            // PTRACE_LISTEN: the tracer has already been shown this group-stop
            // and asked for it to stay in force. Wait it out WITHOUT reporting
            // again -- a second report is precisely what a listening tracee
            // must not produce -- and answer the two things that end a listen.
            if (__atomic_load_n(&current->ptrace.listening, __ATOMIC_ACQUIRE)) {
                lock(&group->lock, 0);
                // Every reason to stop waiting is re-asked on each pass: a poke
                // wakes this wait without consuming an interruption
                // (wake_waiting_task), which is how PTRACE_INTERRUPT reaches a
                // listening tracee at all, and is the rule at every other
                // poke-filtering wait.
                while (group->stopped &&
                        __atomic_load_n(&current->ptrace.listening, __ATOMIC_ACQUIRE) &&
                        !task_trap_stop_pending(current))
                    wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
                bool lifted = !group->stopped;
                unlock(&group->lock);

                // A PTRACE_INTERRUPT ends the listen and re-reports the stop.
                // The tracee is still group-stopped, so Linux's do_jobctl_trap
                // reports the STOP SIGNAL again (status 0x80137f) rather than
                // SIGTRAP; ptrace_group_stop() on the next pass does exactly
                // that, and consumes the flag as every stop does. Measured on
                // Linux 6.12.101.
                if (task_trap_stop_pending(current)) {
                    ptrace_listen_end();
                    continue;
                }
                // The tracer resumed us instead. Its resume cleared the flag
                // and lifted the stop, and it expects no report for that.
                if (!__atomic_load_n(&current->ptrace.listening, __ATOMIC_ACQUIRE))
                    continue;
                ptrace_listen_end();
                // A SIGCONT lifted the stop. Linux tells a listening tracer
                // about that with a PTRACE_EVENT_STOP carrying SIGTRAP, before
                // the tracee runs again -- it is how strace knows to print the
                // SIGCONT and stop expecting the job-control stop to hold.
                if (lifted)
                    ptrace_listen_cont_stop();
                continue;
            }
            ptrace_group_stop();
            continue;
        }
        lock(&group->lock, 0);
        if (group->stopped && !current->ptrace.traced)
            wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
        unlock(&group->lock);
    }

    // A listen never outlives the stop it was asked for. Whatever ended the
    // job-control stop -- a resume, a SIGCONT, a detach -- a flag left set here
    // would make the NEXT group-stop be waited out in silence instead of
    // reported to the tracer.
    if (__atomic_load_n(&current->ptrace.listening, __ATOMIC_ACQUIRE))
        ptrace_listen_end();

    // We were stopped and have just been resumed. If SIGCONT flagged a
    // reportable continue, wake a parent blocked in wait4/waitid(WCONTINUED)
    // (`continued` itself is consumed by the parent's notify_if_continued)
    // and tell it -- once, whichever of the process's threads is back first:
    // continue_unannounced is taken here. Measured with a four-thread child,
    // 24 CLD_CONTINUED notices for 20 SIGCONTs where Linux sends 20.
    // Done from our own context -- never the signal sender's -- so taking
    // pids_lock here respects the pids_lock -> group->lock ordering.
    lock(&group->lock, 0);
    bool announce = group->continue_unannounced;
    group->continue_unannounced = false;
    unlock(&group->lock);
    if (announce) {
        struct task *parent = NULL, *tracer = NULL;
        struct siginfo_ info = {
            .code = CLD_CONTINUED_,
            .child.status = SIGCONT_,
        };
        complex_lockt(&pids_lock, 0);
        struct task *leader = current->group->leader;
        info.child.pid = leader->pid;
        info.child.uid = leader->uid;
        parent = leader->parent;
        if (parent != NULL) {
            task_ref_cnt_mod(parent, 1);
            notify(&parent->group->child_exit);
        }
        tracer = cldstop_leader_tracer_locked(leader, parent);
        if (tracer != NULL) {
            task_ref_cnt_mod(tracer, 1);
            notify(&tracer->group->child_exit);
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
            notify_parent_cldstop(parent, info);
            task_ref_cnt_mod(parent, -1);
        }
        if (tracer != NULL) {
            notify_parent_cldstop(tracer, info);
            task_ref_cnt_mod(tracer, -1);
        }
    }
}

void receive_signals(void) {  
    lock(&current->group->lock, 0);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    // For the shared queue, below: the saved mask coming back and each
    // handler's mask change what this thread can take there.
    sigset_t_ entry_wake_blocked = task_wake_blocked(current);

    // Deliver pending unblocked signals one at a time, in the order Linux's
    // get_signal takes them (signal_next_deliverable_locked: what an
    // instruction raised, then the thread's own queue, then the process's),
    // each chosen against the mask as it is by then: Linux's
    // exit_to_user_mode_loop calls get_signal once per signal. Setting up a
    // handler changes the mask (signal_handler_mask_set) -- its sa_mask, and
    // its own signal unless SA_NODEFER -- and a signal that mask blocks waits
    // for the handler's sigreturn, then runs after it. One it does not block
    // gets a frame stacked on top. So when several become deliverable at once
    // (a sigprocmask that unblocks a whole set), the handlers of those the
    // earlier handlers do not block RUN in the reverse of the order they were
    // taken (LIFO), each frame saving the mask the one below it left.
    // The mask used to be read once, before this loop, and everything it let
    // through was stacked: a SIGUSR2 the SIGUSR1 handler's sa_mask blocks ran
    // first, on top of it, where Linux runs it after -- order 2,1 against 1,2.
    //
    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Its temporary mask stays in force
    // here, as on Linux: it decides until a handler is set up, the handler
    // runs with it plus its own mask, and the frame keeps the saved mask for
    // the handler's sigreturn (sigmask_to_save). If nothing here runs a
    // handler, the saved mask comes back once nothing more gets through, and
    // what it lets through is delivered in turn (restore_saved_sigmask). This
    // used to put the saved mask back first and deliver whatever EITHER mask
    // let through, all stacked: a handler after sigsuspend ran with the old
    // mask rather than the one sigsuspend waited with, and a signal the
    // temporary mask blocked ran on top of the one that ended the wait.
    for (;;) {
        // A PTRACE_EVENT_STOP owed to a tracer comes before any signal, and is
        // asked about again before EACH one, as Linux's get_signal loop asks
        // for JOBCTL_TRAP_MASK: a SIGCONT's notice can arrive while this loop
        // is already running, and the SIGCONT it announces is queued after
        // it. Taken with no lock held, as a signal-delivery-stop is.
        if (current->ptrace.traced && task_trap_stop_pending(current) &&
                !current->group->stopped) {
            unlock(&sighand->lock);
            ptrace_trap_stop_if_pending();
            lock(&sighand->lock, 0);
        }
        // The shared (process-directed) queue too, once the thread's own has
        // nothing -- e.g. a SIGCHLD sent via send_signal_to_process to a
        // sibling thread of this process, see kernel/exit.c.
        bool best_is_group;
        struct sigqueue *best = signal_next_deliverable_locked(sighand,
                ~current->blocked, true, NULL, &best_is_group);
        if (best == NULL) {
            // No handler took the saved mask into its frame: put it back, and
            // look again with it.
            if (current->has_saved_mask) {
                current->has_saved_mask = false;
                current->blocked = current->saved_mask;
                continue;
            }
            break;
        }

        int sig = best->info.sig;
        struct siginfo_ info = best->info;
        signal_timer_taken(current, best);
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

    // Linux's get_signal ends with recalc_sigpending: this thread goes on
    // looking at the shared queue only if something there is still its to
    // take. Anything still queued that it was told to take and its mask now
    // blocks -- the saved mask back, a handler's -- goes to a sibling.
    group_pending_mask_changed_locked(entry_wake_blocked);
    unlock(&sighand->lock);
    signal_group_handoff();

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock, 0);
        bool now_stopped = current->group->stopped;
        // group_exit_code is (stop_sig << 8 | 0x7f) while stopped; recover the
        // bare stop signal for the SIGCHLD si_status.
        int stop_sig = (current->group->group_exit_code >> 8) & 0xff;
        unlock(&current->group->lock);
        if (now_stopped) {
            // The stop SIGCHLD must carry CLD_STOPPED + the stop signal and
            // the child's pid/uid, not SIGINFO_NIL (which a SA_SIGINFO
            // handler / sigwaitinfo would read as SI_KERNEL with no child).
            struct siginfo_ info = {
                .code = CLD_STOPPED_,
                .child.status = stop_sig,
            };
            complex_lockt(&pids_lock, 0);
            // To the leader's parent, naming the leader, whichever thread took
            // the stop signal -- Linux's do_notify_parent_cldstop, which reports
            // a group-stop for the group. This told current->parent, naming
            // current, and a thread's parent is the thread that created it: a
            // stop taken by a thread other than the leader (tgkill'd to it, or
            // a process whose leader had already left) was announced to the
            // stopped process itself, and its parent was told nothing.
            struct task *leader = current->group->leader;
            info.child.pid = leader->pid;
            info.child.uid = leader->uid;
            struct task *parent = leader->parent;
            // A tracee's group-stop is reported to its tracer (ptrace_group_
            // stop), and Linux's ptrace_stop tells the parent too only when the
            // tracer is someone else. A parent tracing its child was told
            // twice.
            if (parent != NULL) {
                struct task *tracer = cldstop_tracer_locked(current);
                if (tracer != NULL && tracer->group == parent->group)
                    parent = NULL;
            }
            if (parent != NULL) {
                task_ref_cnt_mod(parent, 1);
                notify(&parent->group->child_exit);
            }
            unlock(&pids_lock);
            // SA_NOCLDSTOP: the parent asked NOT to be told when a child
            // merely stops or continues. Only the stop notification is
            // suppressed -- the child's eventual exit still raises SIGCHLD --
            // and wait(WUNTRACED) still reports the stop, because the flag is
            // about the signal, not about waitability.
            if (parent != NULL) {
                notify_parent_cldstop(parent, info);
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
    if (action) {
        sighand->action[sig] = *action;
        // As Linux's do_sigaction does. A handler's mask is added to the
        // blocked set while it runs, and one built with sigfillset -- common --
        // held back a SIGKILL until the handler returned.
        sighand->action[sig].mask &= ~UNBLOCKABLE_MASK;
    }
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

// Call with sighand->lock held, and signal_group_handoff() once it is dropped
// -- a syscall's way out does it (handle_interrupt), and so does any caller
// about to wait with the new mask in place.
static void sigmask_set(sigset_t_ set) {
    sigset_t_ old = task_wake_blocked(current);
    current->blocked = (set & ~UNBLOCKABLE_MASK);
    group_pending_mask_changed_locked(old);
}

void sigmask_set_blocked(sigset_t_ set) {
    lock(&current->sighand->lock, 0);
    sigmask_set(set);
    unlock(&current->sighand->lock);
    signal_group_handoff();
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
    // The call is about to wait with this mask, which may block a process
    // signal this thread was told to take.
    signal_group_handoff();
}

void sigmask_clear_temp(void) {
    lock(&current->sighand->lock, 0);
    if (current->has_saved_mask) {
        sigset_t_ old = task_wake_blocked(current);
        current->blocked = current->saved_mask;
        current->has_saved_mask = false;
        group_pending_mask_changed_locked(old);
    }
    unlock(&current->sighand->lock);
    signal_group_handoff();
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
    if (current->group_handoff != 0) {
        // The mask it waits with blocks a process signal this thread was told
        // to take: a sibling takes it instead (sigmask_set).
        unlock(&current->sighand->lock);
        signal_group_handoff();
        lock(&current->sighand->lock, 0);
    }
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    // ERESTARTNOHAND, as on Linux: only a handler running ends the wait. A
    // job-control stop used to end it too -- the plain _EINTR here reached the
    // guest as soon as the process was continued, where Linux resumes the
    // wait. Measured: SIGSTOP 200ms in and SIGCONT at 400ms, sigsuspend failed
    // with EINTR at ~406ms; Linux returns at 800ms, when a handled signal
    // arrives. pause, msgrcv and msgsnd are the same.
    return signal_restart_or_eintr_nohand(_EINTR);
}

int_t sys_pause(void) {
    lock(&current->sighand->lock, 0);
    TASK_MAY_BLOCK {
        while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
            continue;
    }
    unlock(&current->sighand->lock);
    // ERESTARTNOHAND: see sys_rt_sigsuspend_guest.
    return signal_restart_or_eintr_nohand(_EINTR);
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

    // Whatever ended the wait, take what is there, as Linux's
    // do_sigtimedwait dequeues once more after its timeout. A process signal
    // this thread was told to take as the timeout came would otherwise be
    // left queued with a thread that blocks it. The waited-for signals are
    // blocked again from here (Linux puts back real_blocked), so what the
    // thread cannot take any more goes to a sibling.
    bool found = signal_take_next_locked(current, set, &info);
    group_pending_mask_changed_locked(task_wake_blocked(current) & ~set);
    unlock(&current->sighand->lock);
    signal_group_handoff();
    if (!found && err == _ETIMEDOUT) {
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }
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
bool may_signal_task(struct task *task, dword_t sig) {
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

// A signal from userspace to `task`, after the permission check: to its
// process when `process` (kill, sigqueue, pidfd_send_signal), otherwise to the
// thread itself (tkill, tgkill, rt_tgsigqueueinfo).
//
// A process's signal waits on the process's queue, as on Linux, where any of
// its threads can see it and take it (send_signal_to_process), and one thread
// that can take it is told -- `task`, the thread the pid names, if it can. It
// used to be put on the queue of one thread chosen as it was sent: a thread
// sigwaiting for it, else one that did not block it, else `task`. No other
// thread looks at that queue. Measured on alpine-amd64-test against Linux 6.12,
// with every thread blocking the signal and kill(getpid()) sending it: a
// worker's sigtimedwait started afterwards waited out its whole timeout, and a
// worker's sigpending and signalfd did not see the signal, where Linux finds
// it at once; a worker that then unblocked it did not take it. mariadbd and
// most sysv daemons take their signals in a sigwait thread, which is between
// calls whenever it is handling the last one. And with the thread's own
// signals: kill(getpid(), SIGUSR1) and raise(SIGUSR2) ran SIGUSR2's handler
// first, where Linux, taking the thread's own queue first, runs SIGUSR1's.
//
// A first attempt at this, in August 2026, hung signal_restart,
// signal_stop_cont and process_conformance: the process's path lacked what
// send_signal does for SIGCONT and the stop signals. It has both now:
// signal_prepare_stop_cont, on every queue of the process, and
// signal_resume_group.
static int signal_send_checked(struct task *task, dword_t sig, struct siginfo_ info,
        bool process) {
    // FIXME: Need to check references to kernel here to be sure they are zero
    if (!may_signal_task(task, sig))
        return _EPERM;
    if (process) {
        send_signal_to_process(task, (int) sig, info);
    } else {
        signal_prepare_stop_cont_threads(task, (int) sig);
        send_signal(task, (int) sig, info);
    }
    return 0;
}

// What kill(2), tkill(2) and tgkill(2) send. kill reports SI_USER and the other
// two SI_TKILL; a handler that inspects si_code must see the right one -- glibc
// raise() routes through tgkill, so this is common.
//
// The sender is a PROCESS, whichever of its threads sends: Linux's
// prepare_kill_siginfo gives si_pid task_tgid_vnr(current), for tkill and
// tgkill too. This gave the sending thread's id, which is its process's only
// when the main thread sends -- so a signal from any other thread named a pid
// no process had, and a program that answers si_pid, or checks it against the
// child it forked, answered nobody.
static struct siginfo_ kill_siginfo(int si_code) {
    return (struct siginfo_) {
        .code = si_code,
        .kill.pid = current->tgid,
        .kill.uid = current->uid,
    };
}

int signal_kill_process(struct task *task, dword_t sig, int si_code) {
    return signal_send_checked(task, sig, kill_siginfo(si_code), true);
}

// A signal to the task `pid` names -- to its process when `process`, to the
// thread itself otherwise -- which must be in the process `tgid` when that is
// not 0. What kill, tkill, tgkill, rt_sigqueueinfo and rt_tgsigqueueinfo do
// once they know what to send.
static int signal_send_to_pid(pid_t_ pid, pid_t_ tgid, dword_t sig, struct siginfo_ info,
        bool process) {
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task_zombie(pid);
    if (task == NULL || (tgid != 0 && task->tgid != tgid)) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    // Only a pid that names something is asked about the signal, as on Linux:
    // an invalid signal to a pid nobody has is ESRCH.
    if (sig >= NUM_SIGS) {
        unlock(&pids_lock);
        return _EINVAL;
    }

    // An exited-but-unreaped (zombie) task still exists for kill() on
    // Linux: the signal is discarded but the call returns 0, not ESRCH.
    // stress-ng does kill(child, SIGKILL) right after the child exits,
    // before wait4() reaps it. rt_sigqueueinfo and rt_tgsigqueueinfo say 0
    // for one too; they said ESRCH.
    if (task->zombie) {
        unlock(&pids_lock);
        return 0;
    }

    // To the process the pid names, through the thread it names -- which
    // may have exited, the leader of a process whose main thread left with
    // pthread_exit, and whose signal a thread still running then takes.
    task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);
    int err = signal_send_checked(task, sig, info, process);
    task_ref_cnt_mod(task, -1);
    return err;
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
    //
    // A leader that is a corpse is not a process that is gone: its other
    // threads may run on, and the leader stays registered until the last of
    // them exits. Each process is sent the signal through its leader, as
    // Linux's kill_pgrp does, and it waits on the process's queue for a
    // thread that can take it (signal_kill_process).
    size_t skipped = 0;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        if (tgroup->leader == NULL || !tgroup_live_locked(tgroup)) {
            skipped++;
            continue;
        }
        task_ref_cnt_mod(tgroup->leader, 1);
        targets[target_count++] = (struct kill_target) {.task = tgroup->leader};
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
        int kill_err = signal_kill_process(targets[i].task, sig, si_code);
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
        (void) signal_kill_process(targets[i].task, sig, si_code);
        task_ref_cnt_mod(targets[i].task, -1);
    }
    if (targets != stack_targets)
        free(targets);
    return target_count > 0 ? 0 : _ESRCH;
}

// si_code distinguishes the sender: SI_USER for kill(2), SI_TKILL for
// tkill/tgkill(2). Linux forces this on the receiving side, so we thread it
// down from the syscall entry point rather than letting kill_task assume SI_USER.
// kill(2) is PROCESS-directed, and waits on the process's queue for a thread
// that can take it; see signal_send_checked, and tests/manual/sigwait_kill.c
// for the daemon that could not be stopped while it did not. thread_directed
// distinguishes tkill/tgkill (deliver to THIS thread's private queue, which is
// their entire purpose) from kill (deliver to the process).
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
        err = signal_send_to_pid(pid, tgid, sig, kill_siginfo(si_code), !thread_directed);
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

// rt_sigqueueinfo(2) and rt_tgsigqueueinfo(2) send the caller's own siginfo as
// the caller wrote it, but for si_signo, which is the signal. sigqueue() fills
// in si_pid itself, with getpid(). A code below zero is a user's own and is
// taken on trust, sender and all. A code of zero or more says the kernel sent
// it, and SI_TKILL says tgkill did, both of which name the sender themselves:
// only a thread signalling itself may claim those (Linux's do_rt_sigqueueinfo
// and do_rt_tgsigqueueinfo). Itself means its own THREAD id, which Linux
// compares with task_pid_vnr(current), so a thread other than the main one is
// refused even when it names its own process.
//
// These replaced si_code with SI_QUEUE and si_pid with the sending thread's
// id, so sigqueue() from any thread but the main one named a pid no process
// had, and passed kill()'s code or the kernel's on to any process at all.
static int queueinfo_claim_checked(const struct siginfo_ *info, pid_t_ target) {
    if ((info->code >= 0 || info->code == SI_TKILL_) && target != current->pid)
        return _EPERM;
    return 0;
}

dword_t sys_rt_sigqueueinfo_guest(pid_t_ pid, dword_t sig, guest_addr_t uinfo_addr) {
    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;
    info.sig = (int_t) sig;
    err = queueinfo_claim_checked(&info, pid);
    if (err < 0)
        return err;
    // One process, never a group: no pid is zero or less, and Linux says
    // ESRCH. Signal 0 asks only whether the process is there and may be
    // signalled, as it does of kill().
    if (pid <= 0)
        return _ESRCH;

    // Process-directed, exactly as kill(2) is: Linux routes rt_sigqueueinfo
    // through kill_proc_info/group_send_sig_info, so any thread of the target
    // that can take the signal is a legitimate destination. Queueing straight
    // into the resolved task's private queue meant a sibling already parked in
    // sigwait()/sigtimedwait() -- the whole reason a program uses sigqueue --
    // waited out its timeout while the signal sat undeliverable beside it.
    return signal_send_to_pid(pid, 0, sig, info, true);
}

dword_t sys_rt_tgsigqueueinfo_guest(pid_t_ tgid, pid_t_ tid, dword_t sig, guest_addr_t uinfo_addr) {
    struct siginfo_ info;
    int err = siginfo_from_user(current, uinfo_addr, &info);
    if (err < 0)
        return err;
    info.sig = (int_t) sig;
    if (tgid <= 0 || tid <= 0)
        return _EINVAL;
    err = queueinfo_claim_checked(&info, tid);
    if (err < 0)
        return err;
    return signal_send_to_pid(tid, tgid, sig, info, false);
}

dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, dword_t sig, addr_t uinfo_addr) {
    return sys_rt_tgsigqueueinfo_guest(tgid, tid, sig, uinfo_addr);
}

// ---- checkpoint (kernel/anonfd_ckpt.h) ------------------------------------

bool signalfd_fd_is(struct fd *fd) {
    return fd != NULL && fd->ops == &signalfd_ops;
}

uint64_t signalfd_ckpt_mask(struct fd *fd) {
    struct signalfd_state *state = fd->data;
    return state != NULL ? (uint64_t) state->mask : 0;
}

struct fd *signalfd_ckpt_new(uint64_t mask) {
    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct signalfd_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    *state = (struct signalfd_state) {.mask = (sigset_t_) mask};
    fd->data = state;
    return fd;
}
