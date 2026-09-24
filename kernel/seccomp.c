// seccomp(2) and prctl(PR_SET_SECCOMP): strict mode and classic-BPF filters.
// See kernel/seccomp.h for what this is and why it exists.
//
// The rules below are Linux's (kernel/seccomp.c, net/core/filter.c), and every
// behaviour a caller can observe was measured against Linux 6.12 by
// tests/manual/seccomp_filter.c: which programs are refused and with what
// errno, what each action does to the call and to the caller, how stacked
// filters combine, what a fork, a thread and an exec inherit.
#include <stdlib.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/signal.h"
#include "kernel/ptrace.h"
#include "kernel/seccomp.h"
#include "util/list.h"

#define SECCOMP_SET_MODE_STRICT_  0
#define SECCOMP_SET_MODE_FILTER_  1
#define SECCOMP_GET_ACTION_AVAIL_ 2
#define SECCOMP_GET_NOTIF_SIZES_  3

#define SECCOMP_FILTER_FLAG_TSYNC_              (1u << 0)
#define SECCOMP_FILTER_FLAG_LOG_                (1u << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW_         (1u << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER_       (1u << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH_        (1u << 4)
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV_ (1u << 5)
// The flags this kernel knows. NEW_LISTENER and WAIT_KILLABLE_RECV (which
// needs it) belong to user notification, which is not implemented: refusing
// them with EINVAL is exactly what a kernel from before 5.0 says, so a caller
// learns it has no supervisor fd rather than getting one that never answers.
#define SECCOMP_FILTER_FLAGS_KNOWN_ (SECCOMP_FILTER_FLAG_TSYNC_ | \
        SECCOMP_FILTER_FLAG_LOG_ | SECCOMP_FILTER_FLAG_SPEC_ALLOW_ | \
        SECCOMP_FILTER_FLAG_TSYNC_ESRCH_)

#define SECCOMP_RET_KILL_PROCESS_ 0x80000000u
#define SECCOMP_RET_KILL_THREAD_  0x00000000u
#define SECCOMP_RET_TRAP_         0x00030000u
#define SECCOMP_RET_ERRNO_        0x00050000u
#define SECCOMP_RET_USER_NOTIF_   0x7fc00000u
#define SECCOMP_RET_TRACE_        0x7ff00000u
#define SECCOMP_RET_LOG_          0x7ffc0000u
#define SECCOMP_RET_ALLOW_        0x7fff0000u
#define SECCOMP_RET_ACTION_FULL_  0xffff0000u
#define SECCOMP_RET_DATA_         0x0000ffffu

// SIGSYS's si_code for a seccomp trap.
#define SYS_SECCOMP_ 1

#define AUDIT_ARCH_I386_    0x40000003u
#define AUDIT_ARCH_X86_64_  0xc000003eu
#define AUDIT_ARCH_AARCH64_ 0xc00000b7u
#define AUDIT_ARCH_RISCV64_ 0xc00000f3u

#define BPF_MAXINSNS_ 4096
// Linux's MAX_INSNS_PER_PATH: every filter a call runs through, counted with
// a four-instruction penalty each, must fit in 256 KiB of instructions.
#define SECCOMP_MAX_PATH_INSNS_ ((1u << 18) / 8)
#define BPF_MEMWORDS_ 16
// struct seccomp_data: int nr, u32 arch, u64 instruction_pointer, u64 args[6].
#define SECCOMP_DATA_SIZE_ 64
#define MAX_ERRNO_ 4095

// struct sock_filter and struct sock_fprog. The instruction is the same eight
// bytes under every ABI; the program header is a length and a pointer, laid
// out by the pointer size.
struct sock_filter_ {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};

struct seccomp_filter {
    atomic_uint refcount;
    // The filter installed before this one, which every call also runs
    // through. Held: a chain is kept alive by its newest filter.
    struct seccomp_filter *prev;
    // SECCOMP_FILTER_FLAG_LOG: log every action but ALLOW that this filter
    // decides.
    bool log;
    unsigned len;
    struct sock_filter_ insns[];
};

// ---- classic BPF, the part of it seccomp allows ----------------------------

