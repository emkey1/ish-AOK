// Restartable sequences: rseq(2), and the CPU number a thread reports.
//
// glibc 2.35 and later register an rseq area for every thread as it starts and
// read sched_getcpu() out of it. An rseq area also lets a thread run a
// "critical section" on per-CPU data without atomics: the kernel promises that
// if the thread is preempted, migrated or interrupted by a signal inside one,
// it resumes at the section's abort handler rather than in the middle of it.
//
// AOK cannot see a host preemption, so it cannot keep that promise the way
// Linux does. What it does instead is make sharing a CPU number rare: each
// registered thread of an address space takes the CPU number fewest of its
// other registered threads hold, and keeps it (the thread never migrates). As
// long as a process has no more registered threads than there are CPUs, no two
// of them share one, per-CPU data has exactly one writer, a preemption has no
// one to race -- and the only interruption left, the thread's own signal
// handler, does abort its critical section, exactly as on Linux.
//
// Past that, threads share, and two threads that share a CPU number can be
// inside critical sections on the same CPU's data at once, which on Linux they
// cannot. That is the one thing this does not provide -- and the reason
// gVisor, on platforms where it cannot see preemption either, refuses rseq
// outright. Refusing only the threads past the CPU count is not an option:
// glibc treats a failed registration in a process whose first one succeeded
// as fatal ("Fatal glibc error: rseq registration failed" -- the guest suite
// found that on the first try), so a registration succeeds for every thread
// or for none. glibc itself only READS cpu_id, for sched_getcpu. Code that
// runs critical sections of its own in a process with more threads than CPUs
// is what this leaves exposed; none of it is in the guests AOK ships.
//
// Measured against Linux 6.12: tests/manual/rseq_register.c.

#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "kernel/rseq.h"
#include "platform/platform.h"

#define RSEQ_FLAG_UNREGISTER_ 1
#define RSEQ_CS_VERSION_ 0
#define ORIG_RSEQ_SIZE_ 32
#define RSEQ_ALIGN_ 32
#define RSEQ_CPU_ID_UNINITIALIZED_ ((dword_t) -1)

// struct rseq (linux/rseq.h), as far as this kernel fills it in: the original
// 32-byte layout plus node_id and mm_cid (6.3), which sit inside it.
#define RSEQ_OFF_CPU_ID_START 0
#define RSEQ_OFF_CPU_ID 4
#define RSEQ_OFF_RSEQ_CS 8
#define RSEQ_OFF_FLAGS 16
#define RSEQ_OFF_NODE_ID 20
#define RSEQ_OFF_MM_CID 24

struct rseq_cs_ {
    dword_t version;
    dword_t flags;
    qword_t start_ip;
    qword_t post_commit_offset;
    qword_t abort_ip;
};

// How many CPUs a thread can be on: the guest's CPU count, which
// sched_getaffinity, sysconf(_SC_NPROCESSORS_*), /proc and /sys all report
// (get_cpu_count). Capped by the slot mask.
static int rseq_ncpus(void) {
    int n = get_cpu_count();
    if (n < 1)
        n = 1;
    if (n > 64)
        n = 64;
    return n;
}

int task_current_cpu(struct task *task) {
    if (task->rseq_area != 0 && task->rseq_cpu >= 0)
        return task->rseq_cpu;
    // An unregistered thread is on the virtual CPU /proc/stat charges it to.
    int ncpu = rseq_ncpus();
    return (int) (task->pid % (dword_t) ncpu);
}

// The least used CPU number of the address space, taken: a free one whenever
// there is one. A compare-and-swap on the chosen count, so two threads
// registering at once cannot both take the same free number.
static int rseq_slot_take(struct mm *mm) {
    int ncpu = rseq_ncpus();
    for (;;) {
        int best = 0;
        uint16_t best_users = UINT16_MAX;
        for (int i = 0; i < ncpu; i++) {
            uint16_t users = atomic_load(&mm->rseq_cpu_users[i]);
            if (users < best_users) {
                best = i;
                best_users = users;
                if (users == 0)
                    break;
            }
        }
        if (best_users == UINT16_MAX)
            return 0;
        if (atomic_compare_exchange_weak(&mm->rseq_cpu_users[best], &best_users,
                    (uint16_t) (best_users + 1)))
            return best;
    }
}

