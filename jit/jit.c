#define DEFAULT_CHANNEL instr
#include "debug.h"
#include "jit/jit.h"
#include "jit/gen.h"
#include "jit/frame.h"
#include "emu/cpu.h"
#include "emu/memory.h"
#include "emu/interrupt.h"
#include "kernel/task.h"
#include "util/list.h"
#include "util/sync.h"
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int current_pid(struct task *task);
static atomic_bool amd64_jit_enabled = true;
static atomic_ulong amd64_jit_compile_attempts;
static atomic_ulong amd64_jit_compile_successes;
static atomic_ulong amd64_jit_compile_fallbacks;
static atomic_ulong amd64_jit_compile_fallback_by_key[512];
static pthread_mutex_t i386_single_step_comm_lock = PTHREAD_MUTEX_INITIALIZER;
static char i386_single_step_comm[16] = "";
static pthread_mutex_t i386_no_cache_comm_lock = PTHREAD_MUTEX_INITIALIZER;
static char i386_no_cache_comm[16] = "";
static pthread_mutex_t i386_special_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t_ i386_special_trace_tgid;
static char i386_special_trace_comm[16] = "";
static unsigned i386_special_trace_count;

#define AMD64_CC1_JIT_TRACE_COUNT 128
struct amd64_cc1_jit_trace {
    guest_addr_t block_addr;
    uint8_t bytes[8];
    bool have_bytes;
    qword_t before_rip;
    qword_t after_rip;
    qword_t before_rsp;
    qword_t after_rsp;
    qword_t before_rax;
    qword_t after_rax;
    qword_t before_rdi;
    qword_t after_rdi;
    qword_t before_rsi;
    qword_t after_rsi;
    qword_t before_rdx;
    qword_t after_rdx;
    qword_t before_rcx;
    qword_t after_rcx;
    dword_t before_eflags;
    dword_t after_eflags;
    int interrupt;
};

static struct amd64_cc1_jit_trace amd64_cc1_jit_trace[AMD64_CC1_JIT_TRACE_COUNT];
static unsigned amd64_cc1_jit_trace_next;
static pid_t_ amd64_cc1_jit_trace_pid;

static inline bool amd64_cc1_jit_trace_enabled(void) {
    // Diagnostic ring buffer for bisecting cc1 JIT issues. OFF unless
    // ISH_AMD64_CC1_TRACE is set: it snapshots the full guest cpu_state on every
    // compiled block, which is pure overhead on a normal compile. Gated per
    // compiled block in the amd64 frontend; only "cc1" begins with 'c', so the
    // one-byte pre-filter skips the strcmp for every other guest.
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_AMD64_CC1_TRACE") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        current->comm[0] == 'c' &&
        strcmp(current->comm, "cc1") == 0;
}

enum amd64_cc1_force_interp_mode {
    amd64_cc1_force_interp_none = 0,
    amd64_cc1_force_interp_all,
    amd64_cc1_force_interp_suspect_ranges,
    amd64_cc1_force_interp_suspect_ranges_wide,
};

static int amd64_cc1_force_interp_mode(void) {
    static int mode = -1;
    if (mode != -1)
        return mode;

    const char *value = getenv("ISH_AMD64_CC1_FORCE_INTERP");
    if (value == NULL) {
        // Default OFF: cc1 runs under the JIT like every other guest. This was
        // force_interp_all while bisecting a cc1 miscompile that no longer
        // reproduces (JIT-vs-interp .o output is byte-identical across the
        // corpus); forcing the interpreter makes every gcc/g++ compile ~3x
        // slower. Set ISH_AMD64_CC1_FORCE_INTERP=1 to re-enable if a regression
        // turns up.
        mode = amd64_cc1_force_interp_none;
        return mode;
    }
    if (strcmp(value, "1") == 0 ||
            strcmp(value, "true") == 0 || strcmp(value, "on") == 0 ||
            strcmp(value, "yes") == 0 || strcmp(value, "all") == 0) {
        mode = amd64_cc1_force_interp_all;
        return mode;
    }
    if (strcmp(value, "ranges") == 0 || strcmp(value, "suspect") == 0 ||
            strcmp(value, "narrow") == 0) {
        mode = amd64_cc1_force_interp_suspect_ranges;
        return mode;
    }
    if (strcmp(value, "ranges-wide") == 0 || strcmp(value, "suspect-wide") == 0 ||
            strcmp(value, "wide") == 0) {
        mode = amd64_cc1_force_interp_suspect_ranges_wide;
        return mode;
    }
    mode = amd64_cc1_force_interp_none;
    return mode;
}

static bool amd64_cc1_force_interp_block(guest_addr_t ip) {
    // env-driven RIP-range force-interp, for bisecting an amd64 JIT bug in ANY
    // guest: ISH_AMD64_FORCE_INTERP_LO/_HI (hex) force blocks in [LO, HI) to the
    // interpreter. Read once per process (so each bisection run picks its own range).
    static int range_init = 0;
    static guest_addr_t range_lo = 0, range_hi = 0;
    if (!range_init) {
        const char *lo = getenv("ISH_AMD64_FORCE_INTERP_LO");
        const char *hi = getenv("ISH_AMD64_FORCE_INTERP_HI");
        if (lo != NULL && hi != NULL) {
            range_lo = (guest_addr_t) strtoull(lo, NULL, 0);
            range_hi = (guest_addr_t) strtoull(hi, NULL, 0);
        }
        range_init = 1;
    }
    if (range_hi != 0 && current != NULL && current->abi == GUEST_ABI_AMD64 &&
            ip >= range_lo && ip < range_hi)
        return true;

    if (!amd64_cc1_jit_trace_enabled())
        return false;

    switch (amd64_cc1_force_interp_mode()) {
    case amd64_cc1_force_interp_all:
        return true;
    case amd64_cc1_force_interp_suspect_ranges:
        return (ip >= 0x7ffffdf9f03eull && ip <= 0x7ffffdf9f17eull) ||
            (ip >= 0x7ffffdf83480ull && ip <= 0x7ffffdf83814ull) ||
            (ip >= 0x7ffffdf83a3aull && ip <= 0x7ffffdf83d99ull) ||
            (ip >= 0xf6b080ull && ip <= 0xf7832eull) ||
            (ip >= 0x7ffffdc1adb8ull && ip <= 0x7ffffdc2730dull) ||
            (ip >= 0x7ffffdc88398ull && ip <= 0x7ffffdc8859dull);
    case amd64_cc1_force_interp_suspect_ranges_wide:
        return (ip >= 0x7ffffdf9f03eull && ip <= 0x7ffffdf9f17eull) ||
            (ip >= 0x7ffffdf83480ull && ip <= 0x7ffffdf83814ull) ||
            (ip >= 0x7ffffdf83a3aull && ip <= 0x7ffffdf83d99ull) ||
            (ip >= 0xf6b080ull && ip <= 0xf7832eull) ||
            (ip >= 0x7ffffdc1adb8ull && ip <= 0x7ffffdc2730dull) ||
            (ip >= 0x7ffffdc88398ull && ip <= 0x7ffffdc8859dull) ||
            (ip >= 0x7ffffdc9a330ull && ip <= 0x7ffffdc9a663ull) ||
            (ip >= 0x7ffffdcb83e3ull && ip <= 0x7ffffdcb847bull);
    default:
        return false;
    }
}

static void amd64_cc1_jit_trace_record(guest_addr_t block_addr,
        struct tlb *tlb,
        const struct cpu_state *before, const struct cpu_state *after, int interrupt) {
    struct amd64_cc1_jit_trace *trace;
    if (!amd64_cc1_jit_trace_enabled())
        return;
    if (amd64_cc1_jit_trace_pid != current->pid) {
        memset(amd64_cc1_jit_trace, 0, sizeof(amd64_cc1_jit_trace));
        amd64_cc1_jit_trace_next = 0;
        amd64_cc1_jit_trace_pid = current->pid;
    }
    trace = &amd64_cc1_jit_trace[amd64_cc1_jit_trace_next++ % AMD64_CC1_JIT_TRACE_COUNT];
    memset(trace, 0, sizeof(*trace));
    trace->block_addr = block_addr;
    trace->have_bytes = tlb != NULL && tlb_read(tlb, block_addr, trace->bytes, sizeof(trace->bytes));
    trace->before_rip = before->amd64_rip;
    trace->after_rip = after->amd64_rip;
    trace->before_rsp = before->amd64_regs[amd64_rsp];
    trace->after_rsp = after->amd64_regs[amd64_rsp];
    trace->before_rax = before->amd64_regs[amd64_rax];
    trace->after_rax = after->amd64_regs[amd64_rax];
    trace->before_rdi = before->amd64_regs[amd64_rdi];
    trace->after_rdi = after->amd64_regs[amd64_rdi];
    trace->before_rsi = before->amd64_regs[amd64_rsi];
    trace->after_rsi = after->amd64_regs[amd64_rsi];
    trace->before_rdx = before->amd64_regs[amd64_rdx];
    trace->after_rdx = after->amd64_regs[amd64_rdx];
    trace->before_rcx = before->amd64_regs[amd64_rcx];
    trace->after_rcx = after->amd64_regs[amd64_rcx];
    trace->before_eflags = before->eflags;
    trace->after_eflags = after->eflags;
    trace->interrupt = interrupt;
}

void dump_amd64_cc1_jit_trace(const struct cpu_state *cpu) {
    unsigned total, count, start, i;
    (void) cpu;
    if (!amd64_cc1_jit_trace_enabled())
        return;
    if (amd64_cc1_jit_trace_pid != current->pid)
        return;
    total = amd64_cc1_jit_trace_next;
    if (total == 0)
        return;
    count = total < AMD64_CC1_JIT_TRACE_COUNT ? total : AMD64_CC1_JIT_TRACE_COUNT;
    start = total >= AMD64_CC1_JIT_TRACE_COUNT ? total - AMD64_CC1_JIT_TRACE_COUNT : 0;
    printk("amd64 cc1 jit trace (%u entries):\n", count);
    for (i = 0; i < count; i++) {
        const struct amd64_cc1_jit_trace *trace =
            &amd64_cc1_jit_trace[(start + i) % AMD64_CC1_JIT_TRACE_COUNT];
        printk("cc1-jit[%03u] block=%#llx int=%d rip=%#llx->%#llx rsp=%#llx->%#llx rax=%#llx->%#llx rdi=%#llx->%#llx rsi=%#llx->%#llx rdx=%#llx->%#llx rcx=%#llx->%#llx eflags=%#x->%#x\n",
               i,
               (unsigned long long) trace->block_addr,
               trace->interrupt,
               (unsigned long long) trace->before_rip,
               (unsigned long long) trace->after_rip,
               (unsigned long long) trace->before_rsp,
               (unsigned long long) trace->after_rsp,
               (unsigned long long) trace->before_rax,
               (unsigned long long) trace->after_rax,
               (unsigned long long) trace->before_rdi,
               (unsigned long long) trace->after_rdi,
               (unsigned long long) trace->before_rsi,
               (unsigned long long) trace->after_rsi,
               (unsigned long long) trace->before_rdx,
               (unsigned long long) trace->after_rdx,
               (unsigned long long) trace->before_rcx,
               (unsigned long long) trace->after_rcx,
               trace->before_eflags,
               trace->after_eflags);
        if (trace->have_bytes) {
            printk("cc1-jit[%03u] bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   i,
                   trace->bytes[0], trace->bytes[1], trace->bytes[2], trace->bytes[3],
                   trace->bytes[4], trace->bytes[5], trace->bytes[6], trace->bytes[7]);
        }
    }
}