// Opcodes, spelled as numbers: <linux/filter.h>'s names, class | size | mode
// or class | op | source.
enum {
    LD_W_ABS = 0x20, LD_W_LEN = 0x80, LDX_W_LEN = 0x81,
    LD_IMM = 0x00, LDX_IMM = 0x01, LD_MEM = 0x60, LDX_MEM = 0x61,
    ST = 0x02, STX = 0x03, TAX = 0x07, TXA = 0x87,
    ADD_K = 0x04, ADD_X = 0x0c, SUB_K = 0x14, SUB_X = 0x1c,
    MUL_K = 0x24, MUL_X = 0x2c, DIV_K = 0x34, DIV_X = 0x3c,
    OR_K = 0x44, OR_X = 0x4c, AND_K = 0x54, AND_X = 0x5c,
    LSH_K = 0x64, LSH_X = 0x6c, RSH_K = 0x74, RSH_X = 0x7c,
    NEG = 0x84, XOR_K = 0xa4, XOR_X = 0xac,
    JA = 0x05, JEQ_K = 0x15, JEQ_X = 0x1d, JGT_K = 0x25, JGT_X = 0x2d,
    JGE_K = 0x35, JGE_X = 0x3d, JSET_K = 0x45, JSET_X = 0x4d,
    RET_K = 0x06, RET_A = 0x16,
};

// seccomp_check_filter's list: classic BPF without the packet loads (only
// 32-bit loads of struct seccomp_data), without MOD, and without the
// ancillary extensions. Anything else is EINVAL.
static bool bpf_code_allowed(uint16_t code) {
    switch (code) {
        case LD_W_ABS: case LD_W_LEN: case LDX_W_LEN:
        case LD_IMM: case LDX_IMM: case LD_MEM: case LDX_MEM:
        case ST: case STX: case TAX: case TXA:
        case ADD_K: case ADD_X: case SUB_K: case SUB_X:
        case MUL_K: case MUL_X: case DIV_K: case DIV_X:
        case OR_K: case OR_X: case AND_K: case AND_X:
        case LSH_K: case LSH_X: case RSH_K: case RSH_X:
        case NEG: case XOR_K: case XOR_X:
        case JA: case JEQ_K: case JEQ_X: case JGT_K: case JGT_X:
        case JGE_K: case JGE_X: case JSET_K: case JSET_X:
        case RET_K: case RET_A:
            return true;
    }
    return false;
}

static bool bpf_is_cond_jump(uint16_t code) {
    switch (code) {
        case JEQ_K: case JEQ_X: case JGT_K: case JGT_X:
        case JGE_K: case JGE_X: case JSET_K: case JSET_X:
            return true;
    }
    return false;
}

// bpf_check_classic plus seccomp_check_filter: every instruction allowed and
// in range, every jump forward and inside the program, the last instruction a
// return, and no scratch word read on any path before it is written. What
// passes cannot loop, cannot fault, and always returns.
static int bpf_check(const struct sock_filter_ *f, unsigned len) {
    for (unsigned pc = 0; pc < len; pc++) {
        const struct sock_filter_ *in = &f[pc];
        if (!bpf_code_allowed(in->code))
            return _EINVAL;
        switch (in->code) {
            case DIV_K:
                if (in->k == 0)
                    return _EINVAL;
                break;
            case LSH_K:
            case RSH_K:
                if (in->k >= 32)
                    return _EINVAL;
                break;
            case LD_MEM: case LDX_MEM: case ST: case STX:
                if (in->k >= BPF_MEMWORDS_)
                    return _EINVAL;
                break;
            case JA:
                if (in->k >= len - pc - 1)
                    return _EINVAL;
                break;
            case LD_W_ABS:
                // 32-bit aligned and inside struct seccomp_data.
                if (in->k >= SECCOMP_DATA_SIZE_ || (in->k & 3) != 0)
                    return _EINVAL;
                break;
            default:
                if (bpf_is_cond_jump(in->code) &&
                        (pc + in->jt + 1 >= len || pc + in->jf + 1 >= len))
                    return _EINVAL;
                break;
        }
    }
    if (f[len - 1].code != RET_K && f[len - 1].code != RET_A)
        return _EINVAL;

    // check_load_and_stores: which scratch words are written on EVERY path
    // into each instruction. Jumps only go forward, so one pass does it.
    uint16_t *valid_in = malloc(len * sizeof(*valid_in));
    if (valid_in == NULL)
        return _ENOMEM;
    for (unsigned pc = 0; pc < len; pc++)
        valid_in[pc] = 0xffff;
    uint16_t valid = 0;
    int err = 0;
    for (unsigned pc = 0; pc < len; pc++) {
        valid &= valid_in[pc];
        const struct sock_filter_ *in = &f[pc];
        if (in->code == ST || in->code == STX) {
            valid |= 1u << in->k;
        } else if (in->code == LD_MEM || in->code == LDX_MEM) {
            if (!(valid & (1u << in->k))) {
                err = _EINVAL;
                break;
            }
        } else if (in->code == JA) {
            valid_in[pc + 1 + in->k] &= valid;
            valid = 0xffff;
        } else if (bpf_is_cond_jump(in->code)) {
            valid_in[pc + 1 + in->jt] &= valid;
            valid_in[pc + 1 + in->jf] &= valid;
            valid = 0xffff;
        }
    }
    free(valid_in);
    return err;
}