static void rseq_slot_give(struct mm *mm, int slot) {
    if (mm != NULL && slot >= 0 && slot < 64)
        atomic_fetch_sub(&mm->rseq_cpu_users[slot], 1);
}

// rseq_update_cpu_node_id / rseq_reset_rseq_cpu_node_id.
static int rseq_write_ids(guest_addr_t area, dword_t cpu_id_start, dword_t cpu_id,
        dword_t mm_cid) {
    dword_t node = 0;
    if (user_put(area + RSEQ_OFF_CPU_ID_START, cpu_id_start) ||
            user_put(area + RSEQ_OFF_CPU_ID, cpu_id) ||
            user_put(area + RSEQ_OFF_NODE_ID, node) ||
            user_put(area + RSEQ_OFF_MM_CID, mm_cid))
        return _EFAULT;
    return 0;
}

static void rseq_forget(struct task *task) {
    task->rseq_area = 0;
    task->rseq_len = 0;
    task->rseq_sig = 0;
    task->rseq_cpu = -1;
}

int_t sys_rseq(addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig) {
    return sys_rseq_guest(rseq_addr, rseq_len, flags, sig);
}

int_t sys_rseq_guest(guest_addr_t area, dword_t len, dword_t flags, dword_t sig) {
    STRACE("rseq(%#llx, %u, %#x, %#x)", (unsigned long long) area, len, flags, sig);

    if (flags & RSEQ_FLAG_UNREGISTER_) {
        if (flags & ~RSEQ_FLAG_UNREGISTER_)
            return _EINVAL;
        if (current->rseq_area == 0 || current->rseq_area != area)
            return _EINVAL;
        if (len != current->rseq_len)
            return _EINVAL;
        if (sig != current->rseq_sig)
            return _EPERM;
        int err = rseq_write_ids(area, 0, RSEQ_CPU_ID_UNINITIALIZED_, 0);
        if (err < 0)
            return err;
        rseq_slot_give(current->mm, current->rseq_cpu);
        rseq_forget(current);
        return 0;
    }

    if (flags != 0)
        return _EINVAL;

    if (current->rseq_area != 0) {
        // Already registered: tell apart a repeat of the same registration
        // from a different one.
        if (current->rseq_area != area || len != current->rseq_len)
            return _EINVAL;
        if (sig != current->rseq_sig)
            return _EPERM;
        return _EBUSY;
    }

    // The original 32-byte area on a 32-byte boundary, or a longer one --
    // everything this kernel fills in fits in the original.
    if (len < ORIG_RSEQ_SIZE_ || (area & (RSEQ_ALIGN_ - 1)) != 0)
        return _EINVAL;
    if (!guest_abi_range_valid(current->abi, area, len))
        return _EFAULT;

    // A stale rseq_cs left by a previous user of the area is cleared rather
    // than refused: libcs reuse areas for new threads without clearing them.
    qword_t cs;
    if (user_get(area + RSEQ_OFF_RSEQ_CS, cs))
        return _EFAULT;
    if (cs != 0) {
        qword_t zero = 0;
        if (user_put(area + RSEQ_OFF_RSEQ_CS, zero))
            return _EFAULT;
    }

    int slot = rseq_slot_take(current->mm);
    int err = rseq_write_ids(area, (dword_t) slot, (dword_t) slot, (dword_t) slot);
    if (err < 0) {
        rseq_slot_give(current->mm, slot);
        return err;
    }
    current->rseq_area = area;
    current->rseq_len = len;
    current->rseq_sig = sig;
    current->rseq_cpu = slot;
    return 0;
}

void rseq_fork(struct task *child, bool shares_mm) {
    // Linux's rseq_fork: a thread, or a vfork child, sharing the address
    // space starts with no registration of its own. A forked process keeps
    // its parent's -- and the parent's CPU number, which nothing else in the
    // new address space holds.
    if (shares_mm) {
        rseq_forget(child);
        return;
    }
    // The copied address space inherited the parent's counts along with
    // everything else; its only thread is this one.
    for (int i = 0; i < 64; i++)
        atomic_store(&child->mm->rseq_cpu_users[i], 0);
    if (child->rseq_area != 0 && child->rseq_cpu >= 0 && child->rseq_cpu < 64)
        atomic_store(&child->mm->rseq_cpu_users[child->rseq_cpu], 1);
}

