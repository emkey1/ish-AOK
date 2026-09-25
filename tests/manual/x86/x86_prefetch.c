// x86_prefetch -- the 0F 0D prefetch group runs as a no-op on both x86
// guests, at any address, and its register form is #UD.
//
// 0F 0D /1 is PREFETCHW. GCC emits it for __builtin_prefetch(p, 1) whenever
// PRFCHW is enabled, which is -march=broadwell and later and every znver, so
// binaries built for a modern target execute it. Both engines raised #UD for
// the whole group, and such a program died with "Illegal instruction" at its
// first write prefetch. Intel CPUs before Broadwell run 0F 0D as a NOP
// without advertising it, so a guest may execute it whatever CPUID says.
//
// What hardware does (camd, AMD Zen+, Linux 6.12, both -m64 and -m32):
//   - every /reg value with a memory operand is a prefetch: /0 PREFETCH,
//     /1 PREFETCHW, /2 PREFETCHWT1, and /3-/7 run as PREFETCH;
//   - none of them faults, whatever the address -- NULL, a PROT_NONE page and
//     a kernel address are all prefetched without a signal;
//   - 66, F2, F3 and REX prefixes change nothing;
//   - the register form (mod == 3) and a LOCK prefix are #UD.
//
// A prefetch has no visible effect, so the witness for "it executed" is the
// instruction LENGTH, as in cpuid_xsave's CLFLUSH probe: each form is followed
// by a MOV whose result is checked, and a decoder that sized the prefetch
// wrongly would run its displacement bytes as code and never reach that MOV.
// The straight-line probe puts several prefetches in one basic block between
// ADDs, so a JIT that dropped or mis-sized one inside a block shows it too.
// UD2 is the positive control for the SIGILL catcher.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("x86_prefetch: SKIP (x86 only)\n");
    return 0;
}
#else

#define WITNESS 0x5a5aul

static sigjmp_buf env;
static volatile int got_sig;
static volatile uintptr_t got_addr, got_pc;

