// amd64_ret_pop.c -- RET imm16 (C2) and POP r/m (8F /0), 64 and 16 bits.
// RET imm16 returns and then releases imm16 bytes of arguments. POP r/m: a
// memory destination's address uses RSP as already advanced (`popq 8(%rsp)`
// stores where the SDM says), a destination page never touched before is
// faulted in and the right value lands (#487: musl's sigsetjmp pops its
// return address into a fresh jmp_buf), `pop %rsp` loads RSP, 16-bit forms
// move RSP by 2 and keep the rest of a register. A destination that cannot be
// written, and a stack that cannot be read, fault at the instruction with RSP
// unchanged. The gadgets are jit/gadgets-aarch64/math.S's amd64_ret_imm and
// amd64_pop_rm_*. (No pushw $imm here: it is not compiled, and a block that
// falls back is interpreted onward -- the interpreter would run the POPs.)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, long k, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%ld): %#llx, want %#llx\n", what, k,
               (unsigned long long) got, (unsigned long long) want);
}

// call a function that returns with `ret $N` after the caller pushed N bytes
#define RET_IMM(n) \
    static uint64_t ret_imm_##n(void) { \
        uint64_t before, after; \
        __asm__ volatile("mov %%rsp, %0\n sub $" #n ", %%rsp\n call 1f\n jmp 2f\n" \
                         "1: ret $" #n "\n2: mov %%rsp, %1" : "=&r"(before), "=&r"(after) :: "memory"); \
        return before - after; }
RET_IMM(0)
RET_IMM(8)
RET_IMM(24)
RET_IMM(4096)
RET_IMM(65528)

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
extern char pop_fault_w[], pop_fault_r[];

int main(void) {
    check("ret $0", 0, ret_imm_0(), 0);
    check("ret $8", 8, ret_imm_8(), 0);
    check("ret $24", 24, ret_imm_24(), 0);
    check("ret $4096", 4096, ret_imm_4096(), 0);
    check("ret $65528", 65528, ret_imm_65528(), 0);

    uint64_t slot[4] = {0}, r, rsp0, rsp1;
    // popq (%rbx)
    __asm__ volatile("mov %%rsp, %1\n pushq $0x1234567\n popq (%3)\n mov %%rsp, %2"
                     : "+m"(slot), "=&r"(rsp0), "=&r"(rsp1) : "r"(slot) : "memory");
    check("popq (mem)", 0, slot[0], 0x1234567);
    check("popq (mem): rsp back", 0, rsp1, rsp0);
    // popq 8(%rsp): the address uses the advanced RSP -- after the pop, the
    // slot 8 above the new RSP is the one 16 above the old
    __asm__ volatile("pushq $3\n pushq $2\n pushq $1\n popq 8(%%rsp)\n popq %0\n popq %1"
                     : "=r"(r), "=r"(rsp1) :: "memory");
    check("popq 8(%rsp): stored at the advanced RSP + 8", 0, r, 2);
    check("popq 8(%rsp): the slot above", 0, rsp1, 1);
    // 16-bit to memory and to a register
    uint16_t w[2] = {0xaaaa, 0xbbbb};
    __asm__ volatile("mov %%rsp, %1\n sub $2, %%rsp\n movw $0x7788, (%%rsp)\n popw (%3)\n mov %%rsp, %2"
                     : "+m"(w), "=&r"(rsp0), "=&r"(rsp1) : "r"(w) : "memory");
    check("popw (mem)", 0, w[0], 0x7788);
    check("popw (mem): neighbour", 0, w[1], 0xbbbb);
    check("popw (mem): rsp back", 0, rsp1, rsp0);
    r = 0x1111222233334444ull;
    __asm__ volatile("sub $2, %%rsp\n movw $0x5566, (%%rsp)\n .byte 0x66, 0x8f, 0xc0" : "+a"(r) :: "memory");   // popw %ax via 8F /0
    check("popw %ax (8F): keeps 16-63", 0, r, 0x1111222233335566ull);
    __asm__ volatile("pushq $0x42\n .byte 0x8f, 0xc0" : "=a"(r) :: "memory");        // popq %rax via 8F /0
    check("popq %rax (8F)", 0, r, 0x42);
    // pop %rsp via 8F: RSP = the popped value
    __asm__ volatile("mov %%rsp, %1\n lea -64(%%rsp), %%rax\n push %%rax\n .byte 0x8f, 0xc4\n mov %%rsp, %0\n mov %1, %%rsp"
                     : "=&r"(r), "=&r"(rsp0) :: "rax", "memory");
    check("pop %rsp (8F)", 0, rsp0 - r, 64);
    // a destination page never touched
    for (int k = 0; k < 8; k++) {
        uint64_t *fresh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t v = 0xabcd0000ull + k;
        __asm__ volatile("mov %%rsp, %0\n push %2\n popq (%3)\n sub %%rsp, %0" : "=&r"(rsp0), "=m"(*fresh) : "r"(v), "r"(fresh) : "memory");
        check("popq into a fresh page", k, fresh[0], v);
        check("popq into a fresh page: rsp back", k, rsp0, 0);
        munmap(fresh, 4096);
    }
    // faults
    static char alt[65536];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof(alt) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    uint8_t *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *hole = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    {
        static volatile uint64_t sp_before;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("push $5\n mov %%rsp, (%1)\n .globl pop_fault_w\n pop_fault_w: popq (%0)"
                             :: "r"(ro), "r"(&sp_before) : "memory");
        check("popq to read-only: SIGSEGV there", 0, f_addr, (uintptr_t) ro);
        check("popq to read-only: rip", 0, f_pc, (uintptr_t) pop_fault_w);
        check("popq to read-only: rsp unchanged", 0, f_rsp, sp_before);
    }
    {
        uint64_t saved;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov %%rsp, %0\n mov %1, %%rsp\n .globl pop_fault_r\n pop_fault_r: popq (%2)\n mov %0, %%rsp"
                             : "=&r"(saved) : "r"(hole + 64), "r"(slot) : "memory");
        check("popq from an unreadable stack: SIGSEGV there", 0, f_addr, (uintptr_t) hole + 64);
        check("popq from an unreadable stack: rip", 0, f_pc, (uintptr_t) pop_fault_r);
        check("popq from an unreadable stack: rsp unchanged", 0, f_rsp, (uintptr_t) hole + 64);
    }
    printf("amd64_ret_pop: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
