// x86_cpuid_xgetbv.c -- CPUID's leaf and subleaf rules, as the CPUID gadgets
// look them up (jit/gadgets-aarch64/misc.S cpuid_lookup on emu/cpuid.h's
// tables): an unimplemented leaf below the maximum reads as zeros, one above it
// as the highest leaf's feature words, any extended leaf past the extended
// maximum as that maximum; leaf 7 and leaf 0xD answer by subleaf, an unknown
// subleaf with zeros; every leaf writes all four registers (RAX..RDX's upper
// halves cleared on amd64). XGETBV(0) is XCR0 = leaf 0xD subleaf 0's EAX
// (with EDX); XGETBV of any other register is #GP at the instruction (SIGSEGV,
// si_addr 0). i386 and amd64.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, unsigned long got, unsigned long want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s: %#lx, want %#lx\n", what, got, want);
}
struct r { unsigned long a, b, c, d; };
static struct r cpuid(uint32_t leaf, uint32_t sub) {
#if defined(__x86_64__)
    struct r r = { 0xffffffff00000000ul | leaf, ~0ul, 0xffffffff00000000ul | sub, ~0ul };
#else
    struct r r = { leaf, ~0ul, sub, ~0ul };
#endif
    __asm__ volatile("cpuid" : "+a"(r.a), "+b"(r.b), "+c"(r.c), "+d"(r.d));
    return r;
}
static sigjmp_buf jb;
static volatile uintptr_t f_pc, f_addr;
static volatile int f_sig;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    f_sig = sig;
    f_addr = (uintptr_t) si->si_addr;
#if defined(__x86_64__)
    f_pc = ((ucontext_t *) ctx)->uc_mcontext.gregs[REG_RIP];
#else
    f_pc = ((ucontext_t *) ctx)->uc_mcontext.gregs[REG_EIP];
#endif
    siglongjmp(jb, 1);
}
extern char xgetbv1_insn[];

int main(void) {
    struct r l0 = cpuid(0, 0), l1 = cpuid(1, 0), e0 = cpuid(0x80000000u, 0);
    uint32_t max = (uint32_t) l0.a, emax = (uint32_t) e0.a;
    check("leaf 0: GenuineIntel", l0.b == 0x756e6547 && l0.d == 0x49656e69 && l0.c == 0x6c65746e, 1);
    check("leaf 0: the maximum reaches 7 and 0xD", max >= 0xd, 1);
    for (uint32_t leaf = 2; leaf <= max; leaf++) {
        if (leaf == 7 || leaf == 0xd) continue;
        struct r x = cpuid(leaf, 0);
        check("an unimplemented leaf below the maximum: zeros", x.a | x.b | x.c | x.d, 0);
    }
    struct r above = cpuid(max + 1, 0), above2 = cpuid(0x40000000u, 5);
    check("above the maximum: the feature words of leaf 1 (ecx)", above.c, l1.c);
    check("above the maximum: (edx)", above.d, l1.d);
    check("above the maximum: eax, ebx zero", above.a | above.b, 0);
    check("0x40000000: the same", above2.c == l1.c && above2.d == l1.d, 1);
    struct r ext = cpuid(emax + 1, 0);
    if (emax + 1 != 0x80000001u)
        check("past the extended maximum: eax = that maximum", ext.a, emax);
    check("leaf 7, subleaf 1: zeros", ({ struct r x = cpuid(7, 1); x.a | x.b | x.c | x.d; }), 0);
    check("leaf 0xD, subleaf 3: zeros", ({ struct r x = cpuid(0xd, 3); x.a | x.b | x.c | x.d; }), 0);
    check("leaf 0xD, subleaf 9: zeros", ({ struct r x = cpuid(0xd, 9); x.a | x.b | x.c | x.d; }), 0);
#if defined(__x86_64__)
    check("all four written (upper halves too)", (l1.a | l1.b | l1.c | l1.d) >> 32, 0);
#endif
    unsigned long xa, xd;
    __asm__ volatile("xgetbv" : "=a"(xa), "=d"(xd) : "c"(0));
    struct r d0 = cpuid(0xd, 0);
    check("xgetbv(0) = leaf 0xD subleaf 0 eax", xa, d0.a);
    check("xgetbv(0) edx", xd, d0.d);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    f_sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile(".globl xgetbv1_insn\nxgetbv1_insn: xgetbv" : : "c"(1) : "eax", "edx");
    check("xgetbv(1): SIGSEGV", f_sig, SIGSEGV);
    check("xgetbv(1): at the instruction", f_pc, (uintptr_t) xgetbv1_insn);
    check("xgetbv(1): si_addr 0", f_addr, 0);
    printf("x86_cpuid_xgetbv: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