// Run one validated program over the call's seccomp_data, as 16 native-order
// words. Classic BPF is 32-bit unsigned throughout: a shift by X uses its low
// five bits, and a division by an X of 0 ends the program returning 0 --
// which to seccomp is SECCOMP_RET_KILL_THREAD, as it is on Linux.
static uint32_t bpf_run(const struct seccomp_filter *f, const uint32_t data[16]) {
    uint32_t A = 0, X = 0, M[BPF_MEMWORDS_] = {0};
    unsigned pc = 0;
    for (;;) {
        const struct sock_filter_ *in = &f->insns[pc++];
        uint32_t k = in->k;
        switch (in->code) {
            case LD_W_ABS: A = data[k / 4]; break;
            case LD_W_LEN: A = SECCOMP_DATA_SIZE_; break;
            case LDX_W_LEN: X = SECCOMP_DATA_SIZE_; break;
            case LD_IMM: A = k; break;
            case LDX_IMM: X = k; break;
            case LD_MEM: A = M[k]; break;
            case LDX_MEM: X = M[k]; break;
            case ST: M[k] = A; break;
            case STX: M[k] = X; break;
            case TAX: X = A; break;
            case TXA: A = X; break;
            case ADD_K: A += k; break;
            case ADD_X: A += X; break;
            case SUB_K: A -= k; break;
            case SUB_X: A -= X; break;
            case MUL_K: A *= k; break;
            case MUL_X: A *= X; break;
            case DIV_K: A /= k; break;
            case DIV_X:
                if (X == 0)
                    return 0;
                A /= X;
                break;
            case OR_K: A |= k; break;
            case OR_X: A |= X; break;
            case AND_K: A &= k; break;
            case AND_X: A &= X; break;
            case LSH_K: A <<= k; break;
            case LSH_X: A <<= (X & 31); break;
            case RSH_K: A >>= k; break;
            case RSH_X: A >>= (X & 31); break;
            case NEG: A = 0u - A; break;
            case XOR_K: A ^= k; break;
            case XOR_X: A ^= X; break;
            case JA: pc += k; break;
            case JEQ_K: pc += A == k ? in->jt : in->jf; break;
            case JEQ_X: pc += A == X ? in->jt : in->jf; break;
            case JGT_K: pc += A > k ? in->jt : in->jf; break;
            case JGT_X: pc += A > X ? in->jt : in->jf; break;
            case JGE_K: pc += A >= k ? in->jt : in->jf; break;
            case JGE_X: pc += A >= X ? in->jt : in->jf; break;
            case JSET_K: pc += (A & k) ? in->jt : in->jf; break;
            case JSET_X: pc += (A & X) ? in->jt : in->jf; break;
            case RET_K: return k;
            case RET_A: return A;
            default: return SECCOMP_RET_KILL_PROCESS_; // unreachable once checked
        }
    }
}

// ---- filters -----------------------------------------------------------------

void seccomp_filter_retain(struct seccomp_filter *filter) {
    if (filter != NULL)
        atomic_fetch_add(&filter->refcount, 1);
}

void seccomp_filter_release(struct seccomp_filter *filter) {
    // Iterative: a chain can be thousands deep, and dropping the last
    // reference to its head drops one on each filter below it in turn.
    while (filter != NULL && atomic_fetch_sub(&filter->refcount, 1) == 1) {
        struct seccomp_filter *prev = filter->prev;
        free(filter);
        filter = prev;
    }
}

unsigned seccomp_filter_count(const struct seccomp_filter *filter) {
    unsigned n = 0;
    for (; filter != NULL; filter = filter->prev)
        n++;
    return n;
}

static struct seccomp_filter *filter_new(const struct sock_filter_ *insns, unsigned len, bool log) {
    struct seccomp_filter *f = malloc(sizeof(*f) + len * sizeof(struct sock_filter_));
    if (f == NULL)
        return NULL;
    atomic_init(&f->refcount, 1);
    f->prev = NULL;
    f->log = log;
    f->len = len;
    memcpy(f->insns, insns, len * sizeof(struct sock_filter_));
    return f;
}

