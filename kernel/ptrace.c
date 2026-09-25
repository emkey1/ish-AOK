#include "ptrace.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/abi/amd64.h"
#include "kernel/abi/i386.h"
#include "kernel/signal.h"
#include "task.h"
#include <string.h>

static struct task *ptrace_tracer(struct task *task) {
    if (task->ptrace.tracer != NULL)
        return task->ptrace.tracer;
    return task->parent;
}

static bool ptrace_traced_by(const struct task *tracer, const struct task *tracee) {
    return tracee->ptrace.tracer == tracer ||
        (tracee->ptrace.traced && tracee->ptrace.tracer == NULL && tracee->parent == tracer);
}

static int ptrace_resume_signal(guest_addr_t data) {
    if (data == 0)
        return 0;
    if (data >= NUM_SIGS)
        return _EINVAL;
    return (int) data;
}

static void ptrace_resume_child_locked(struct task *child, int resume_sig,
        bool single_step, bool stop_at_syscall, bool detach) {
    // What the injected signal carries. From a signal-delivery-stop, Linux's
    // ptrace_signal delivers the very signal the tracee was taking, siginfo
    // and all, and a different one as SI_USER from the tracer. This was queued
    // with no siginfo whatever the stop, so a traced program's handler saw
    // SI_KERNEL and si_pid 0 -- including the SIGCHLD a traced shell gets for
    // a child: strace -f takes that one at its delivery-stop and passes it on.
    // Measured on Linux 6.12 by tests/manual/ptrace_tracee_exit.c. From any
    // other stop Linux's send_sig says SI_KERNEL, which SIGINFO_NIL is.
    // A signal injected to resume a group-stop, or any other event stop, is
    // discarded rather than delivered (ptrace.stop_discards_signal). Delivering
    // it made a group-stop inescapable for a tracer that re-injects each stop's
    // own signal, as gdb and strace both can: the SIGSTOP went straight back
    // in, the tracee group-stopped again, and a probe logged 2.8 million
    // consecutive stops in four minutes where Linux takes two and runs on.
    //
    // Zeroed here rather than skipped at the send below, so that the group-stop
    // is lifted as it is for any other signal-free resume -- which is what lets
    // the tracee actually run.
    if (resume_sig != 0 && child->ptrace.stop_discards_signal)
        resume_sig = 0;

    struct siginfo_ resume_info = SIGINFO_NIL;
    if (resume_sig != 0 && child->ptrace_delivery_stop) {
        if (resume_sig == child->ptrace.info.sig) {
            resume_info = child->ptrace.info;
        } else {
            struct task *tracer = ptrace_tracer(child);
            resume_info.code = SI_USER_;
            resume_info.kill.pid = tracer != NULL ? tracer->pid : current->pid;
            resume_info.kill.uid = tracer != NULL ? tracer->uid : current->uid;
        }
    }
    child->ptrace_delivery_stop = false;
    child->ptrace.stop_discards_signal = false;
    // Pinned until the end. Everything past the unlock below runs with the
    // tracee free to run, and a tracee that is let go can exit and be freed
    // before this reaches its group or its signal queue. Guard Malloc caught
    // exactly that under strace -f killed mid-run: PTRACE_CONT faulting on
    // child->group, the child a thread that had exited and been freed. A
    // traced task's exit now waits for its tracer, but a detached one's does
    // not.
    task_ref_cnt_mod(child, 1);
    child->cpu.tf = single_step;
    child->ptrace.stop_at_syscall = stop_at_syscall;
    if (!stop_at_syscall)
        child->ptrace.syscall_stopped = false;
    child->ptrace.stopped = false;
    // Any resume ends a PTRACE_LISTEN: the tracee stops waiting the job-control
    // stop out and runs, which is what the tracer just asked for.
    __atomic_store_n(&child->ptrace.listening, false, __ATOMIC_RELEASE);
    child->ptrace.signal = 0;
    // A signal injected by the tracer must be delivered (run its action) the
    // next time the tracee processes signals, not re-reported as another
    // signal-delivery-stop — otherwise the tracer re-injects it forever.
    child->ptrace.deliver_sig = resume_sig;
    child->ptrace.trap_event = 0;
    child->ptrace.eventmsg = 0;
    if (detach) {
        child->ptrace.traced = false;
        child->ptrace.tracer = NULL;
        child->ptrace.options = 0;
        child->ptrace.sysgood = false;
        // Not previously cleared. A later PTRACE_ATTACH of the same task would
        // otherwise inherit it and have its group-stops reported as seize-style
        // event-stops to a tracer that never seized anything.
        child->ptrace.seized = false;
        // A stop still owed goes with the tracer, as __ptrace_unlink clears
        // JOBCTL_TRAP_MASK. Before the notify below, under the lock the tracee
        // is parked holding: once released it is free to reach a checkpoint,
        // and it must find nothing owed there.
        __atomic_store_n(&child->ptrace.trap_stop, false, __ATOMIC_RELEASE);
        __atomic_store_n(&child->ptrace_trap_notify, false, __ATOMIC_RELEASE);
    }
    // A ptrace resume also lifts any job-control (group) stop on the tracee:
    // ptrace control takes precedence over SIGSTOP/SIGCONT job control, so a
    // tracer continuing a group-stopped tracee must let it run. Without this a
    // tracee that group-stops (e.g. strace re-injecting the post-fork SIGSTOP)
    // would wait forever in handle_interrupt's group-stop loop.
    //
    // Before the tracee is woken, not after. Lifted once the lock was
    // dropped, a tracee woken first came back round group_stop_wait, found
    // the group still stopped and reported the group-stop it had just been
    // let out of a second time, then waited in it for a resume its tracer
    // did not know it owed: 13 of 80,000 PTRACE_CONTs from a group-stop, seized
    // and not, in a loop that stops and resumes a tracee; none since. Linux's
    // tracee never comes back to a group-stop its tracer ended.
    // ptrace.lock -> group->lock; nothing takes them the other way round.
    if (resume_sig == 0) {
        lock(&child->group->lock, 0);
        if (child->group->stopped) {
            child->group->stopped = false;
            notify(&child->group->stopped_cond);
        }
        unlock(&child->group->lock);
    }
    notify(&child->ptrace.cond);
    unlock(&child->ptrace.lock);
    if (resume_sig != 0)
        send_signal(child, resume_sig, resume_info);
    task_ref_cnt_mod(child, -1);
}

// A failed lookup here means one of: the pid does not exist, it is not a
// tracee of ours, or it is not stopped when the request needs it to be.
// Linux answers ESRCH to all three -- ptrace's EPERM is reserved for
// PTRACE_ATTACH being refused. Reporting EPERM told a tracer it lacked
// permission for a process that had simply exited, and debuggers act on that
// difference: one retries or reports a permission problem to the user, the
// other reaps the child and moves on.
static struct task *find_tracee(pid_t_ pid, bool require_stopped) {
    complex_lockt(&pids_lock, 0);
    struct task *tracee = pid_get_task_zombie(pid);
    if (tracee == NULL) {
        unlock(&pids_lock);
        return NULL;
    }

    lock(&tracee->ptrace.lock, 0);
    bool visible = ptrace_traced_by(current, tracee);
    bool stopped_ok = !require_stopped || tracee->ptrace.stopped;
    unlock(&pids_lock);

    if (!visible || !stopped_ok) {
        unlock(&tracee->ptrace.lock);
        return NULL;
    }
    return tracee;
}

// Returns stopped tracee with the given pid, locked with the ptrace lock
static struct task *find_child(pid_t_ pid) {
    return find_tracee(pid, true);
}

bool ptrace_attach_fork_child(struct task *child, struct task *tracee) {
    // The tracer is read under pids_lock, which is what its exit holds while
    // it lets go of its tracees. Read before, as it was, a fork racing the
    // tracer's exit attached the child to a tracer that had already detached
    // everything and was about to be freed -- and a traced child's exit now
    // waits for that tracer to collect it.
    complex_lockt(&pids_lock, 0);
    struct task *tracer = tracee->ptrace.traced ? ptrace_tracer(tracee) : NULL;
    if (tracer == NULL) {
        unlock(&pids_lock);
        return false;
    }
    child->ptrace.traced = true;
    child->ptrace.seized = tracee->ptrace.seized;
    child->ptrace.sysgood = tracee->ptrace.sysgood;
    child->ptrace.options = tracee->ptrace.options;
    child->ptrace.tracer = tracer;
    // The same link, so the same privilege: Linux's ptrace_init_task hands the
    // child its parent's ptracer_cred, not the tracer's credentials of today.
    child->ptrace_link_capable = tracee->ptrace_link_capable;
    list_add(&tracer->ptracees, &child->ptrace_siblings);
    unlock(&pids_lock);
    return true;
}

static void sync_i386_shadows_from_amd64_ptrace(struct cpu_state *cpu) {
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

static bool ptrace_in_syscall_entry_stop(const struct task *task) {
    return task->ptrace.stopped &&
        task->ptrace.stop_at_syscall &&
        task->ptrace.syscall_stopped;
}

static void get_user_regs_amd64(struct task *task, struct user_regs_struct_amd64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_regs_, 0, sizeof(*user_regs_));
    user_regs_->r15 = cpu->amd64_regs[amd64_r15];
    user_regs_->r14 = cpu->amd64_regs[amd64_r14];
    user_regs_->r13 = cpu->amd64_regs[amd64_r13];
    user_regs_->r12 = cpu->amd64_regs[amd64_r12];
    user_regs_->rbp = cpu->amd64_regs[amd64_rbp];
    user_regs_->rbx = cpu->amd64_regs[amd64_rbx];
    user_regs_->r11 = cpu->amd64_regs[amd64_r11];
    user_regs_->r10 = cpu->amd64_regs[amd64_r10];
    user_regs_->r9 = cpu->amd64_regs[amd64_r9];
    user_regs_->r8 = cpu->amd64_regs[amd64_r8];
    user_regs_->rax = cpu->amd64_regs[amd64_rax];
    user_regs_->rcx = cpu->amd64_regs[amd64_rcx];
    user_regs_->rdx = cpu->amd64_regs[amd64_rdx];
    user_regs_->rsi = cpu->amd64_regs[amd64_rsi];
    user_regs_->rdi = cpu->amd64_regs[amd64_rdi];
    user_regs_->orig_rax = task->ptrace.syscall;
    user_regs_->rip = cpu->amd64_rip;
    user_regs_->cs = 0x33;
    user_regs_->eflags = cpu->eflags;
    user_regs_->rsp = cpu->amd64_regs[amd64_rsp];
    user_regs_->ss = 0x2b;
    user_regs_->fs_base = cpu->tls_ptr;
    user_regs_->ds = cpu->amd64_sreg[AMD64_SREG_DS];
    user_regs_->es = cpu->amd64_sreg[AMD64_SREG_ES];
    user_regs_->fs = cpu->amd64_sreg[AMD64_SREG_FS];
    user_regs_->gs = cpu->amd64_sreg[AMD64_SREG_GS];
    if (ptrace_in_syscall_entry_stop(task))
        user_regs_->rax = (qword_t) (sqword_t) _ENOSYS;
}

