// i386_flags_pushf_lahf.c -- what PUSHF and LAHF read after each ALU op,
// whose ZF/SF/PF/AF the i386 JIT keeps lazily (CPU_res, op1/op2): ADD SUB ADC
// SBB AND OR XOR INC DEC NEG CMP TEST on random operands, against a model of
// the SDM. LAHF must equal PUSHF's low byte. SAHF and POPF then PUSHF give back
// what they loaded; POPF setting TF traps after the next instruction. The
// gadgets are jit/gadgets-aarch64/control.S's pushf/popf/sahf/lahf, whose
// collapse and expand are asm (i386_collapse_flags / i386_expand_flags).
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, uint32_t a, uint32_t b, uint32_t got, uint32_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%#x, %#x): %#x, want %#x\n", what, a, b, got, want);
}

#define CF 0x1
#define PF 0x4
#define AF 0x10
#define ZF 0x40
#define SF 0x80
#define OF 0x800
#define ARITH (CF | PF | AF | ZF | SF | OF)

static uint32_t zsp(uint32_t r) {
    return (r == 0 ? ZF : 0) | (r & 0x80000000u ? SF : 0) | (__builtin_parity(r & 0xff) ? 0 : PF);
}
static uint32_t add_flags(uint32_t a, uint32_t b, uint32_t c) {
    uint64_t w = (uint64_t) a + b + c;
    uint32_t r = (uint32_t) w;
    return zsp(r) | (w >> 32 ? CF : 0) | (((a ^ b ^ r) & 0x10) ? AF : 0) |
           ((~(a ^ b) & (a ^ r) & 0x80000000u) ? OF : 0);
}
static uint32_t sub_flags(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a - b - c;
    int borrow = (uint64_t) b + c > a;
    return zsp(r) | (borrow ? CF : 0) | (((a ^ b ^ r) & 0x10) ? AF : 0) |
           (((a ^ b) & (a ^ r) & 0x80000000u) ? OF : 0);
}
static uint32_t logic_flags(uint32_t r) { return zsp(r); }   // CF OF AF clear (AF undefined: AOK clears)

static uint32_t rs = 0x9e3779b9u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

#define OP2(insn, a, b, f, l) \
    __asm__ volatile("mov %2, %%eax\n " insn " %3, %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1" \
                     : "=r"(f), "=r"(l) : "r"(a), "r"(b) : "eax", "cc")
#define OP1(insn, a, f, l) \
    __asm__ volatile("mov %2, %%eax\n " insn " %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1" \
                     : "=r"(f), "=r"(l) : "r"(a) : "eax", "cc")
// an op whose flags cross a block boundary (the jmp), so they are really
// left lazy for PUSHF to collapse
#define OP2J(insn, a, b, f) \
    __asm__ volatile("mov %1, %%eax\n " insn " %2, %%eax\n jmp 1f\n 1: pushf\n pop %0" \
                     : "=r"(f) : "r"(a), "r"(b) : "eax", "cc")

static volatile int traps;
static volatile uintptr_t trap_pc;
static void on_trap(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    traps++;
    trap_pc = uc->uc_mcontext.gregs[REG_EIP];
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
}
extern char tf_after_nop32[];