// The architecture a call is made under, as AUDIT_ARCH_* and so as a filter
// compares it. A native program's calls are numbered asm-generic, which is
// aarch64's numbering (kernel/calls.c syscall_dispatch_native).
static uint32_t seccomp_arch(enum guest_abi abi) {
    switch (abi) {
        case GUEST_ABI_AMD64: return AUDIT_ARCH_X86_64_;
        case GUEST_ABI_ARM64: return AUDIT_ARCH_AARCH64_;
        case GUEST_ABI_RISCV64: return AUDIT_ARCH_RISCV64_;
        case GUEST_ABI_I386:
        default: return AUDIT_ARCH_I386_;
    }
}

// Strict mode's four calls, per ABI (Linux's mode1_syscalls). 32-bit x86 gets
// sigreturn and not rt_sigreturn, which is Linux's choice too
// (__NR_seccomp_sigreturn_32 is __NR_ia32_sigreturn).
static bool strict_allowed(enum guest_abi abi, int nr) {
    switch (abi) {
        case GUEST_ABI_AMD64:
            return nr == 0 || nr == 1 || nr == 60 || nr == 15;
        case GUEST_ABI_ARM64:
        case GUEST_ABI_RISCV64:
            return nr == 63 || nr == 64 || nr == 93 || nr == 139;
        case GUEST_ABI_I386:
        default:
            return nr == 3 || nr == 4 || nr == 1 || nr == 119;
    }
}

// Linux's audit record for a seccomp action, to the kernel log (dmesg):
// every kill, every SECCOMP_RET_LOG, and whatever a LOG-flagged filter
// decides. A sandbox that kills something is otherwise silent about what.
static void seccomp_log(int nr, uint32_t arch, qword_t ip, int sig, uint32_t action) {
    printk("audit: type=1326 audit(0.0:0): auid=4294967295 uid=%u gid=%u ses=4294967295 "
           "subj=unconfined pid=%d comm=\"%s\" exe=\"?\" sig=%d arch=%x syscall=%d "
           "compat=%d ip=%#llx code=%#x\n",
           current->uid, current->gid, current->pid, current->comm, sig, arch, nr,
           arch == AUDIT_ARCH_I386_ ? 1 : 0, (unsigned long long) ip, action);
}

static bool seccomp_last_thread(void) {
    unsigned live = 0;
    complex_lockt(&pids_lock, 0);
    struct task *t;
    list_for_each_entry(&current->group->threads, t, group_links) {
        if (!t->exiting)
            live++;
    }
    unlock(&pids_lock);
    return live <= 1;
}

// SECCOMP_RET_KILL_*: the call is not made, and the caller does not survive
// it. Linux sends SIGSYS with its handler forced to the default and no
// tracer stop (SA_IMMUTABLE), so the process ends killed by SIGSYS; a
// KILL_THREAD in a process with other live threads ends only this thread.
static noreturn void seccomp_kill(uint32_t action) {
    __atomic_store_n(&current->seccomp_mode, SECCOMP_MODE_DEAD_, __ATOMIC_RELEASE);
    if (action == SECCOMP_RET_KILL_THREAD_ && !seccomp_last_thread())
        do_exit(current, SIGSYS_);
    do_exit_group(SIGSYS_);
}

static void seccomp_send_sigsys(int nr, uint32_t arch, qword_t ip, uint32_t data) {
    struct siginfo_ info = {
        .sig = SIGSYS_,
        .sig_errno = (int_t) data,
        .code = SYS_SECCOMP_,
        .sigsys.addr = (guest_addr_t) ip,
        .sigsys.syscall = nr,
        .sigsys.arch = arch,
    };
    // A synchronous trap: a handler the caller has blocked or ignored is set
    // back to the default and unblocked, so it dies rather than looping on
    // the call (Linux's force_sig_info_to_task, HANDLER_CURRENT).
    deliver_signal(current, SIGSYS_, info);
}