// Linux's set_segment_reg truncates to 16 bits, takes the selector only if it
// is null or has RPL 3 (anything else is EIO), and leaves the base alone.
static void set_user_sreg_amd64(struct cpu_state *cpu, unsigned sreg, qword_t value) {
    word_t sel = (word_t) value;
    if (sel == 0 || (sel & 3) == 3)
        cpu->amd64_sreg[sreg] = sel;
}

static void set_user_regs_amd64(struct task *task, const struct user_regs_struct_amd64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    cpu->amd64_regs[amd64_r15] = user_regs_->r15;
    cpu->amd64_regs[amd64_r14] = user_regs_->r14;
    cpu->amd64_regs[amd64_r13] = user_regs_->r13;
    cpu->amd64_regs[amd64_r12] = user_regs_->r12;
    cpu->amd64_regs[amd64_rbp] = user_regs_->rbp;
    cpu->amd64_regs[amd64_rbx] = user_regs_->rbx;
    cpu->amd64_regs[amd64_r11] = user_regs_->r11;
    cpu->amd64_regs[amd64_r10] = user_regs_->r10;
    cpu->amd64_regs[amd64_r9] = user_regs_->r9;
    cpu->amd64_regs[amd64_r8] = user_regs_->r8;
    cpu->amd64_regs[amd64_rax] = user_regs_->rax;
    cpu->amd64_regs[amd64_rcx] = user_regs_->rcx;
    cpu->amd64_regs[amd64_rdx] = user_regs_->rdx;
    cpu->amd64_regs[amd64_rsi] = user_regs_->rsi;
    cpu->amd64_regs[amd64_rdi] = user_regs_->rdi;
    cpu->amd64_rip = user_regs_->rip;
    cpu->eflags = (dword_t) user_regs_->eflags;
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
    cpu->amd64_regs[amd64_rsp] = user_regs_->rsp;
    cpu->tls_ptr = user_regs_->fs_base;
    set_user_sreg_amd64(cpu, AMD64_SREG_DS, user_regs_->ds);
    set_user_sreg_amd64(cpu, AMD64_SREG_ES, user_regs_->es);
    set_user_sreg_amd64(cpu, AMD64_SREG_FS, user_regs_->fs);
    set_user_sreg_amd64(cpu, AMD64_SREG_GS, user_regs_->gs);
    task->ptrace.syscall = (int) user_regs_->orig_rax;
    if (ptrace_in_syscall_entry_stop(task))
        cpu->amd64_regs[amd64_rax] = user_regs_->orig_rax;
    sync_i386_shadows_from_amd64_ptrace(cpu);
}

static void get_user_fpregs_amd64(struct task *task, struct user_fpregs_struct_amd64_ *user_fpregs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_fpregs_, 0, sizeof(*user_fpregs_));
    user_fpregs_->cwd = cpu->fcw;
    user_fpregs_->swd = cpu->fsw;
    // A stopped tracee's flags are all in cpu_state already: they are folded
    // in whenever it leaves guest code (emu/fpenv.c).
    user_fpregs_->mxcsr = cpu->mxcsr;

    for (int i = 0; i < 8; i++) {
        const float80 value = cpu->fp[i];
        for (int j = 0; j < 4; j++)
            user_fpregs_->st[i].significand[j] = (word_t) (value.signif >> (j * 16));
        user_fpregs_->st[i].exponent = value.signExp;
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            user_fpregs_->xmm[i].element[j] = cpu->xmm[i].u32[j];
}

static void set_user_fpregs_amd64(struct cpu_state *cpu, const struct user_fpregs_struct_amd64_ *user_fpregs_) {
    cpu->fcw = user_fpregs_->cwd;
    cpu->fsw = user_fpregs_->swd;
    cpu->mxcsr = user_fpregs_->mxcsr & 0xffff;

    for (int i = 0; i < 8; i++) {
        uint64_t significand = 0;
        for (int j = 0; j < 4; j++)
            significand |= (uint64_t) user_fpregs_->st[i].significand[j] << (j * 16);
        cpu->fp[i] = (float80) {
            .signif = significand,
            .signExp = user_fpregs_->st[i].exponent,
        };
    }

    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++)
            cpu->xmm[i].u32[j] = user_fpregs_->xmm[i].element[j];
}

// PSTATE as seen by an EL0 debugger: just the NZCV bits (31:28), which is
// exactly how cpu->arm64_nzcv is packed. All mode/mask bits are 0 (EL0t).
#define ARM64_PSTATE_NZCV_MASK 0xf0000000u

static void get_user_regs_arm64(struct task *task, struct user_pt_regs_arm64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_regs_, 0, sizeof(*user_regs_));
    for (int i = 0; i < arm64_reg_count; i++)
        user_regs_->regs[i] = cpu->arm64_regs[i];
    user_regs_->sp = cpu->arm64_sp;
    user_regs_->pc = cpu->arm64_pc;
    user_regs_->pstate = cpu->arm64_nzcv & ARM64_PSTATE_NZCV_MASK;
    // At a syscall-entry stop x8 still holds the number the tracee loaded,
    // but report the canonical ptrace.syscall so a prior NT_ARM_SYSTEM_CALL
    // rewrite is reflected, mirroring orig_eax/orig_rax handling.
    if (ptrace_in_syscall_entry_stop(task))
        user_regs_->regs[arm64_x8] = (qword_t) (sqword_t) task->ptrace.syscall;
}

static void set_user_regs_arm64(struct task *task, const struct user_pt_regs_arm64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    for (int i = 0; i < arm64_reg_count; i++)
        cpu->arm64_regs[i] = user_regs_->regs[i];
    cpu->arm64_sp = user_regs_->sp;
    cpu->arm64_pc = user_regs_->pc;
    cpu->arm64_nzcv = (dword_t) user_regs_->pstate & ARM64_PSTATE_NZCV_MASK;
    // Syscall dispatch re-reads x8 after the entry stop resumes, so keep the
    // reported syscall number in sync with what will actually run.
    if (ptrace_in_syscall_entry_stop(task))
        task->ptrace.syscall = (int) user_regs_->regs[arm64_x8];
}

static void get_user_fpregs_arm64(struct task *task, struct user_fpsimd_state_arm64_ *user_fpregs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_fpregs_, 0, sizeof(*user_fpregs_));
    for (int i = 0; i < 32; i++) {
        user_fpregs_->vregs[i][0] = cpu->arm64_v[i].qw[0];
        user_fpregs_->vregs[i][1] = cpu->arm64_v[i].qw[1];
    }
    user_fpregs_->fpsr = cpu->arm64_fpsr;
    user_fpregs_->fpcr = cpu->arm64_fpcr;
}

static void set_user_fpregs_arm64(struct cpu_state *cpu, const struct user_fpsimd_state_arm64_ *user_fpregs_) {
    for (int i = 0; i < 32; i++) {
        cpu->arm64_v[i].qw[0] = user_fpregs_->vregs[i][0];
        cpu->arm64_v[i].qw[1] = user_fpregs_->vregs[i][1];
    }
    cpu->arm64_fpsr = user_fpregs_->fpsr;
    cpu->arm64_fpcr = user_fpregs_->fpcr;
}

static void get_user_regs_riscv64(struct task *task, struct user_regs_struct_riscv64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_regs_, 0, sizeof(*user_regs_));
    for (int i = 1; i < riscv64_reg_count; i++)
        user_regs_->regs[i - 1] = cpu->riscv64_regs[i];
    user_regs_->pc = cpu->riscv64_pc;
    // At a syscall-entry stop a7 still holds the number the tracee loaded,
    // but report the canonical ptrace.syscall so a rewrite via SETREGSET is
    // reflected, mirroring arm64's x8 handling.
    if (ptrace_in_syscall_entry_stop(task))
        user_regs_->regs[riscv64_a7 - 1] = (qword_t) (sqword_t) task->ptrace.syscall;
}

static void set_user_regs_riscv64(struct task *task, const struct user_regs_struct_riscv64_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    for (int i = 1; i < riscv64_reg_count; i++)
        cpu->riscv64_regs[i] = user_regs_->regs[i - 1];
    cpu->riscv64_pc = user_regs_->pc;
    // Syscall dispatch re-reads a7 after the entry stop resumes, so keep the
    // reported syscall number in sync with what will actually run.
    if (ptrace_in_syscall_entry_stop(task))
        task->ptrace.syscall = (int) user_regs_->regs[riscv64_a7 - 1];
}

static void get_user_fpregs_riscv64(struct task *task, struct user_fpregs_struct_riscv64_ *user_fpregs_) {
    struct cpu_state *cpu = &task->cpu;
    memset(user_fpregs_, 0, sizeof(*user_fpregs_));
    for (int i = 0; i < 32; i++)
        user_fpregs_->f[i] = cpu->riscv64_f[i];
    user_fpregs_->fcsr = cpu->riscv64_fcsr;
}

static void set_user_fpregs_riscv64(struct cpu_state *cpu, const struct user_fpregs_struct_riscv64_ *user_fpregs_) {
    for (int i = 0; i < 32; i++)
        cpu->riscv64_f[i] = user_fpregs_->f[i];
    cpu->riscv64_fcsr = user_fpregs_->fcsr;
}

static size_t ptrace_word_size(const struct task *task) {
    return guest_abi_is_64bit(task->abi) ? sizeof(qword_t) : sizeof(dword_t);
}

static int ptrace_put_eventmsg(struct task *tracer, guest_addr_t data_addr, qword_t eventmsg) {
    if (guest_abi_is_64bit(tracer->abi)) {
        return user_put(data_addr, eventmsg);
    } else {
        dword_t compat_eventmsg = (dword_t) eventmsg;
        return user_put(data_addr, compat_eventmsg);
    }
}

struct ptrace_iovec_ {
    guest_addr_t base;
    qword_t len;
};

static int ptrace_iovec_get(struct task *tracer, guest_addr_t iov_addr, struct ptrace_iovec_ *iov) {
    if (guest_abi_is_64bit(tracer->abi)) {
        struct amd64_iovec_ raw;
        if (user_get(iov_addr, raw))
            return _EFAULT;
        *iov = (struct ptrace_iovec_) {
            .base = raw.base,
            .len = raw.len,
        };
    } else {
        struct i386_iovec_ raw;
        if (user_get(iov_addr, raw))
            return _EFAULT;
        *iov = (struct ptrace_iovec_) {
            .base = raw.base,
            .len = raw.len,
        };
    }
    return 0;
}

