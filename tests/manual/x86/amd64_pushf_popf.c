// amd64_pushf_popf.c -- PUSHF/POPF (9C/9D), 64-bit and 16-bit (0x66), against
// the SDM for user mode: POPF loads CF PF AF ZF SF DF OF NT, and in 64-bit AC
// and ID too (the CPUID-detection toggle); IF and IOPL are not changed by
// user code; setting TF single-steps -- SIGTRAP after the instruction that
// follows the POPF; PUSHF gives bit 1 set, bits 3, 5 and 15 clear,
// IF set; the flags an ALU op leaves (eager CF/OF included) are what PUSHF
// reads; 16-bit forms move RSP by 2 and touch only the low word; the stack
// pointer moves only when the access worked -- a PUSHF into an unmapped
// stack and a POPF from one fault at the instruction with RSP unchanged.
// The gadgets are jit/gadgets-aarch64/math.S's amd64_pushf*/amd64_popf*. (No
// 16-bit push/pop of a register here: those are not compiled, and a block
// that falls back is interpreted onward.)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t in, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%#llx): %#llx, want %#llx\n", what, (unsigned long long) in,
               (unsigned long long) got, (unsigned long long) want);
}

#define W64 0x244cd5ull      // CF PF AF ZF SF DF OF NT AC ID (TF apart: it traps)
#define W16 0x4cd5ull

static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static sigjmp_buf jb;
static volatile uintptr_t f_addr, f_pc, f_rsp;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    ucontext_t *uc = ctx;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.gregs[REG_RIP];
    f_rsp = uc->uc_mcontext.gregs[REG_RSP];
    siglongjmp(jb, 1);
}
extern char pf_push_insn[], pf_pop_insn[], tf_after_nop[];
static volatile int traps;
static volatile uintptr_t trap_pc;
static void on_trap(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    traps++;
    trap_pc = uc->uc_mcontext.gregs[REG_RIP];
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;    // stop stepping
}