static uint32_t run_filters(const struct seccomp_filter *f, const uint32_t data[16],
                            const struct seccomp_filter **match) {
    // Fail closed: filter mode with no filter is a bug, not permission.
    if (f == NULL)
        return SECCOMP_RET_KILL_PROCESS_;
    uint32_t ret = SECCOMP_RET_ALLOW_;
    *match = NULL;
    for (; f != NULL; f = f->prev) {
        uint32_t cur = bpf_run(f, data);
        // The most restrictive action wins, compared as Linux compares them:
        // signed, so KILL_PROCESS (0x80000000) outranks everything.
        if ((int32_t) (cur & SECCOMP_RET_ACTION_FULL_) < (int32_t) (ret & SECCOMP_RET_ACTION_FULL_)) {
            ret = cur;
            *match = f;
        }
    }
    return ret;
}

enum seccomp_verdict seccomp_syscall_enter(enum guest_abi abi, qword_t nr64,
        const qword_t args[6], qword_t ip, sqword_t *result, bool recheck) {
    int mode = __atomic_load_n(&current->seccomp_mode, __ATOMIC_ACQUIRE);
    // As Linux's syscall_get_nr: the number is an int.
    int nr = (int) (sqword_t) nr64;
    uint32_t arch = seccomp_arch(abi);

    if (mode == SECCOMP_MODE_STRICT_) {
        if (strict_allowed(abi, nr))
            return SECCOMP_RUN;
        seccomp_log(nr, arch, ip, SIGKILL_, SECCOMP_RET_KILL_THREAD_);
        // Linux's __secure_computing_strict: do_exit(SIGKILL), this thread.
        __atomic_store_n(&current->seccomp_mode, SECCOMP_MODE_DEAD_, __ATOMIC_RELEASE);
        do_exit(current, SIGKILL_);
    }
    if (mode == SECCOMP_MODE_DEAD_)
        do_exit(current, SIGKILL_);

    uint32_t data[16] = {
        (uint32_t) nr, arch, (uint32_t) ip, (uint32_t) (ip >> 32),
    };
    // A 32-bit guest's arguments are 32-bit registers, zero-extended, as a
    // 32-bit kernel's unsigned longs are.
    bool narrow = abi == GUEST_ABI_I386;
    for (int i = 0; i < 6; i++) {
        qword_t a = narrow ? (qword_t) (dword_t) args[i] : args[i];
        data[4 + 2 * i] = (uint32_t) a;
        data[5 + 2 * i] = (uint32_t) (a >> 32);
    }

    const struct seccomp_filter *match = NULL;
    const struct seccomp_filter *head = __atomic_load_n(&current->seccomp_filter, __ATOMIC_ACQUIRE);
    uint32_t ret = run_filters(head, data, &match);
    uint32_t action = ret & SECCOMP_RET_ACTION_FULL_;
    uint32_t rdata = ret & SECCOMP_RET_DATA_;
    bool logged = match != NULL && match->log;

    switch (action) {
        case SECCOMP_RET_ALLOW_:
            return SECCOMP_RUN;

        case SECCOMP_RET_LOG_:
            seccomp_log(nr, arch, ip, 0, action);
            return SECCOMP_RUN;

        case SECCOMP_RET_ERRNO_:
            if (logged)
                seccomp_log(nr, arch, ip, 0, action);
            // The call is not made and returns -data, capped at MAX_ERRNO;
            // ERRNO with data 0 answers success without doing anything.
            if (rdata > MAX_ERRNO_)
                rdata = MAX_ERRNO_;
            *result = -(sqword_t) rdata;
            return SECCOMP_ANSWERED;

        case SECCOMP_RET_TRAP_:
            if (logged)
                seccomp_log(nr, arch, ip, SIGSYS_, action);
            // Not made, and the registers are left as they were for the
            // handler to see (Linux's syscall_rollback): no result is written.
            seccomp_send_sigsys(nr, arch, ip, rdata);
            return SECCOMP_SKIPPED;

        case SECCOMP_RET_TRACE_:
            // A tracer already decided this call after its stop; the recheck
            // only asks whether some OTHER filter forbids what it turned into.
            if (recheck)
                return SECCOMP_RUN;
            if (logged)
                seccomp_log(nr, arch, ip, 0, action);
            // No tracer asking for these stops: the call fails ENOSYS.
            if (!current->ptrace.traced || !(current->ptrace.options & PTRACE_O_TRACESECCOMP_)) {
                *result = _ENOSYS;
                return SECCOMP_ANSWERED;
            }
            {
                struct siginfo_ info = {
                    .sig = SIGTRAP_,
                    .code = SI_USER_,
                    .kill.pid = current->pid,
                    .kill.uid = current->uid,
                };
                ptrace_event_stop(SIGTRAP_, &info, PTRACE_EVENT_SECCOMP_, rdata);
            }
            if (current->exiting)
                return SECCOMP_SKIPPED;
            // The caller re-reads the (possibly rewritten) call and asks again.
            return SECCOMP_TRACED;

        case SECCOMP_RET_USER_NOTIF_:
            // No listener can exist (see SECCOMP_FILTER_FLAGS_KNOWN_), and a
            // notification nobody receives fails the call ENOSYS on Linux.
            if (logged)
                seccomp_log(nr, arch, ip, 0, action);
            *result = _ENOSYS;
            return SECCOMP_ANSWERED;

        case SECCOMP_RET_KILL_THREAD_:
        case SECCOMP_RET_KILL_PROCESS_:
        default:
            // An action Linux does not know counts as KILL_PROCESS.
            seccomp_log(nr, arch, ip, SIGSYS_, action);
            seccomp_kill(action == SECCOMP_RET_KILL_THREAD_ ? action : SECCOMP_RET_KILL_PROCESS_);
    }
}

