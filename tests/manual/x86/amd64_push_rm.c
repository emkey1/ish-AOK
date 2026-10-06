// amd64_push_rm.c -- PUSH r/m (FF /6): a memory operand (RIP-relative, as PLT
// stubs and position-independent code use it; [rsp], whose address is taken
// before RSP moves; base+index+disp; %gs), 16-bit with 0x66, and the register
// form FF F0+r. RSP moves by 8 (or 2) only once both accesses worked: an
// unreadable operand and an unwritable stack each fault at the instruction
// with RSP unchanged. The gadget is math.S's amd64_push_rm_mem64/16.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s: %#llx, want %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
}
__attribute__((visibility("hidden"), used)) uint64_t ripvar = 0x1122334455667788ull;
static uint64_t table[8] = {10, 11, 12, 13, 14, 15, 16, 17};

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
extern char pushrm_src_fault[], pushrm_dst_fault[];

int main(void) {
    uint64_t r, sp0, sp1;
    __asm__ volatile("mov %%rsp, %1\n pushq ripvar(%%rip)\n mov %%rsp, %2\n pop %0"
                     : "=r"(r), "=&r"(sp0), "=&r"(sp1) :: "memory");
    check("push rip-relative", r, 0x1122334455667788ull);
    check("push: rsp - 8", sp0 - sp1, 8);
    // push (%rsp): the address before RSP moves
    __asm__ volatile("pushq $0x55\n pushq (%%rsp)\n pop %0\n pop %%rax" : "=r"(r) :: "rax", "memory");
    check("push (%rsp)", r, 0x55);
    __asm__ volatile("pushq $7\n pushq $8\n pushq 8(%%rsp)\n pop %0\n add $16, %%rsp" : "=r"(r) :: "memory");
    check("push 8(%rsp)", r, 7);
    // base + index * 8 + disp
    uint64_t idx = 5;
    __asm__ volatile("pushq 8(%1, %2, 8)\n pop %0" : "=r"(r) : "r"(table), "r"(idx) : "memory");
    check("push 8(base,idx,8)", r, 16);
    // 16-bit
    uint16_t w = 0xbeef;
    __asm__ volatile("mov %%rsp, %1\n pushw (%3)\n mov %%rsp, %2\n movzwl (%%rsp), %k0\n add $2, %%rsp"
                     : "=&r"(r), "=&r"(sp0), "=&r"(sp1) : "r"(&w) : "memory");
    check("pushw (mem)", r, 0xbeef);
    check("pushw: rsp - 2", sp0 - sp1, 2);
    // the register form, FF /6 mod 3 (assemblers pick 50+r; encode it by hand)
    __asm__ volatile("mov $0x99, %%rcx\n .byte 0xff, 0xf1\n pop %0" : "=r"(r) :: "rcx", "memory");
    check("push %rcx via FF F1", r, 0x99);
    // %gs
    if (syscall(SYS_arch_prctl, 0x1001, (unsigned long) table) == 0) {
        __asm__ volatile("pushq %%gs:16\n pop %0" : "=r"(r) :: "memory");
        check("push %gs:16", r, 12);
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
    uint8_t *hole = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    {
        static volatile uint64_t sp_before;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov %%rsp, (%1)\n .globl pushrm_src_fault\n pushrm_src_fault: pushq (%0)\n pop %%rax"
                             :: "r"(hole + 8), "r"(&sp_before) : "rax", "memory");
        check("unreadable operand: SIGSEGV there", f_addr, (uintptr_t) hole + 8);
        check("unreadable operand: rip", f_pc, (uintptr_t) pushrm_src_fault);
        check("unreadable operand: rsp unchanged", f_rsp, sp_before);
    }
    {
        uint64_t saved;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov %%rsp, %0\n mov %1, %%rsp\n .globl pushrm_dst_fault\n pushrm_dst_fault: pushq ripvar(%%rip)\n mov %0, %%rsp"
                             : "=&r"(saved) : "r"(hole + 8) : "memory");
        check("unwritable stack: SIGSEGV at the slot", f_addr, (uintptr_t) hole);
        check("unwritable stack: rip", f_pc, (uintptr_t) pushrm_dst_fault);
        check("unwritable stack: rsp unchanged", f_rsp, (uintptr_t) hole + 8);
    }
    printf("amd64_push_rm: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