// forceinline: this gates ~5 amd64_jit_debug() points per executed block in the
// hottest emulator loop. As a plain (un-inlined) call the cached check still cost
// a measurable slice; inlined it is a single static load + compare.
static inline __attribute__((always_inline)) bool amd64_jit_debug_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    return enabled == 1;
}

// Single cached gate for the per-block debug machinery in the amd64 frontend
// (force-interp ranges, cc1 JIT trace). All off by default; when so, the frontend
// skips amd64_cc1_force_interp_block() and the cc1 cpu_state snapshot entirely
// rather than calling them on every executed block (the hottest loop in the emu).
static inline bool amd64_frontend_debug_active(void) {
    static int active = -1;
    if (active == -1)
        active = (getenv("ISH_AMD64_FORCE_INTERP_HI") != NULL ||
                  getenv("ISH_AMD64_CC1_TRACE") != NULL) ? 1 : 0;
    return active == 1;
}

static void amd64_jit_debug_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[amd64-jit] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

// Guard the (varargs) debug call at the call site so the arguments aren't even
// evaluated — let alone the variadic call made — when JIT debug is disabled.
// This runs several times per compiled block / frontend entry, so the bare
// call (which previously only returned early *inside* the function) showed up
// as ~420 sampled stacks in the amd64-JIT hot path.
#define amd64_jit_debug(...) do { \
    if (amd64_jit_debug_enabled()) \
        amd64_jit_debug_impl(__VA_ARGS__); \
} while (0)

static void amd64_jit_note_compile_attempt(void) {
    atomic_fetch_add_explicit(&amd64_jit_compile_attempts, 1, memory_order_relaxed);
}

static void amd64_jit_note_compile_success(void) {
    atomic_fetch_add_explicit(&amd64_jit_compile_successes, 1, memory_order_relaxed);
}

static bool amd64_jit_stats_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT_STATS") != NULL ? 1 : 0;
    return enabled == 1;
}

// Dump the per-opcode JIT fallback counts sorted by frequency, so the
// most-often-unimplemented ops (the ones worth turning into native gadgets
// to cut JIT->interp bridges) rank first. key 0x000-0x0ff = one-byte opcode,
// 0x100-0x1ff = 0F-prefixed (op2). Enable with ISH_TRACE_AMD64_JIT_STATS=1.
static void amd64_jit_dump_fallback_histogram(unsigned long total) {
    struct { unsigned key; unsigned long count; } ent[512];
    unsigned n = 0;
    for (unsigned k = 0; k < 512; k++) {
        unsigned long c = atomic_load_explicit(&amd64_jit_compile_fallback_by_key[k],
                memory_order_relaxed);
        if (c != 0) {
            ent[n].key = k;
            ent[n].count = c;
            n++;
        }
    }
    for (unsigned i = 1; i < n; i++) {
        unsigned key = ent[i].key;
        unsigned long count = ent[i].count;
        unsigned j = i;
        while (j > 0 && ent[j - 1].count < count) {
            ent[j] = ent[j - 1];
            j--;
        }
        ent[j].key = key;
        ent[j].count = count;
    }
    fprintf(stderr, "[amd64-jit-stats] block-fallbacks=%lu distinct-ops=%u (top by frequency):\n", total, n);
    unsigned show = n < 24 ? n : 24;
    for (unsigned i = 0; i < show; i++) {
        unsigned key = ent[i].key;
        unsigned long pm = total ? (1000UL * ent[i].count / total) : 0;
        fprintf(stderr, "[amd64-jit-stats]   %s%02x  count=%lu  (%lu.%lu%%)\n",
               key >= 0x100 ? "0f " : "   ", key & 0xff, ent[i].count, pm / 10, pm % 10);
    }
}

static void amd64_jit_note_compile_fallback(const struct gen_state *state, guest_addr_t ip) {
    unsigned key = (state->amd64_fallback_flags & 0x01)
        ? 0x100u | state->amd64_fallback_op2
        : state->amd64_fallback_opcode;
    unsigned long total = atomic_fetch_add_explicit(&amd64_jit_compile_fallbacks, 1,
            memory_order_relaxed) + 1;
    unsigned long key_total = atomic_fetch_add_explicit(&amd64_jit_compile_fallback_by_key[key], 1,
            memory_order_relaxed) + 1;

    if (amd64_jit_debug_enabled() && (total <= 32 || (total & (total - 1)) == 0)) {
        printk("[amd64-jit] fallback total=%lu op=%s%02x flags=%#x count=%lu ip=%#llx fallback_ip=%#llx attempts=%lu success=%lu comm=%s\n",
               total,
               (state->amd64_fallback_flags & 0x01) ? "0f" : "",
               (state->amd64_fallback_flags & 0x01) ? state->amd64_fallback_op2 : state->amd64_fallback_opcode,
               state->amd64_fallback_flags,
               key_total,
               (unsigned long long) ip,
               (unsigned long long) state->amd64_fallback_ip,
               atomic_load_explicit(&amd64_jit_compile_attempts, memory_order_relaxed),
               atomic_load_explicit(&amd64_jit_compile_successes, memory_order_relaxed),
               current != NULL ? current->comm : "?");
    }

    // Periodic sorted dump (powers of two >= 256) so a single run prints the
    // frequency ranking as it converges, without needing an exit hook.
    if (amd64_jit_stats_enabled() && total >= 8 && (total & (total - 1)) == 0)
        amd64_jit_dump_fallback_histogram(total);
}

bool amd64_jit_is_enabled(void) {
    return atomic_load_explicit(&amd64_jit_enabled, memory_order_relaxed);
}

void amd64_jit_set_enabled(bool enabled) {
    atomic_store_explicit(&amd64_jit_enabled, enabled, memory_order_relaxed);
}

bool i386_single_step_comm_matches(const char *comm) {
    bool match = false;
    if (comm == NULL || comm[0] == '\0')
        return false;
    pthread_mutex_lock(&i386_single_step_comm_lock);
    match = i386_single_step_comm[0] != '\0' &&
            strcmp(i386_single_step_comm, comm) == 0;
    pthread_mutex_unlock(&i386_single_step_comm_lock);
    return match;
}

