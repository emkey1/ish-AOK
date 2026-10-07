// VEX.vvvv's top bit in 32-bit mode, as an AMD Ryzen has it (camd runs this
// natively): as a register it is ignored -- only xmm0-7 exist, and C4 E1 29
// FE CB (vvvv field 0101: register 10) is VPADDD xmm1, xmm2, xmm3 -- but
// where vvvv must be 1111b it counts: VMOVDQU or VPSHUFD with it clear is
// #UD. A raw encoding that runs is checked against the same instruction
// encoded normally. (A C5 cannot clear the bit: that byte would be LDS's
// ModRM.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(__i386__)
int main(void) { printf("i386_vex_vvvv3: SKIP (32-bit only)\n"); return 0; }
#else
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) { sig = s; siglongjmp(jb, 1); }
static uint8_t in[3][16] __attribute__((aligned(16))), out[2][16] __attribute__((aligned(16)));
#define CASE(name, want, raw, plain) do { \
    sig = 0; \
    memset(out, 0, sizeof(out)); \
    if (!sigsetjmp(jb, 1)) \
        __asm__ volatile("movdqu (%0), %%xmm1\n movdqu 16(%0), %%xmm2\n movdqu 32(%0), %%xmm3\n .byte " raw "\n" \
                         "movdqu %%xmm1, (%1)\n movdqu (%0), %%xmm1\n " plain "\n movdqu %%xmm1, 16(%1)" \
                         :: "r"(in), "r"(out) : "memory", "xmm1", "xmm2", "xmm3"); \
    checks++; \
    if ((sig != (want) || (!sig && memcmp(out[0], out[1], 16))) && bad++ < 20) \
        printf("FAIL %s: signal %d (want %d)%s\n", name, sig, want, sig ? "" : ", a different result"); \
} while (0)

static unsigned long checks, bad;       /* (static: kept across the longjmp) */

int main(void) {
    signal(SIGILL, handler);
    signal(SIGSEGV, handler);
    for (int i = 0; i < 48; i++)
        ((uint8_t *) in)[i] = (uint8_t) (i * 37 + 11);
    CASE("vpaddd, vvvv 1010", 0, "0xc4, 0xe1, 0x29, 0xfe, 0xcb", "vpaddd %%xmm3, %%xmm2, %%xmm1");
    CASE("vpxor, vvvv 1011", 0, "0xc4, 0xe1, 0x21, 0xef, 0xcb", "vpxor %%xmm3, %%xmm3, %%xmm1");
    CASE("vmovdqu, vvvv 1000", SIGILL, "0xc4, 0xe1, 0x3a, 0x6f, 0xcb", "vmovdqu %%xmm3, %%xmm1");
    CASE("vpshufd, vvvv 1000", SIGILL, "0xc4, 0xe1, 0x39, 0x70, 0xcb, 0x1b", "vpshufd $0x1b, %%xmm3, %%xmm1");
    printf("i386_vex_vvvv3: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
#endif
