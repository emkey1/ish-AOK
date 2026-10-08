// x86_movbe.c -- MOVBE (0F 38 F0 /r, a load; F1 /r, a store), 16, 32 and
// (64-bit builds) 64 bits: the operand moved byte-reversed, a 16-bit load
// keeping the register's upper bits and a 32-bit one zeroing them, no flag
// changed, a register operand #UD, an operand across a page boundary, and a
// fault on an unmapped page before anything changes. Checked on an AMD
// Ryzen (camd) in 64- and 32-bit builds; CPUID leaf 1 ECX bit 22.
#define _GNU_SOURCE
#include <cpuid.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile int got_sig, skip;
static void on_sig(int sig, siginfo_t *si, void *ucv) {
    (void) si;
    got_sig = sig;
    ucontext_t *uc = ucv;
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] += skip;
#else
    uc->uc_mcontext.gregs[REG_EIP] += skip;
#endif
}

static uint16_t bswap16(uint16_t v) { return (uint16_t) (v >> 8 | v << 8); }

int main(void) {
    unsigned a, b, c, d;
    __cpuid(1, a, b, c, d);
    CHECK(c & (1u << 22), "CPUID leaf 1 ECX has no MOVBE bit");
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    uint8_t *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static const int offs[] = {0, 100, 4096 - 1, 4096 - 2, 4096 - 3, 4096 - 7};
    for (unsigned oi = 0; oi < sizeof offs / sizeof offs[0]; oi++) {
        uint8_t *p = page + offs[oi];
        for (int i = 0; i < 8; i++)
            p[i] = (uint8_t) (0x11 * (i + 1) + oi);
        uint32_t m32;
        uint16_t m16;
        memcpy(&m32, p, 4);
        memcpy(&m16, p, 2);
        // loads, flags kept (carry set going in)
        unsigned long r = (unsigned long) -1, fl;
        __asm__ volatile("stc\n movbe (%2), %w0\n pushf\n pop %1" : "+r"(r), "=r"(fl) : "r"(p) : "memory", "cc");
        CHECK(((unsigned long) (uint16_t) r) == bswap16(m16) && (r >> 16) == ((unsigned long) -1 >> 16) && (fl & 1),
              "movbe r16, m16 at %d: %#lx (flags %#lx)", offs[oi], r, fl);
        r = (unsigned long) -1;
        __asm__ volatile("clc\n movbe (%2), %k0\n pushf\n pop %1" : "+r"(r), "=r"(fl) : "r"(p) : "memory", "cc");
        CHECK(r == __builtin_bswap32(m32) && !(fl & 1), "movbe r32, m32 at %d: %#lx", offs[oi], r);
#if defined(__x86_64__)
        uint64_t m64, r64 = 0;
        memcpy(&m64, p, 8);
        __asm__ volatile("movbe (%1), %0" : "=r"(r64) : "r"(p) : "memory");
        CHECK(r64 == __builtin_bswap64(m64), "movbe r64, m64 at %d: %#llx", offs[oi], (unsigned long long) r64);
#endif
        // stores, the register unchanged
        uint8_t back[8];
        uint32_t v32 = 0xa1b2c3d4u;
        unsigned long keep = v32;
        __asm__ volatile("movbe %k1, (%0)" :: "r"(p), "r"(keep) : "memory");
        memcpy(back, p, 4);
        CHECK(back[0] == 0xa1 && back[1] == 0xb2 && back[2] == 0xc3 && back[3] == 0xd4 && keep == v32,
              "movbe m32, r32 at %d: %02x%02x%02x%02x", offs[oi], back[0], back[1], back[2], back[3]);
        p[2] = 0x5a;
        __asm__ volatile("movbe %w1, (%0)" :: "r"(p), "r"(keep) : "memory");
        CHECK(p[0] == 0xc3 && p[1] == 0xd4 && p[2] == 0x5a, "movbe m16, r16 at %d", offs[oi]);
#if defined(__x86_64__)
        uint64_t v64 = 0x0102030405060708ull;
        __asm__ volatile("movbe %1, (%0)" :: "r"(p), "r"(v64) : "memory");
        for (int i = 0; i < 8; i++)
            CHECK(p[i] == i + 1, "movbe m64, r64 at %d: byte %d is %02x", offs[oi], i, p[i]);
#endif
    }
    // a register operand: #UD (0F 38 F0 C0)
    got_sig = 0;
    skip = 4;
    __asm__ volatile(".byte 0x0f, 0x38, 0xf0, 0xc0" ::: "eax", "memory");
    CHECK(got_sig == SIGILL, "movbe with a register operand: signal %d", got_sig);
    // a fault on the second page before the register changes
    mprotect(page + 4096, 4096, PROT_NONE);
    got_sig = 0;
    skip = 4;                                   // movbe (%ecx/%rcx), %eax: 0f 38 f0 01
    unsigned long r = 0x12345678;
    uint8_t *p = page + 4096 - 2;
    __asm__ volatile(".byte 0x0f, 0x38, 0xf0, 0x01" : "+a"(r) : "c"(p) : "memory");
    CHECK(got_sig == SIGSEGV && r == 0x12345678, "movbe across into an unmapped page: signal %d, %#lx", got_sig, r);
    printf("x86_movbe: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