// ---- installing -------------------------------------------------------------

// Linux's is_ancestor: `parent` is somewhere in `child`'s chain (NULL is
// everyone's).
static bool is_ancestor(const struct seccomp_filter *parent, const struct seccomp_filter *child) {
    if (parent == NULL)
        return true;
    for (; child != NULL; child = child->prev)
        if (child == parent)
            return true;
    return false;
}

static bool may_assign_mode(int mode) {
    int now = __atomic_load_n(&current->seccomp_mode, __ATOMIC_ACQUIRE);
    return now == SECCOMP_MODE_DISABLED_ || now == mode;
}

static int_t set_mode_strict(void) {
    complex_lockt(&pids_lock, 0);
    int_t err = _EINVAL;
    if (may_assign_mode(SECCOMP_MODE_STRICT_)) {
        __atomic_store_n(&current->seccomp_mode, SECCOMP_MODE_STRICT_, __ATOMIC_RELEASE);
        err = 0;
    }
    unlock(&pids_lock);
    return err;
}

// Read a struct sock_fprog and the program it points at.
static struct seccomp_filter *prepare_user_filter(guest_addr_t uprog, bool log) {
    qword_t insns_addr;
    uint16_t len;
    if (guest_abi_is_64bit(current->abi)) {
        struct { uint16_t len; uint16_t pad[3]; uint64_t filter; } fprog;
        if (user_read(uprog, &fprog, sizeof(fprog)))
            return ERR_PTR(_EFAULT);
        len = fprog.len;
        insns_addr = fprog.filter;
    } else {
        struct { uint16_t len; uint16_t pad; uint32_t filter; } fprog;
        if (user_read(uprog, &fprog, sizeof(fprog)))
            return ERR_PTR(_EFAULT);
        len = fprog.len;
        insns_addr = fprog.filter;
    }
    if (len == 0 || len > BPF_MAXINSNS_)
        return ERR_PTR(_EINVAL);
    // Linux asks this before it reads the program: installing a filter takes
    // no_new_privs or CAP_SYS_ADMIN, so an unprivileged process cannot change
    // how a set-id program it goes on to exec behaves.
    if (!current->no_new_privs && !current_capable(CAP_SYS_ADMIN_))
        return ERR_PTR(_EACCES);
    if (insns_addr == 0)
        return ERR_PTR(_EINVAL);
    struct sock_filter_ *insns = malloc(len * sizeof(*insns));
    if (insns == NULL)
        return ERR_PTR(_ENOMEM);
    if (user_read((guest_addr_t) insns_addr, insns, len * sizeof(*insns))) {
        free(insns);
        return ERR_PTR(_EFAULT);
    }
    int err = bpf_check(insns, len);
    if (err < 0) {
        free(insns);
        return ERR_PTR(err);
    }
    struct seccomp_filter *f = filter_new(insns, len, log);
    free(insns);
    return f != NULL ? f : ERR_PTR(_ENOMEM);
}

// With pids_lock held. The first thread of this process that could not take
// the caller's filters: one that has filters of its own that are not an
// ancestor of the caller's, or one in strict mode. 0 when all can.
static pid_t_ tsync_blocker(const struct seccomp_filter *caller_head) {
    struct task *t;
    list_for_each_entry(&current->group->threads, t, group_links) {
        if (t == current || t->exiting)
            continue;
        int mode = __atomic_load_n(&t->seccomp_mode, __ATOMIC_ACQUIRE);
        if (mode == SECCOMP_MODE_DISABLED_)
            continue;
        if (mode == SECCOMP_MODE_FILTER_ && is_ancestor(t->seccomp_filter, caller_head))
            continue;
        return t->pid;
    }
    return 0;
}

