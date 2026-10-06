// FF /2 /4 /6 shapes the JIT used to hand to C (amd64_jit_ff_group): CALL and
// JMP through memory with a 0x66 prefix (the TLS general-dynamic sequence's
// `data16 rex.W call *__tls_get_addr@GOTPCREL(%rip)`), with REX.W or not;
// through %fs- and %gs-relative memory; PUSH through %fs memory; and LOCK on
// each, which is #UD. Every answer checked on an AMD Ryzen (camd).
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static long target_hits;
__attribute__((noinline, used)) static long target(void) {
    target_hits++;
    return 0x1234;
}
static long (*volatile fp)(void) = target;
static __thread long (*tls_fp)(void);
static __thread unsigned long tls_word;
static unsigned long gs_area[4];

// Each asm that calls or pushes steps over the red zone first.
#define IN "sub $128, %%rsp\n"
#define OUT "\nadd $128, %%rsp"

static sigjmp_buf jb;
static volatile int got_sig;
static void on_sig(int sig) {
    got_sig = sig;
    siglongjmp(jb, 1);
}

int main(void) {
    long r;

    // 66 48 FF 15: data16 rex.W call *disp(%rip)
    target_hits = 0;
    __asm__ volatile(IN ".byte 0x66, 0x48, 0xff, 0x15\n .long fp - 1f\n 1:" OUT
                     : "=a"(r) : : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(r == 0x1234 && target_hits == 1, "66 48 ff 15 call: r=%lx hits=%ld", r, target_hits);

    // 66 FF 15 (no REX.W): AMD makes a near call 16-bit with 0x66, Intel
    // ignores it; AOK takes it as 64-bit (not run: it would crash on AMD)

    // 66 48 FF 25: data16 rex.W jmp *disp(%rip), returning through a call
    target_hits = 0;
    __asm__ volatile(IN "call 2f\n jmp 3f\n 2: .byte 0x66, 0x48, 0xff, 0x25\n .long fp - 1f\n 1:\n 3:" OUT
                     : "=a"(r) : : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(r == 0x1234 && target_hits == 1, "66 48 ff 25 jmp: r=%lx hits=%ld", r, target_hits);

    // register forms with 0x66 and REX.W
    target_hits = 0;
    __asm__ volatile(IN ".byte 0x66, 0x48, 0xff, 0xd1" OUT     // call *%rcx
                     : "=a"(r) : "c"(target) : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(r == 0x1234 && target_hits == 1, "66 48 ff d1 call: r=%lx", r);

    // %fs-relative call, jmp and push: the offset of a __thread variable from
    // the thread pointer
    tls_fp = target;
    tls_word = 0xfeedfacecafebeefUL;
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    long off_fp = (long) ((unsigned long) &tls_fp - tp), off_w = (long) ((unsigned long) &tls_word - tp);
    target_hits = 0;
    __asm__ volatile(IN "call *%%fs:(%%rcx)" OUT : "=a"(r) : "c"(off_fp) : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(r == 0x1234 && target_hits == 1, "call *%%fs:(%%rcx): r=%lx", r);
    target_hits = 0;
    __asm__ volatile(IN "call 2f\n jmp 3f\n 2: jmp *%%fs:(%%rcx)\n 3:" OUT : "=a"(r) : "c"(off_fp)
                     : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(r == 0x1234 && target_hits == 1, "jmp *%%fs:(%%rcx): r=%lx", r);
    unsigned long popped;
    __asm__ volatile(IN "pushq %%fs:(%%rcx)\n pop %0" OUT : "=r"(popped) : "c"(off_w) : "memory");
    CHECK(popped == 0xfeedfacecafebeefUL, "push %%fs:(%%rcx): %lx", popped);
    __asm__ volatile(IN ".byte 0x66, 0x64, 0x48, 0xff, 0x14, 0x0d\n .long 0" OUT : "=a"(r) : "c"(off_fp)
                     : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");   // data16 fs rex.W call *0(,%rcx)
    CHECK(r == 0x1234, "66 48 64 ff 14 0d call: r=%lx", r);

    // %gs-relative, with a base set by arch_prctl
    gs_area[1] = (unsigned long) target;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, gs_area) == 0) {
        target_hits = 0;
        __asm__ volatile(IN "call *%%gs:8" OUT : "=a"(r) : : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
        CHECK(r == 0x1234 && target_hits == 1, "call *%%gs:8: r=%lx", r);
        syscall(SYS_arch_prctl, ARCH_SET_GS, 0);
    } else {
        printf("arch_prctl(ARCH_SET_GS) failed: skipped the %%gs call\n");
    }

    // LOCK on CALL/JMP/PUSH, memory or register: #UD
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    static const char *const lname[] = {"lock call *fp(%rip)", "lock jmp *fp(%rip)", "lock push fp(%rip)", "lock call *%rcx"};
    for (int i = 0; i < 4; i++) {
        got_sig = 0;
        target_hits = 0;
        if (sigsetjmp(jb, 1) == 0) {
            switch (i) {
            case 0: __asm__ volatile(IN ".byte 0xf0, 0xff, 0x15\n .long fp - 1f\n 1:" OUT ::: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"); break;
            case 1: __asm__ volatile(IN "call 2f\n jmp 3f\n 2: .byte 0xf0, 0xff, 0x25\n .long fp - 1f\n 1:\n 3:" OUT ::: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"); break;
            case 2: __asm__ volatile(IN ".byte 0xf0, 0xff, 0x35\n .long fp - 1f\n 1:\n pop %%rax" OUT ::: "rax", "memory"); break;
            case 3: __asm__ volatile(IN ".byte 0xf0, 0xff, 0xd1" OUT :: "c"(target) : "rax", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"); break;
            }
        }
        CHECK(got_sig == SIGILL && target_hits == 0, "%s: signal %d, hits %ld", lname[i], got_sig, target_hits);
    }
    (void) fp;

    printf("amd64_ff_indirect: %s (%d checks, %d failures)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
