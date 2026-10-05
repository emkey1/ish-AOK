// A page fault inside an amd64 memory move whose base and destination are in
// the JIT's register cache (x20-x27 for rax..rdi; math.S amd64_sld/sst keep the
// cache live across the access): the guest's SIGSEGV handler must see the
// registers' CURRENT values in its ucontext, including ones the instructions
// just before the fault changed and the JIT has not written back. The
// gadgets' slow paths spill the cache before the segfault exit.
//
// Loads, stores, add and cmp reg,[mem] at 32 and 64 bits, through a cached
// base (rcx = NULL), and through r9 with r10 as the data register; indexed
// addresses; 16-bit and byte moves and a byte compare.
// Passes on real x86 and under the interpreter (echo 0 > /proc/ish/amd64_jit).
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static sigjmp_buf back;
static uint64_t seen_rax, seen_rdx, seen_rsi, seen_rdi, seen_rbx;

static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void) sig; (void) si;
    ucontext_t *uc = uc_;
    seen_rax = uc->uc_mcontext.gregs[REG_RAX];
    seen_rdx = uc->uc_mcontext.gregs[REG_RDX];
    seen_rsi = uc->uc_mcontext.gregs[REG_RSI];
    seen_rdi = uc->uc_mcontext.gregs[REG_RDI];
    seen_rbx = uc->uc_mcontext.gregs[REG_RBX];
    siglongjmp(back, 1);
}

static int failures;
static void check(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL: %s: handler saw %#llx, want %#llx\n", what, (unsigned long long) got,
               (unsigned long long) want);
        failures++;
    }
}

#define FAULTING(name, insn) do { \
    seen_rax = seen_rdx = seen_rsi = seen_rdi = 0; \
    if (sigsetjmp(back, 1) == 0) { \
        __asm__ volatile( \
            "movq $0x1111111111111111, %%rax\n" \
            "addq $1, %%rax\n"                /* cached and dirty */ \
            "movq $0x2222, %%rdx\n"           \
            "leaq 5(%%rdx), %%rsi\n"          \
            "movq %%rsi, %%rdi\n"             \
            "xorl %%ecx, %%ecx\n"             /* the cached base: NULL */ \
            insn "\n"                         \
            ::: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r9", "r10", "memory", "cc"); \
        printf("FAIL: %s did not fault\n", name); failures++; \
    } else { \
        check(name " rax", seen_rax, 0x1111111111111112ull); \
        check(name " rdx", seen_rdx, 0x2222); \
        check(name " rsi", seen_rsi, 0x2227); \
        check(name " rdi", seen_rdi, 0x2227); \
    } \
} while (0)

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    FAULTING("load64", "movq 16(%%rcx), %%rbx");
    FAULTING("load32", "movl 16(%%rcx), %%ebx");
    FAULTING("store64", "movq %%rax, 16(%%rcx)");
    FAULTING("store32", "movl %%eax, 16(%%rcx)");
    FAULTING("add64 reg,mem", "addq 16(%%rcx), %%rbx");
    FAULTING("cmp32 reg,mem", "cmpl 16(%%rcx), %%ebx");
    // r8-r15 as the base and as the data register (they live in memory, not
    // in the cache), and the same right after a jump: a fresh block, where
    // nothing has loaded the cache yet -- the gadget's slow path still writes
    // x20-x27 back, so the JIT must have loaded them first.
    FAULTING("load64 r10,[r9]", "xorl %%r9d, %%r9d\n movq 16(%%r9), %%r10");
    FAULTING("store32 [r9],r10d", "xorl %%r9d, %%r9d\n movl %%r10d, 16(%%r9)");
    FAULTING("sub64 r10,[r9]", "xorl %%r9d, %%r9d\n subq 16(%%r9), %%r10");
    FAULTING("load64 r10,[r9] new block", "xorl %%r9d, %%r9d\n jmp 1f\n 1: movq 16(%%r9), %%r10");
    FAULTING("cmp64 r10,[r9] new block", "xorl %%r9d, %%r9d\n jmp 1f\n 1: cmpq 16(%%r9), %%r10");
    // Indexed addresses (an amd64_ea gadget, then the access through x3),
    // and the 16-bit and byte forms.
    FAULTING("load32 [rcx+rdx*2]", "movl 16(%%rcx,%%rdx,2), %%ebx");
    FAULTING("store16 [r9+r10]", "xorl %%r9d, %%r9d\n movl $8, %%r10d\n movw %%ax, 16(%%r9,%%r10)");
    FAULTING("movzbl [rcx+rdi]", "movzbl 16(%%rcx,%%rdi), %%ebx");
    FAULTING("cmpb dl,[rcx+rdi]", "cmpb 16(%%rcx,%%rdi), %%dl");
    FAULTING("store8 [rcx]", "movb %%al, 16(%%rcx)");
    FAULTING("movzwl [r9]", "xorl %%r9d, %%r9d\n movzwl 16(%%r9), %%r10d");
    FAULTING("inc32 [rcx]", "incl 16(%%rcx)");
    FAULTING("dec64 [rcx+rdx*8]", "decq 16(%%rcx,%%rdx,8)");
    FAULTING("cmp64 [rcx],rax", "cmpq %%rax, 16(%%rcx)");
    FAULTING("cmpb [rcx],imm", "cmpb $7, 16(%%rcx)");
    FAULTING("testb [rcx+rdi],imm", "testb $1, 16(%%rcx,%%rdi)");
    FAULTING("movslq [rcx],rbx", "movslq 16(%%rcx), %%rbx");
    FAULTING("movslq [r9],r10", "xorl %%r9d, %%r9d\n movslq 16(%%r9), %%r10");
    printf("amd64_fault_regs: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