void i386_single_step_comm_set(const char *comm) {
    pthread_mutex_lock(&i386_single_step_comm_lock);
    if (comm == NULL) {
        i386_single_step_comm[0] = '\0';
    } else {
        strncpy(i386_single_step_comm, comm, sizeof(i386_single_step_comm));
        i386_single_step_comm[sizeof(i386_single_step_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_single_step_comm_lock);
}

void i386_single_step_comm_get(char *buf, size_t bufsize) {
    if (buf == NULL || bufsize == 0)
        return;
    pthread_mutex_lock(&i386_single_step_comm_lock);
    strncpy(buf, i386_single_step_comm, bufsize);
    buf[bufsize - 1] = '\0';
    pthread_mutex_unlock(&i386_single_step_comm_lock);
}

bool i386_no_cache_comm_matches(const char *comm) {
    bool match = false;
    if (comm == NULL || comm[0] == '\0')
        return false;
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    match = i386_no_cache_comm[0] != '\0' &&
            strcmp(i386_no_cache_comm, comm) == 0;
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
    return match;
}

void i386_no_cache_comm_set(const char *comm) {
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    if (comm == NULL) {
        i386_no_cache_comm[0] = '\0';
    } else {
        strncpy(i386_no_cache_comm, comm, sizeof(i386_no_cache_comm));
        i386_no_cache_comm[sizeof(i386_no_cache_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
}

void i386_no_cache_comm_get(char *buf, size_t bufsize) {
    if (buf == NULL || bufsize == 0)
        return;
    pthread_mutex_lock(&i386_no_cache_comm_lock);
    strncpy(buf, i386_no_cache_comm, bufsize);
    buf[bufsize - 1] = '\0';
    pthread_mutex_unlock(&i386_no_cache_comm_lock);
}

void i386_special_trace_reset(pid_t_ tgid, const char *comm) {
    pthread_mutex_lock(&i386_special_trace_lock);
    i386_special_trace_tgid = tgid;
    i386_special_trace_count = 0;
    if (comm == NULL) {
        i386_special_trace_comm[0] = '\0';
    } else {
        strncpy(i386_special_trace_comm, comm, sizeof(i386_special_trace_comm));
        i386_special_trace_comm[sizeof(i386_special_trace_comm) - 1] = '\0';
    }
    pthread_mutex_unlock(&i386_special_trace_lock);
}

void i386_trace_special_op(const char *op, addr_t ip) {
    (void) op;
    (void) ip;
}

void i386_trace_special_reg_op(const char *op, addr_t ip, int reg) {
    (void) op;
    (void) ip;
    (void) reg;
}

// Defined in app/hook.c; installs EXC_BAD_ACCESS handler on the calling thread.
// No-op on non-arm64.  Forward-declared here to avoid a circular header dep.
extern void jit_install_thread_exception_handler(void);
// True when the Mach EXC_BAD_ACCESS handler is live for this process (device
// app). When it is, guest-memory bad accesses are translated in the Mach
// handler and we must NOT also install a POSIX SIGBUS handler (Mach preempts
// POSIX delivery anyway). False on the standalone CLI, where the POSIX handler
// is the only path. Defined in app/hook.c / platform/standalone.c.
extern bool jit_host_fault_mach_active(void);

// Thread-local jetsam_lock pointer for JIT crash recovery.
// Set to &jit->jetsam_lock while this thread is in cpu_step_to_interrupt (i.e.
// while the read lock is held).  Cleared before returning.  Accessed by
// jit_crash_fn() which runs on the same thread after a Mach exception redirect.
__thread wrlock_t *jit_crash_lock = NULL;
__thread lock_t *jit_crash_mutex_lock = NULL;
__thread sigjmp_buf jit_crash_unwind_buf;
__thread bool jit_crash_unwind_active = false;
__thread struct jit_frame *jit_crash_frame = NULL;
__thread struct cpu_state *jit_crash_cpu = NULL;
__thread int jit_crash_interrupt = INT_GPF;
// 64-bit: carries the guest fault address (amd64_rip / arm64_pc / eip, or a
// reverse-mapped SIGBUS address). Must be guest_addr_t, not addr_t (32-bit),
// or 64-bit amd64/arm64 addresses are truncated to their low 32 bits.
__thread guest_addr_t jit_crash_addr = 0;

static inline void jit_crash_track_mutex_lock(lock_t *mutex) {
    jit_crash_mutex_lock = mutex;
    lock(mutex, 0);
}

static inline void jit_crash_track_mutex_unlock(lock_t *mutex) {
    unlock(mutex);
    if (jit_crash_mutex_lock == mutex)
        jit_crash_mutex_lock = NULL;
}

static void jit_block_disconnect(struct jit *jit, struct jit_block *block);
static void jit_block_free(struct jit *jit, struct jit_block *block);
static void jit_free_jetsam(struct jit *jit);
static void jit_resize_hash(struct jit *jit, size_t new_size);

// Called by hook.c's Mach exception handler when a JIT thread faults while
// executing translated code. The handler redirects the faulting thread's PC
// here, so this runs on the faulting thread and can safely access thread-local
// state.
//
// Releasing the jetsam_lock read lock prevents write-lock waiters
// (cpu_run_to_interrupt doing jetsam cleanup) from blocking forever, which
// would hang any guest program that spawns many OS threads (e.g. Go programs).
__attribute__((__noreturn__))
void jit_crash_fn(void) {
    // If this thread faults while holding the guest-atomic mutex, release it so
    // other guest threads are not permanently blocked behind a failed JIT entry.
    extern lock_t atomic_l_lock;
    if (pthread_equal(atomic_l_lock.owner, pthread_self())) {
        atomic_l_lock.owner = zero_init(pthread_t);
        modify_locks_held_count(current, -1);
        pthread_mutex_unlock(&atomic_l_lock.m);
    }

    if (jit_crash_mutex_lock != NULL && pthread_equal(jit_crash_mutex_lock->owner, pthread_self())) {
        printk("JIT: crash recovery (pid %d) - releasing jit mutex after bad-access fault\n",
               current ? current->pid : -1);
        unlock(jit_crash_mutex_lock);
        jit_crash_mutex_lock = NULL;
    }

    if (jit_crash_lock != NULL) {
        printk("JIT: crash recovery (pid %d) - releasing jetsam_lock after bad-access fault\n",
               current ? current->pid : -1);
        pthread_rwlock_unlock(&jit_crash_lock->l);
        jit_crash_lock = NULL;
        if (jit_crash_unwind_active)
            siglongjmp(jit_crash_unwind_buf, 1);
    } else {
        // EXC_BAD_ACCESS outside JIT execution context — real bug, let it crash.
        abort();
    }
    pthread_exit(NULL);
}

// Shared translation of a host bad-access address (caught by the POSIX SIGBUS
// handler on the CLI, or the Mach EXC_BAD_ACCESS handler on device) into a
// pending guest SIGBUS. Returns true only when host_addr backs a live guest
// page in the current address space — i.e. a genuine guest-memory fault such as
// a store to a now-past-EOF page of a file-backed mmap whose backing file was
// truncated under the mapping. On success it arms the JIT crash-unwind to
// return INT_BUS carrying the guest fault address; the kernel then delivers a
// guest SIGBUS (BUS_ADRERR), matching Linux. Returns false for faults that
// don't map to guest memory (real JIT/host bugs), which are left to crash so
// they are not silently hidden.
bool jit_translate_host_fault(void *host_addr) {
    if (!jit_crash_unwind_active || current == NULL)
        return false;
    guest_addr_t guest;
    if (!mem_host_addr_to_guest(current->mem, host_addr, &guest))
        return false;
    jit_crash_interrupt = INT_BUS;
    jit_crash_addr = guest;
    return true;
}

// Faulting-thread entry point for the device (Mach) path. The Mach exception
// handler (app/hook.c) runs on a dedicated server pthread where `current` is
// NULL, so it cannot reverse-map the fault itself. Instead it redirects the
// FAULTING thread's PC here (fault address in the first argument register), so
// this runs with the guest thread's `current` intact. If the address backs a
// guest page, unwind with a guest SIGBUS; otherwise it is a real bad access
// (wild pointer in a helper, corrupted state) and we crash, logging the
// address. A re-entrancy guard covers only the page-table walk: if the walk
// itself faults it re-enters here, and we abort rather than loop.
__attribute__((__noreturn__))
void jit_crash_bus_fn(void *host_addr) {
    static __thread bool bus_dispatch_active = false;
    if (bus_dispatch_active)
        abort(); // the reverse-map walk faulted -> genuine memory corruption
    bus_dispatch_active = true;
    bool translated = jit_translate_host_fault(host_addr);
    bus_dispatch_active = false;
    if (translated)
        jit_crash_fn(); // noreturn: releases locks and unwinds with INT_BUS
    printk("JIT: untranslatable bad access at host %p (pid %d) - crashing\n",
           host_addr, current ? current->pid : -1);
    abort();
}

// POSIX SIGBUS handler (standalone CLI only — the device app catches these via
// Mach before POSIX delivery). Runs on a per-thread altstack (SA_ONSTACK).
static void jit_host_sigbus_handler(int sig, siginfo_t *info, void *uctx) {
    (void) uctx;
    if (info != NULL && jit_translate_host_fault(info->si_addr)) {
        // Unwind out of the faulting gadget back to cpu_step_to_interrupt's
        // sigsetjmp, which returns INT_BUS. jit_crash_fn releases the jetsam
        // read lock (and any guest-atomic lock an LSE helper held) before
        // siglongjmp'ing. SA_NODEFER keeps SIGBUS unblocked across the longjmp:
        // that sigsetjmp was armed with savesigs=0, so the signal mask is not
        // restored, and without NODEFER a subsequent truncation fault would be
        // silently blocked forever.
        jit_crash_fn(); // noreturn
    }
    // Not a translatable guest fault: real bug. Restore the default disposition
    // and return so the instruction re-faults into a normal crash/core dump,
    // preserving debuggability.
    signal(sig, SIG_DFL);
}

// Process-wide SIGBUS disposition, installed once.
static void jit_install_host_fault_sigaction(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = jit_host_sigbus_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
}

// Per-thread setup for the CLI POSIX host-fault handler: a dedicated altstack
// (the guest thread's host stack may be in any state when a JIT store faults)
// plus the one-time process-wide sigaction. Called once per JIT pthread. The
// altstack is a thread-local array so it is reclaimed automatically at thread
// exit (no per-thread malloc leak across churny fork/exec/thread workloads).
static void jit_install_host_fault_signal_handler(void) {
    static __thread char alt_stack[128 * 1024];
    stack_t ss = {0};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, NULL);

    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, jit_install_host_fault_sigaction);
}

// Acquire jetsam write lock with a short timeout. Uses non-blocking
// pthread_rwlock_trywrlock in a retry loop so that a permanently stuck
// read-lock holder (e.g. a goroutine that crashed in jit_enter while LLDB
// intercepted the exception before jit_crash_fn could release the lock)
// doesn't deadlock all write-lock waiters. Callers must unlock with
// pthread_rwlock_unlock(&jit->jetsam_lock.l) on success.
static bool jetsam_write_lock_timed(struct jit *jit) {
    static const struct timespec kDelay = {0, 5000000}; // 5ms per retry
    for (int i = 0; i < 1000; i++) {                    // up to 5s total
        if (pthread_rwlock_trywrlock(&jit->jetsam_lock.l) == 0)
            return true;
        nanosleep(&kDelay, NULL);
    }
    return false;
}

static void jit_cleanup_jetsam_if_needed(struct jit *jit) {
    if (jit == NULL)
        return;

    lock(&jit->lock, 0);
    if (list_empty(&jit->jetsam)) {
        unlock(&jit->lock);
        return;
    }

    // Set write_wanted before taking the write lock so goroutines still in
    // jit_enter exit promptly at the next block boundary and drop their read
    // lock on jetsam_lock.
    unlock(&jit->lock);
    __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
    if (jetsam_write_lock_timed(jit)) {
        lock(&jit->lock, 0);
        jit_free_jetsam(jit);
        // Goroutines compare against cleanup_seq to detect stale last_block
        // pointers after a cleanup pass.
        atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
        __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);
        pthread_rwlock_unlock(&jit->jetsam_lock.l);
        unlock(&jit->lock);
    } else {
        // If we time out, leave jetsam pending for a later pass rather than
        // pinning all JIT goroutines in yield mode.
        __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);
    }
}

static bool jit_i386_gpf_addr_accessible(guest_addr_t addr, int type) {
    if (current == NULL || addr == 0)
        return false;

    bool accessible = false;
    if (trylockr(&current->mem->lock) != 0)
        return false;
    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry != NULL && entry->data != NULL && entry->data->data != NULL) {
        accessible = type != MEM_WRITE || P_WRITABLE(entry->flags);
    }
    read_unlock(&current->mem->lock);
    return accessible;
}

static bool jit_i386_gpf_looks_retryable(struct cpu_state *cpu) {
    if (current == NULL || current->abi == GUEST_ABI_AMD64)
        return false;
    if (cpu->segfault_addr == 0)
        return false;

    bool read_ok = jit_i386_gpf_addr_accessible(cpu->segfault_addr, MEM_READ);
    bool write_ok = jit_i386_gpf_addr_accessible(cpu->segfault_addr, MEM_WRITE);
    if (cpu->segfault_was_write)
        return write_ok;
    return read_ok || write_ok;
}

struct jit *jit_new(struct mmu *mmu) {
    struct jit *jit = calloc(1, sizeof(struct jit));
    if (!jit) {
        // Handle allocation failure
        printk("ERROR: Failed to allocate memory for JIT\n");
        return NULL;
    }
    lock(&jit->lock, 0);
    jit->mmu = mmu;
    jit_resize_hash(jit, JIT_INITIAL_HASH_SIZE);
    jit->page_hash = calloc(JIT_PAGE_HASH_SIZE, sizeof(*jit->page_hash));
    list_init(&jit->jetsam);
    lock_init(&jit->lock, "jit_new\0");
    wrlock_init(&jit->jetsam_lock);
    unlock(&jit->lock);
    return jit;
}

void jit_free(struct jit *jit) {
    if (!jit) return;
    lock(&jit->lock, 0);
    for (size_t i = 0; i < jit->hash_size; i++) {
        struct jit_block *block, *tmp;
        if (list_null(&jit->hash[i]))
            continue;
        list_for_each_entry_safe(&jit->hash[i], block, tmp, chain) {
            jit_block_free(jit, block);
        }
    }
    jit_free_jetsam(jit);
    free(jit->page_hash);
    free(jit->hash);
    // jetsam_lock uses the raw pthread_rwlock `.l` directly on its per-block
    // hot path (rdlock/trywrlock), so this teardown drain must too — the
    // wrlock_t API now drives a separate mutex+condvar (rw_locks.h) and would
    // desync from those `.l` acquires.
    pthread_rwlock_wrlock(&jit->jetsam_lock.l);
    unlock(&jit->lock);
    free(jit);
}

static inline struct list *blocks_list(struct jit *jit, page_t page, int i) {
    // Page numbers are dense small integers, so plain modulo distributes fine here.
    return &jit->page_hash[page % JIT_PAGE_HASH_SIZE].blocks[i];
}

// Block addresses are clustered (aligned entry points, dense code pages), and
// hash sizes are powers of two, so a plain modulo uses only the low address
// bits and piles blocks into a fraction of the buckets. Mix with the 64-bit
// golden ratio and take high bits before reducing.
static inline size_t jit_hash_bucket(guest_addr_t addr, size_t size) {
    return (size_t) ((addr * 0x9E3779B97F4A7C15ull) >> 32) % size;
}

