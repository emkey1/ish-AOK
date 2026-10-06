// amd64_mxcsr.c -- LDMXCSR / STMXCSR (0F AE /2 /3): a reserved bit is #GP(0)
// (SIGSEGV, SI_KERNEL, si_addr NULL) with MXCSR unchanged; any other value
// reads back as written, flags included; the rounding mode reaches the
// arithmetic (1/3 rounds differently up and down); flags the arithmetic
// raises accumulate until LDMXCSR replaces them; FXSAVE's MXCSR_MASK says
// which bits are writable (0xffff here -- AMD adds its MM bit 17).
// Checked against real hardware (an AMD Ryzen). The gadgets are math.S's
// amd64_ldmxcsr_m / amd64_stmxcsr_m.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s: %#llx, want %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
}
static sigjmp_buf jb;
static volatile int sig, code;
static void *volatile addr;
static void on_segv(int s, siginfo_t *si, void *ctx) {
    (void) ctx;
    sig = s; code = si->si_code; addr = si->si_addr;
    siglongjmp(jb, 1);
}
static uint32_t st(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v) :: "memory"); return v; }
static void ld(uint32_t v) { __asm__ volatile("ldmxcsr %0" :: "m"(v) : "memory"); }

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    static const uint32_t ok[] = {0x1f80, 0x0000, 0xffff, 0x1fc0, 0x9f80, 0x3f80, 0x5f80, 0x7f80, 0x1fbf};
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        ld(ok[i]);
        check("round trip", st(), ok[i]);
    }
    uint8_t fx[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave %0" : "=m"(fx));
    uint32_t mask;
    memcpy(&mask, fx + 28, 4);
    ld(0x1f80);
    for (int b = 16; b < 32; b++) {
        uint32_t v = 0x1f80u | (1u << b);
        if (mask & (1u << b))
            continue;                    // AMD's MM (17) is writable there
        sig = 0;
        if (!sigsetjmp(jb, 1))
            ld(v);
        check("reserved bit: SIGSEGV", sig, SIGSEGV);
        check("reserved bit: SI_KERNEL", code, 0x80);
        check("reserved bit: si_addr", (uint64_t) (uintptr_t) addr, 0);
        check("reserved bit: MXCSR unchanged", st(), 0x1f80);
    }

    // the rounding mode reaches the arithmetic: 1/3 in single
    volatile float one = 1.0f, three = 3.0f;
    volatile float up, down;
    ld(0x5f80);                          // up
    up = one / three;
    ld(0x3f80);                          // down
    down = one / three;
    ld(0x1f80);
    uint32_t ub, db;
    float u = up, d = down;
    memcpy(&ub, &u, 4); memcpy(&db, &d, 4);
    check("RC up - RC down", ub - db, 1);

    // flags accumulate (PE from 1/3, then ZE from 1/0) and LDMXCSR clears them
    ld(0x1f80);
    volatile float zero = 0.0f, r = one / three;
    (void) r;
    check("PE raised", st() & 0x3f, 0x20);
    r = one / zero;
    check("ZE joins PE", st() & 0x3f, 0x24);
    ld(0x1f80);
    check("LDMXCSR replaces the flags", st() & 0x3f, 0);

    __asm__ volatile("fxsave %0" : "=m"(fx) :: "memory");
    uint32_t saved;
    memcpy(&saved, fx + 24, 4);
    check("MXCSR_MASK (bit 17 is AMD's MM)", mask & ~0x20000u, 0xffff);
    check("FXSAVE's MXCSR", saved, 0x1f80);

    printf("amd64_mxcsr: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