static int_t set_mode_filter(uint_t flags, guest_addr_t uprog) {
    if (flags & ~SECCOMP_FILTER_FLAGS_KNOWN_)
        return _EINVAL;
    struct seccomp_filter *f = prepare_user_filter(uprog, flags & SECCOMP_FILTER_FLAG_LOG_);
    if (IS_ERR(f))
        return PTR_ERR(f);

    complex_lockt(&pids_lock, 0);
    int_t err = _EINVAL;
    if (!may_assign_mode(SECCOMP_MODE_FILTER_))
        goto out;

    struct seccomp_filter *old = current->seccomp_filter;
    unsigned long total = f->len;
    for (const struct seccomp_filter *w = old; w != NULL; w = w->prev)
        total += w->len + 4;
    if (total > SECCOMP_MAX_PATH_INSNS_) {
        err = _ENOMEM;
        goto out;
    }

    // TSYNC: every other thread must end up running exactly this chain. One
    // that has filters that are not part of it (or is in strict mode) cannot,
    // and the call fails with that thread's id -- or ESRCH, for a caller that
    // asked not to be handed a positive number it could mistake for a fd.
    if (flags & SECCOMP_FILTER_FLAG_TSYNC_) {
        pid_t_ blocker = tsync_blocker(old);
        if (blocker != 0) {
            err = (flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH_) ? _ESRCH : blocker;
            goto out;
        }
    }

    // The new filter takes over this thread's reference to the old chain.
    f->prev = old;
    __atomic_store_n(&current->seccomp_filter, f, __ATOMIC_RELEASE);
    __atomic_store_n(&current->seccomp_mode, SECCOMP_MODE_FILTER_, __ATOMIC_RELEASE);
    f = NULL;

    if (flags & SECCOMP_FILTER_FLAG_TSYNC_) {
        struct seccomp_filter *head = current->seccomp_filter;
        struct task *t;
        list_for_each_entry(&current->group->threads, t, group_links) {
            if (t == current || t->exiting)
                continue;
            // What it had is an ancestor of `head` (tsync_blocker said so),
            // so dropping its reference frees nothing another thread may be
            // reading at this moment: `head` keeps it alive.
            seccomp_filter_retain(head);
            struct seccomp_filter *theirs = t->seccomp_filter;
            __atomic_store_n(&t->seccomp_filter, head, __ATOMIC_RELEASE);
            seccomp_filter_release(theirs);
            // Otherwise a thread that sets no_new_privs, installs a filter
            // and dies would leave its siblings filtered but able to exec
            // set-id programs, which only the flag's holder may be.
            if (current->no_new_privs)
                t->no_new_privs = true;
            __atomic_store_n(&t->seccomp_mode, SECCOMP_MODE_FILTER_, __ATOMIC_RELEASE);
        }
    }
    err = 0;
out:
    unlock(&pids_lock);
    seccomp_filter_release(f);
    return err;
}

static int_t get_action_avail(guest_addr_t uaction) {
    uint32_t action;
    if (user_get(uaction, action))
        return _EFAULT;
    switch (action) {
        case SECCOMP_RET_KILL_PROCESS_:
        case SECCOMP_RET_KILL_THREAD_:
        case SECCOMP_RET_TRAP_:
        case SECCOMP_RET_ERRNO_:
        case SECCOMP_RET_TRACE_:
        case SECCOMP_RET_LOG_:
        case SECCOMP_RET_ALLOW_:
            return 0;
    }
    // Including SECCOMP_RET_USER_NOTIF: see SECCOMP_FILTER_FLAGS_KNOWN_.
    return _EOPNOTSUPP;
}

int_t sys_seccomp_guest(uint_t op, uint_t flags, guest_addr_t uargs) {
    STRACE("seccomp(%u, %#x, %#llx)", op, flags, (unsigned long long) uargs);
    switch (op) {
        case SECCOMP_SET_MODE_STRICT_:
            if (flags != 0 || uargs != 0)
                return _EINVAL;
            return set_mode_strict();
        case SECCOMP_SET_MODE_FILTER_:
            return set_mode_filter(flags, uargs);
        case SECCOMP_GET_ACTION_AVAIL_:
            if (flags != 0)
                return _EINVAL;
            return get_action_avail(uargs);
        // SECCOMP_GET_NOTIF_SIZES, and anything newer: not this kernel's.
        default:
            return _EINVAL;
    }
}