void jit_invalidate_range(struct jit *jit, page_t start, page_t end) {
    lock(&jit->lock, 0);
    struct jit_block *block, *tmp;
    for (page_t page = start; page < end; page++) {
        for (int i = 0; i <= 1; i++) {
            struct list *blocks = blocks_list(jit, page, i);
            if (list_null(blocks))
                continue;
            list_for_each_entry_safe(blocks, block, tmp, page[i]) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
        }
    }
    unlock(&jit->lock);
}

void jit_invalidate_page(struct jit *jit, page_t page) {
    jit_invalidate_range(jit, page, page + 1);
}

void jit_invalidate_all(struct jit *jit) {
    struct mem *mem = container_of(jit->mmu, struct mem, mmu);
    jit_invalidate_range(jit, 0, mem->page_limit);
}

static void jit_resize_hash(struct jit *jit, size_t new_size) {
    TRACE_(verbose, "%d resizing hash to %lu, using %lu bytes for gadgets\n", current_pid(current), new_size, jit->mem_used);
    struct list *new_hash = calloc(new_size, sizeof(struct list));
    for (size_t i = 0; i < jit->hash_size; i++) {
        if (list_null(&jit->hash[i]))
            continue;
        struct jit_block *block, *tmp;
        list_for_each_entry_safe(&jit->hash[i], block, tmp, chain) {
            list_remove(&block->chain);
            list_init_add(&new_hash[jit_hash_bucket(block->addr, new_size)], &block->chain);
        }
    }
    free(jit->hash);
    jit->hash = new_hash;
    jit->hash_size = new_size;
}

static void jit_insert(struct jit *jit, struct jit_block *block) {
    jit->mem_used += block->used;
    jit->num_blocks++;
    // target an average hash chain length of 1-2
    if (jit->num_blocks >= jit->hash_size * 2)
        jit_resize_hash(jit, jit->hash_size * 2);

    list_init_add(&jit->hash[jit_hash_bucket(block->addr, jit->hash_size)], &block->chain);
    list_init_add(blocks_list(jit, PAGE(block->addr), 0), &block->page[0]);
    if (PAGE(block->addr) != PAGE(block->end_addr))
        list_init_add(blocks_list(jit, PAGE(block->end_addr), 1), &block->page[1]);
}

static struct jit_block *jit_lookup(struct jit *jit, guest_addr_t addr) {
    struct list *bucket = &jit->hash[jit_hash_bucket(addr, jit->hash_size)];
    if (list_null(bucket))
        return NULL;
    struct jit_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static struct jit_block *jit_block_compile_common(guest_addr_t ip, struct tlb *tlb,
        bool amd64, bool arm64, bool riscv64, bool *fallback_to_interp) {
    struct gen_state state;
    TRACE("%d %08llx --- compiling:\n", current_pid(current), (unsigned long long) ip);
    if (amd64)
        amd64_jit_debug("compile ip=%llx comm=%s abi=%d", (unsigned long long) ip,
                current != NULL ? current->comm : "(null)",
                current != NULL ? current->abi : -1);
    if (amd64)
        amd64_jit_note_compile_attempt();

    if (fallback_to_interp != NULL)
        *fallback_to_interp = false;

    bool started = arm64 ? gen_start_arm64(ip, &state)
                 : riscv64 ? gen_start_riscv64(ip, &state)
                 : amd64 ? gen_start_amd64(ip, &state)
                 : gen_start(ip, &state);
    if (!started)
        return NULL;
    state.oom_active = true;
    if (setjmp(state.oom_recovery) != 0) {
        // OOM hit during compilation; free the partial block and signal failure
        free(state.block);
        return NULL;
    }
    while (true) {
        if (!gen_step(&state, tlb))
            break;
        // no block should span more than 2 pages
        // guarantee this by limiting total block size to 1 page
        // guarantee that by stopping as soon as there's less space left than
        // the maximum length of an x86 instruction
        // TODO refuse to decode instructions longer than 15 bytes
        guest_addr_t consumed = arm64 ? state.arm64_ip - ip
                              : riscv64 ? state.riscv64_ip - ip
                              : amd64 ? state.amd64_ip - ip
                              : state.ip - ip;
        if (consumed >= PAGE_SIZE - 15) {
            gen_exit(&state);
            break;
        }
    }

    if (amd64 && (state.amd64_abort_block_to_interp || state.amd64_fallback_to_interp)) {
        amd64_jit_debug("compile fallback ip=%llx fallback_ip=%llx",
                (unsigned long long) ip,
                (unsigned long long) state.amd64_fallback_ip);
        amd64_jit_note_compile_fallback(&state, ip);
        free(state.block);
        if (fallback_to_interp != NULL)
            *fallback_to_interp = true;
        return NULL;
    }

    gen_end(&state);
    if (amd64) {
        amd64_jit_note_compile_success();
        amd64_jit_debug("compile block ip=%llx end=%llx size=%zu",
                (unsigned long long) ip,
                (unsigned long long) state.block->end_addr,
                state.size);
    }
    if (arm64)
        assert(state.arm64_ip - ip <= PAGE_SIZE);
    else if (riscv64)
        assert(state.riscv64_ip - ip <= PAGE_SIZE);
    else if (amd64)
        assert(state.amd64_ip - ip <= PAGE_SIZE);
    else
        assert(state.ip - ip <= PAGE_SIZE);
    state.block->used = state.capacity;
    return state.block;
}

static struct jit_block *jit_block_compile(addr_t ip, struct tlb *tlb) {
    return jit_block_compile_common(ip, tlb, false, false, false, NULL);
}

static struct jit_block *jit_block_compile_amd64(guest_addr_t ip, struct tlb *tlb,
        bool *fallback_to_interp) {
    return jit_block_compile_common(ip, tlb, true, false, false, fallback_to_interp);
}

static struct jit_block *jit_block_compile_arm64(guest_addr_t ip, struct tlb *tlb) {
    return jit_block_compile_common(ip, tlb, false, true, false, NULL);
}

static struct jit_block *jit_block_compile_riscv64(guest_addr_t ip, struct tlb *tlb) {
    return jit_block_compile_common(ip, tlb, false, false, true, NULL);
}

// Remove all pointers to the block. It can't be freed yet because another
// thread may be executing it.
static void jit_block_disconnect(struct jit *jit, struct jit_block *block) {
    if (jit != NULL) {
        jit->mem_used -= block->used;
        jit->num_blocks--;
    }
    list_remove(&block->chain);
    for (int i = 0; i <= 1; i++) {
        list_remove(&block->page[i]);
        list_remove_safe(&block->jumps_from_links[i]);

        struct jit_block *prev_block, *tmp;
        
        list_for_each_entry_safe(&block->jumps_from[i], prev_block, tmp, jumps_from_links[i]) {
            if (prev_block->jump_ip[i] != NULL)
                *prev_block->jump_ip[i] = prev_block->old_jump_ip[i]; // Crashed here June 12 2022, 19 Nov 2022
            list_remove(&prev_block->jumps_from_links[i]);
        }
    }
}

static void jit_block_free(struct jit *jit, struct jit_block *block) {
    jit_block_disconnect(jit, block);
    free(block);
}

static void jit_free_jetsam(struct jit *jit) {
    struct jit_block *block, *tmp;
    list_for_each_entry_safe(&jit->jetsam, block, tmp, jetsam) {
        list_remove(&block->jetsam);
        free(block);
    }
}

int jit_enter(struct jit_block *block, struct jit_frame *frame, struct tlb *tlb);

static inline size_t jit_cache_hash(guest_addr_t ip) {
    // Same mixing rationale as jit_hash_bucket: ip ^ (ip >> 12) preserved the
    // low-bit clustering of block addresses, causing conflict evictions in
    // this direct-mapped cache.
    return (size_t) ((ip * 0x9E3779B97F4A7C15ull) >> 32) % JIT_CACHE_SIZE;
}

static inline bool cpu_take_poke(struct cpu_state *cpu) {
    return __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST);
}

static inline bool jit_should_yield(struct jit *jit, struct cpu_state *cpu) {
    if (__atomic_load_n(&jit->write_wanted, __ATOMIC_SEQ_CST))
        return true;
    return cpu_take_poke(cpu);
}