int main(void) {
    uint64_t base;
    __asm__ volatile("pushfq\n pop %0" : "=r"(base));
    check("base: IF set, bit 1 set", base, base & 0x202, 0x202);
    for (int i = 0; i < 3000; i++) {
        uint64_t v = rnd() & ~0x100ull, got, back;   // TF single-steps: tried on its own below
        // 64-bit: POPF v, then PUSHF
        __asm__ volatile("push %1\n popfq\n pushfq\n pop %0\n cld" : "=r"(got) : "r"(v) : "cc");
        uint64_t want = (base & ~W64) | (v & W64);
        want = (want | 2) & ~(uint64_t) 0x8028;
        check("popfq/pushfq", v, got, want);
        // 16-bit: the low word only, RSP by 2 each way
        uint64_t rsp0, rsp1, rsp2;
        uint16_t v16 = (uint16_t) v;
        __asm__ volatile("mov %%rsp, %1\n pushfw\n mov %%rsp, %2\n popfw\n mov %%rsp, %3\n"
                         "sub $2, %%rsp\n movw %w4, (%%rsp)\n popfw\n pushfw\n movzwl (%%rsp), %k0\n"
                         "add $2, %%rsp\n cld"
                         : "=&r"(back), "=&r"(rsp0), "=&r"(rsp1), "=&r"(rsp2) : "r"(v16) : "cc", "memory");
        check("pushfw moves rsp by 2", v, rsp0 - rsp1, 2);
        check("popfw moves rsp back", v, rsp2, rsp0);
        uint16_t w16 = (uint16_t) (((base & ~W16) | (v16 & W16) | 2) & ~0x8028ull);
        check("popfw/pushfw low word", v, back & 0xffff, w16);
        // the flags an ALU op leaves: add overflow, sub borrow
        uint64_t a = rnd(), b = rnd(), f1, f2;
        __asm__ volatile("add %2, %3\n pushfq\n pop %0\n" : "=r"(f1), "+r"(a) : "r"(b), "1"(a) : "cc");
        a = rnd();
        __asm__ volatile("sub %2, %1\n pushfq\n pop %0\n" : "=r"(f2), "+r"(a) : "r"(b) : "cc");
        check("pushf after add: bit 1, IF", a, f1 & 0x202, 0x202);
        check("pushf after sub: bits 3, 5 clear", a, f2 & 0x28, 0);
    }
    // carry/overflow from an ALU op are what PUSHF reads
    {
        uint64_t f, x = 0x7fffffffffffffffull;
        __asm__ volatile("add $1, %1\n pushfq\n pop %0" : "=r"(f), "+r"(x) :: "cc");
        check("0x7fff..+1: OF, SF set, CF clear", 0, f & 0x881, 0x880);
        x = 0;
        __asm__ volatile("sub $1, %1\n pushfq\n pop %0" : "=r"(f), "+r"(x) :: "cc");
        check("0-1: CF, SF set, OF clear", 0, f & 0x881, 0x81);
    }
    // ID toggles (CPUID detection); TF, IF, IOPL, NT stay
    {
        uint64_t f0, f1;
        __asm__ volatile("pushfq\n pop %0\n mov %0, %1\n xor $0x200000, %1\n push %1\n popfq\n pushfq\n pop %1"
                         : "=&r"(f0), "=&r"(f1) :: "cc");
        check("ID toggles", f0, (f0 ^ f1) & 0x200000, 0x200000);
        __asm__ volatile("pushfq\n pop %0\n mov %0, %1\n xor $0x3200, %1\n push %1\n popfq\n pushfq\n pop %1"
                         : "=&r"(f0), "=&r"(f1) :: "cc");
        check("IF, IOPL unchanged", f0, (f0 ^ f1) & 0x3200, 0);
        __asm__ volatile("pushfq\n pop %0\n mov %0, %1\n xor $0x4000, %1\n push %1\n popfq\n pushfq\n pop %1\n"
                         "push %0\n popfq" : "=&r"(f0), "=&r"(f1) :: "cc");
        check("NT toggles", f0, (f0 ^ f1) & 0x4000, 0x4000);
        __asm__ volatile("pushfq\n pop %0\n mov %0, %1\n or $0x400, %1\n push %1\n popfq\n pushfq\n pop %1\n cld"
                         : "=&r"(f0), "=&r"(f1) :: "cc");
        check("DF set by popf", f0, f1 & 0x400, 0x400);
    }
    // POPF setting TF: SIGTRAP after the instruction that follows it
    {
        struct sigaction st;
        memset(&st, 0, sizeof(st));
        st.sa_sigaction = on_trap;
        st.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &st, NULL);
        traps = 0;
        __asm__ volatile("pushfq\n orq $0x100, (%%rsp)\n popfq\n nop\n .globl tf_after_nop\n tf_after_nop: nop\n"
                         ::: "cc", "memory");
        check("TF: one SIGTRAP", 0, traps, 1);
        check("TF: after the instruction following POPF", 0, trap_pc, (uintptr_t) tf_after_nop);
        signal(SIGTRAP, SIG_DFL);
    }
    // faults: an unmapped stack, on an alternate signal stack
    static char altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    uint8_t *hole = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int k = 0; k < 2; k++) {
        uint64_t sp = (uint64_t) hole + (k == 0 ? 4096 + 8 * 0 : 64), saved;
        // push: rsp just above the hole's end lands the store in it; pop: rsp in it
        if (k == 0) sp = (uint64_t) hole + 4096;
        f_addr = 0;
        if (!sigsetjmp(jb, 1)) {
            if (k == 0)
                __asm__ volatile("mov %%rsp, %0\n mov %1, %%rsp\n .globl pf_push_insn\n pf_push_insn: pushfq\n mov %0, %%rsp"
                                 : "=&r"(saved) : "r"(sp) : "memory");
            else
                __asm__ volatile("mov %%rsp, %0\n mov %1, %%rsp\n .globl pf_pop_insn\n pf_pop_insn: popfq\n mov %0, %%rsp"
                                 : "=&r"(saved) : "r"(sp) : "memory", "cc");
        }
        check(k ? "popf fault: in the hole" : "pushf fault: in the hole", k,
              f_addr >= (uintptr_t) hole && f_addr < (uintptr_t) hole + 4096, 1);
        check(k ? "popf fault: rsp unchanged" : "pushf fault: rsp unchanged", k, f_rsp, sp);
        check(k ? "popf fault: rip" : "pushf fault: rip", k, f_pc,
              (uintptr_t) (k ? pf_pop_insn : pf_push_insn));
    }
    printf("amd64_pushf_popf: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