static int ptrace_iovec_put(struct task *tracer, guest_addr_t iov_addr, const struct ptrace_iovec_ *iov) {
    if (guest_abi_is_64bit(tracer->abi)) {
        struct amd64_iovec_ raw = {
            .base = iov->base,
            .len = iov->len,
        };
        if (user_put(iov_addr, raw))
            return _EFAULT;
    } else {
        struct i386_iovec_ raw = {
            .base = (addr_t) iov->base,
            .len = (dword_t) iov->len,
        };
        if (user_put(iov_addr, raw))
            return _EFAULT;
    }
    return 0;
}

static int ptrace_getregset_write(struct task *tracer, guest_addr_t iov_addr,
        const void *buf, size_t buf_size) {
    struct ptrace_iovec_ iov;
    int err = ptrace_iovec_get(tracer, iov_addr, &iov);
    if (err < 0)
        return err;

    size_t copy_len = iov.len < buf_size ? (size_t) iov.len : buf_size;
    if (copy_len != 0 && user_write(iov.base, buf, copy_len))
        return _EFAULT;

    iov.len = copy_len;
    return ptrace_iovec_put(tracer, iov_addr, &iov);
}

// `buf` holds the registers as they are, and as much of it as the tracer
// supplied is overwritten: Linux's ptrace_regset copies min(iov_len, the set's
// size), so a tracer may set only the first few. A short vector was EIO here.
// The length taken is reported back, as for a GETREGSET.
static int ptrace_setregset_read(struct task *tracer, guest_addr_t iov_addr,
        void *buf, size_t buf_size) {
    struct ptrace_iovec_ iov;
    int err = ptrace_iovec_get(tracer, iov_addr, &iov);
    if (err < 0)
        return err;
    size_t copy_len = iov.len < buf_size ? (size_t) iov.len : buf_size;
    if (copy_len != 0 && user_read(iov.base, buf, copy_len))
        return _EFAULT;
    iov.len = copy_len;
    return ptrace_iovec_put(tracer, iov_addr, &iov);
}

// The size of one slot of a register set, which a request's length must be a
// whole number of -- Linux's regset->size, and ptrace_regset's EINVAL for any
// other length -- or 0 for a set this tracee's architecture does not have.
//
// Debuggers probe with that EINVAL. gdb on riscv64 finds the width of the
// floating-point registers by asking for NT_PRFPREG in 4-byte slots first and
// taking EINVAL to mean "not those"; answered instead, it concluded the
// registers were 4 bytes wide, printed "bfd requires flen 8, but target has
// flen 4", and could not insert a single breakpoint.
static size_t ptrace_regset_slot(const struct task *child, qword_t note_type) {
    switch (child->abi) {
        case GUEST_ABI_AMD64:
            switch (note_type) {
                case NT_PRSTATUS_: case NT_PRFPREG_: case NT_X86_XSTATE_: return 8;
            }
            return 0;
        case GUEST_ABI_ARM64:
            switch (note_type) {
                case NT_PRSTATUS_: case NT_ARM_TLS_: return 8;
                case NT_PRFPREG_: case NT_ARM_HW_BREAK_: case NT_ARM_HW_WATCH_:
                case NT_ARM_SYSTEM_CALL_: return 4;
            }
            return 0;
        case GUEST_ABI_RISCV64:
            switch (note_type) {
                case NT_PRSTATUS_: case NT_PRFPREG_: return 8;
            }
            return 0;
        case GUEST_ABI_I386:
        default:
            switch (note_type) {
                case NT_PRSTATUS_: case NT_PRFPREG_: return 4;
                case NT_X86_XSTATE_: return 8;
            }
            return 0;
    }
}

static int ptrace_regset_check(struct task *tracer, struct task *child, guest_addr_t iov_addr,
        qword_t note_type) {
    size_t slot = ptrace_regset_slot(child, note_type);
    if (slot == 0)
        return _EINVAL;
    struct ptrace_iovec_ iov;
    int err = ptrace_iovec_get(tracer, iov_addr, &iov);
    if (err < 0)
        return err;
    if (iov.len % slot != 0)
        return _EINVAL;
    return 0;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void get_user_regs(struct cpu_state *cpu, struct user_regs_struct_ *user_regs_) {
    user_regs_->ebx = cpu->ebx;
    user_regs_->ecx = cpu->ecx;
    user_regs_->edx = cpu->edx;
    user_regs_->esi = cpu->esi;
    user_regs_->edi = cpu->edi;
    user_regs_->ebp = cpu->ebp;
    user_regs_->eax = cpu->eax;
//  user_regs_->xds = cpu->xds;
//  user_regs_->xes = cpu->xes;
//  user_regs_->xfs = cpu->xfs;
//  user_regs_->xgs = cpu->xgs;
    user_regs_->orig_eax = cpu->eax;
    user_regs_->eip = cpu->eip;
//  user_regs_->xcs = cpu->xcs;
    user_regs_->eflags = cpu->eflags;
    user_regs_->esp = cpu->esp;
//  user_regs_->xss = cpu->xss;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void get_user_regs_and_syscall(struct task *task, struct user_regs_struct_ *user_regs_) {
    get_user_regs(&task->cpu, user_regs_);
    user_regs_->orig_eax = task->ptrace.syscall;
    if (ptrace_in_syscall_entry_stop(task))
        user_regs_->eax = (dword_t) (sdword_t) _ENOSYS;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void set_user_regs(struct task *task, struct user_regs_struct_ *user_regs_) {
    struct cpu_state *cpu = &task->cpu;
    cpu->ebx = user_regs_->ebx;
    cpu->ecx = user_regs_->ecx;
    cpu->edx = user_regs_->edx;
    cpu->esi = user_regs_->esi;
    cpu->edi = user_regs_->edi;
    cpu->ebp = user_regs_->ebp;
    cpu->eax = user_regs_->eax;
//  cpu->xds = user_regs_->xds;
//  cpu->xes = user_regs_->xes;
//  cpu->xfs = user_regs_->xfs;
//  cpu->xgs = user_regs_->xgs;
//  cpu->eax = user_regs_->orig_eax;
    cpu->eip = user_regs_->eip;
//  cpu->xcs = user_regs_->xcs;
    cpu->eflags = user_regs_->eflags;
    expand_flags(cpu);
    cpu->df_offset = cpu->df ? -1 : 1;
    cpu->esp = user_regs_->esp;
//  cpu->xss = user_regs_->xss;
    task->ptrace.syscall = (int) user_regs_->orig_eax;
    if (ptrace_in_syscall_entry_stop(task))
        cpu->eax = user_regs_->orig_eax;
}

static int ptrace_getregset(struct task *tracer, struct task *child, guest_addr_t iov_addr,
        qword_t note_type) {
    int check = ptrace_regset_check(tracer, child, iov_addr, note_type);
    if (check < 0)
        return check;
    switch (note_type) {
        case NT_PRSTATUS_: {
            if (child->abi == GUEST_ABI_ARM64) {
                struct user_pt_regs_arm64_ user_regs_arm64 = {};
                get_user_regs_arm64(child, &user_regs_arm64);
                return ptrace_getregset_write(tracer, iov_addr, &user_regs_arm64, sizeof(user_regs_arm64));
            } else if (child->abi == GUEST_ABI_RISCV64) {
                struct user_regs_struct_riscv64_ user_regs_riscv64 = {};
                get_user_regs_riscv64(child, &user_regs_riscv64);
                return ptrace_getregset_write(tracer, iov_addr, &user_regs_riscv64, sizeof(user_regs_riscv64));
            } else if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64 = {};
                get_user_regs_amd64(child, &user_regs_amd64);
                return ptrace_getregset_write(tracer, iov_addr, &user_regs_amd64, sizeof(user_regs_amd64));
            } else {
                struct user_regs_struct_ user_regs_ = {};
                get_user_regs_and_syscall(child, &user_regs_);
                user_regs_.orig_eax = child->ptrace.syscall;
                return ptrace_getregset_write(tracer, iov_addr, &user_regs_, sizeof(user_regs_));
            }
        }
        case NT_PRFPREG_:
        case NT_X86_XSTATE_: {
            if (child->abi == GUEST_ABI_ARM64) {
                if (note_type != NT_PRFPREG_)
                    return _EINVAL;
                struct user_fpsimd_state_arm64_ user_fpregs_arm64 = {};
                get_user_fpregs_arm64(child, &user_fpregs_arm64);
                return ptrace_getregset_write(tracer, iov_addr, &user_fpregs_arm64, sizeof(user_fpregs_arm64));
            } else if (child->abi == GUEST_ABI_RISCV64) {
                if (note_type != NT_PRFPREG_)
                    return _EINVAL;
                struct user_fpregs_struct_riscv64_ user_fpregs_riscv64 = {};
                get_user_fpregs_riscv64(child, &user_fpregs_riscv64);
                return ptrace_getregset_write(tracer, iov_addr, &user_fpregs_riscv64, sizeof(user_fpregs_riscv64));
            } else if (child->abi == GUEST_ABI_AMD64) {
                struct user_fpregs_struct_amd64_ user_fpregs_amd64 = {};
                get_user_fpregs_amd64(child, &user_fpregs_amd64);
                return ptrace_getregset_write(tracer, iov_addr, &user_fpregs_amd64, sizeof(user_fpregs_amd64));
            } else {
                struct user_fpregs_struct_ user_fpregs_ = {};
                return ptrace_getregset_write(tracer, iov_addr, &user_fpregs_, sizeof(user_fpregs_));
            }
        }
        case NT_ARM_SYSTEM_CALL_: {
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            int syscall_no = child->ptrace.syscall;
            return ptrace_getregset_write(tracer, iov_addr, &syscall_no, sizeof(syscall_no));
        }
        // TPIDR_EL0, the thread pointer, then TPIDR2_EL0, which is zero
        // without SME -- Linux 6.12's tls_get, 16 bytes. How a debugger finds
        // a thread's TLS, and so its thread: gdb's ps_get_thread_area reads
        // it here for libthread_db, and without it every gdb session on an
        // arm64 guest said "Cannot find user-level thread for LWP" and ran
        // with "thread debugging will not be available".
        case NT_ARM_TLS_: {
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            qword_t tls[2] = { child->cpu.arm64_tpidr, 0 };
            return ptrace_getregset_write(tracer, iov_addr, tls, sizeof(tls));
        }
        // The hardware breakpoint and watchpoint registers. There are none --
        // nothing in AOK arms one -- and that is the answer: an ARMv8 debug
        // architecture with no slots, which a debugger reads as "use software
        // ones", as x86's debug registers read as zero (PEEKUSER below). gdb
        // asks both at every start and, refused, printed "Unable to determine
        // the number of hardware watchpoints available" and the same for
        // breakpoints on every run.
        case NT_ARM_HW_BREAK_:
        case NT_ARM_HW_WATCH_: {
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            struct user_hwdebug_state_arm64_ state = {
                .dbg_info = 0x6 << 8,   // ID_AA64DFR0_EL1.DebugVer: ARMv8
            };
            return ptrace_getregset_write(tracer, iov_addr, &state, sizeof(state));
        }
        default:
            return _EINVAL;
    }
}

// SETREGSET of the debug registers: with no slots there is nothing to arm, so
// a write that enables one is refused and anything else -- a debugger clearing
// what it thinks is there -- succeeds. However short the vector: a tracer that
// knows the count writes only that many entries, here none.
static int ptrace_set_hwdebug(struct task *tracer, guest_addr_t iov_addr) {
    struct user_hwdebug_state_arm64_ state = {
        .dbg_info = 0x6 << 8,
    };
    int err = ptrace_setregset_read(tracer, iov_addr, &state, sizeof(state));
    if (err < 0)
        return err;
    for (int i = 0; i < 16; i++)
        if (state.dbg_regs[i].ctrl & 1)
            return _ENOSPC;
    return 0;
}

static int ptrace_setregset(struct task *tracer, struct task *child, guest_addr_t iov_addr,
        qword_t note_type) {
    int check = ptrace_regset_check(tracer, child, iov_addr, note_type);
    if (check < 0)
        return check;
    switch (note_type) {
        case NT_PRSTATUS_: {
            if (child->abi == GUEST_ABI_ARM64) {
                struct user_pt_regs_arm64_ user_regs_arm64;
                get_user_regs_arm64(child, &user_regs_arm64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_regs_arm64, sizeof(user_regs_arm64));
                if (err < 0)
                    return err;
                set_user_regs_arm64(child, &user_regs_arm64);
            } else if (child->abi == GUEST_ABI_RISCV64) {
                struct user_regs_struct_riscv64_ user_regs_riscv64;
                get_user_regs_riscv64(child, &user_regs_riscv64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_regs_riscv64, sizeof(user_regs_riscv64));
                if (err < 0)
                    return err;
                set_user_regs_riscv64(child, &user_regs_riscv64);
            } else if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64;
                get_user_regs_amd64(child, &user_regs_amd64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_regs_amd64, sizeof(user_regs_amd64));
                if (err < 0)
                    return err;
                set_user_regs_amd64(child, &user_regs_amd64);
            } else {
                struct user_regs_struct_ user_regs_ = {};
                get_user_regs_and_syscall(child, &user_regs_);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_regs_, sizeof(user_regs_));
                if (err < 0)
                    return err;
                set_user_regs(child, &user_regs_);
            }
            return 0;
        }
        case NT_PRFPREG_:
        case NT_X86_XSTATE_: {
            if (child->abi == GUEST_ABI_ARM64) {
                if (note_type != NT_PRFPREG_)
                    return _EINVAL;
                struct user_fpsimd_state_arm64_ user_fpregs_arm64;
                get_user_fpregs_arm64(child, &user_fpregs_arm64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_fpregs_arm64, sizeof(user_fpregs_arm64));
                if (err < 0)
                    return err;
                set_user_fpregs_arm64(&child->cpu, &user_fpregs_arm64);
            } else if (child->abi == GUEST_ABI_RISCV64) {
                if (note_type != NT_PRFPREG_)
                    return _EINVAL;
                struct user_fpregs_struct_riscv64_ user_fpregs_riscv64;
                get_user_fpregs_riscv64(child, &user_fpregs_riscv64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_fpregs_riscv64, sizeof(user_fpregs_riscv64));
                if (err < 0)
                    return err;
                set_user_fpregs_riscv64(&child->cpu, &user_fpregs_riscv64);
            } else if (child->abi == GUEST_ABI_AMD64) {
                struct user_fpregs_struct_amd64_ user_fpregs_amd64;
                get_user_fpregs_amd64(child, &user_fpregs_amd64);
                int err = ptrace_setregset_read(tracer, iov_addr, &user_fpregs_amd64, sizeof(user_fpregs_amd64));
                if (err < 0)
                    return err;
                set_user_fpregs_amd64(&child->cpu, &user_fpregs_amd64);
            } else {
                struct user_fpregs_struct_ user_fpregs_ = {};
                int err = ptrace_setregset_read(tracer, iov_addr, &user_fpregs_, sizeof(user_fpregs_));
                if (err < 0)
                    return err;
                // TODO set floating point registers for i386 tracees
                (void) user_fpregs_;
            }
            return 0;
        }
        case NT_ARM_HW_BREAK_:
        case NT_ARM_HW_WATCH_:
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            return ptrace_set_hwdebug(tracer, iov_addr);
        case NT_ARM_TLS_: {
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            // TPIDR_EL0 first, as Linux's tls_set takes it; TPIDR2_EL0 has
            // nowhere to go without SME.
            qword_t tls[2] = { child->cpu.arm64_tpidr, 0 };
            int err = ptrace_setregset_read(tracer, iov_addr, tls, sizeof(tls));
            if (err < 0)
                return err;
            child->cpu.arm64_tpidr = tls[0];
            return 0;
        }
        case NT_ARM_SYSTEM_CALL_: {
            if (child->abi != GUEST_ABI_ARM64)
                return _EINVAL;
            int syscall_no = child->ptrace.syscall;
            int err = ptrace_setregset_read(tracer, iov_addr, &syscall_no, sizeof(syscall_no));
            if (err < 0)
                return err;
            child->ptrace.syscall = syscall_no;
            // Dispatch decodes the number from x8 after the entry stop
            // resumes, so changing the syscall means rewriting x8 too.
            if (ptrace_in_syscall_entry_stop(child))
                child->cpu.arm64_regs[arm64_x8] = (qword_t) (sqword_t) syscall_no;
            return 0;
        }
        default:
            return _EINVAL;
    }
}