static int cpu_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit *jit = cpu->mmu->jit;

    // Install EXC_BAD_ACCESS on this thread once, for JIT crash recovery.
    // Thread-level exception ports are scoped to only this pthread; no other
    // thread in the process is affected.  Cost is one Mach call per OS thread
    // lifetime (guarded by a thread-local flag), not per jit_enter call.
    static __thread bool exception_handler_installed = false;
    if (!exception_handler_installed) {
        jit_install_thread_exception_handler();
        // When the Mach handler isn't active (standalone CLI), a host SIGBUS on
        // a truncated file-backed guest mmap would otherwise kill the process.
        // Install a POSIX handler that translates it into a guest SIGBUS.
        if (!jit_host_fault_mach_active())
            jit_install_host_fault_signal_handler();
        exception_handler_installed = true;
    }

    // Keep the hot path off malloc/free. With iOS debug malloc enabled
    // (guard pages + scribbling), even these small short-lived allocations
    // are expensive enough to dominate guest startup helpers like /bin/uname.
    struct jit_block *cache[JIT_CACHE_SIZE] = {};
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    frame->cpu = *cpu;
    assert(jit->mmu == cpu->mmu);

    jit_crash_frame = frame;
    jit_crash_cpu = cpu;
    jit_crash_interrupt = INT_GPF;
    jit_crash_addr = frame->cpu.eip;
    // savesigs=0: this sigsetjmp is the JIT crash-recovery target, reached only via
    // jit_crash_fn() from the Mach EXC_BAD_ACCESS handler. That handler redirects the
    // faulting thread (thread_set_state) without touching the host signal mask, so the
    // mask is invariant from here to any fault — there is nothing to restore. Saving it
    // (savesigs=1) cost a sigprocmask + sigaltstack syscall pair on EVERY block-dispatch
    // entry: ~35% of host time for a syscall-heavy guest. siglongjmp(...,1) at line ~418
    // honors the savemask=0 flag and acts as _longjmp (no mask restore).
    if (sigsetjmp(jit_crash_unwind_buf, 0) != 0) {
        if (jit_crash_cpu != NULL && jit_crash_frame != NULL)
            *jit_crash_cpu = jit_crash_frame->cpu;
        cpu->segfault_addr = jit_crash_addr;
        cpu->segfault_was_write = false;
        jit_crash_unwind_active = false;
        jit_crash_mutex_lock = NULL;
        jit_crash_frame = NULL;
        jit_crash_cpu = NULL;
        return jit_crash_interrupt;
    }
    jit_crash_unwind_active = true;

    // Use pthread directly (not read_lock) to block in the kernel rather than
    // spinning through atomic_l_lock — eliminates mutex saturation when many
    // goroutines wait for a jetsam write-lock to clear.
    pthread_rwlock_rdlock(&jit->jetsam_lock.l);
    // Register lock for crash recovery: on any EXC_BAD_ACCESS (null gadget
    // dispatch PC=0, or non-zero bad address), hook.c's handler redirects the
    // thread to jit_crash_fn() which reads this pointer and releases the lock.
    jit_crash_lock = &jit->jetsam_lock;

    // Track cleanup_seq locally (NOT in frame, which would corrupt assembly
    // gadget offsets for ret_cache — see frame.h "keep in sync with asm").
    unsigned last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
    addr_t last_retry_eip = 0;
    guest_addr_t last_retry_addr = 0;
    bool last_retry_write = false;
    unsigned last_retry_count = 0;

    int interrupt = INT_NONE;
    while (interrupt == INT_NONE) {
        // Another task thread can change this address space while we are still
        // running translated blocks. Revalidate the software TLB at block
        // boundaries so stale cached host pointers do not survive mmap/munmap/COW.
        if (tlb->mem_changes != cpu->mmu->changes)
            tlb_refresh(tlb, cpu->mmu);

        // Check write_wanted before any potentially slow operation (block lookup,
        // compilation). This ensures we release the read lock promptly even if we
        // haven't reached jit_enter yet — e.g. while waiting for jit->lock or
        // inside jit_block_compile under debug malloc.
        if (jit_should_yield(jit, cpu)) {
            interrupt = INT_TIMER;
            break;
        }
        addr_t ip = frame->cpu.eip;
        size_t cache_index = jit_cache_hash(ip);
        struct jit_block *block = cache[cache_index];
        if (block == NULL || block->addr != ip) {
            lock(&jit->lock, 0);
            block = jit_lookup(jit, ip);
            if (block == NULL) {
                // Compile outside jetsam_lock: jit_block_compile allocates memory,
                // and under debug malloc (guard pages + scribbling) this is very slow.
                // Holding jetsam_lock during compilation starves jetsam write-lock
                // waiters, and can chain-deadlock when malloc zone locks are contested
                // between goroutines that do and don't yet hold jetsam_lock.
                unlock(&jit->lock);
                jit_crash_lock = NULL;  // Lock released; disable crash recovery until re-acquired
                pthread_rwlock_unlock(&jit->jetsam_lock.l);

                if (jit_should_yield(jit, cpu)) {
                    interrupt = INT_TIMER;
                    goto done_unlocked;
                }

                block = jit_block_compile(ip, tlb);

                if (block == NULL) {
                    // OOM attempt 1: free already-invalidated (jetsam) blocks and retry.
                    // Set write_wanted before write_lock so goroutines executing in
                    // jit_enter see the flag at the next block boundary and exit promptly,
                    // releasing their read locks without waiting for the cycle counter.
                    __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                    if (jetsam_write_lock_timed(jit)) {
                        lock(&jit->lock, 0);
                        jit_free_jetsam(jit);
                        unlock(&jit->lock);
                        atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                        pthread_rwlock_unlock(&jit->jetsam_lock.l);
                        memset(cache, 0, sizeof(cache));
                        memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                        frame->last_block = NULL;
                    }
                    __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                    if (jit_should_yield(jit, cpu)) {
                        interrupt = INT_TIMER;
                        goto done_unlocked;
                    }
                    block = jit_block_compile(ip, tlb);

                    if (block == NULL) {
                        // OOM attempt 2: flush the entire JIT cache for this task.
                        printk("JIT OOM at %#x pid %d: flushed entire cache\n", ip, current->pid);
                        __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                        if (jetsam_write_lock_timed(jit)) {
                            // jit_invalidate_all acquires/releases jit->lock internally
                            jit_invalidate_all(jit);
                            lock(&jit->lock, 0);
                            jit_free_jetsam(jit);
                            unlock(&jit->lock);
                            atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                            pthread_rwlock_unlock(&jit->jetsam_lock.l);
                            memset(cache, 0, sizeof(cache));
                            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                            frame->last_block = NULL;
                        }
                        __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                        if (jit_should_yield(jit, cpu)) {
                            interrupt = INT_TIMER;
                            goto done_unlocked;
                        }
                        block = jit_block_compile(ip, tlb);
                        if (block == NULL) {
                            // Still OOM even after full flush: kill this guest task
                            printk("JIT OOM at %#x pid %d: even after full flush, killing task\n",
                                   ip, current->pid);
                            jit_crash_unwind_active = false;
                            jit_crash_frame = NULL;
                            jit_crash_cpu = NULL;
                            jit_crash_lock = NULL;
                            return INT_GPF;
                        }
                    }
                }

                // Re-acquire jetsam_lock now that compilation is done. If a jetsam
                // cleanup is already pending, discard the block and yield — it will
                // be recompiled (or found in the hash) on the next call.
                pthread_rwlock_rdlock(&jit->jetsam_lock.l);
                jit_crash_lock = &jit->jetsam_lock;  // Re-enable crash recovery
                if (jit_should_yield(jit, cpu)) {
                    jit_block_free(NULL, block);
                    interrupt = INT_TIMER;
                    break;
                }

                // Insert into hash. Another thread may have compiled the same block
                // while we were outside the locks; if so, use theirs and discard ours.
                lock(&jit->lock, 0);
                struct jit_block *existing = jit_lookup(jit, ip);
                if (existing != NULL) {
                    jit_block_free(NULL, block);
                    block = existing;
                } else {
                    jit_insert(jit, block);
                }
            } else {
                TRACE("%d %08x --- missed cache\n", current_pid(current), ip);
            }
            cache[cache_index] = block;
            unlock(&jit->lock);
        }
        struct jit_block *last_block = frame->last_block;
        // If cleanup_seq changed since last_block was set, jetsam blocks were
        // freed (by cpu_run_to_interrupt or an OOM path). last_block may be a
        // dangling pointer, AND the cache may contain stale pointers to freed
        // blocks whose memory has been reused — clear both.
        if (atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed) != last_block_cleanup_seq) {
            last_block = frame->last_block = NULL;
            memset(cache, 0, sizeof(cache));
            // Also clear the assembly-level return cache: it holds raw pointers
            // into jit_block code arrays. If jetsam freed those blocks, a ret
            // gadget reading a stale entry reads scribbled memory and stores
            // it into frame->last_block, causing a crash on the next iteration.
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
        }
        if (last_block != NULL &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
            lock(&jit->lock, 0);
            // can't mint new pointers to a block that has been marked jetsam
            // and is thus assumed to have no pointers left
            if (!last_block->is_jetsam && !block->is_jetsam) {
                for (int i = 0; i <= 1; i++) {
                    if (last_block->jump_ip[i] != NULL &&
                            (*last_block->jump_ip[i] & 0xffffffff) == block->addr) {
                        // Don't link backward jumps (target addr < source block addr).
                        // Backward edges form loop cycles that cause jit_enter to run
                        // indefinitely without returning to C, which prevents the cycle
                        // counter from firing and starves jetsam_lock write waiters.
                        // Unlinked backward jumps exit via jit_ret each iteration,
                        // allowing the cycle counter and poke checks to fire normally.
                        if (block->addr <= last_block->addr)
                            continue;
                        // Use store-release so that block->code[] writes from
                        // compilation are visible to any thread that reads this
                        // linked pointer. The reader side (gret in entry.S) has
                        // a matching dmb ishld load barrier; together they form
                        // a proper acquire-release pair on AArch64. Without
                        // this, a goroutine can see the new code pointer but
                        // still read 0 from code[0], causing br x0 → PC=0x0.
                        __atomic_store_n(last_block->jump_ip[i], (unsigned long) block->code, __ATOMIC_RELEASE);
                        list_add(&block->jumps_from[i], &last_block->jumps_from_links[i]);
                    }
                }
            }

            unlock(&jit->lock);
        }
        
        frame->last_block = block;
        last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

        // block may be jetsam, but that's ok, because it can't be freed until
        // every thread on this jit is not executing anything

        // Defensive: a block with a null code[0] would crash the goroutine via
        // gret's `br x0` (PC=0x0) and, because the dying thread holds jetsam_lock
        // read, leave write-lock waiters permanently stuck. Detect and discard it
        // here; recompilation on the next iteration will produce a valid block.
        if (__atomic_load_n(&block->code[0], __ATOMIC_RELAXED) == 0) {
            printk("WARNING: JIT block %08x pid %d has null code[0]; invalidating\n",
                   block->addr, current ? current->pid : -1);
            lock(&jit->lock, 0);
            if (!block->is_jetsam) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
            cache[cache_index] = NULL;
            frame->last_block = NULL;
            unlock(&jit->lock);
            continue;
        }

        TRACE("%d %08x --- cycle %ld\n", current_pid(current), ip, frame->cpu.cycle);

        bool force_block_boundary_break = current != NULL && current->force_no_jit_cache;
        // before_block_cpu is consumed only by the cc1 amd64 JIT trace, which is
        // off unless the guest is amd64 'cc1'. Copying the whole cpu_state every
        // block otherwise is pure overhead on the hottest loop in the emulator,
        // so take the snapshot only when the trace is actually enabled.
        bool cc1_trace = amd64_frontend_debug_active() && amd64_cc1_jit_trace_enabled();
        struct cpu_state before_block_cpu;
        if (cc1_trace)
            before_block_cpu = frame->cpu;
        if (force_block_boundary_break)
            __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
        interrupt = jit_enter(block, frame, tlb);
        // Use load (not exchange) so we don't clear write_wanted — only the
        // write-lock holder should clear it after jetsam cleanup completes.
        if (interrupt == INT_NONE && jit_should_yield(jit, cpu))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++frame->cpu.cycle % (1 << 10) == 0)
            interrupt = INT_TIMER;
        if (cc1_trace)
            amd64_cc1_jit_trace_record(block->addr, tlb, &before_block_cpu, &frame->cpu, interrupt);
        *cpu = frame->cpu;
        if (current != NULL && current->force_no_jit_cache) {
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            if (force_block_boundary_break && interrupt == INT_TIMER)
                interrupt = INT_NONE;
        }
        if (interrupt == INT_GPF && current != NULL && current->abi != GUEST_ABI_AMD64) {
            bool retryable = jit_i386_gpf_looks_retryable(cpu);
            if (!retryable)
                goto no_jit_retry;
            bool same_retry = cpu->eip == last_retry_eip &&
                    cpu->segfault_addr == last_retry_addr &&
                    cpu->segfault_was_write == last_retry_write;
            if (!same_retry) {
                last_retry_eip = cpu->eip;
                last_retry_addr = cpu->segfault_addr;
                last_retry_write = cpu->segfault_was_write;
                last_retry_count = 0;
            }
            if (last_retry_count < 1) {
                last_retry_count++;
                lock(&jit->lock, 0);
                if (!block->is_jetsam) {
                    jit_block_disconnect(jit, block);
                    block->is_jetsam = true;
                    list_add(&jit->jetsam, &block->jetsam);
                }
                cache[cache_index] = NULL;
                frame->last_block = NULL;
                memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                unlock(&jit->lock);
                tlb_flush(tlb);
                interrupt = INT_NONE;
                continue;
            }
        }
no_jit_retry:
        ;
    }

    // Release jetsam_lock before freeing: with debug malloc scribbling, free()
    // is O(size) and would hold the lock unnecessarily long.
    jit_crash_lock = NULL;  // Disable crash recovery before unlocking (avoids
                            // double-unlock if EXC_BAD_ACCESS fires during unlock)
    pthread_rwlock_unlock(&jit->jetsam_lock.l);
done_unlocked:
    jit_crash_unwind_active = false;
    jit_crash_frame = NULL;
    jit_crash_cpu = NULL;
    return interrupt;

}