int main(void) {
    for (int i = 0; i < 4000; i++) {
        uint32_t a = rnd(), b = rnd(), f, l;
        if (i % 9 == 0) b = a;
        if (i % 13 == 0) a = 0x7fffffff;
        if (i % 17 == 0) b = 0x80000000u;
        OP2("add", a, b, f, l);
        check("add", a, b, f & ARITH, add_flags(a, b, 0));
        check("add: lahf = pushf low byte", a, b, l, f & 0xff);
        OP2("sub", a, b, f, l);
        check("sub", a, b, f & ARITH, sub_flags(a, b, 0));
        check("sub: lahf", a, b, l, f & 0xff);
        OP2("cmp", a, b, f, l);
        check("cmp", a, b, f & ARITH, sub_flags(a, b, 0));
        OP2("and", a, b, f, l);
        check("and", a, b, f & (ARITH & ~AF), logic_flags(a & b));
        OP2("or", a, b, f, l);
        check("or", a, b, f & (ARITH & ~AF), logic_flags(a | b));
        OP2("xor", a, b, f, l);
        check("xor", a, b, f & (ARITH & ~AF), logic_flags(a ^ b));
        OP2("test", a, b, f, l);
        check("test", a, b, f & (ARITH & ~AF), logic_flags(a & b));
        // adc / sbb with CF set by stc, and clear by clc
        __asm__ volatile("mov %2, %%eax\n stc\n adc %3, %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1"
                         : "=r"(f), "=r"(l) : "r"(a), "r"(b) : "eax", "cc");
        check("adc (CF=1)", a, b, f & ARITH, add_flags(a, b, 1));
        __asm__ volatile("mov %2, %%eax\n stc\n sbb %3, %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1"
                         : "=r"(f), "=r"(l) : "r"(a), "r"(b) : "eax", "cc");
        check("sbb (CF=1)", a, b, f & ARITH, sub_flags(a, b, 1));
        // inc / dec keep CF: set it first
        __asm__ volatile("mov %2, %%eax\n stc\n inc %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1"
                         : "=r"(f), "=r"(l) : "r"(a) : "eax", "cc");
        check("inc (keeps CF)", a, 1, f & ARITH, (add_flags(a, 1, 0) & ~CF) | CF);
        __asm__ volatile("mov %2, %%eax\n clc\n dec %%eax\n pushf\n pop %0\n lahf\n movzbl %%ah, %1"
                         : "=r"(f), "=r"(l) : "r"(a) : "eax", "cc");
        check("dec (keeps CF)", a, 1, f & ARITH, sub_flags(a, 1, 0) & ~CF);
        OP1("neg", a, f, l);
        check("neg", a, 0, f & ARITH, (sub_flags(0, a, 0) & ~CF) | (a ? CF : 0));
        // across a block boundary
        OP2J("add", a, b, f);
        check("add, then a jump", a, b, f & ARITH, add_flags(a, b, 0));
        OP2J("sub", a, b, f);
        check("sub, then a jump", a, b, f & ARITH, sub_flags(a, b, 0));
        // bit 1 set, bits 3 and 5 clear, IF set
        check("reserved bits, IF", a, b, f & 0x22a, 0x202);
        // SAHF then PUSHF: SF ZF AF PF CF from AH; OF kept
        uint32_t ah = rnd() & 0xff, f2;
        __asm__ volatile("mov %1, %%eax\n shl $8, %%eax\n sahf\n pushf\n pop %0" : "=r"(f2) : "r"(ah) : "eax", "cc");
        check("sahf/pushf", ah, 0, f2 & (SF | ZF | AF | PF | CF), ah & (SF | ZF | AF | PF | CF));
        // POPF then PUSHF (TF apart)
        uint32_t v = rnd() & ~0x100u, f3;
        __asm__ volatile("push %1\n popf\n pushf\n pop %0\n cld" : "=r"(f3) : "r"(v) : "cc");
        check("popf/pushf", v, 0, f3 & 0x244dd5, v & 0x244dd5);
    }
    // POPF setting TF: SIGTRAP after the following instruction
    struct sigaction st;
    memset(&st, 0, sizeof(st));
    st.sa_sigaction = on_trap;
    st.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &st, NULL);
    __asm__ volatile("pushf\n orl $0x100, (%%esp)\n popf\n nop\n .globl tf_after_nop32\n tf_after_nop32: nop" ::: "cc", "memory");
    check("TF: one SIGTRAP", 0, 0, traps, 1);
    check("TF: after the next instruction", 0, 0, trap_pc, (uintptr_t) tf_after_nop32);
    printf("i386_flags_pushf_lahf: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