// PTRACE_GET_SYSCALL_INFO: what the stopped tracee's syscall is, read the way
// Linux's ptrace_get_syscall_info reads it. strace 6 asks this at every syscall
// stop once a self-test at startup finds it working, and falls back to reading
// the register set when it does not: the request was missing, so every guest
// strace took the fallback, and the default arm answered EPERM for it.
//
// Which stop decides the op, as on Linux: a syscall stop only with
// TRACESYSGOOD (its si_code is SIGTRAP|0x80 -- a plain SIGTRAP one is NONE),
// told entry from exit by its message; a PTRACE_EVENT_SECCOMP stop is SECCOMP;
// anything else is NONE, which still carries the architecture and the two
// pointers. The return is the size of what the op fills in -- 24, 80, 33 or 84
// -- and only as much of it as the tracer's buffer holds is copied.
static dword_t ptrace_audit_arch(const struct task *task) {
    switch (task->abi) {
        case GUEST_ABI_AMD64: return 0xc000003e;    // AUDIT_ARCH_X86_64
        case GUEST_ABI_ARM64: return 0xc00000b7;    // AUDIT_ARCH_AARCH64
        case GUEST_ABI_RISCV64: return 0xc00000f3;  // AUDIT_ARCH_RISCV64
        case GUEST_ABI_I386:
        default: return 0x40000003;                 // AUDIT_ARCH_I386
    }
}

// Linux's syscall_get_arguments: the six argument registers of each ABI. At a
// syscall-entry or seccomp stop none of them has been overwritten yet, arm64's
// x0 and riscv64's a0 included.
static void ptrace_syscall_args(const struct task *task, qword_t args[6]) {
    const struct cpu_state *cpu = &task->cpu;
    switch (task->abi) {
        case GUEST_ABI_AMD64:
            args[0] = cpu->amd64_regs[amd64_rdi];
            args[1] = cpu->amd64_regs[amd64_rsi];
            args[2] = cpu->amd64_regs[amd64_rdx];
            args[3] = cpu->amd64_regs[amd64_r10];
            args[4] = cpu->amd64_regs[amd64_r8];
            args[5] = cpu->amd64_regs[amd64_r9];
            break;
        case GUEST_ABI_ARM64:
            for (int i = 0; i < 6; i++)
                args[i] = cpu->arm64_regs[arm64_x0 + i];
            break;
        case GUEST_ABI_RISCV64:
            for (int i = 0; i < 6; i++)
                args[i] = cpu->riscv64_regs[riscv64_a0 + i];
            break;
        case GUEST_ABI_I386:
        default:
            args[0] = cpu->ebx;
            args[1] = cpu->ecx;
            args[2] = cpu->edx;
            args[3] = cpu->esi;
            args[4] = cpu->edi;
            args[5] = cpu->ebp;
            break;
    }
}

// The number in the register the ABI passes it in -- what a seccomp stop,
// which comes before any syscall stop would have recorded ptrace.syscall, has
// to go on.
static qword_t ptrace_syscall_nr_reg(const struct task *task) {
    const struct cpu_state *cpu = &task->cpu;
    switch (task->abi) {
        case GUEST_ABI_AMD64: return cpu->amd64_regs[amd64_rax];
        case GUEST_ABI_ARM64: return cpu->arm64_regs[arm64_x8];
        case GUEST_ABI_RISCV64: return cpu->riscv64_regs[riscv64_a7];
        case GUEST_ABI_I386:
        default: return cpu->eax;
    }
}

// The result register, sign-extended from the ABI's width (a 32-bit guest's
// -ENOENT is -2, not 0xfffffffe).
static sqword_t ptrace_syscall_rval(const struct task *task) {
    const struct cpu_state *cpu = &task->cpu;
    switch (task->abi) {
        case GUEST_ABI_AMD64: return (sqword_t) cpu->amd64_regs[amd64_rax];
        case GUEST_ABI_ARM64: return (sqword_t) cpu->arm64_regs[arm64_x0];
        case GUEST_ABI_RISCV64: return (sqword_t) cpu->riscv64_regs[riscv64_a0];
        case GUEST_ABI_I386:
        default: return (sqword_t) (sdword_t) cpu->eax;
    }
}

static void ptrace_syscall_pointers(const struct task *task, qword_t *ip, qword_t *sp) {
    const struct cpu_state *cpu = &task->cpu;
    switch (task->abi) {
        case GUEST_ABI_AMD64:
            *ip = cpu->amd64_rip;
            *sp = cpu->amd64_regs[amd64_rsp];
            break;
        case GUEST_ABI_ARM64:
            *ip = cpu->arm64_pc;
            *sp = cpu->arm64_sp;
            break;
        case GUEST_ABI_RISCV64:
            *ip = cpu->riscv64_pc;
            *sp = cpu->riscv64_regs[riscv64_sp];
            break;
        case GUEST_ABI_I386:
        default:
            *ip = cpu->eip;
            *sp = cpu->esp;
            break;
    }
}

