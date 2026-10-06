// x87_env.c -- the x87's environment and state images, against what real
// hardware (an AMD Ryzen, camd, -m32 and 64-bit) writes and reads:
//
//  * FNSTENV (28 bytes; 14 with an operand-size prefix): control, status and
//    the full tag word (11 empty, 01 zero, 10 special, 00 valid, by physical
//    register), the high halves of the 32-bit form's words FFFF. It then
//    masks every exception, and ES and B clear with them.
//  * FNSAVE (108; 94): the environment and the eight registers in stack
//    order, then FNINIT.
//  * FLDENV, FRSTOR: control (only the bits that exist: v & 1F3F | 40),
//    status, and the tag word's empty registers; ES recomputed.
//  * FXSAVE: 160 bytes plus the mode's XMM registers (8 or 16) and nothing
//    else of the 512; the abridged tag by physical register; the registers in
//    stack order; MXCSR_MASK. FXRSTOR reads them back (ES recomputed) and is
//    #GP for a reserved MXCSR bit; both are #GP for an area not 16-byte
//    aligned.
//  * FLDCW keeps the bits that exist and recomputes ES; FNINIT; FNCLEX.
//  * Arithmetic leaves C0, C2 and C3 as they were.
//
// The instruction and data pointers and the opcode, which AOK does not keep,
// are not compared.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("FAIL %s: %#llx, want %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
}
static void checkb(const char *what, const uint8_t *got, const uint8_t *want, int n) {
    for (int i = 0; i < n; i++) {
        checks++;
        if (got[i] != want[i] && bad++ < 40)
            printf("FAIL %s byte %d: %02x, want %02x\n", what, i, got[i], want[i]);
    }
}
static uint16_t sw(void) { uint16_t s; __asm__ volatile("fnstsw %0" : "=m"(s)); return s; }
static uint16_t cw(void) { uint16_t c; __asm__ volatile("fnstcw %0" : "=m"(c)); return c; }
static void ldcw(uint16_t c) { __asm__ volatile("fldcw %0" :: "m"(c)); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static sigjmp_buf jb;
static volatile int sig;
static void on_segv(int s) { sig = s; siglongjmp(jb, 1); }

// The state every image test starts from: 1, 0, pi, +inf (1/0, ZE, masked):
// TOP 4, physical 4-7 valid with tags 10 (inf), 00 (pi), 01 (0), 00 (1).
static void state(void) {
    __asm__ volatile("fninit\n fld1\n fldz\n fldpi\n fld1\n fldz\n fdivrp" ::: "memory");
}
static const uint8_t st_inf[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x7f};
static const uint8_t st_pi[10] = {0x35, 0xc2, 0x68, 0x21, 0xa2, 0xda, 0x0f, 0xc9, 0x00, 0x40};
static const uint8_t st_zero[10] = {0};
static const uint8_t st_one[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};

int main(void) {
    static uint8_t b[1024] __attribute__((aligned(64)));
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    // FNSTENV, 32-bit
    state();
    memset(b, 0xaa, sizeof b);
    __asm__ volatile("fnstenv %0" : "=m"(*(uint8_t (*)[28]) b));
    check("fnstenv cw", rd32(b), 0xffff037f);
    check("fnstenv sw", rd32(b + 4), 0xffff2004);
    check("fnstenv tag", rd32(b + 8), 0xffff12ff);
    check("fnstenv fds", rd32(b + 24) >> 16, 0xffff);
    check("fnstenv: nothing past 28", b[28], 0xaa);
    check("after fnstenv: cw", cw(), 0x037f);
    check("after fnstenv: sw", sw(), 0x2004);
    // ... and 16-bit
    state();
    memset(b, 0xaa, sizeof b);
    __asm__ volatile(".byte 0x66\n fnstenv %0" : "=m"(*(uint8_t (*)[28]) b));
    check("fnstenv16 cw", rd16(b), 0x037f);
    check("fnstenv16 sw", rd16(b + 2), 0x2004);
    check("fnstenv16 tag", rd16(b + 4), 0x12ff);
    check("fnstenv16: nothing past 14", b[14], 0xaa);

    // FNSAVE, both widths: the registers in stack order, then FNINIT
    for (int w16 = 0; w16 < 2; w16++) {
        state();
        memset(b, 0xaa, sizeof b);
        if (w16)
            __asm__ volatile(".byte 0x66\n fnsave %0" : "=m"(*(uint8_t (*)[108]) b));
        else
            __asm__ volatile("fnsave %0" : "=m"(*(uint8_t (*)[108]) b));
        int r = w16 ? 14 : 28, end = r + 80;
        check(w16 ? "fnsave16 tag" : "fnsave tag", w16 ? rd16(b + 4) : rd32(b + 8), w16 ? 0x12ff : 0xffff12ff);
        checkb("fnsave st0", b + r, st_inf, 10);
        checkb("fnsave st1", b + r + 10, st_pi, 10);
        checkb("fnsave st2", b + r + 20, st_zero, 10);
        checkb("fnsave st3", b + r + 30, st_one, 10);
        check("fnsave: nothing past the area", b[end], 0xaa);
        check("after fnsave: cw", cw(), 0x037f);
        check("after fnsave: sw", sw(), 0);
        uint8_t fx[512] __attribute__((aligned(16)));
        __asm__ volatile("fxsave %0" : "=m"(fx));
        check("after fnsave: all empty", fx[4], 0);
        // FRSTOR puts it all back
        if (w16)
            __asm__ volatile(".byte 0x66\n frstor %0" :: "m"(*(uint8_t (*)[108]) b));
        else
            __asm__ volatile("frstor %0" :: "m"(*(uint8_t (*)[108]) b));
        check("frstor: sw", sw(), 0x2004);
        __asm__ volatile("fxsave %0" : "=m"(fx));
        check("frstor: tag", fx[4], 0xf0);
        checkb("frstor: st1", fx + 48, st_pi, 10);
        __asm__ volatile("fninit");
    }

    // FXSAVE: what it writes
    for (int pass = 0; pass < 2; pass++) {
        state();
        memset(b, 0xaa, sizeof b);
        __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) b));
        check("fxsave cw", rd16(b), 0x037f);
        check("fxsave sw", rd16(b + 2), 0x2004);
        check("fxsave tag", b[4], 0xf0);
        check("fxsave byte 5", b[5], 0);
        check("fxsave mxcsr_mask (bit 17 is AMD's)", rd32(b + 28) & ~0x20000u, 0xffff);
        checkb("fxsave st0", b + 32, st_inf, 10);
        checkb("fxsave st1", b + 48, st_pi, 10);
        checkb("fxsave st3", b + 80, st_one, 10);
        for (int i = 0; i < 8; i++)
            for (int j = 10; j < 16; j++)
                check("fxsave register padding", b[32 + 16 * i + j], 0);
        int written = 160 + 16 * (sizeof(void *) == 8 ? 16 : 8);
        int untouched = 0;
        for (int i = written; i < 512; i++)
            untouched += b[i] == 0xaa;
        check("fxsave leaves the rest", untouched, 512 - written);
        check("fxsave: nothing past 512", b[512], 0xaa);
        __asm__ volatile("fninit");
    }

    // FLDCW: the bits that exist; ES follows the masks
    __asm__ volatile("fninit");
    ldcw(0x0000); check("fldcw 0000", cw(), 0x0040);
    __asm__ volatile("fninit");
    ldcw(0xffff); check("fldcw ffff", cw(), 0x1f7f);
    __asm__ volatile("fninit");
    ldcw(0xf0c0); check("fldcw f0c0", cw(), 0x1040);
    __asm__ volatile("fninit\n fldz\n fldz\n fdivrp\n fstp %%st(0)" ::: "memory");   // 0/0: IE, masked
    check("IE", sw(), 0x0001);
    ldcw(0x37e);
    check("unmasking IE sets ES and B", sw(), 0x8081);
    __asm__ volatile("fnstenv %0" : "=m"(*(uint8_t (*)[28]) b));
    check("fnstenv stores ES", rd32(b + 4) & 0xffff, 0x8081);
    check("fnstenv masks", cw(), 0x037f);
    check("fnstenv: ES clears with the masks", sw(), 0x0001);
    __asm__ volatile("fnclex");
    check("fnclex", sw(), 0);

    // FLDENV: control's real bits, status with ES recomputed, the tag's empties
    __asm__ volatile("fninit\n fld1\n fld1\n fld1" ::: "memory");
    __asm__ volatile("fnstenv %0" : "=m"(*(uint8_t (*)[28]) b));
    b[0] = 0x7f; b[1] = 0xe3;          // cw with reserved bits
    b[4] = 0x81; b[5] = 0x38;          // sw: IE, ES, TOP 7
    b[8] = 0xfc; b[9] = 0x3f;          // tag: physical 0 and 7 hold values
    __asm__ volatile("fldenv %0" :: "m"(*(uint8_t (*)[28]) b));
    check("fldenv cw", cw(), 0x037f);
    check("fldenv sw (ES recomputed)", sw(), 0x3801);
    __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) (b + 512)));
    check("fldenv tag", b[516], 0x81);
    __asm__ volatile("fnclex\n fninit");

    // FXRSTOR: status as given, ES recomputed, the abridged tag as given
    __asm__ volatile("fninit\n fld1" ::: "memory");
    __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) b));
    b[2] = 0x81; b[3] = 0x38; b[4] = 0x5a; b[0] = 0x7f; b[1] = 0xf3;
    __asm__ volatile("fxrstor %0" :: "m"(*(uint8_t (*)[512]) b));
    check("fxrstor cw", cw(), 0x137f);
    check("fxrstor sw", sw(), 0x3801);
    __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) (b + 512)));
    check("fxrstor tag", b[516], 0x5a);
    __asm__ volatile("fnclex\n fninit");

    // FXRSTOR with a reserved MXCSR bit, FXSAVE and FXRSTOR misaligned: #GP
    __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) b));
    b[26] = 1;                         // MXCSR bit 16
    sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile("fxrstor %0" :: "m"(*(uint8_t (*)[512]) b));
    check("fxrstor reserved mxcsr: SIGSEGV", sig, SIGSEGV);
    sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512]) (b + 8)));
    check("fxsave misaligned: SIGSEGV", sig, SIGSEGV);
    sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile("fxrstor %0" :: "m"(*(uint8_t (*)[512]) (b + 8)));
    check("fxrstor misaligned: SIGSEGV", sig, SIGSEGV);

    // Arithmetic leaves C0 C2 C3 (an unordered compare sets them)
    __asm__ volatile("fninit\n fldz\n fldz\n fdivrp\n ftst\n fstp %%st(0)\n fnclex\n fld1\n fld1\n faddp" ::: "memory");
    check("C0 C2 C3 through faddp", sw() & 0x4500, 0x4500);
    __asm__ volatile("fninit");

    printf("x87_env: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
