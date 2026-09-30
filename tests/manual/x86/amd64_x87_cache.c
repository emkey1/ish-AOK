// The amd64 JIT runs a register-form x87 instruction (other than FNSTSW AX)
// through a gadget that keeps the guest register cache live across the C
// call instead of storing and reloading it (amd64_x87_reg_gadget in
// jit/gadgets-aarch64/math.S). Two things can go wrong with that, and this
// checks both:
//
//  - general registers written just before the x87 op, and still only in the
//    cache, must come out of it intact; and
//  - an undefined register form (d9 d1) raises SIGILL from inside that gadget,
//    so the signal frame must see those registers and the faulting rip,
//    which means the gadget has to store the cache and rip before leaving.
//
// The memory forms keep the cache too (amd64_x87_mem): the same two checks
// with fldl/fstpl addressed through a register, the fault from an fldl of an
// unmapped page.
//
// Positive control: FNSTSW AX (df e0) takes the flushing bridge and must
// still write ax.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static volatile int sigill_seen, sigsegv_seen;
static uint64_t sig_regs[6];
static uint64_t sig_rip;
extern char ud_insn[], pf_insn[];

static void on_sigill(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    greg_t *g = uc->uc_mcontext.gregs;
    sig_regs[0] = g[REG_RAX];
    sig_regs[1] = g[REG_RBX];
    sig_regs[2] = g[REG_RCX];
    sig_regs[3] = g[REG_RDX];
    sig_regs[4] = g[REG_RSI];
    sig_regs[5] = g[REG_RDI];
    sig_rip = g[REG_RIP];
    g[REG_RIP] += 2;
    sigill_seen++;
}

static void on_sigsegv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    greg_t *g = uc->uc_mcontext.gregs;
    sig_regs[0] = g[REG_RAX];
    sig_regs[1] = g[REG_RBX];
    sig_regs[2] = g[REG_RCX];
    sig_regs[3] = g[REG_RDX];
    sig_regs[4] = g[REG_RSI];
    sig_regs[5] = g[REG_RDI];
    sig_rip = g[REG_RIP];
    g[REG_RIP] += 2;   // fldl (%rdx): dd 02
    sigsegv_seen++;
}

