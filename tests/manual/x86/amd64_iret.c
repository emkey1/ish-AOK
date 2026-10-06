// IRETD and IRETW on amd64 (IRETQ is amd64_segment_regs' and gpf_siginfo's):
// IRETD pops five 4-byte slots, so its RIP and RSP are below 4 GiB -- code and
// a stack mapped low; the flags it pops take POPF's rule (DF here); a CS of
// 0x23 or 0 is #GP; IRETW pops 2-byte slots, so its RIP is below 64 KiB,
// under mmap_min_addr: the jump lands and faults there. And an IRETQ that
// sets TF traps after the first instruction at the target. Answers from an
// AMD Ryzen (camd).
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile uint64_t got_addr, got_rip;
static void on_sig(int sig, siginfo_t *si, void *uc) {
    got_sig = sig;
    got_code = si->si_code;
    got_addr = (uint64_t) si->si_addr;
    got_rip = ((ucontext_t *) uc)->uc_mcontext.gregs[REG_RIP];
    if (sig == SIGTRAP)
        ((ucontext_t *) uc)->uc_mcontext.gregs[REG_EFL] &= ~0x100ULL;
    siglongjmp(jb, 1);
}

__attribute__((used)) static uint64_t saved_rsp, landed_rsp, landed_flags;

#define LOW_CODE 0x20000000UL
#define LOW_STACK 0x20010000UL

// iretd to LOW_CODE with RSP = LOW_STACK + 0x800 and the given flags and
// selectors; the low code records RSP and the flags and jumps back to
// iretd_back
extern char iretd_back[];
__attribute__((noinline, noclone)) static void do_iretd(uint32_t cs, uint32_t ss, uint32_t flags) {
    __asm__ volatile(
        "mov %%rsp, saved_rsp(%%rip)\n"
        "sub $32, %%rsp\n"
        "movl %[rip], 0(%%rsp)\n"
        "movl %[cs], 4(%%rsp)\n"
        "movl %[fl], 8(%%rsp)\n"
        "movl %[sp], 12(%%rsp)\n"
        "movl %[ss], 16(%%rsp)\n"
        "iretl\n"
        ".globl iretd_back\n"
        "iretd_back:\n"
        "mov saved_rsp(%%rip), %%rsp\n"
        "cld\n"
        :: [rip] "i"(LOW_CODE), [cs] "r"(cs), [ss] "r"(ss), [fl] "r"(flags), [sp] "i"(LOW_STACK + 0x800)
        : "memory", "rax", "rcx", "rdx");
}

int main(void) {
    // (IRETW leaves RSP in the first 64 KiB, unmapped: handlers need their own stack)
    static unsigned char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    unsigned char *code = mmap((void *) LOW_CODE, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    unsigned char *stack = mmap((void *) LOW_STACK, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (code != (void *) LOW_CODE || stack != (void *) LOW_STACK) {
        printf("amd64_iret: SKIP (no low mappings)\n");
        return 0;
    }
    // mov %rsp, landed_rsp; pushfq; pop landed_flags; movabs $9b, %rax; jmp *%rax
    unsigned char *p = code;
    uint64_t a;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0x24; *p++ = 0x25;      // mov %rsp, abs32
    a = (uint64_t) &landed_rsp; memcpy(p, &a, 4); p += 4;
    *p++ = 0x9c;                                             // pushfq
    *p++ = 0x8f; *p++ = 0x04; *p++ = 0x25;                   // pop abs32
    a = (uint64_t) &landed_flags; memcpy(p, &a, 4); p += 4;
    *p++ = 0x48; *p++ = 0xb8;                                // movabs $back, %rax
    unsigned char *back_imm = p; p += 8;
    *p++ = 0xff; *p++ = 0xe0;                                // jmp *%rax
    int abs_ok = (uint64_t) &landed_rsp < 0x80000000UL && (uint64_t) &landed_flags < 0x80000000UL;
    if (!abs_ok) {
        // a PIE binary: the globals are out of abs32 reach; use the low stack page instead
        p = code;
        *p++ = 0x48; *p++ = 0x89; *p++ = 0x24; *p++ = 0x25;
        a = LOW_STACK + 0xf00; memcpy(p, &a, 4); p += 4;
        *p++ = 0x9c;
        *p++ = 0x8f; *p++ = 0x04; *p++ = 0x25;
        a = LOW_STACK + 0xf08; memcpy(p, &a, 4); p += 4;
        *p++ = 0x48; *p++ = 0xb8;
        back_imm = p; p += 8;
        *p++ = 0xff; *p++ = 0xe0;
    }
    uint64_t back = (uint64_t) iretd_back;
    memcpy(back_imm, &back, 8);

    // IRETD: lands, RSP from the frame, DF from the frame
    got_sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        do_iretd(0x33, 0x2b, 0x602);                          // IF | DF | bit 1
    uint64_t lrsp = abs_ok ? landed_rsp : *(uint64_t *) (LOW_STACK + 0xf00);
    uint64_t lfl = abs_ok ? landed_flags : *(uint64_t *) (LOW_STACK + 0xf08);
    CHECK(got_sig == 0 && lrsp == LOW_STACK + 0x800 && (lfl & 0x400), "iretd: signal %d rsp %llx flags %llx",
          got_sig, (unsigned long long) lrsp, (unsigned long long) lfl);

    // IRETD with CS 0x23 (would leave 64-bit mode) and CS 0, SS 0: #GP
    static const struct { uint32_t cs, ss; const char *what; } bad[] = {
        {0x23, 0x2b, "cs 0x23"}, {0, 0x2b, "cs 0"}, {0x33, 0, "ss 0"}, {0x2b, 0x2b, "cs 0x2b"},
    };
    for (int i = 0; i < 4; i++) {
        got_sig = 0;
        if (sigsetjmp(jb, 1) == 0)
            do_iretd(bad[i].cs, bad[i].ss, 0x202);
        CHECK(got_sig == SIGSEGV && got_code == 0x80, "iretd %s: signal %d code %d", bad[i].what, got_sig, got_code);
    }

    // IRETW to 0x1000 (2-byte slots): lands there, under mmap_min_addr, and faults
    got_sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile(
            "mov %%rsp, saved_rsp(%%rip)\n"
            "sub $16, %%rsp\n"
            "movw $0x1000, 0(%%rsp)\n movw $0x33, 2(%%rsp)\n movw $0x202, 4(%%rsp)\n"
            "movw $0x7f00, 6(%%rsp)\n movw $0x2b, 8(%%rsp)\n"
            "iretw\n" ::: "memory");
    CHECK(got_sig == SIGSEGV && got_addr == 0x1000 && got_rip == 0x1000, "iretw: signal %d addr %llx rip %llx",
          got_sig, (unsigned long long) got_addr, (unsigned long long) got_rip);

    // IRETQ setting TF: the trap comes after the first instruction at the target
    got_sig = 0;
    unsigned char *t = code + 0x800;
    t[0] = 0x90; t[1] = 0x90; t[2] = 0x0f; t[3] = 0x0b;        // nop; nop; ud2
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile(
            "mov %%rsp, saved_rsp(%%rip)\n"
            "mov %%rsp, %%rax\n"
            "push $0x2b\n push %%rax\n push $0x302\n push $0x33\n push %0\n"
            "iretq\n" :: "r"(t) : "rax", "memory");
    CHECK(got_sig == SIGTRAP && got_rip == (uint64_t) t + 1, "iretq with TF: signal %d rip %llx (target %llx)", got_sig,
          (unsigned long long) got_rip, (unsigned long long) t);

    printf("amd64_iret: %s (%d checks, %d failures)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