int_t sys_seccomp(dword_t op, dword_t flags, addr_t uargs) {
    return sys_seccomp_guest(op, flags, uargs);
}

int_t seccomp_prctl_set(qword_t mode, guest_addr_t filter) {
    STRACE("prctl(PR_SET_SECCOMP, %llu, %#llx)", (unsigned long long) mode,
           (unsigned long long) filter);
    switch (mode) {
        case SECCOMP_MODE_STRICT_:
            // The prctl form always ignored its third argument here.
            return set_mode_strict();
        case SECCOMP_MODE_FILTER_:
            return set_mode_filter(0, filter);
        default:
            return _EINVAL;
    }
}

int_t seccomp_prctl_get(void) {
    // Strict mode never gets here: prctl is not one of its four calls.
    int mode = __atomic_load_n(&current->seccomp_mode, __ATOMIC_ACQUIRE);
    return mode == SECCOMP_MODE_DEAD_ ? SECCOMP_MODE_FILTER_ : mode;
}

// ---- checkpoint ---------------------------------------------------------------

unsigned seccomp_ckpt_export(const struct seccomp_filter *filter,
        struct seccomp_ckpt_prog *out, unsigned max) {
    unsigned n = seccomp_filter_count(filter);
    if (out == NULL || n > max)
        return n;
    // Oldest first: the chain runs newest first, so fill from the back.
    unsigned i = n;
    for (; filter != NULL; filter = filter->prev) {
        i--;
        out[i] = (struct seccomp_ckpt_prog) {
            .log = filter->log, .len = filter->len, .insns = filter->insns,
        };
    }
    return n;
}

// Filters rebuilt during the restore in progress, so tasks that shared a
// chain when they were saved share one again: TSYNC asks whether a thread's
// filters are an ANCESTOR of the caller's, which is a question about identity,
// and two equal copies are not each other's ancestors. Guarded by the
// restore's being single-threaded (kernel/checkpoint.c restores one task at a
// time); seccomp_ckpt_import_done() lets go of them.
static struct seccomp_filter **import_seen;
static unsigned import_seen_count, import_seen_cap;

static struct seccomp_filter *import_find(const struct seccomp_filter *prev,
        const struct seccomp_ckpt_prog *p) {
    for (unsigned i = 0; i < import_seen_count; i++) {
        struct seccomp_filter *f = import_seen[i];
        if (f->prev == prev && f->log == p->log && f->len == p->len &&
                memcmp(f->insns, p->insns, p->len * sizeof(struct sock_filter_)) == 0)
            return f;
    }
    return NULL;
}

static void import_remember(struct seccomp_filter *f) {
    if (import_seen_count == import_seen_cap) {
        unsigned cap = import_seen_cap ? import_seen_cap * 2 : 16;
        struct seccomp_filter **grown = realloc(import_seen, cap * sizeof(*grown));
        if (grown == NULL)
            return; // only costs sharing, never correctness
        import_seen = grown;
        import_seen_cap = cap;
    }
    seccomp_filter_retain(f);
    import_seen[import_seen_count++] = f;
}

struct seccomp_filter *seccomp_ckpt_import(const struct seccomp_ckpt_prog *progs,
        unsigned count) {
    struct seccomp_filter *head = NULL; // a reference we own, or NULL
    for (unsigned i = 0; i < count; i++) {
        const struct seccomp_ckpt_prog *p = &progs[i];
        if (p->len == 0 || p->len > BPF_MAXINSNS_ ||
                bpf_check(p->insns, p->len) < 0) {
            seccomp_filter_release(head);
            return NULL;
        }
        struct seccomp_filter *same = import_find(head, p);
        if (same != NULL) {
            // It already holds its own reference on `head`.
            seccomp_filter_retain(same);
            seccomp_filter_release(head);
            head = same;
            continue;
        }
        struct seccomp_filter *f = filter_new(p->insns, p->len, p->log);
        if (f == NULL) {
            seccomp_filter_release(head);
            return NULL;
        }
        f->prev = head;
        head = f;
        import_remember(f);
    }
    return head;
}

void seccomp_ckpt_import_done(void) {
    for (unsigned i = 0; i < import_seen_count; i++)
        seccomp_filter_release(import_seen[i]);
    free(import_seen);
    import_seen = NULL;
    import_seen_count = import_seen_cap = 0;
}