int main(void) {
    // Registers written right before fadd, read right after.
    for (int i = 0; i < 2000; i++) {
        uint64_t out[6];
        double one = 1.0;
        __asm__ volatile(
            "fldl %[one]\n"
            "fldl %[one]\n"
            "mov %[k], %%rax\n"
            "lea 1(%%rax), %%rbx\n"
            "lea 2(%%rax), %%rcx\n"
            "lea 3(%%rax), %%rdx\n"
            "lea 4(%%rax), %%rsi\n"
            "lea 5(%%rax), %%rdi\n"
            "fadd %%st(1), %%st\n"
            "fxch %%st(1)\n"
            "fchs\n"
            "fstp %%st(0)\n"
            "fstp %%st(0)\n"
            "mov %%rax, 0(%[out])\n"
            "mov %%rbx, 8(%[out])\n"
            "mov %%rcx, 16(%[out])\n"
            "mov %%rdx, 24(%[out])\n"
            "mov %%rsi, 32(%[out])\n"
            "mov %%rdi, 40(%[out])\n"
            :
            : [one] "m" (one), [k] "r" ((uint64_t) i * 0x100000001ull), [out] "r" (out)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "memory", "st", "st(1)");
        for (int r = 0; r < 6; r++)
            if (out[r] != (uint64_t) i * 0x100000001ull + r) {
                CHECK(0, "iteration %d reg %d = %#llx", i, r, (unsigned long long) out[r]);
                goto done_regs;
            }
    }
done_regs:
    printf("regs across x87: %s\n", failures ? "FAIL" : "ok");

    // Memory forms: registers written right before, one used as the address.
    int mem_bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t out[6];
        double src[2] = {1.5, 0}, dst = 0;
        __asm__ volatile(
            "mov %[k], %%rax\n"
            "lea 1(%%rax), %%rbx\n"
            "lea 2(%%rax), %%rcx\n"
            "mov %[src], %%rdx\n"
            "lea 4(%%rax), %%rsi\n"
            "lea 5(%%rax), %%rdi\n"
            "fldl (%%rdx)\n"
            "fstpl %[dst]\n"
            "mov %%rax, 0(%[out])\n"
            "mov %%rbx, 8(%[out])\n"
            "mov %%rcx, 16(%[out])\n"
            "mov %%rdx, 24(%[out])\n"
            "mov %%rsi, 32(%[out])\n"
            "mov %%rdi, 40(%[out])\n"
            : [dst] "=m" (dst)
            : [k] "r" ((uint64_t) i * 0x100000001ull), [src] "r" (src), [out] "r" (out)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "memory");
        uint64_t k = (uint64_t) i * 0x100000001ull;
        uint64_t want[6] = {k, k + 1, k + 2, (uint64_t) src, k + 4, k + 5};
        if (memcmp(out, want, sizeof(want)) != 0 || dst != 1.5) {
            CHECK(0, "mem iteration %d: rax=%#llx rdx=%#llx dst=%g", i,
                    (unsigned long long) out[0], (unsigned long long) out[3], dst);
            mem_bad = 1;
            break;
        }
    }
    printf("regs across x87 memory forms: %s\n", mem_bad ? "FAIL" : "ok");

    // FNSTSW AX: the flushing path, still writes ax (positive control).
    {
        uint64_t rax_out;
        __asm__ volatile(
            "mov $0x1234567800000000, %%rax\n"
            "fninit\n"
            "fnstsw %%ax\n"
            "mov %%rax, %[r]\n"
            : [r] "=r" (rax_out) : : "rax");
        CHECK(rax_out == 0x1234567800000000ull, "fnstsw ax gave %#llx", (unsigned long long) rax_out);
        printf("fnstsw ax: %#llx\n", (unsigned long long) rax_out);
    }

    // #UD from an x87 register form, with registers only in the cache.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigill;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    for (int i = 0; i < 50; i++) {
        uint64_t k = 0xabc000000000ull + i * 16;
        __asm__ volatile(
            "mov %[k], %%rax\n"
            "lea 1(%%rax), %%rbx\n"
            "lea 2(%%rax), %%rcx\n"
            "lea 3(%%rax), %%rdx\n"
            "lea 4(%%rax), %%rsi\n"
            "lea 5(%%rax), %%rdi\n"
            ".globl ud_insn\n"
            "ud_insn: .byte 0xd9, 0xd1\n"
            :
            : [k] "r" (k)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "memory");
        int bad = 0;
        for (int r = 0; r < 6; r++)
            if (sig_regs[r] != k + r)
                bad = 1;
        if (bad || sig_rip != (uint64_t) ud_insn || sigill_seen != i + 1) {
            CHECK(0, "sigill %d: seen=%d rip=%#llx want %#llx rax=%#llx rdi=%#llx", i, sigill_seen,
                    (unsigned long long) sig_rip, (unsigned long long) (uint64_t) ud_insn,
                    (unsigned long long) sig_regs[0], (unsigned long long) sig_regs[5]);
            break;
        }
    }
    printf("sigill from x87 reg form: %d seen\n", sigill_seen);

    // #PF from an x87 memory form, with registers only in the cache.
    sa.sa_sigaction = on_sigsegv;
    sigaction(SIGSEGV, &sa, NULL);
    for (int i = 0; i < 50; i++) {
        uint64_t k = 0xdef000000000ull + i * 16;
        __asm__ volatile(
            "mov %[k], %%rax\n"
            "lea 1(%%rax), %%rbx\n"
            "lea 2(%%rax), %%rcx\n"
            "mov $16, %%rdx\n"
            "lea 4(%%rax), %%rsi\n"
            "lea 5(%%rax), %%rdi\n"
            ".globl pf_insn\n"
            "pf_insn: fldl (%%rdx)\n"
            :
            : [k] "r" (k)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "memory");
        uint64_t want[6] = {k, k + 1, k + 2, 16, k + 4, k + 5};
        if (memcmp(sig_regs, want, sizeof(want)) != 0 || sig_rip != (uint64_t) pf_insn || sigsegv_seen != i + 1) {
            CHECK(0, "sigsegv %d: seen=%d rip=%#llx want %#llx rax=%#llx rdx=%#llx", i, sigsegv_seen,
                    (unsigned long long) sig_rip, (unsigned long long) (uint64_t) pf_insn,
                    (unsigned long long) sig_regs[0], (unsigned long long) sig_regs[3]);
            break;
        }
    }
    printf("sigsegv from x87 memory form: %d seen\n", sigsegv_seen);

    printf("amd64_x87_cache: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