// AArch64 guest JIT main loop (aarch64_guest_plan.md's JIT gadget port).
// Mirrors cpu_step_to_interrupt (i386) closely -- same crash-recovery/
// jetsam-lock/block-cache/block-chaining machinery, which is guest-arch-
// agnostic reuse (block/jit structures don't know or care which guest
// architecture compiled them) -- with two deliberate simplifications for
// this first slice:
//   - ip tracking uses frame->cpu.arm64_pc, not eip.
//   - i386's INT_GPF retry-on-fault dance (jit_i386_gpf_looks_retryable)
//     is omitted; that logic exists for an i386-JIT-specific correctness
//     workaround that hasn't been shown to apply here. If real testing
//     surfaces an equivalent class of transient fault for arm64 gadgets,
//     add the analogous retry then, informed by what's actually observed
//     -- not speculatively now.
static int cpu_step_to_interrupt_arm64(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit *jit = cpu->mmu->jit;

    static __thread bool exception_handler_installed = false;
    if (!exception_handler_installed) {
        jit_install_thread_exception_handler();
        // When the Mach handler isn't active (standalone CLI), a host SIGBUS on
        // a truncated file-backed guest mmap would otherwise kill the process.
        // Install a POSIX handler that translates it into a guest SIGBUS.
        if (!jit_host_fault_mach_active())
            jit_install_host_fault_signal_handler();
        exception_handler_installed = true;
    }

    struct jit_block *cache[JIT_CACHE_SIZE] = {};
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    frame->cpu = *cpu;
    assert(jit->mmu == cpu->mmu);

    jit_crash_frame = frame;
    jit_crash_cpu = cpu;
    jit_crash_interrupt = INT_GPF;
    jit_crash_addr = frame->cpu.arm64_pc;
    if (sigsetjmp(jit_crash_unwind_buf, 0) != 0) {
        if (jit_crash_cpu != NULL && jit_crash_frame != NULL)
            *jit_crash_cpu = jit_crash_frame->cpu;
        cpu->segfault_addr = jit_crash_addr;
        cpu->segfault_was_write = false;
        jit_crash_unwind_active = false;
        jit_crash_mutex_lock = NULL;
        jit_crash_frame = NULL;
        jit_crash_cpu = NULL;
        return jit_crash_interrupt;
    }
    jit_crash_unwind_active = true;

    pthread_rwlock_rdlock(&jit->jetsam_lock.l);
    jit_crash_lock = &jit->jetsam_lock;

    unsigned last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

    int interrupt = INT_NONE;
    while (interrupt == INT_NONE) {
        if (tlb->mem_changes != cpu->mmu->changes)
            tlb_refresh(tlb, cpu->mmu);

        if (jit_should_yield(jit, cpu)) {
            interrupt = INT_TIMER;
            break;
        }
        guest_addr_t ip = frame->cpu.arm64_pc;
        size_t cache_index = jit_cache_hash(ip);
        struct jit_block *block = cache[cache_index];
        if (block == NULL || block->addr != ip) {
            lock(&jit->lock, 0);
            block = jit_lookup(jit, ip);
            if (block == NULL) {
                unlock(&jit->lock);
                jit_crash_lock = NULL;
                pthread_rwlock_unlock(&jit->jetsam_lock.l);

                if (jit_should_yield(jit, cpu)) {
                    interrupt = INT_TIMER;
                    goto done_unlocked_arm64;
                }

                block = jit_block_compile_arm64(ip, tlb);
                if (block == NULL) {
                    __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                    if (jetsam_write_lock_timed(jit)) {
                        jit_invalidate_all(jit);
                        lock(&jit->lock, 0);
                        jit_free_jetsam(jit);
                        unlock(&jit->lock);
                        atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                        pthread_rwlock_unlock(&jit->jetsam_lock.l);
                        memset(cache, 0, sizeof(cache));
                        memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                        frame->last_block = NULL;
                    }
                    __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                    if (jit_should_yield(jit, cpu)) {
                        interrupt = INT_TIMER;
                        goto done_unlocked_arm64;
                    }
                    block = jit_block_compile_arm64(ip, tlb);
                    if (block == NULL) {
                        printk("JIT OOM at %#llx pid %d: even after full flush, killing task\n",
                               (unsigned long long) ip, current ? current->pid : -1);
                        jit_crash_unwind_active = false;
                        jit_crash_frame = NULL;
                        jit_crash_cpu = NULL;
                        jit_crash_lock = NULL;
                        return INT_GPF;
                    }
                }

                pthread_rwlock_rdlock(&jit->jetsam_lock.l);
                jit_crash_lock = &jit->jetsam_lock;
                if (jit_should_yield(jit, cpu)) {
                    jit_block_free(NULL, block);
                    interrupt = INT_TIMER;
                    break;
                }

                lock(&jit->lock, 0);
                struct jit_block *existing = jit_lookup(jit, ip);
                if (existing != NULL) {
                    jit_block_free(NULL, block);
                    block = existing;
                } else {
                    jit_insert(jit, block);
                }
            } else {
                TRACE("%d %08llx --- missed cache (arm64)\n", current_pid(current), (unsigned long long) ip);
            }
            cache[cache_index] = block;
            unlock(&jit->lock);
        }
        struct jit_block *last_block = frame->last_block;
        if (atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed) != last_block_cleanup_seq) {
            last_block = frame->last_block = NULL;
            memset(cache, 0, sizeof(cache));
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
        }
        if (last_block != NULL &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
            lock(&jit->lock, 0);
            if (!last_block->is_jetsam && !block->is_jetsam) {
                for (int i = 0; i <= 1; i++) {
                    // Unchained arm64 targets carry bit 63 (control.S's
                    // tagged-word scheme); a patched word is a host code
                    // pointer with bit 63 clear. Requiring the tag makes
                    // re-checking an already-patched slot a no-op instead
                    // of a (low-probability) false re-patch.
                    if (last_block->jump_ip[i] != NULL &&
                            (*last_block->jump_ip[i] >> 63) != 0 &&
                            (*last_block->jump_ip[i] & 0xffffffffffffULL) == block->addr) {
                        // Backward edges (loops) ARE linked here, unlike the
                        // i386/amd64 loops: arm64_chain enforces a per-entry
                        // chain budget (jit_frame.chain_budget), so a chained
                        // loop returns to C every few thousand dispatches and
                        // cannot starve jetsam writers or the cycle counter.
                        __atomic_store_n(last_block->jump_ip[i], (unsigned long) block->code, __ATOMIC_RELEASE);
                        list_add(&block->jumps_from[i], &last_block->jumps_from_links[i]);
                    }
                }
            }
            unlock(&jit->lock);
        }

        frame->last_block = block;
        last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

        if (__atomic_load_n(&block->code[0], __ATOMIC_RELAXED) == 0) {
            printk("WARNING: JIT block %08llx pid %d has null code[0]; invalidating\n",
                   (unsigned long long) block->addr, current ? current->pid : -1);
            lock(&jit->lock, 0);
            if (!block->is_jetsam) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
            cache[cache_index] = NULL;
            frame->last_block = NULL;
            unlock(&jit->lock);
            continue;
        }

        TRACE("%d %08llx --- cycle %ld (arm64)\n", current_pid(current), (unsigned long long) ip, frame->cpu.cycle);
        bool force_block_boundary_break = current != NULL && current->force_no_jit_cache;
        if (force_block_boundary_break)
            __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
        frame->chain_budget = 8192; // see jit_frame.chain_budget
        interrupt = jit_enter(block, frame, tlb);
        if (interrupt == INT_NONE && jit_should_yield(jit, cpu))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++frame->cpu.cycle % (1 << 10) == 0)
            interrupt = INT_TIMER;
        *cpu = frame->cpu;
        if (current != NULL && current->force_no_jit_cache) {
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            if (force_block_boundary_break && interrupt == INT_TIMER)
                interrupt = INT_NONE;
        }
    }

    jit_crash_lock = NULL;
    pthread_rwlock_unlock(&jit->jetsam_lock.l);
done_unlocked_arm64:
    jit_crash_unwind_active = false;
    jit_crash_frame = NULL;
    jit_crash_cpu = NULL;
    return interrupt;
}

