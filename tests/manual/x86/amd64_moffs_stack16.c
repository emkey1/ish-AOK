// amd64_moffs_stack16.c -- MOV AL/AX/EAX/RAX <-> [moffs] (A0-A3) with a 64-bit
// offset, a 32-bit one (0x67) and %gs, and the 16-bit stack forms: PUSH/POP
// r16, PUSH imm16 and imm8 (sign-extended to 16), POP %sp, LEAVE with 0x66, and
// PUSH/POP with REX.W. Against the SDM: an 8- or 16-bit register load keeps the
// rest of the register, a 32-bit one zero-extends; a 16-bit push or pop moves
// RSP by 2; `popw %sp` loads SP after the +2; REX.W wins over 0x66. A moffs
// load from an unmapped page faults at the instruction. The offsets are
// assembly-time constants into a mapping fixed at 0x50000000. Gadgets: math.S's
// amd64_mov_load*/store* (fed the offset), amd64_push16_*, amd64_pop16_reg,
// amd64_leave16.
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

static sigjmp_buf jb;
static volatile uintptr_t f_addr, f_pc;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = ((ucontext_t *) ctx)->uc_mcontext.gregs[REG_RIP];
    siglongjmp(jb, 1);
}
extern char moffs_fault_insn[];

#define BASE 0x50000000ul
#define PAT 0x8877665544332211ull

