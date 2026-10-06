// 0F AE on amd64, the forms the JIT used to send to C (amd64_jit_0f_rm): LOCK
// and 0x66 on the fences, FXSAVE, LDMXCSR and STMXCSR are #UD; a segment
// prefix on a fence is ignored; CLFLUSH and CLFLUSHOPT (66) fault on a line
// that is not readable; XSAVE, XRSTOR, XSAVEOPT, RDFSBASE and CLWB run only
// where CPUID says so (#UD otherwise). Answers from an AMD Ryzen (camd), the
// feature-gated ones checked against the CPUID of the CPU running the test.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fails, checks;
static sigjmp_buf jb;
static volatile int sig;
static void h(int s) {
    sig = s;
    siglongjmp(jb, 1);
}
__attribute__((used, aligned(64))) static unsigned char area[1024];
__attribute__((used)) static uint32_t mx = 0x1f80;

#define T(name, want, code) do { \
    sig = 0; \
    if (sigsetjmp(jb, 1) == 0) \
        __asm__ volatile(code ::: "memory", "rax", "rcx", "rdx"); \
    checks++; \
    if (sig != (want)) { \
        fails++; \
        printf("FAIL: %s: signal %d, want %d\n", name, sig, want); \
    } \
} while (0)

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int osxsave = (c >> 27) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    int fsgsbase = b & 1, clwb = (b >> 24) & 1;

    T("lock mfence", SIGILL, ".byte 0xf0, 0x0f, 0xae, 0xf0");
    T("66 mfence", SIGILL, ".byte 0x66, 0x0f, 0xae, 0xf0");
    T("fs mfence", 0, ".byte 0x64, 0x0f, 0xae, 0xf0");
    T("gs lfence", 0, ".byte 0x65, 0x0f, 0xae, 0xe8");
    T("66 lfence", SIGILL, ".byte 0x66, 0x0f, 0xae, 0xe8");
    T("66 sfence (pcommit)", SIGILL, ".byte 0x66, 0x0f, 0xae, 0xf8");
    T("0f ae /4, a register", SIGILL, ".byte 0x0f, 0xae, 0xe0");
    T("66 fxsave", SIGILL, "lea area(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x00");
    T("66 fxrstor", SIGILL, "lea area(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x08");
    T("66 ldmxcsr", SIGILL, "lea mx(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x10");
    T("66 stmxcsr", SIGILL, "lea mx(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x18");
    T("lock fxsave", SIGILL, "lea area(%%rip), %%rax\n .byte 0xf0, 0x0f, 0xae, 0x00");
    T("lock ldmxcsr", SIGILL, "lea mx(%%rip), %%rax\n .byte 0xf0, 0x0f, 0xae, 0x10");
    T("f3 0f ae /2 (wrfsbase-like) mem", SIGILL, "lea mx(%%rip), %%rax\n .byte 0xf3, 0x0f, 0xae, 0x10");
    T("clflush", 0, "lea area(%%rip), %%rax\n clflush (%%rax)");
    T("clflushopt", 0, "lea area(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x38");
    T("fs clflush", 0, "xor %%eax, %%eax\n .byte 0x64, 0x0f, 0xae, 0x38");
    T("lock clflush", SIGILL, "lea area(%%rip), %%rax\n .byte 0xf0, 0x0f, 0xae, 0x38");
    T("clflush unmapped", SIGSEGV, "mov $0x10, %%eax\n clflush (%%rax)");

    unsigned char *none = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *ro = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile("clflush (%0)" :: "r"(none) : "memory");
    checks++;
    if (sig != SIGSEGV) { fails++; printf("FAIL: clflush PROT_NONE: signal %d\n", sig); }
    sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile("clflush (%0)" :: "r"(ro + 100) : "memory");
    checks++;
    if (sig != 0) { fails++; printf("FAIL: clflush PROT_READ: signal %d\n", sig); }

    int x = osxsave ? 0 : SIGILL;
    T("xsave", x, "lea area(%%rip), %%rcx\n xor %%eax, %%eax\n xor %%edx, %%edx\n .byte 0x0f, 0xae, 0x21");
    T("xrstor", x, "lea area(%%rip), %%rcx\n xor %%eax, %%eax\n xor %%edx, %%edx\n .byte 0x0f, 0xae, 0x29");
    T("xsaveopt", x, "lea area(%%rip), %%rcx\n xor %%eax, %%eax\n xor %%edx, %%edx\n .byte 0x0f, 0xae, 0x31");
    T("rdfsbase", fsgsbase ? 0 : SIGILL, ".byte 0xf3, 0x48, 0x0f, 0xae, 0xc0");
    T("clwb", clwb ? 0 : SIGILL, "lea area(%%rip), %%rax\n .byte 0x66, 0x0f, 0xae, 0x30");

    printf("amd64_0fae: %s (%d checks, %d failures)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