// riscv64 frontend: a mechanical clone of cpu_step_to_interrupt_arm64
// above with the pc field and compile function swapped — same jetsam,
// crash-unwind, cache, and chaining behavior. Kept separate like the
// amd64 frontend rather than parameterized; if a third copy ever
// appears, factor the loop.
static int cpu_step_to_interrupt_riscv64(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit *jit = cpu->mmu->jit;

    static __thread bool exception_handler_installed = false;
    if (!exception_handler_installed) {
        jit_install_thread_exception_handler();
        // When the Mach handler isn't active (standalone CLI), a host SIGBUS on
        // a truncated file-backed guest mmap would otherwise kill the process.
        // Install a POSIX handler that translates it into a guest SIGBUS.
        if (!jit_host_fault_mach_active())
            jit_install_host_fault_signal_handler();
        exception_handler_installed = true;
    }

    struct jit_block *cache[JIT_CACHE_SIZE] = {};
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    frame->cpu = *cpu;
    assert(jit->mmu == cpu->mmu);

    jit_crash_frame = frame;
    jit_crash_cpu = cpu;
    jit_crash_interrupt = INT_GPF;
    jit_crash_addr = frame->cpu.riscv64_pc;
    if (sigsetjmp(jit_crash_unwind_buf, 0) != 0) {
        if (jit_crash_cpu != NULL && jit_crash_frame != NULL)
            *jit_crash_cpu = jit_crash_frame->cpu;
        cpu->segfault_addr = jit_crash_addr;
        cpu->segfault_was_write = false;
        jit_crash_unwind_active = false;
        jit_crash_mutex_lock = NULL;
        jit_crash_frame = NULL;
        jit_crash_cpu = NULL;
        return jit_crash_interrupt;
    }
    jit_crash_unwind_active = true;

    pthread_rwlock_rdlock(&jit->jetsam_lock.l);
    jit_crash_lock = &jit->jetsam_lock;

    unsigned last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

    int interrupt = INT_NONE;
    while (interrupt == INT_NONE) {
        if (tlb->mem_changes != cpu->mmu->changes)
            tlb_refresh(tlb, cpu->mmu);

        if (jit_should_yield(jit, cpu)) {
            interrupt = INT_TIMER;
            break;
        }
        guest_addr_t ip = frame->cpu.riscv64_pc;
        size_t cache_index = jit_cache_hash(ip);
        struct jit_block *block = cache[cache_index];
        if (block == NULL || block->addr != ip) {
            lock(&jit->lock, 0);
            block = jit_lookup(jit, ip);
            if (block == NULL) {
                unlock(&jit->lock);
                jit_crash_lock = NULL;
                pthread_rwlock_unlock(&jit->jetsam_lock.l);

                if (jit_should_yield(jit, cpu)) {
                    interrupt = INT_TIMER;
                    goto done_unlocked_riscv64;
                }

                block = jit_block_compile_riscv64(ip, tlb);
                if (block == NULL) {
                    __atomic_store_n(&jit->write_wanted, 1, __ATOMIC_SEQ_CST);
                    if (jetsam_write_lock_timed(jit)) {
                        jit_invalidate_all(jit);
                        lock(&jit->lock, 0);
                        jit_free_jetsam(jit);
                        unlock(&jit->lock);
                        atomic_fetch_add_explicit(&jit->cleanup_seq, 1, memory_order_relaxed);
                        pthread_rwlock_unlock(&jit->jetsam_lock.l);
                        memset(cache, 0, sizeof(cache));
                        memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                        frame->last_block = NULL;
                    }
                    __atomic_store_n(&jit->write_wanted, 0, __ATOMIC_SEQ_CST);

                    if (jit_should_yield(jit, cpu)) {
                        interrupt = INT_TIMER;
                        goto done_unlocked_riscv64;
                    }
                    block = jit_block_compile_riscv64(ip, tlb);
                    if (block == NULL) {
                        printk("JIT OOM at %#llx pid %d: even after full flush, killing task\n",
                               (unsigned long long) ip, current ? current->pid : -1);
                        jit_crash_unwind_active = false;
                        jit_crash_frame = NULL;
                        jit_crash_cpu = NULL;
                        jit_crash_lock = NULL;
                        return INT_GPF;
                    }
                }

                pthread_rwlock_rdlock(&jit->jetsam_lock.l);
                jit_crash_lock = &jit->jetsam_lock;
                if (jit_should_yield(jit, cpu)) {
                    jit_block_free(NULL, block);
                    interrupt = INT_TIMER;
                    break;
                }

                lock(&jit->lock, 0);
                struct jit_block *existing = jit_lookup(jit, ip);
                if (existing != NULL) {
                    jit_block_free(NULL, block);
                    block = existing;
                } else {
                    jit_insert(jit, block);
                }
            } else {
                TRACE("%d %08llx --- missed cache (riscv64)\n", current_pid(current), (unsigned long long) ip);
            }
            cache[cache_index] = block;
            unlock(&jit->lock);
        }
        struct jit_block *last_block = frame->last_block;
        if (atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed) != last_block_cleanup_seq) {
            last_block = frame->last_block = NULL;
            memset(cache, 0, sizeof(cache));
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
        }
        if (last_block != NULL &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
            lock(&jit->lock, 0);
            if (!last_block->is_jetsam && !block->is_jetsam) {
                for (int i = 0; i <= 1; i++) {
                    // Unchained arm64 targets carry bit 63 (control.S's
                    // tagged-word scheme); a patched word is a host code
                    // pointer with bit 63 clear. Requiring the tag makes
                    // re-checking an already-patched slot a no-op instead
                    // of a (low-probability) false re-patch.
                    if (last_block->jump_ip[i] != NULL &&
                            (*last_block->jump_ip[i] >> 63) != 0 &&
                            (*last_block->jump_ip[i] & 0xffffffffffffULL) == block->addr) {
                        // Backward edges (loops) ARE linked here, unlike the
                        // i386/amd64 loops: arm64_chain enforces a per-entry
                        // chain budget (jit_frame.chain_budget), so a chained
                        // loop returns to C every few thousand dispatches and
                        // cannot starve jetsam writers or the cycle counter.
                        __atomic_store_n(last_block->jump_ip[i], (unsigned long) block->code, __ATOMIC_RELEASE);
                        list_add(&block->jumps_from[i], &last_block->jumps_from_links[i]);
                    }
                }
            }
            unlock(&jit->lock);
        }

        frame->last_block = block;
        last_block_cleanup_seq = atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);

        if (__atomic_load_n(&block->code[0], __ATOMIC_RELAXED) == 0) {
            printk("WARNING: JIT block %08llx pid %d has null code[0]; invalidating\n",
                   (unsigned long long) block->addr, current ? current->pid : -1);
            lock(&jit->lock, 0);
            if (!block->is_jetsam) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
            cache[cache_index] = NULL;
            frame->last_block = NULL;
            unlock(&jit->lock);
            continue;
        }

        TRACE("%d %08llx --- cycle %ld (arm64)\n", current_pid(current), (unsigned long long) ip, frame->cpu.cycle);
        bool force_block_boundary_break = current != NULL && current->force_no_jit_cache;
        if (force_block_boundary_break)
            __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
        frame->chain_budget = 8192; // see jit_frame.chain_budget
        interrupt = jit_enter(block, frame, tlb);
        if (interrupt == INT_NONE && jit_should_yield(jit, cpu))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++frame->cpu.cycle % (1 << 10) == 0)
            interrupt = INT_TIMER;
        *cpu = frame->cpu;
        if (current != NULL && current->force_no_jit_cache) {
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            if (force_block_boundary_break && interrupt == INT_TIMER)
                interrupt = INT_NONE;
        }
    }

    jit_crash_lock = NULL;
    pthread_rwlock_unlock(&jit->jetsam_lock.l);
done_unlocked_riscv64:
    jit_crash_unwind_active = false;
    jit_crash_frame = NULL;
    jit_crash_cpu = NULL;
    return interrupt;
}

static int cpu_single_step(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    if (!gen_start(cpu->eip, &state))
        return INT_GPF;
    gen_step(&state, tlb);
    gen_exit(&state);
    gen_end(&state);

    struct jit_block *block = state.block;
    struct jit_frame frame = {.cpu = *cpu};
    int interrupt = jit_enter(block, &frame, tlb);
    // *cpu is still the pre-block state here (committed below), so pass it
    // directly as the trace's "before" instead of snapshotting the cpu_state.
    amd64_cc1_jit_trace_record(block->addr, tlb, cpu, &frame.cpu, interrupt);
    *cpu = frame.cpu;
    jit_block_free(NULL, block);
    if (interrupt == INT_NONE)
        interrupt = INT_DEBUG;
    return interrupt;
}

static int cpu_single_step_no_debug(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    if (!gen_start(cpu->eip, &state))
        return INT_GPF;
    gen_step(&state, tlb);
    gen_exit(&state);
    gen_end(&state);

    struct jit_block *block = state.block;
    struct jit_frame frame = {.cpu = *cpu};
    int interrupt = jit_enter(block, &frame, tlb);
    // *cpu is still the pre-block state here (committed below), so pass it
    // directly as the trace's "before" instead of snapshotting the cpu_state.
    amd64_cc1_jit_trace_record(block->addr, tlb, cpu, &frame.cpu, interrupt);
    *cpu = frame.cpu;
    jit_block_free(NULL, block);
    return interrupt;
}