static void on_sig(int sig, siginfo_t *info, void *ucv) {
    ucontext_t *uc = ucv;
    got_sig = sig;
    got_addr = (uintptr_t) info->si_addr;
#if defined(__x86_64__)
    got_pc = (uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
#else
    got_pc = (uintptr_t) uc->uc_mcontext.gregs[REG_EIP];
#endif
    siglongjmp(env, 1);
}

static char line[256] __attribute__((aligned(64)));
static void *volatile target;

// One instruction, then the MOV witness. The target address is in EDI/RDI
// (%1), and a zero index in ECX/RCX (%2).
#define PROBE(name, insn)                                                     \
    static unsigned long name(void) {                                         \
        unsigned long out = 0;                                                \
        __asm__ volatile(insn "\n\tmov $0x5a5a, %0"                           \
                         : "=r"(out) : "D"(target), "c"((uintptr_t) 0)        \
                         : "memory");                                         \
        return out;                                                           \
    }

// modrm 10 rrr 111: [EDI/RDI + disp32], for each value of the reg field.
PROBE(p_reg0, ".byte 0x0f, 0x0d, 0x87\n\t.long 0x40")
PROBE(p_reg1, ".byte 0x0f, 0x0d, 0x8f\n\t.long 0x40")
PROBE(p_reg2, ".byte 0x0f, 0x0d, 0x97\n\t.long 0x40")
PROBE(p_reg3, ".byte 0x0f, 0x0d, 0x9f\n\t.long 0x40")
PROBE(p_reg4, ".byte 0x0f, 0x0d, 0xa7\n\t.long 0x40")
PROBE(p_reg5, ".byte 0x0f, 0x0d, 0xaf\n\t.long 0x40")
PROBE(p_reg6, ".byte 0x0f, 0x0d, 0xb7\n\t.long 0x40")
PROBE(p_reg7, ".byte 0x0f, 0x0d, 0xbf\n\t.long 0x40")
// The mnemonics, and the other addressing forms a compiler produces.
PROBE(p_prefetch, "prefetch (%1)")
PROBE(p_prefetchw, "prefetchw (%1)")
PROBE(p_prefetchwt1, "prefetchwt1 (%1)")
PROBE(p_disp8, "prefetchw 0x10(%1)")
PROBE(p_sib, "prefetchw 0x40(%1,%2,8)")
// modrm 00 001 101: an absolute disp32 on i386, RIP-relative on amd64. Either
// way the address is never touched, so no relocation is needed.
PROBE(p_disp32_only, ".byte 0x0f, 0x0d, 0x0d\n\t.long 0x1000")
#if defined(__x86_64__)
PROBE(p_global, "prefetchw line(%%rip)")
PROBE(p_rexw, ".byte 0x48, 0x0f, 0x0d, 0x0f")
PROBE(p_rexr, ".byte 0x44, 0x0f, 0x0d, 0x0f")

// A base register only REX.B can name.
static unsigned long p_rexb(void) {
    unsigned long out = 0;
    __asm__ volatile("mov %1, %%r9\n\tprefetchw 8(%%r9)\n\tmov $0x5a5a, %0"
                     : "=r"(out) : "r"(target) : "r9", "memory");
    return out;
}
#endif
// Prefixes hardware ignores.
PROBE(p_66, ".byte 0x66, 0x0f, 0x0d, 0x0f")
PROBE(p_f2, ".byte 0xf2, 0x0f, 0x0d, 0x0f")
PROBE(p_f3, ".byte 0xf3, 0x0f, 0x0d, 0x0f")
// #UD: the register form (modrm 11 rrr 000) and LOCK.
PROBE(p_mod3_0, ".byte 0x0f, 0x0d, 0xc0")
PROBE(p_mod3_1, ".byte 0x0f, 0x0d, 0xc8")
PROBE(p_mod3_2, ".byte 0x0f, 0x0d, 0xd0")
PROBE(p_mod3_7, ".byte 0x0f, 0x0d, 0xf8")
PROBE(p_lock, ".byte 0xf0, 0x0f, 0x0d, 0x0f")
// Positive control: UD2 must be caught, or a pass above means nothing.
PROBE(p_ud2, "ud2")

// Three prefetches inside one basic block, between ADDs. 1 + 2 + 4 + 8.
static unsigned long p_block(void) {
    unsigned long out;
    __asm__ volatile("mov $1, %0\n\t"
                     "prefetchw (%1)\n\t"
                     "add $2, %0\n\t"
                     "prefetch 0x40(%1)\n\t"
                     "add $4, %0\n\t"
                     "prefetchwt1 -8(%1)\n\t"
                     "add $8, %0"
                     : "=&r"(out) : "D"(target) : "cc", "memory");
    return out == 15 ? WITNESS : out;
}

// What a compiler targeting PRFCHW makes of a write prefetch: PREFETCHW. The
// accesses are volatile because GCC 15 vectorizes the plain loop and drops the
// prefetch from it.
__attribute__((target("prfchw"), noinline))
static unsigned long builtin_write_prefetch(volatile unsigned long *a, size_t n) {
    unsigned long sum = 0;
    for (size_t i = 0; i < n; i++) {
        __builtin_prefetch((const void *) (uintptr_t) &a[i + 8], 1, 3);
        a[i] += i;
        sum += a[i];
    }
    return sum;
}

// Whether the compiler kept a PREFETCHW (0F 0D /1, memory form) in it. The scan
// stops at the first RET byte, so it can miss one but never reads a neighbour.
static int builtin_has_prefetchw(void) {
    const unsigned char *code = (const unsigned char *) (uintptr_t) builtin_write_prefetch;
    for (int i = 0; i < 512 && code[i] != 0xc3; i++)
        if (code[i] == 0x0f && code[i + 1] == 0x0d &&
                ((code[i + 2] >> 3) & 7) == 1 && (code[i + 2] >> 6) != 3)
            return 1;
    return 0;
}

static unsigned long p_builtin(void) {
    static unsigned long a[64 + 8];
    unsigned long want = 0;
    for (size_t i = 0; i < 64; i++) {
        a[i] = 3 * i;
        want += 4 * i;
    }
    // Enough calls for the loop to run from a translated, cached block.
    for (int round = 0; round < 200; round++) {
        unsigned long got = builtin_write_prefetch(a, 64);
        if (got != want)
            return got;
        for (size_t i = 0; i < 64; i++)
            a[i] -= i;
    }
    return WITNESS;
}

enum where { W_LINE, W_NULL, W_NONE, W_KERNEL };

struct probe {
    const char *name;
    unsigned long (*run)(void);
    enum where where;
    int want_sig;       // 0, or SIGILL
    int ud_len;         // bytes before the 0F of a #UD form (its prefixes)
};

static const struct probe probes[] = {
    { "0f 0d /0 disp32",        p_reg0,        W_LINE,   0, 0 },
    { "0f 0d /1 disp32",        p_reg1,        W_LINE,   0, 0 },
    { "0f 0d /2 disp32",        p_reg2,        W_LINE,   0, 0 },
    { "0f 0d /3 disp32",        p_reg3,        W_LINE,   0, 0 },
    { "0f 0d /4 disp32",        p_reg4,        W_LINE,   0, 0 },
    { "0f 0d /5 disp32",        p_reg5,        W_LINE,   0, 0 },
    { "0f 0d /6 disp32",        p_reg6,        W_LINE,   0, 0 },
    { "0f 0d /7 disp32",        p_reg7,        W_LINE,   0, 0 },
    { "prefetch",               p_prefetch,    W_LINE,   0, 0 },
    { "prefetchw",              p_prefetchw,   W_LINE,   0, 0 },
    { "prefetchwt1",            p_prefetchwt1, W_LINE,   0, 0 },
    { "prefetchw disp8",        p_disp8,       W_LINE,   0, 0 },
    { "prefetchw sib",          p_sib,         W_LINE,   0, 0 },
    { "prefetchw disp32 only",  p_disp32_only, W_LINE,   0, 0 },
#if defined(__x86_64__)
    { "prefetchw global(%rip)", p_global,      W_LINE,   0, 0 },
    { "prefetchw rex.b base",   p_rexb,        W_LINE,   0, 0 },
    { "rex.w prefetchw",        p_rexw,        W_LINE,   0, 0 },
    { "rex.r prefetchw",        p_rexr,        W_LINE,   0, 0 },
#endif
    { "66 prefetchw",           p_66,          W_LINE,   0, 0 },
    { "f2 prefetchw",           p_f2,          W_LINE,   0, 0 },
    { "f3 prefetchw",           p_f3,          W_LINE,   0, 0 },
    { "prefetchw NULL",         p_prefetchw,   W_NULL,   0, 0 },
    { "prefetchw PROT_NONE",    p_prefetchw,   W_NONE,   0, 0 },
    { "prefetch PROT_NONE",     p_reg0,        W_NONE,   0, 0 },
    { "prefetchw kernel",       p_prefetchw,   W_KERNEL, 0, 0 },
    { "three in one block",     p_block,       W_LINE,   0, 0 },
    { "__builtin_prefetch(p,1)", p_builtin,    W_LINE,   0, 0 },
    { "0f 0d /0 register",      p_mod3_0,      W_LINE,   SIGILL, 0 },
    { "0f 0d /1 register",      p_mod3_1,      W_LINE,   SIGILL, 0 },
    { "0f 0d /2 register",      p_mod3_2,      W_LINE,   SIGILL, 0 },
    { "0f 0d /7 register",      p_mod3_7,      W_LINE,   SIGILL, 0 },
    { "lock prefetchw",         p_lock,        W_LINE,   SIGILL, 1 },
    { "ud2 (control)",          p_ud2,         W_LINE,   SIGILL, 0 },
};

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    void *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (none == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    // The explicit forms above are the real test; this one only shows what a
    // compiled program meets, so say so when the compiler left it out.
    if (!builtin_has_prefetchw())
        printf("note: no PREFETCHW in builtin_write_prefetch; that case tests nothing\n");

    void *kernel = (void *) (sizeof(void *) == 8 ? (uintptr_t) 0xffffffff81000000ull
                                                 : (uintptr_t) 0xc1000000u);

    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        const struct probe *p = &probes[i];
        target = p->where == W_LINE ? (void *) line
               : p->where == W_NULL ? NULL
               : p->where == W_NONE ? none : kernel;
        got_sig = 0;
        got_addr = got_pc = 0;
        unsigned long out = 0;
        if (sigsetjmp(env, 1) == 0)
            out = p->run();
        test_logf("  %-24s sig=%-2d out=%#lx pc=%#lx addr=%#lx\n", p->name, got_sig,
                  out, (unsigned long) got_pc, (unsigned long) got_addr);

        if (p->want_sig == 0) {
            if (got_sig != 0 || out != WITNESS) {
                printf("FAIL %s: signal %d, witness %#lx (want no signal, %#lx)\n",
                       p->name, got_sig, out, WITNESS);
                failures_total++;
            }
            continue;
        }

        if (got_sig != p->want_sig) {
            printf("FAIL %s: signal %d (want %d)%s\n", p->name, got_sig, p->want_sig,
                   got_sig == 0 ? ", it ran" : "");
            failures_total++;
            continue;
        }
        // The fault is reported at the instruction, not past it or at its
        // block: si_addr and the saved PC both point at its first byte.
        const unsigned char *pc = (const unsigned char *) got_pc;
        const unsigned char *op = pc + p->ud_len;
        int at_insn = got_addr == got_pc && op[0] == 0x0f &&
                      (op[1] == 0x0d || (p->run == p_ud2 && op[1] == 0x0b)) &&
                      (p->ud_len == 0 || pc[0] == 0xf0);
        if (!at_insn) {
            printf("FAIL %s: SIGILL at pc %#lx si_addr %#lx, bytes %02x %02x %02x "
                   "(want both at the instruction)\n", p->name, (unsigned long) got_pc,
                   (unsigned long) got_addr, pc[0], pc[1], pc[2]);
            failures_total++;
        }
    }
    return finish_suite("x86_prefetch");
}
#endif