// Call with the tracee stopped and its ptrace lock held (find_child).
static int_t ptrace_get_syscall_info(struct task *child, qword_t user_size, guest_addr_t data) {
    struct ptrace_syscall_info_ info;
    memset(&info, 0, sizeof(info));
    info.op = PTRACE_SYSCALL_INFO_NONE_;
    info.arch = ptrace_audit_arch(child);
    ptrace_syscall_pointers(child, &info.instruction_pointer, &info.stack_pointer);
    size_t actual = offsetof(struct ptrace_syscall_info_, entry);

    int code = child->ptrace.has_siginfo ? child->ptrace.info.code : 0;
    if (code == (SIGTRAP_ | 0x80)) {
        if (child->ptrace.eventmsg == PTRACE_EVENTMSG_SYSCALL_ENTRY_) {
            info.op = PTRACE_SYSCALL_INFO_ENTRY_;
            // The canonical number, as the register set reports it here: a
            // tracer's rewrite at this stop is what will run.
            info.entry.nr = (qword_t) (sqword_t) child->ptrace.syscall;
            ptrace_syscall_args(child, info.entry.args);
            actual = offsetof(struct ptrace_syscall_info_, entry.args) + sizeof(info.entry.args);
        } else if (child->ptrace.eventmsg == PTRACE_EVENTMSG_SYSCALL_EXIT_) {
            // Linux's syscall_get_error: an error is -4095..-1, and then rval
            // is that errno; anything else is the plain return value.
            sqword_t rval = ptrace_syscall_rval(child);
            info.op = PTRACE_SYSCALL_INFO_EXIT_;
            info.exit.rval = rval;
            info.exit.is_error = rval < 0 && rval >= -4095;
            actual = offsetof(struct ptrace_syscall_info_, exit.is_error) + sizeof(info.exit.is_error);
        }
    } else if (code == ((PTRACE_EVENT_SECCOMP_ << 8) | SIGTRAP_)) {
        info.op = PTRACE_SYSCALL_INFO_SECCOMP_;
        info.seccomp.nr = ptrace_syscall_nr_reg(child);
        if (child->abi == GUEST_ABI_I386)
            info.seccomp.nr = (dword_t) info.seccomp.nr;
        ptrace_syscall_args(child, info.seccomp.args);
        info.seccomp.ret_data = (dword_t) child->ptrace.eventmsg;
        actual = offsetof(struct ptrace_syscall_info_, seccomp.ret_data) + sizeof(info.seccomp.ret_data);
    }

    size_t write = user_size < actual ? (size_t) user_size : actual;
    if (write != 0 && user_write(data, &info, write))
        return _EFAULT;
    return (int_t) actual;
}

static bool ptrace_sigkill_pending(void) {
    lock(&current->sighand->lock, 0);
    bool pending = sigset_has(current->pending | current->sighand->pending, SIGKILL_);
    unlock(&current->sighand->lock);
    return pending;
}

// `event` is the PTRACE_EVENT_* this stop reports, or 0 for a signal or syscall
// stop. `eventmsg` is what PTRACE_GETEVENTMSG answers while the tracee sits in
// it -- the new task's pid at a clone/fork/vfork event, 0 at a
// signal-delivery-stop. Linux's ptrace_stop() sets the message at every stop
// and wait never clears it, so a tracer can wait for the stop first and ask
// for the message afterwards, which is the only order it can ask in.
//
// `info` is what PTRACE_GETSIGINFO answers while the tracee sits in the stop,
// or NULL for a stop that HAS no siginfo -- Linux's ptrace_stop() takes the
// same argument, and do_jobctl_trap passes NULL for an unseized tracee's
// group-stop, which is why GETSIGINFO fails there (ptrace.has_siginfo).
//
// `trap_stop` marks the stop that answers ptrace.trap_stop itself
// (ptrace_trap_stop_if_pending), which is owed only while the flag is still set.
// `delivery` marks a signal-delivery-stop (ptrace_signal_stop), the one stop a
// resume with a signal re-delivers with the stop's own siginfo.
static void ptrace_stop_common(int sig, const struct siginfo_ *info, bool syscall_stop,
        bool delivery, int event, qword_t eventmsg, bool trap_stop) {
    struct task *tracer = NULL;

    // wait4() publishes and consumes ptrace-stop state while holding pids_lock.
    // Publish the stop and notify child_exit under the same lock so the tracer
    // can't miss the stop between its wait4 scan and sleep.
    complex_lockt(&pids_lock, 0);
    lock(&current->ptrace.lock, 0);
    // Checked again under the lock: a detach, or another stop, can have taken
    // the trap back since the lockless look. A task no longer traced must not
    // stop at all -- it would wait for a resume nobody is left to send.
    if (trap_stop && (!task_trap_stop_pending(current) || !current->ptrace.traced)) {
        unlock(&current->ptrace.lock);
        unlock(&pids_lock);
        return;
    }
    // A task with SIGKILL pending does not stop, as in Linux's ptrace_stop.
    // It used to report the stop and then wait for a wakeup the kill had
    // already spent: a tracee killed inside a syscall under PTRACE_SYSCALL
    // reported its syscall-exit stop and then waited forever, while its tracer
    // waited for it to die. pids_lock -> ptrace.lock -> sighand->lock is the
    // order the wait below already uses.
    if (ptrace_sigkill_pending()) {
        unlock(&current->ptrace.lock);
        unlock(&pids_lock);
        do_exit_group(SIGKILL_);
    }
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    if (syscall_stop && current->ptrace.sysgood)
        current->ptrace.signal |= 0x80;
    // Set in the same critical section as `stopped`. The event used to be
    // recorded in a lock section of its own beforehand, so a PTRACE_INTERRUPT
    // landing between the two could relabel an event-stop as its own.
    current->ptrace.trap_event = event;
    current->ptrace.eventmsg = eventmsg;
    current->ptrace.stop_discards_signal = !delivery && !syscall_stop;
    current->ptrace.has_siginfo = info != NULL;
    current->ptrace.info = info != NULL ? *info : SIGINFO_NIL;
    current->ptrace_delivery_stop = delivery;
    // Any stop answers a pending PTRACE_INTERRUPT, as Linux's ptrace_stop
    // clears JOBCTL_TRAP_STOP for every stop: a tracee interrupted on its way
    // into some other stop reports that stop and nothing more. An interrupt
    // that arrives from here on sets it again, and is taken once the tracee is
    // resumed, which is what Linux does with one sent to a stopped tracee.
    //
    // When the interrupt was a queued SIGTRAP, a stop reached first left it
    // behind: it came back as a second, plain SIGTRAP stop, a tracer
    // re-injected it, and strace -f killed the program it had started in 23 of
    // 50 runs.
    __atomic_store_n(&current->ptrace.trap_stop, false, __ATOMIC_RELEASE);
    // A SIGCONT's notice is answered only by a PTRACE_EVENT_STOP, as Linux's
    // ptrace_stop clears JOBCTL_TRAP_NOTIFY for a stop whose si_code says
    // PTRACE_EVENT_STOP and for no other: a tracee that reaches a syscall stop
    // first still owes the event-stop once it is resumed. Measured on 6.12: a
    // SIGCONT landing at a syscall-entry stop gives the exit stop, then
    // 0x80057f, then the SIGCONT's own signal-delivery-stop.
    if (event == PTRACE_EVENT_STOP_)
        __atomic_store_n(&current->ptrace_trap_notify, false, __ATOMIC_RELEASE);
    unlock(&current->ptrace.lock);

    // What the tracer is sent: Linux's do_notify_parent_cldstop(for_ptracer),
    // naming the stopped thread itself. This sent the stopped process's EXIT
    // signal with an empty siginfo -- SI_KERNEL and no pid, or nothing at all
    // for a child cloned with exit signal 0 -- and past the tracer's
    // SA_NOCLDSTOP, which it never asked about.
    //
    // do_jobctl_trap is the one caller of Linux's ptrace_stop that says
    // CLD_STOPPED: a PTRACE_EVENT_STOP, or an unseized tracee's group-stop, the
    // one stop with no siginfo. Its si_status is the group's stop signal: the
    // stop signal for a group-stop, and 0 for the rest, which are taken while
    // the group is not stopped (an interrupt, a seized child's first stop, the
    // SIGCONT that ended a listen). Every other stop is CLD_TRAPPED with the
    // signal it reports, less TRACESYSGOOD's bit -- SIGTRAP for a syscall or
    // event stop. Measured on Linux 6.12 for each of these but a seized
    // child's first stop and a listen's end, which take the interrupt's path
    // there as here.
    struct siginfo_ notice = {
        .code = CLD_TRAPPED_,
        .child.pid = current->pid,
        .child.uid = current->uid,
        .child.status = sig & 0x7f,
    };
    if (event == PTRACE_EVENT_STOP_ || info == NULL) {
        notice.code = CLD_STOPPED_;
        if (sig == SIGTRAP_)
            notice.child.status = 0;
    }
    tracer = ptrace_tracer(current);
    if (tracer != NULL) {
        task_ref_cnt_mod(tracer, 1);
        notify(&tracer->group->child_exit);
    }
    unlock(&pids_lock);

    if (tracer != NULL) {
        notify_parent_cldstop(tracer, notice);
        task_ref_cnt_mod(tracer, -1);
    }

    lock(&current->ptrace.lock, 0);
    TASK_MAY_BLOCK {
        // SIGKILL is checked before each wait as well as after each wakeup: one
        // that arrived between publishing the stop and getting here has already
        // had its wakeup, and one that PTRACE_KILL sends along with its resume
        // must still end the task here rather than let it run on.
        for (;;) {
            if (ptrace_sigkill_pending()) {
                STRACE("%d received a SIGKILL in ptrace stop\n", current->pid);
                unlock(&current->ptrace.lock);
                do_exit_group(SIGKILL_);
            }
            if (!current->ptrace.stopped)
                break;
            wait_for_ignore_signals(&current->ptrace.cond, &current->ptrace.lock, NULL);
        }
    }
    unlock(&current->ptrace.lock);
}

// A signal-delivery-stop: `info` describes a signal the task was taking.
void ptrace_signal_stop(int sig, struct siginfo_ *info) {
    ptrace_stop_common(sig, info, false, true, 0, 0, false);
}

void ptrace_event_stop(int sig, struct siginfo_ *info, int event, qword_t eventmsg) {
    struct siginfo_ event_info = *info;
    event_info.sig = SIGTRAP_;
    event_info.code = (event << 8) | SIGTRAP_;
    ptrace_stop_common(sig, &event_info, false, false, event, eventmsg, false);
}