void rseq_exec(struct task *task) {
    if (task->rseq_area != 0)
        rseq_slot_give(task->mm, task->rseq_cpu);
    rseq_forget(task);
}

void rseq_exit(struct task *task) {
    if (task->rseq_area != 0)
        rseq_slot_give(task->mm, task->rseq_cpu);
    rseq_forget(task);
}

static guest_addr_t *rseq_ip(struct cpu_state *cpu, enum guest_abi abi, guest_addr_t *scratch) {
    switch (abi) {
        case GUEST_ABI_AMD64:
            return &cpu->amd64_rip;
        case GUEST_ABI_ARM64:
            return &cpu->arm64_pc;
        case GUEST_ABI_RISCV64:
            return &cpu->riscv64_pc;
        default:
            *scratch = cpu->eip;
            return scratch;
    }
}

// Linux's rseq_signal_deliver -> rseq_ip_fixup: a signal about to be delivered
// inside the registered critical section sends the thread to its abort handler
// first, so the handler's frame returns there. Outside one, the stale rseq_cs
// is cleared. False when the descriptor is not a valid one, which Linux answers
// with SIGSEGV.
bool rseq_signal_deliver(void) {
    guest_addr_t area = current->rseq_area;
    if (area == 0)
        return true;
    qword_t cs_addr;
    if (user_get(area + RSEQ_OFF_RSEQ_CS, cs_addr))
        return false;
    if (cs_addr == 0)
        return true;
    struct rseq_cs_ cs;
    if (user_read(cs_addr, &cs, sizeof(cs)))
        return false;
    // rseq_get_rseq_cs: version 0, a range that does not wrap, and an abort
    // handler outside it, all inside the address space. The flags words are
    // deprecated: any bit set, in the descriptor or the area, is refused.
    qword_t end = cs.start_ip + cs.post_commit_offset;
    if (cs.version != RSEQ_CS_VERSION_ || end < cs.start_ip ||
            !guest_abi_addr_valid(current->abi, end) ||
            !guest_abi_addr_valid(current->abi, cs.abort_ip) ||
            (cs.abort_ip >= cs.start_ip && cs.abort_ip < end))
        return false;
    dword_t area_flags;
    if (cs.flags != 0 || user_get(area + RSEQ_OFF_FLAGS, area_flags) || area_flags != 0)
        return false;

    guest_addr_t scratch;
    guest_addr_t *ip = rseq_ip(&current->cpu, current->abi, &scratch);
    qword_t zero = 0;
    if (*ip < cs.start_ip || *ip >= end)
        return user_put(area + RSEQ_OFF_RSEQ_CS, zero) == 0;

    // The four bytes before the abort handler must hold the registration's
    // signature: that is what stops a forged descriptor from sending the
    // thread anywhere it likes.
    dword_t sig;
    if (user_get(cs.abort_ip - 4, sig) || sig != current->rseq_sig)
        return false;
    if (user_put(area + RSEQ_OFF_RSEQ_CS, zero))
        return false;
    *ip = cs.abort_ip;
    if (ip == &scratch || current->abi == GUEST_ABI_AMD64)
        current->cpu.eip = (dword_t) cs.abort_ip;
    return true;
}

// getcpu(2). It returned 0 without writing either answer, so sched_getcpu()
// got whatever was on the caller's stack. The same CPU rseq reports.
static dword_t getcpu_common(guest_addr_t cpu_addr, guest_addr_t node_addr) {
    dword_t cpu = (dword_t) task_current_cpu(current);
    dword_t node = 0;
    if (cpu_addr != 0 && user_put(cpu_addr, cpu))
        return _EFAULT;
    if (node_addr != 0 && user_put(node_addr, node))
        return _EFAULT;
    return 0;
}

dword_t sys_getcpu(addr_t cpu_addr, addr_t node_addr, addr_t UNUSED(tcache)) {
    return getcpu_common(cpu_addr, node_addr);
}

dword_t sys_getcpu_guest(guest_addr_t cpu_addr, guest_addr_t node_addr, guest_addr_t UNUSED(tcache)) {
    return getcpu_common(cpu_addr, node_addr);
}
