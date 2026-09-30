// The non-temporal stores MOVNTPS (0F 2B), MOVNTPD (66 0F 2B), MOVNTDQ
// (66 0F E7) and MOVNTI (0F C3, and its REX.W form) must store their value --
// MOVNTPS, MOVNTPD and MOVNTI were SIGILL on both x86 engines while real
// hardware (camd) runs them -- and their register forms, which do not exist,
// must stay #UD (SIGILL, ILL_ILLOPN) as on hardware.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static sigjmp_buf jb;
static volatile int sig;
static void on_sig(int s) { sig = s; siglongjmp(jb, 1); }

static int fails;
#if defined(__SSE__)
#define XMM0_CLOBBER "xmm0",
#else
#define XMM0_CLOBBER   // i386 built without -msse2: gcc refuses the name
#endif
#define STORE(name, text, check) do { \
    memset(dst, 0, sizeof(dst)); sig = 0; \
    if (!sigsetjmp(jb, 1)) \
        __asm__ volatile(text :: "r" (src), "r" (dst) : XMM0_CLOBBER "memory", "eax"); \
    int ok = !sig && (check); \
    printf("%-10s %s (signal %d)\n", name, ok ? "ok" : "FAIL", sig); \
    fails += !ok; \
} while (0)

// A register form, run from a code page so the assembler need not accept it.
static void regform(const char *name, const unsigned char *bytes, int len) {
    unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(code, bytes, len);
    code[len] = 0xc3;
    sig = 0;
    if (!sigsetjmp(jb, 1))
        ((void (*)(void)) code)();
    int ok = sig == SIGILL;
    printf("%-10s %s (signal %d, want SIGILL)\n", name, ok ? "ok" : "FAIL", sig);
    fails += !ok;
    munmap(code, 4096);
}

int main(void) {
    signal(SIGILL, on_sig);
    signal(SIGSEGV, on_sig);
    static uint32_t src[4] __attribute__((aligned(16))) = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    static uint32_t dst[4] __attribute__((aligned(16)));
    STORE("movntps", "movaps (%0), %%xmm0\n movntps %%xmm0, (%1)", !memcmp(dst, src, 16));
    STORE("movntpd", "movapd (%0), %%xmm0\n movntpd %%xmm0, (%1)", !memcmp(dst, src, 16));
    STORE("movntdq", "movdqa (%0), %%xmm0\n movntdq %%xmm0, (%1)", !memcmp(dst, src, 16));
    STORE("movnti", "movl $0x55667788, %%eax\n movnti %%eax, (%1)", dst[0] == 0x55667788 && dst[1] == 0);
#if defined(__x86_64__)
    {
        uint64_t v = 0x0123456789abcdefull, out[2] = {0, 0};
        sig = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("movnti %1, (%0)" :: "r" (out), "r" (v) : "memory");
        int ok = !sig && out[0] == v && out[1] == 0;
        printf("%-10s %s (signal %d)\n", "movnti r64", ok ? "ok" : "FAIL", sig);
        fails += !ok;
    }
#endif
    static const unsigned char movntps_reg[] = { 0x0f, 0x2b, 0xc0 };
    static const unsigned char movnti_reg[] = { 0x0f, 0xc3, 0xc0 };
    regform("movntps xmm,xmm", movntps_reg, 3);
    regform("movnti reg,reg", movnti_reg, 3);
    printf("movnt_stores: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