int main(void) {
    uint64_t *m = mmap((void *) BASE, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *) BASE) {
        printf("amd64_moffs_stack16: SKIP (no fixed mapping)\n");
        return 0;
    }
    uint64_t r;
    m[0] = PAT;
    // loads
    r = 0xaaaaaaaaaaaaaaaaull;
    __asm__ volatile("movabs 0x50000000, %%al" : "+a"(r));
    check("A0 al <- moffs64 (keeps 8-63)", r, 0xaaaaaaaaaaaaaa11ull);
    r = 0xaaaaaaaaaaaaaaaaull;
    __asm__ volatile("movabs 0x50000000, %%ax" : "+a"(r));
    check("66 A1 ax <- moffs64 (keeps 16-63)", r, 0xaaaaaaaaaaaa2211ull);
    r = 0xaaaaaaaaaaaaaaaaull;
    __asm__ volatile("movabs 0x50000000, %%eax" : "+a"(r));
    check("A1 eax <- moffs64 (zero-extends)", r, 0x44332211ull);
    __asm__ volatile("movabs 0x50000000, %%rax" : "=a"(r));
    check("48 A1 rax <- moffs64", r, PAT);
    r = 0xaaaaaaaaaaaaaaaaull;
    __asm__ volatile(".byte 0x67, 0xa1\n .long 0x50000004" : "+a"(r));   // mov eax, [moffs32]
    check("67 A1 eax <- moffs32", r, 0x88776655ull);
    // stores
    m[1] = 0;
    __asm__ volatile("movabs %%al, 0x50000008" :: "a"(0x1234567890abcdefull) : "memory");
    check("A2 moffs64 <- al", m[1], 0xef);
    __asm__ volatile("movabs %%ax, 0x5000000a" :: "a"(0x1234567890abcdefull) : "memory");
    check("66 A3 moffs64 <- ax", m[1], 0xcdef00efull);
    m[1] = 0;
    __asm__ volatile("movabs %%eax, 0x50000008" :: "a"(0x1234567890abcdefull) : "memory");
    check("A3 moffs64 <- eax", m[1], 0x90abcdefull);
    __asm__ volatile("movabs %%rax, 0x50000008" :: "a"(0x1234567890abcdefull) : "memory");
    check("48 A3 moffs64 <- rax", m[1], 0x1234567890abcdefull);
    m[2] = 0;
    __asm__ volatile(".byte 0x67, 0x48, 0xa3\n .long 0x50000010" :: "a"(0x55aa55aa55aa55aaull) : "memory");
    check("67 48 A3 moffs32 <- rax", m[2], 0x55aa55aa55aa55aaull);
    // %gs-relative
    if (syscall(SYS_arch_prctl, 0x1001 /* ARCH_SET_GS */, BASE - 0x100) == 0) {
        __asm__ volatile(".byte 0x65, 0x48, 0xa1\n .quad 0x100" : "=a"(r));   // mov rax, gs:[0x100]
        check("65 48 A1 rax <- gs:moffs", r, PAT);
        __asm__ volatile(".byte 0x65, 0x48, 0xa3\n .quad 0x118" :: "a"(0x77ull) : "memory");
        check("65 48 A3 gs:moffs <- rax", m[3], 0x77);
    }
    // a fault
    munmap((void *) (BASE + 4096), 4096);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    f_addr = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile(".globl moffs_fault_insn\nmoffs_fault_insn: movabs 0x50001008, %%rax" ::: "rax");
    check("moffs fault: si_addr", f_addr, BASE + 4096 + 8);
    check("moffs fault: rip", f_pc, (uintptr_t) moffs_fault_insn);

    // 16-bit stack forms
    uint64_t sp0, sp1, b = 0x1111222233334444ull, c = 0x5555666677778888ull;
    __asm__ volatile("mov %%rsp, %0\n pushw %w2\n popw %w3\n mov %%rsp, %1"
                     : "=&r"(sp0), "=&r"(sp1), "+r"(b), "+r"(c) :: "memory");
    check("pushw/popw r16: rsp back", sp1, sp0);
    check("popw r16: low word, the rest kept", c, 0x5555666677774444ull);
    __asm__ volatile("mov %%rsp, %0\n pushw $0x1234\n mov %%rsp, %1\n movzwl (%%rsp), %%eax\n add $2, %%rsp\n mov %%rax, %2"
                     : "=&r"(sp0), "=&r"(sp1), "=&r"(r) :: "rax", "memory");
    check("pushw imm16: rsp -2", sp0 - sp1, 2);
    check("pushw imm16: value", r, 0x1234);
    __asm__ volatile("pushw $-3\n movzwl (%%rsp), %%eax\n add $2, %%rsp\n mov %%rax, %0" : "=r"(r) :: "rax", "memory");
    check("pushw imm8: sign-extended to 16", r, 0xfffd);
    // exactly two bytes: the word above the pushed one is untouched
    __asm__ volatile("sub $8, %%rsp\n movabs $0x1122334455667788, %%rax\n mov %%rax, (%%rsp)\n"
                     "pushw $0x1234\n pushw %%ax\n movzwl 2(%%rsp), %k1\n mov 4(%%rsp), %%rax\n add $12, %%rsp\n mov %%rax, %0"
                     : "=r"(r), "=r"(sp0) :: "rax", "memory");
    check("pushw: two bytes, nothing above", r, 0x1122334455667788ull);
    check("pushw r16: two bytes, the word above kept", sp0, 0x1234);
    // popw %sp: SP = the popped word, upper 48 of RSP kept, after the +2
    __asm__ volatile("mov %%rsp, %0\n mov %%rsp, %%rax\n sub $0x40, %%ax\n sub $2, %%rsp\n mov %%ax, (%%rsp)\n"
                     "popw %%sp\n mov %%rsp, %1\n mov %0, %%rsp"
                     : "=&r"(sp0), "=&r"(sp1) :: "rax", "memory");
    check("popw %sp", sp1, (sp0 & ~0xffffull) | ((sp0 - 0x40) & 0xffff));
    // leavew: rsp = rbp + 2, bp = [rbp] (low word), rbp's upper bits kept.
    // Fixed registers: at -O2 an "r" operand can be %rbp itself.
    {
        static uint64_t res[3];
        __asm__ volatile("mov %%rsp, %%rbx\n push %%rbp\n sub $16, %%rsp\n mov %%rsp, %%rbp\n"
                         "movw $0xbeef, (%%rsp)\n leavew\n mov %%rsp, (%%rdi)\n mov %%rbp, 8(%%rdi)\n"
                         "lea -8(%%rbx), %%rsp\n pop %%rbp\n mov %%rbx, 16(%%rdi)"
                         :: "D"(res) : "rbx", "memory");
        check("leavew: rsp", res[0], res[2] - 8 - 16 + 2);
        check("leavew: bp", res[1] & 0xffff, 0xbeef);
        check("leavew: rbp upper", res[1] >> 16, (res[2] - 8 - 16) >> 16);
    }
    // REX.W wins over 0x66: a 64-bit push and pop
    __asm__ volatile("mov %%rsp, %0\n .byte 0x66, 0x48, 0x53\n mov %%rsp, %1\n .byte 0x66, 0x48, 0x59\n"
                     : "=&r"(sp0), "=&r"(sp1), "=c"(c) : "b"(0x0102030405060708ull) : "memory");
    check("66 48 push: rsp -8", sp0 - sp1, 8);
    check("66 48 pop: 64-bit", c, 0x0102030405060708ull);
    printf("amd64_moffs_stack16: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