static int cpu_step_to_interrupt_amd64_frontend(struct cpu_state *cpu, struct tlb *tlb) {
    struct jit *jit = cpu->mmu->jit;
    enum { AMD64_FRONTEND_TIMER_BLOCK_QUANTUM = 1024 };
    struct jit_block *cache[JIT_CACHE_SIZE] = {};
    struct jit_frame frame_storage = {};
    struct jit_frame *frame = &frame_storage;
    int interrupt;
    bool fallback_to_interp;
    guest_addr_t ip;
    struct jit_block *block;
    unsigned blocks_executed = 0;
    int ret;

    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);
    frame->cpu = *cpu;

    if (cpu_take_poke(cpu))
        return INT_TIMER;

    static __thread bool exception_handler_installed = false;
    if (!exception_handler_installed) {
        jit_install_thread_exception_handler();
        // When the Mach handler isn't active (standalone CLI), a host SIGBUS on
        // a truncated file-backed guest mmap would otherwise kill the process.
        // Install a POSIX handler that translates it into a guest SIGBUS.
        if (!jit_host_fault_mach_active())
            jit_install_host_fault_signal_handler();
        exception_handler_installed = true;
    }

    jit_crash_frame = frame;
    jit_crash_cpu = cpu;
    jit_crash_interrupt = INT_GPF;
    jit_crash_addr = frame->cpu.amd64_rip;
    // savesigs=0: see the i386 cpu_step_to_interrupt site — crash recovery arrives from
    // the Mach exception handler, which leaves the host signal mask untouched, so there
    // is no mask to save or restore here.
    if (sigsetjmp(jit_crash_unwind_buf, 0) != 0) {
        if (jit_crash_cpu != NULL && jit_crash_frame != NULL)
            *jit_crash_cpu = jit_crash_frame->cpu;
        if (current != NULL && strcmp(current->comm, "apk") == 0) {
            printk("[amd64-jit] crash unwind apk crash_addr=%#llx frame-rip=%#llx cpu-rip=%#llx eip=%#x rsp=%#llx int=%d\n",
                   (unsigned long long) jit_crash_addr,
                   jit_crash_frame != NULL ? (unsigned long long) jit_crash_frame->cpu.amd64_rip : 0,
                   jit_crash_cpu != NULL ? (unsigned long long) jit_crash_cpu->amd64_rip : 0,
                   jit_crash_cpu != NULL ? jit_crash_cpu->eip : 0,
                   jit_crash_cpu != NULL ? (unsigned long long) jit_crash_cpu->amd64_regs[amd64_rsp] : 0,
                   jit_crash_interrupt);
        }
        cpu->segfault_addr = jit_crash_addr;
        cpu->segfault_was_write = false;
        jit_crash_unwind_active = false;
        jit_crash_frame = NULL;
        jit_crash_cpu = NULL;
        return jit_crash_interrupt;
    }
    jit_crash_unwind_active = true;

    pthread_rwlock_rdlock(&jit->jetsam_lock.l);
    jit_crash_lock = &jit->jetsam_lock;
    // This frontend drops jetsam_lock around block compilation (below). While it
    // is dropped, another thread can take the write lock and run jit_free_jetsam(),
    // freeing blocks still referenced by our per-call cache[] and the assembly
    // ret_cache. Track cleanup_seq (bumped under the write lock after each free)
    // so we can purge those dangling pointers before dereferencing them. The i386
    // cpu_step_to_interrupt has the identical guard; the amd64 frontend was missing
    // it, which let a stale cache[] entry reach jit_block_disconnect() on freed
    // memory and fault (EXC_BAD_ACCESS in jit_block_disconnect under SMP load).
    unsigned last_block_cleanup_seq =
        atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
    while (true) {
        if (tlb->mem_changes != cpu->mmu->changes)
            tlb_refresh(tlb, cpu->mmu);

        fallback_to_interp = false;
        ip = frame->cpu.amd64_rip;
        if (unlikely(amd64_frontend_debug_active()) && amd64_cc1_force_interp_block(ip)) {
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            *cpu = frame->cpu;
            // If an explicit RIP range is set (ISH_AMD64_FORCE_INTERP_LO/_HI), run
            // the interpreter ONLY while rip stays in the range, then hand back to
            // the JIT. A tight range isolates a single instruction, so a bisection
            // pins the exact faulty gadget instead of the whole interp-until-syscall
            // stretch. (debug path; single-threaded use.) Otherwise (cc1 ranges /
            // "all") keep the original interp-until-interrupt behavior.
            if (getenv("ISH_AMD64_FORCE_INTERP_HI") != NULL) {
                bool saved_tf = cpu->tf;
                int rc = INT_DEBUG;
                while (amd64_cc1_force_interp_block(cpu->amd64_rip)) {
                    cpu->tf = 1;
                    rc = cpu_run_to_interrupt_amd64(cpu, tlb);
                    if (rc != INT_DEBUG)
                        break;
                }
                cpu->tf = saved_tf;
                frame->cpu = *cpu;
                frame->cpu.eip = (dword_t) frame->cpu.amd64_rip;
                if (rc == INT_DEBUG)
                    continue; // rip left the range; resume the JIT
                // a real interrupt fired mid-range; return it
                jit_crash_lock = NULL;
                jit_crash_mutex_lock = NULL;
                pthread_rwlock_unlock(&jit->jetsam_lock.l);
                jit_crash_unwind_active = false;
                jit_crash_frame = NULL;
                jit_crash_cpu = NULL;
                *cpu = frame->cpu;
                return rc;
            }
            jit_crash_lock = NULL;
            jit_crash_mutex_lock = NULL;
            pthread_rwlock_unlock(&jit->jetsam_lock.l);
            jit_crash_unwind_active = false;
            jit_crash_frame = NULL;
            jit_crash_cpu = NULL;
            *cpu = frame->cpu;
            return cpu_run_to_interrupt_amd64(cpu, tlb);
        }
        size_t cache_index = jit_cache_hash(ip);
        block = cache[cache_index];
        amd64_jit_debug("frontend enter ip=%llx comm=%s abi=%d",
                (unsigned long long) ip,
                current != NULL ? current->comm : "(null)",
                current != NULL ? current->abi : -1);
        if (block == NULL || block->addr != ip) {
            jit_crash_track_mutex_lock(&jit->lock);
            block = jit_lookup(jit, ip);
            if (block == NULL) {
                jit_crash_track_mutex_unlock(&jit->lock);
                jit_crash_lock = NULL;
                frame->last_block = NULL;
                memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                pthread_rwlock_unlock(&jit->jetsam_lock.l);

                block = jit_block_compile_amd64(ip, tlb, &fallback_to_interp);
                if (block == NULL) {
                    amd64_jit_debug("frontend no-block ip=%llx fallback=%d",
                            (unsigned long long) ip, fallback_to_interp);
                    *cpu = frame->cpu;
                    jit_crash_unwind_active = false;
                    jit_crash_frame = NULL;
                    jit_crash_cpu = NULL;
                    if (fallback_to_interp)
                        return cpu_run_to_interrupt_amd64(cpu, tlb);
                    return INT_GPF;
                }

                pthread_rwlock_rdlock(&jit->jetsam_lock.l);
                jit_crash_lock = &jit->jetsam_lock;
                jit_crash_track_mutex_lock(&jit->lock);
                struct jit_block *existing = jit_lookup(jit, ip);
                if (existing != NULL) {
                    jit_block_free(NULL, block);
                    block = existing;
                } else {
                    jit_insert(jit, block);
                }
            }
            cache[cache_index] = block;
            jit_crash_track_mutex_unlock(&jit->lock);
        }
        // If a jetsam cleanup freed blocks while we had jetsam_lock dropped for
        // compilation, our cache[] and ret_cache may now hold pointers into freed
        // (and possibly reused) block memory. Purge them before they are read by
        // the null-code check below, by jit_enter()'s ret prediction, or by the
        // next iteration's cache lookup. The local `block` stays valid: jit_lookup
        // never returns jetsam blocks and a freshly compiled block is not jetsam,
        // and we hold jetsam_lock continuously from here through jit_enter().
        if (atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed) !=
                last_block_cleanup_seq) {
            memset(cache, 0, sizeof(cache));
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            frame->last_block = NULL;
            last_block_cleanup_seq =
                atomic_load_explicit(&jit->cleanup_seq, memory_order_relaxed);
        }
        if (block->used == 0 || __atomic_load_n(&block->code[0], __ATOMIC_RELAXED) == 0) {
            printk("[amd64-jit] frontend null-code comm=%s pid=%d block=%#llx end=%#llx used=%zu\n",
                   current != NULL ? current->comm : "?",
                   current != NULL ? current->pid : -1,
                   (unsigned long long) block->addr,
                   (unsigned long long) block->end_addr,
                   block->used);
            jit_crash_track_mutex_lock(&jit->lock);
            if (!block->is_jetsam) {
                jit_block_disconnect(jit, block);
                block->is_jetsam = true;
                list_add(&jit->jetsam, &block->jetsam);
            }
            cache[cache_index] = NULL;
            jit_crash_track_mutex_unlock(&jit->lock);
            frame->last_block = NULL;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            *cpu = frame->cpu;
            ret = cpu_run_to_interrupt_amd64(cpu, tlb);
            break;
        }
        amd64_jit_debug("frontend exec block ip=%llx end=%llx",
                (unsigned long long) ip,
                (unsigned long long) block->end_addr);
        amd64_jit_debug("frontend block slots 0=%lx 1=%lx 2=%lx 3=%lx helper=%lx",
                block->used > 0 ? block->code[0] : 0,
                block->used > 1 ? block->code[1] : 0,
                block->used > 2 ? block->code[2] : 0,
                block->used > 3 ? block->code[3] : 0,
                (unsigned long) amd64_step_to_interrupt_jit);
        {
            // Gate on the debug master switch like the i386 frontend
            // (3558ee46): amd64_cc1_jit_trace_enabled is an out-of-line
            // call in the per-block hot loop and showed up at ~2% in a
            // gzip profile.
            bool cc1_trace = amd64_frontend_debug_active() && amd64_cc1_jit_trace_enabled();
            struct cpu_state before_block_cpu;
            if (cc1_trace)
                before_block_cpu = frame->cpu;
        amd64_jit_bridge_set_tlb(tlb);
        interrupt = jit_enter(block, frame, tlb);
        amd64_jit_bridge_set_tlb(NULL);
            if (cc1_trace)
                amd64_cc1_jit_trace_record(block->addr, tlb, &before_block_cpu, &frame->cpu, interrupt);
        }
        if (ip != 0 && frame->cpu.amd64_rip == 0) {
            printk("[amd64-jit] frontend zero-rip comm=%s pid=%d block=%#llx end=%#llx rsp=%#llx int=%d slots=%lx %lx %lx %lx\n",
                   current != NULL ? current->comm : "?",
                   current != NULL ? current->pid : -1,
                   (unsigned long long) block->addr,
                   (unsigned long long) block->end_addr,
                   (unsigned long long) frame->cpu.amd64_regs[amd64_rsp],
                   interrupt,
                   block->used > 0 ? block->code[0] : 0,
                   block->used > 1 ? block->code[1] : 0,
                   block->used > 2 ? block->code[2] : 0,
                   block->used > 3 ? block->code[3] : 0);
        }
        amd64_jit_debug("frontend post block rip=%llx rsp=%llx trap=%x int=%d",
                (unsigned long long) frame->cpu.amd64_rip,
                (unsigned long long) frame->cpu.amd64_regs[amd64_rsp],
                frame->cpu.trapno,
                interrupt);
        frame->cpu.eip = (dword_t) frame->cpu.amd64_rip;
        frame->last_block = NULL;
        if (interrupt != INT_NONE) {
            *cpu = frame->cpu;
            ret = interrupt;
            break;
        }
        if (cpu_take_poke(cpu)) {
            *cpu = frame->cpu;
            ret = INT_TIMER;
            break;
        }
        if (++blocks_executed >= AMD64_FRONTEND_TIMER_BLOCK_QUANTUM) {
            *cpu = frame->cpu;
            ret = INT_TIMER;
            break;
        }
    }
    jit_crash_lock = NULL;
    jit_crash_mutex_lock = NULL;
    pthread_rwlock_unlock(&jit->jetsam_lock.l);
    jit_crash_unwind_active = false;
    jit_crash_frame = NULL;
    jit_crash_cpu = NULL;
    return ret;
}

static int cpu_single_step_amd64_frontend(struct cpu_state *cpu, struct tlb *tlb) {
    // Keep trap-flag semantics on the interpreter until amd64 JIT single-step
    // has an exact one-instruction translation path.
    return cpu_run_to_interrupt_amd64(cpu, tlb);
}

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    // JIT gadget path (jit/guest-arm64/), per aarch64_guest_plan.md's
    // direction change — no interpreter fallback, matching i386's own
    // precedent. cpu_run_to_interrupt_arm64 (emu/arm64_interp.c) still
    // exists and still passes its tests but is no longer called here.
    if (current != NULL && current->abi == GUEST_ABI_ARM64) {
        // Real bug caught by testing: cpu_take_poke()/jit_should_yield()
        // dereference cpu->poked_ptr unconditionally. The i386/amd64 paths
        // set it just before their own dispatch (see below); this early
        // return skipped that assignment entirely, so the very first
        // jit_should_yield() call dereferenced NULL. Same fix, same reason.
        cpu->poked_ptr = &cpu->_poked;
        return cpu_step_to_interrupt_arm64(cpu, tlb);
    }
    if (current != NULL && current->abi == GUEST_ABI_RISCV64) {
        // Same poked_ptr rule as arm64 above.
        cpu->poked_ptr = &cpu->_poked;
        return cpu_step_to_interrupt_riscv64(cpu, tlb);
    }
    if (current != NULL && current->abi == GUEST_ABI_AMD64) {
        if (!amd64_jit_is_enabled())
            return cpu_run_to_interrupt_amd64(cpu, tlb);
        // GNU as stays on the amd64 interpreter: b71ce84d ("Contain amd64
        // assembler JIT crashes") added this to contain real gas crashes
        // under the amd64 JIT frontend that were never root-caused. Keep
        // the bypass until someone re-runs gas under the JIT (much has
        // changed since — see tests/manual/amd64_gas_probe.sh for the
        // probe harness) and either reproduces and fixes the crash or
        // shows it gone.
        if (current->comm[0] == 'a' && strcmp(current->comm, "as") == 0)
            return cpu_run_to_interrupt_amd64(cpu, tlb);
        return cpu->tf ? cpu_single_step_amd64_frontend(cpu, tlb)
                       : cpu_step_to_interrupt_amd64_frontend(cpu, tlb);
    }

    // Keep normal signal/timer pokes per-CPU. The JIT checks write_wanted
    // separately as a jetsam hint; sharing the same flag lets an ordinary poke
    // leave the JIT permanently in "yield now" mode.
    cpu->poked_ptr = &cpu->_poked;

    tlb_refresh(tlb, cpu->mmu);
    int interrupt;
    if (current != NULL && current->force_single_step) {
        interrupt = INT_NONE;
        int steps = 0;
        while (interrupt == INT_NONE) {
            interrupt = cpu_single_step_no_debug(cpu, tlb);
            if (interrupt == INT_NONE && cpu_take_poke(cpu))
                interrupt = INT_TIMER;
            if (interrupt == INT_NONE && ++steps >= 1024) {
                steps = 0;
                interrupt = INT_TIMER;
            }
        }
    } else {
        interrupt = (cpu->tf ? cpu_single_step : cpu_step_to_interrupt)(cpu, tlb); // Crashed here 26 Jul 2022, 27 Aug 2022. -mke
    }
    cpu->trapno = interrupt;

    return interrupt;
}

void jit_cleanup_jetsam_after_interrupt(struct cpu_state *cpu) {
    if (cpu == NULL || cpu->mmu == NULL || cpu->mmu->jit == NULL)
        return;
    jit_cleanup_jetsam_if_needed(cpu->mmu->jit);
}

void cpu_poke(struct cpu_state *cpu) {
    __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
}