// Take the PTRACE_EVENT_STOP this task owes its tracer, if it owes one: the
// stop PTRACE_INTERRUPT asked for, or a seized tracer's new child's first stop.
// Linux's do_jobctl_trap.
//
// Called wherever the task looks for signals -- handle_interrupt,
// native_checkpoint, and task_thread before a new task's first instruction --
// and BEFORE it takes any, since get_signal handles JOBCTL_TRAP_STOP first. The
// mask is never consulted: nothing about the flag is a signal.
//
// Status 0x80057f. The siginfo is do_jobctl_trap's: SIGTRAP, si_code
// (PTRACE_EVENT_STOP << 8) | SIGTRAP, and the stopped task's OWN id and uid
// (task_pid_vnr(current)); the message is 0. Measured on Linux 6.12, for an
// interrupt and for a new child alike. The interrupt stop used to report pid 0.
void ptrace_trap_stop_if_pending(void) {
    if (current == NULL || !task_trap_stop_pending(current))
        return;
    // A process that is job-control stopped reports the STOP SIGNAL instead
    // (do_jobctl_trap: SIGTRAP only when no group-stop is in force), and
    // reporting it is group_stop_wait's, which every caller of this reaches
    // next: ptrace_group_stop is a PTRACE_EVENT_STOP for a seized tracee, so it
    // answers the flag. A SIGCONT's notice followed by a new SIGSTOP is the
    // case: Linux reports one stop, 0x80137f, and this took two.
    if (current->ptrace.traced && current->group->stopped)
        return;
    struct siginfo_ info = {
        .sig = SIGTRAP_,
        .code = (PTRACE_EVENT_STOP_ << 8) | SIGTRAP_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    ptrace_stop_common(SIGTRAP_, &info, false, false, PTRACE_EVENT_STOP_, 0, true);
}

void ptrace_syscall_stop(struct cpu_state *cpu) {
    // The stopped task's own id and uid, as Linux's ptrace_do_notify fills in
    // for every ptrace_notify stop. Measured on 6.12: a syscall stop's
    // PTRACE_GETSIGINFO names the tracee. This said pid 0.
    struct siginfo_ info = {
        .sig = SIGTRAP_,
        .code = current->ptrace.sysgood ? (SIGTRAP_ | 0x80) : SIGTRAP_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };

    lock(&current->ptrace.lock, 0);
    bool entry = !current->ptrace.syscall_stopped;
    if (entry)
        current->ptrace.syscall = current->abi == GUEST_ABI_AMD64 ?
            (int) cpu->amd64_regs[amd64_rax] :
            current->abi == GUEST_ABI_ARM64 ?
            (int) cpu->arm64_regs[arm64_x8] :
            current->abi == GUEST_ABI_RISCV64 ?
            (int) cpu->riscv64_regs[riscv64_a7] : (int) cpu->eax;
    current->ptrace.syscall_stopped = entry;
    unlock(&current->ptrace.lock);

    ptrace_stop_common(SIGTRAP_, &info, true, false, 0,
            entry ? PTRACE_EVENTMSG_SYSCALL_ENTRY_ : PTRACE_EVENTMSG_SYSCALL_EXIT_, false);
}

// Report a job-control group-stop to the tracer and block until it resumes us.
//
// A traced task that enters group-stop (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU) would
// otherwise park silently in handle_interrupt's group-stop loop, where the
// tracer's wait4 can't see it (it never sets ptrace.stopped) and PTRACE_CONT
// has nothing to resume -- so strace -f following a fork/clone child hangs
// forever once the child group-stops on the injected SIGSTOP.
//
// HOW the stop is reported is the one thing the two attach styles disagree
// about, and a tracer of either kind must be able to tell a group-stop from a
// signal-delivery-stop of the same signal. Both arms below are Linux's
// do_jobctl_trap, measured on 6.12.101:
//
//   - A SEIZED tracee (PTRACE_SEIZE, how strace attaches) gets the STOP SIGNAL
//     with a PTRACE_EVENT_STOP event: status 0x80137f for SIGSTOP, 0x80147f for
//     SIGTSTP, 0x80157f for SIGTTIN. The siginfo carries that signal with
//     si_code (PTRACE_EVENT_STOP << 8) | signal and the tracee's OWN pid.
//     strace switches on WSTOPSIG to recognise the group-stop, so reporting
//     SIGTRAP here -- as this used to -- left it unrecognised: strace resumed
//     with PTRACE_CONT, which lifts the group stop, and `kill -STOP` on a
//     process under `strace -f` did not stop it at all. The tracee stayed S
//     where Linux leaves it t, and strace printed nothing where Linux prints
//     "--- stopped by SIGSTOP ---".
//
//   - An UNSEIZED tracee (PTRACE_TRACEME/PTRACE_ATTACH, how gdb attaches) gets
//     the stop signal with NO event and NO siginfo: status 0x137f, and
//     PTRACE_GETSIGINFO fails with EINVAL. That EINVAL is the entire
//     difference between this stop and a signal-delivery-stop of the same
//     SIGSTOP, which reports the identical status word, and it is how gdb and
//     strace tell them apart. Answering GETSIGINFO here made every group-stop
//     look like a signal to re-inject: a probe that injects each stop's own
//     signal logged 2.8 million consecutive 0x137f stops in four minutes,
//     where Linux takes exactly two and runs on.
//
// A resume ends the stop. PTRACE_CONT lifts group->stopped (see
// ptrace_resume_child_locked), and an injected signal is discarded rather than
// re-delivered, as Linux discards what ptrace_stop() returns here. A seizing
// tracer that wants the tracee to STAY job-control stopped says so with
// PTRACE_LISTEN, which parks it in group_stop_wait's listening branch.
void ptrace_group_stop(void) {
    lock(&current->group->lock, 0);
    int stop_sig = (current->group->group_exit_code >> 8) & 0xff;
    unlock(&current->group->lock);
    if (stop_sig == 0)
        stop_sig = SIGSTOP_;

    // No message, as in Linux's do_jobctl_trap. This passed the stop signal,
    // which no tracer could see while wait4 cleared every message.
    if (current->ptrace.seized) {
        struct siginfo_ info = {
            .sig = stop_sig,
            .code = (PTRACE_EVENT_STOP_ << 8) | stop_sig,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        };
        ptrace_stop_common(stop_sig, &info, false, false, PTRACE_EVENT_STOP_, 0, false);
    } else {
        // NULL: no siginfo, so PTRACE_GETSIGINFO gives EINVAL. A group-stop,
        // not a signal-delivery-stop -- nothing is being delivered.
        ptrace_stop_common(stop_sig, NULL, false, false, 0, 0, false);
    }
}

// End a PTRACE_LISTEN. Called by the tracee itself when something has ended the
// listen (group_stop_wait); a tracer's resume clears the flag directly.
void ptrace_listen_end(void) {
    lock(&current->ptrace.lock, 0);
    __atomic_store_n(&current->ptrace.listening, false, __ATOMIC_RELEASE);
    unlock(&current->ptrace.lock);
}

// A SIGCONT has lifted the group-stop a listening tracee was waiting out.
// Linux reports that to the tracer as a PTRACE_EVENT_STOP carrying SIGTRAP --
// status 0x80057f, si_code (PTRACE_EVENT_STOP << 8) | SIGTRAP, the tracee's own
// pid -- before the tracee runs again. Measured on 6.12.101, where strace then
// prints the SIGCONT and resumes the interrupted syscall.
void ptrace_listen_cont_stop(void) {
    struct siginfo_ info = {
        .sig = SIGTRAP_,
        .code = (PTRACE_EVENT_STOP_ << 8) | SIGTRAP_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    ptrace_stop_common(SIGTRAP_, &info, false, false, PTRACE_EVENT_STOP_, 0, false);
}

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, dword_t data) {
    return sys_ptrace_guest(request, pid, addr, data);
}

dword_t sys_ptrace_guest(dword_t request, dword_t pid, guest_addr_t addr, guest_addr_t data) {
    switch (request) {
        case PTRACE_TRACEME_: {
            STRACE("ptrace(PTRACE_TRACEME, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            // A task that is already traced keeps its tracer: EPERM, as
            // Linux's ptrace_traceme answers. It used to re-link to the parent,
            // which took the tracee away from a tracer that had attached to it
            // -- and now would also replace who made the link, the thing a
            // set-id exec asks about (ptrace_link_capable).
            //
            // The link is made with the TRACEE's credentials, not the
            // parent's, as it has been on Linux since the CVE-2019-13272 fix:
            // the tracee chose to be traced, so it is its own privilege that
            // decides whether a set-id program it runs keeps what it grants.
            complex_lockt(&pids_lock, 0);
            lock(&current->ptrace.lock, 0);
            bool already = current->ptrace.traced;
            if (!already) {
                current->ptrace.traced = true;
                current->ptrace.tracer = current->parent;
                current->ptrace_link_capable = current_capable(CAP_SYS_PTRACE_);
            }
            unlock(&current->ptrace.lock);
            unlock(&pids_lock);
            return already ? _EPERM : 0;
        }

        // PTRACE_ATTACH is what gdb uses; it differs from SEIZE in two ways.
        // It takes no options, and it STOPS the tracee -- the tracer's first
        // wait() must report a SIGSTOP signal-delivery-stop. It was missing
        // entirely and fell through to the default arm's EPERM, so attaching a
        // debugger to a running guest process simply could not be done.
        case PTRACE_ATTACH_:
        case PTRACE_SEIZE_: {
            bool seize = request == PTRACE_SEIZE_;
            STRACE("ptrace(PTRACE_%s, %d, %#llx, %#llx)", seize ? "SEIZE" : "ATTACH", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            // SEIZE takes its options in `data` and rejects a nonzero `addr`.
            // ATTACH ignores both, as Linux does.
            if (seize && addr != 0)
                return _EIO;

            complex_lockt(&pids_lock, 0);
            struct task *child = pid_get_task_zombie(pid);
            if (child == NULL) {
                unlock(&pids_lock);
                return _ESRCH;
            }
            // Not a thread of our own process, and not a task that has already
            // exited -- Linux's same_thread_group and exit_state refusals, both
            // EPERM, measured on 6.12. Attaching a zombie used to succeed, and
            // now that a traced zombie belongs to its tracer that would have
            // taken it away from the parent about to reap it. A leader that
            // left while its threads run on counts as exited, as it does on
            // Linux, where it is a zombie.
            if (child->group == current->group || child->zombie ||
                    atomic_load_explicit(&child->exit_finished, memory_order_acquire)) {
                unlock(&pids_lock);
                return _EPERM;
            }
            // Linux's __ptrace_may_access(PTRACE_MODE_ATTACH_REALCREDS): the
            // caller's real ids must be all of the target's, the target must
            // be dumpable, or the caller must hold CAP_SYS_PTRACE. There was
            // no check at all, so any user could attach to a root process --
            // sshd, a setuid program mid-run -- and read and write its memory
            // and registers: root for anyone.
            if (!task_ptrace_may_access(child, PTRACE_MODE_ATTACH_ | PTRACE_MODE_REALCREDS_)) {
                unlock(&pids_lock);
                return _EPERM;
            }

            lock(&child->ptrace.lock, 0);
            if (child->ptrace.traced) {
                unlock(&child->ptrace.lock);
                unlock(&pids_lock);
                return _EPERM;
            }

            child->ptrace.traced = true;
            child->ptrace.seized = seize;
            child->ptrace.tracer = current;
            // The tracer's privilege as it is now, at the attach: dropping it
            // later does not make the link any less privileged (Linux's
            // __ptrace_link records current_cred() here).
            child->ptrace_link_capable = current_capable(CAP_SYS_PTRACE_);
            child->ptrace.options = seize ? data : 0;
            child->ptrace.sysgood = seize && !!(data & PTRACE_O_TRACESYSGOOD_);
            child->ptrace.stop_at_syscall = false;
            child->ptrace.syscall_stopped = false;
            child->ptrace.trap_event = 0;
            child->ptrace.eventmsg = 0;
            // A seizing tracer is owed the group-stop of a tracee that is
            // already job-control stopped: Linux's ptrace_attach sets
            // JOBCTL_TRAP_STOP for a stopped task and waits below until it has
            // trapped. The flag is what still gets a stop reported if a
            // SIGCONT lifts the group-stop before the tracee has come round to
            // report it -- then as 0x80057f, as do_jobctl_trap would.
            bool attach_stopped = child->group->stopped;
            if (seize && attach_stopped)
                __atomic_store_n(&child->ptrace.trap_stop, true, __ATOMIC_RELEASE);
            // A process we are the parent of is found through our children;
            // everything else we trace, threads included, only through this
            // list. A thread's parent here is whichever task created it (or
            // inherited it), so "its parent is ours" says nothing about a
            // thread, and testing that alone could leave a traced thread on no
            // list at all.
            if (!(task_is_leader(child) && child->parent != NULL &&
                    child->parent->group == current->group))
                list_add(&current->ptracees, &child->ptrace_siblings);
            unlock(&child->ptrace.lock);
            // A tracee that was ALREADY group-stopped when we seized it is
            // parked in handle_interrupt's job-control wait with nothing left
            // to wake it, so it would never notice it is now traced and never
            // report the stop -- our wait4 would hang forever. Wake it; the
            // loop there re-checks ptrace.traced on every pass. This is the
            // race ptrace_group_stop() loses when the tracee reaches
            // raise(SIGSTOP) before the tracer reaches ptrace(). Linux does
            // the same in ptrace_attach(), which wakes a __TASK_STOPPED tracee
            // for exactly this reason.
            //
            // pids_lock is still held: it keeps `child` alive across the
            // notify, and pids_lock -> group->lock is the established order
            // (see handle_interrupt's own comment on taking pids_lock only
            // after dropping group->lock).
            lock(&child->group->lock, 0);
            if (child->group->stopped)
                notify(&child->group->stopped_cond);
            unlock(&child->group->lock);
            // ATTACH, unlike SEIZE, stops the tracee: Linux sends it a SIGSTOP
            // that the tracer then sees as a signal-delivery-stop. Sent after
            // ptrace.traced is set, so the tracee reports the stop to us
            // rather than just entering an ordinary job-control stop.
            if (!seize)
                send_signal(child, SIGSTOP_, SIGINFO_NIL);
            // Linux's JOBCTL_TRAPPING: an attach to a task that is already
            // stopped returns only once the task has re-entered the stop as a
            // TRACED one, so the tracer's next wait is that stop's and nothing
            // it does after the attach can come first. strace depends on it:
            // it starts its program stopped, seizes it, and then sends the
            // SIGCONT that lifts the stop itself. AOK returned at once, and in
            // 26 of 40 runs the SIGCONT got there before the tracee had
            // reported anything -- its group-stop was never shown to the tracer
            // at all, where Linux shows it every time (0x80137f).
            //
            // ptrace_stop_common publishes the stop and wakes this tracer's
            // child_exit under pids_lock, which this holds between waits. A
            // cap, and not an endless wait, because an attach must never hang
            // on a tracee that cannot get there; a signal for the tracer ends
            // it too. Either way the stop is still owed and still reported --
            // this only decides what comes first.
            if (!attach_stopped) {
                unlock(&pids_lock);
                return 0;
            }
            // The reference is dropped only once pids_lock is, as everywhere
            // else in this file.
            task_ref_cnt_mod(child, 1);
            struct timespec left = {.tv_sec = 2};
            while (child->ptrace.traced && child->ptrace.tracer == current &&
                    !child->ptrace.stopped && !child->zombie && !child->exiting) {
                if (wait_for_capped(&current->group->child_exit, &pids_lock, &left) != 0)
                    break;
            }
            unlock(&pids_lock);
            task_ref_cnt_mod(child, -1);
            return 0;
        }

        case PTRACE_PEEKTEXT_:
        case PTRACE_PEEKDATA_: {
            STRACE("ptrace(PTRACE_PEEKDATA, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            // A read the tracee's memory refuses is EIO, as Linux's
            // generic_ptrace_peekdata answers any short ptrace_access_vm;
            // EFAULT is only for the tracer's own buffer. It was EFAULT for
            // both.
            int err = 0;
            if (guest_abi_is_64bit(child->abi)) {
                qword_t peek;
                if (user_read_task_ptrace(child, addr, &peek, sizeof(peek)))
                    err = _EIO;
                else if (user_put(data, peek))
                    err = _EFAULT;
            } else {
                dword_t peek;
                if (user_read_task_ptrace(child, addr, &peek, sizeof(peek)))
                    err = _EIO;
                else if (user_put(data, peek))
                    err = _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return err;
        }

        case PTRACE_PEEKUSER_: {
            STRACE("ptrace(PTRACE_PEEKUSER, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            // Neither the real arm64 nor riscv64 kernel has PEEKUSER/GETREGS-
            // family requests (regsets only); don't hand either tracee the
            // i386 layout.
            if (child->abi == GUEST_ABI_ARM64 || child->abi == GUEST_ABI_RISCV64) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }

            if (child->abi == GUEST_ABI_AMD64) {
                qword_t peek;
                if (addr & (sizeof(peek) - 1)) {
                    unlock(&child->ptrace.lock);
                    return _EIO;
                }

                // Real struct user's u_debugreg[8] (x86_64: offsets 848..911,
                // verified against a real kernel header) -- the hardware
                // debug registers DR0-DR7. iSH has no hardware breakpoint/
                // watchpoint support at all (nothing ever arms one), so these
                // always legitimately read as zero -- truthful, not a stub.
                // gdb's own ptrace self-test (linux_ptrace_test_ret_to_nx)
                // and its hardware-watchpoint-capacity probe both read here
                // at every stop; this range previously fell outside the
                // (regs-struct-only) bounds check below and returned EIO,
                // which gdb surfaces as an alarming "Couldn't read debug
                // register: I/O error" instead of silently treating "0
                // registers armed" as normal.
                if (addr >= 848 && addr < 848 + 64) {
                    qword_t zero = 0;
                    if (user_put(data, zero)) {
                        unlock(&child->ptrace.lock);
                        return _EFAULT;
                    }
                    unlock(&child->ptrace.lock);
                    return 0;
                }

                struct user_regs_struct_amd64_ user_regs_amd64 = {};
                get_user_regs_amd64(child, &user_regs_amd64);

                if (addr >= sizeof(user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EIO;
                }

                memcpy(&peek, (char *) &user_regs_amd64 + addr, sizeof(peek));
                if (user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                dword_t peek;
                struct user_ user_ = {};
                get_user_regs_and_syscall(child, &user_.user_regs);

                if (addr & (sizeof(peek) - 1) || addr >= sizeof(struct user_)) {
                    unlock(&child->ptrace.lock);
                    return _EIO;
                }

                memcpy(&peek, (char *) &user_ + addr, sizeof(peek));
                if (user_put(data, peek)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_POKETEXT_:
        case PTRACE_POKEDATA_: {
            STRACE("ptrace(PTRACE_POKEDATA, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            // EIO, as generic_ptrace_pokedata answers a short write, not EFAULT.
            if (user_write_task_ptrace(child, addr, &data, ptrace_word_size(child))) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_CONT_: {
            STRACE("ptrace(PTRACE_CONT, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            int resume_sig = ptrace_resume_signal(data);
            if (resume_sig < 0) {
                unlock(&child->ptrace.lock);
                return resume_sig;
            }

            ptrace_resume_child_locked(child, resume_sig, false, false, false);
            return 0;
        }

        case PTRACE_KILL_: {
            STRACE("ptrace(PTRACE_KILL, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            child->ptrace.stopped = false;
            send_signal(child, SIGKILL_, SIGINFO_NIL);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SINGLESTEP_: {
            STRACE("ptrace(PTRACE_SINGLESTEP, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            int resume_sig = ptrace_resume_signal(data);
            if (resume_sig < 0) {
                unlock(&child->ptrace.lock);
                return resume_sig;
            }

            ptrace_resume_child_locked(child, resume_sig, true, false, false);
            return 0;
        }

        case PTRACE_INTERRUPT_: {
            STRACE("ptrace(PTRACE_INTERRUPT, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            if (addr != 0 || data != 0)
                return _EIO;

            complex_lockt(&pids_lock, 0);
            struct task *child = pid_get_task_zombie(pid);
            if (child != NULL)
                lock(&child->ptrace.lock, 0);
            // Not our tracee is ESRCH, as for every other request (find_tracee).
            // This was EPERM, which strace's detach reports as an error when the
            // task it is detaching from has just exited.
            if (child == NULL || !ptrace_traced_by(current, child)) {
                if (child != NULL)
                    unlock(&child->ptrace.lock);
                unlock(&pids_lock);
                return _ESRCH;
            }
            // Linux: only a SEIZE'd tracee can be interrupted; an ATTACH'd one
            // gets EIO. Measured on 6.12.
            if (!child->ptrace.seized) {
                unlock(&child->ptrace.lock);
                unlock(&pids_lock);
                return _EIO;
            }
            // JOBCTL_TRAP_STOP -- a flag, which neither SIGTRAP's disposition
            // nor the mask can defeat, and set whether or not the tracee is in
            // a stop now. One that is traps again once resumed (measured on
            // 6.12); this used to do nothing for it.
            __atomic_store_n(&child->ptrace.trap_stop, true, __ATOMIC_RELEASE);
            unlock(&child->ptrace.lock);
            // References taken under pids_lock, and the wake made without it,
            // as send_group_signal does: the wake can block on the lock the
            // tracee is waiting under.
            struct sighand *sighand = child->sighand;
            if (sighand != NULL)
                sighand_retain(sighand);
            task_ref_cnt_mod(child, 1);
            unlock(&pids_lock);
            if (sighand != NULL) {
                task_wake_for_ptrace_trap(child, sighand);
                sighand_release(sighand);
            }
            task_ref_cnt_mod(child, -1);
            return 0;
        }

        // PTRACE_LISTEN: the tracer has seen a group-stop and wants the tracee
        // to STAY in it rather than be resumed -- what strace does so that
        // `kill -STOP` on a traced process really stops it. Without this,
        // reporting the group-stop correctly would be worse than not reporting
        // it: strace answers a recognised group-stop with LISTEN, and that fell
        // through to the default arm's EPERM.
        //
        // Linux takes it only from a SEIZED tracee, and only at a stop whose
        // siginfo says PTRACE_EVENT_STOP -- a group-stop or a PTRACE_INTERRUPT
        // stop. An unseized tracee's group-stop and a signal-delivery-stop both
        // give EIO. Measured on 6.12.101, along with the 0 for an INTERRUPT
        // stop, which is why the test is the si_code and not "is a group-stop".
        case PTRACE_LISTEN_: {
            STRACE("ptrace(PTRACE_LISTEN, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            if (addr != 0 || data != 0)
                return _EIO;
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            if (!child->ptrace.seized || !child->ptrace.has_siginfo ||
                    ((child->ptrace.info.code >> 8) & 0xff) != PTRACE_EVENT_STOP_) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }
            // Armed only for a tracee that really is job-control stopped, so
            // the flag can never outlive a stop and swallow the report of the
            // NEXT one. group->stopped is _Atomic and read without its lock, as
            // group_stop_wait reads it -- taking it under ptrace.lock would be a
            // lock order this code deliberately does not have. A tracee
            // listening at a PTRACE_INTERRUPT stop instead (which Linux also
            // allows, and answers 0 for) therefore resumes rather than staying
            // stopped; strace only ever listens at a group-stop.
            if (child->group->stopped)
                __atomic_store_n(&child->ptrace.listening, true, __ATOMIC_RELEASE);
            // A partial resume: the tracee leaves the ptrace stop, so the
            // tracer's wait4 stops reporting it, but group->stopped is NOT
            // lifted. It goes straight back to waiting the job-control stop out
            // in group_stop_wait's listening branch, which is the point.
            //
            // A SIGCONT landing between the group-stop report and this LISTEN
            // therefore leaves the flag unset and the tracee simply runs, where
            // Linux would report the lift as a PTRACE_EVENT_STOP. That window
            // is the price of arming the flag only for a stop that is really in
            // force; it costs a tracer one missed event-stop and never a hang,
            // whereas a flag set with no stop to wait out would silence the
            // report of the NEXT group-stop.
            child->ptrace.stopped = false;
            child->ptrace.signal = 0;
            child->ptrace.trap_event = 0;
            child->ptrace.eventmsg = 0;
            child->ptrace_delivery_stop = false;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);
            return 0;
        }

        case PTRACE_GETREGS_: {
            STRACE("ptrace(PTRACE_GETREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            if (child->abi == GUEST_ABI_ARM64 || child->abi == GUEST_ABI_RISCV64) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64 = {};
                get_user_regs_amd64(child, &user_regs_amd64);
                if (user_put(data, user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                struct user_regs_struct_ user_regs_ = {};
                get_user_regs_and_syscall(child, &user_regs_);
                user_regs_.orig_eax = child->ptrace.syscall;
                if (user_put(data, user_regs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETREGS_: {
            STRACE("ptrace(PTRACE_SETREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            if (child->abi == GUEST_ABI_ARM64 || child->abi == GUEST_ABI_RISCV64) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_regs_struct_amd64_ user_regs_amd64;
                if (user_get(data, user_regs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                set_user_regs_amd64(child, &user_regs_amd64);
            } else {
                struct user_regs_struct_ user_regs_;
                if (user_get(data, user_regs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                set_user_regs(child, &user_regs_);
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        // GDB needs the fpregs functions to exist if you want to evaluate things
        case PTRACE_GETFPREGS_: {
            STRACE("ptrace(PTRACE_GETFPREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            if (child->abi == GUEST_ABI_ARM64 || child->abi == GUEST_ABI_RISCV64) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_fpregs_struct_amd64_ user_fpregs_amd64 = {};
                get_user_fpregs_amd64(child, &user_fpregs_amd64);
                if (user_put(data, user_fpregs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
            } else {
                struct user_fpregs_struct_ user_fpregs_ = {};
                if (user_put(data, user_fpregs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                // TODO get float point registers for i386 tracees
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETFPREGS_: {
            STRACE("ptrace(PTRACE_SETFPREGS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            if (child->abi == GUEST_ABI_ARM64 || child->abi == GUEST_ABI_RISCV64) {
                unlock(&child->ptrace.lock);
                return _EIO;
            }

            if (child->abi == GUEST_ABI_AMD64) {
                struct user_fpregs_struct_amd64_ user_fpregs_amd64;
                if (user_get(data, user_fpregs_amd64)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                }
                set_user_fpregs_amd64(&child->cpu, &user_fpregs_amd64);
            } else {
                struct user_fpregs_struct_ user_fpregs_;
                if (user_get(data, user_fpregs_)) {
                    unlock(&child->ptrace.lock);
                    return _EFAULT;
                } else {
                    // TODO set floating point registers for i386 tracees
                }
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SYSCALL_: {
            STRACE("ptrace(PTRACE_SYSCALL, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            int resume_sig = ptrace_resume_signal(data);
            if (resume_sig < 0) {
                unlock(&child->ptrace.lock);
                return resume_sig;
            }

            ptrace_resume_child_locked(child, resume_sig, false, true, false);
            return 0;
        }

        case PTRACE_DETACH_: {
            STRACE("ptrace(PTRACE_DETACH, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            // find_child's lookup, but keeping pids_lock long enough to take
            // the tracee off our list. The detach never used to: the tracee
            // stayed linked into the tracer's ptracees after it was let go, so
            // a later attach by anyone else linked the same node into a second
            // list, and the old tracer's wait counted a task it no longer
            // traced as a reason not to answer ECHILD.
            complex_lockt(&pids_lock, 0);
            struct task *child = pid_get_task_zombie(pid);
            if (child == NULL) {
                unlock(&pids_lock);
                return _ESRCH;
            }
            lock(&child->ptrace.lock, 0);
            if (!ptrace_traced_by(current, child) || !child->ptrace.stopped) {
                unlock(&child->ptrace.lock);
                unlock(&pids_lock);
                return _ESRCH;
            }
            int resume_sig = ptrace_resume_signal(data);
            if (resume_sig < 0) {
                unlock(&child->ptrace.lock);
                unlock(&pids_lock);
                return resume_sig;
            }
            list_remove_safe(&child->ptrace_siblings);
            // Still holding ptrace.lock, as find_child's callers do: a stopped
            // tracee cannot get on with exiting until it is released.
            unlock(&pids_lock);

            ptrace_resume_child_locked(child, resume_sig, false, false, true);
            return 0;
        }

        case PTRACE_SETOPTIONS_: {
            STRACE("ptrace(PTRACE_SETOPTIONS, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_tracee(pid, false);
            if (!child) return _ESRCH;
            // Ideally we would have this condition, but strace annonyingly
            // uses PTRACE_O_SYSGOOD | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT
            // (we don't support the other two). Since this isn't a big deal we
            // will just pretend like we do support it to make that check pass.
            // if (data == PTRACE_O_TRACESYSGOOD_ || !data) {
            if (true) {
                child->ptrace.sysgood = !!(data & PTRACE_O_TRACESYSGOOD_);
                child->ptrace.options = data;
                unlock(&child->ptrace.lock);
                return 0;
            } else {
                unlock(&child->ptrace.lock);
                return _EINVAL;
            }
        }

        case PTRACE_GETSIGINFO_: {
            STRACE("ptrace(PTRACE_GETSIGINFO, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            // A stop with no siginfo -- an unseized tracee's group-stop -- is
            // EINVAL, as Linux's ptrace_getsiginfo is for a NULL last_siginfo.
            // A tracer has no other way to tell that group-stop from a
            // signal-delivery-stop of the same signal, and answering it made
            // gdb and strace re-inject the stop signal forever.
            if (!child->ptrace.has_siginfo) {
                unlock(&child->ptrace.lock);
                return _EINVAL;
            }
            if (data && siginfo_to_user(current, data, &child->ptrace.info)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_GETEVENTMSG_: {
            STRACE("ptrace(PTRACE_GETEVENTMSG, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            qword_t eventmsg = child->ptrace.eventmsg;
            if (data && ptrace_put_eventmsg(current, data, eventmsg)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);
            return 0;
        }

        case PTRACE_GETREGSET_: {
            STRACE("ptrace(PTRACE_GETREGSET, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            int err = ptrace_getregset(current, child, data, addr);
            unlock(&child->ptrace.lock);
            return err < 0 ? err : 0;
        }

        case PTRACE_SETREGSET_: {
            STRACE("ptrace(PTRACE_SETREGSET, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;

            int err = ptrace_setregset(current, child, data, addr);
            unlock(&child->ptrace.lock);
            return err < 0 ? err : 0;
        }

        case PTRACE_GET_SYSCALL_INFO_: {
            STRACE("ptrace(PTRACE_GET_SYSCALL_INFO, %d, %#llx, %#llx)", pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            int_t res = ptrace_get_syscall_info(child, addr, data);
            unlock(&child->ptrace.lock);
            return res;
        }

        // A request this kernel does not know: EIO, as Linux's ptrace_request
        // answers one -- once the target has passed ptrace_check_attach, which
        // is ESRCH for anything that is not our stopped tracee. EPERM told a
        // tracer probing a feature (strace asks for PTRACE_SET_SYSCALL_INFO,
        // which only 6.16 has) that it lacked the privilege, not the request.
        default: {
            STRACE("ptrace(%d, %d, %#llx, %#llx)", request, pid,
                    (unsigned long long) addr, (unsigned long long) data);
            struct task *child = find_child(pid);
            if (!child) return _ESRCH;
            unlock(&child->ptrace.lock);
            return _EIO;
        }
    }
}
