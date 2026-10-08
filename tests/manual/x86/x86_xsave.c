// x86_xsave.c -- XSAVE and XRSTOR, the XSAVE state in a signal frame,
// ptrace's NT_X86_XSTATE, CPUID leaf 0x0D, AT_MINSIGSTKSZ and the altstack
// a frame that size needs, in 64- and 32-bit builds.
//
// The XSAVE image is the standard format at Intel's offsets (Sapphire
// Rapids': opmask 1088, ZMM_Hi256 1152, Hi16_ZMM 1664, 2688 bytes), XCR0
// x87|SSE|AVX|opmask|ZMM_Hi256|Hi16_ZMM. The expectations are the SDM's
// and hardware's: XSAVE writes only the requested components (MXCSR for SSE
// or AVX) and their XSTATE_BV bits; XRSTOR loads the requested components
// XSTATE_BV has and initializes the others, loads MXCSR for SSE or AVX,
// and #GPs -- before changing anything -- on XSTATE_BV outside XCR0,
// header bytes 8-23 not zero, an unaligned area or (SSE or AVX asked for)
// MXCSR's reserved bits; 32-bit code reaches registers 0-7 only. XINUSE,
// the XSTATE_BV bit XSAVE writes, is AOK's by value (a component not in its
// initial configuration); the SDM lets it be 1 for one that is, and Intel
// (SDE) tracks modification, so "sde" skips those bits -- and what SDE does
// not model: the x87 pointers, faults, the kernel's parts -- and checks the
// rest against Intel SDE 10.13.1 -spr (camd: `sde64 -spr -- ./x86_xsave
// sde`). The #GP rules are an AMD Ryzen's (camd), where SDE aborts. SDE's
// MXCSR_MASK has bit 17 set as well; AOK's, as its FXSAVE's, is 0xffff.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <cpuid.h>

#if defined(__x86_64__) && !defined(NO512)
#define NR 32
#define NX 16
#define BITS64 1
#elif defined(__x86_64__)
#define NR 16
#define NX 16
#define BITS64 1
#else
#define NR 8
#define NX 8
#define BITS64 0
#endif
#define XSIZE 2688
#ifdef NO512
// -DNO512, run natively on an AVX2 machine (camd's Ryzen: XCR0 7): the x87,
// SSE and AVX components against real hardware, which SDE does not model
// (it runs those parts of XSAVE/XRSTOR on the host -- its MXCSR_MASK is the
// AMD host's -- and loses x87 state across them). "sde" there too.
#define XCR0 0x7ull
#else
#define XCR0 0xe7ull
#endif

static int sde, failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; if (failures < 60) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// The routine's block: registers in, the instruction, registers out (FXSAVE
// for x87, MXCSR and the low XMMs). Offsets fixed for the assembly below.
struct ctl {
    uint8_t z_in[32][64];        // 0
    uint8_t z_out[32][64];       // 2048
    uint64_t k_in[8];            // 4096
    uint64_t k_out[8];           // 4160
    uint8_t fx_out[512];         // 4224
    uint8_t st_in[3][16];        // 4736
    uint32_t mask_lo, mask_hi;   // 4784
    uint32_t op;                 // 4792: 0 XSAVE, 1 XRSTOR, 2 nothing, 3 UD2 (a signal), 4 INT3 (a ptrace stop)
    uint16_t fcw, pad;           // 4796
    uint32_t mxcsr;              // 4800
    uint32_t nld;                // 4804
    uint64_t area;               // 4808
    uint32_t saved_mxcsr, pad2;  // 4816
} __attribute__((aligned(64)));
_Static_assert(__builtin_offsetof(struct ctl, area) == 4808, "ctl layout");
void xs_run(struct ctl *c);
void xs_capture(void);           // into xs_entry: what a signal handler starts with
static struct ctl xs_entry __attribute__((used));

#define S(x) #x
#define ZL(n, r) "vmovdqu64 " S(n) "*64(%" r "), %zmm" S(n) "\n"
#define ZS(n, r) "vmovdqu64 %zmm" S(n) ", 2048+" S(n) "*64(%" r ")\n"
#define ZC(n, r) "vmovdqu64 %zmm" S(n) ", 2048+" S(n) "*64(%" r ")\n"
#define KL(n, r) "kmovq 4096+" S(n) "*8(%" r "), %k" S(n) "\n"
#define KS(n, r) "kmovq %k" S(n) ", 4160+" S(n) "*8(%" r ")\n"
#define Z8(M, r) M(0, r) M(1, r) M(2, r) M(3, r) M(4, r) M(5, r) M(6, r) M(7, r)
#define Z32(M, r) Z8(M, r) M(8, r) M(9, r) M(10, r) M(11, r) M(12, r) M(13, r) M(14, r) M(15, r) \
    M(16, r) M(17, r) M(18, r) M(19, r) M(20, r) M(21, r) M(22, r) M(23, r) \
    M(24, r) M(25, r) M(26, r) M(27, r) M(28, r) M(29, r) M(30, r) M(31, r)
#ifdef NO512
#undef ZL
#undef ZS
#undef ZC
#undef KL
#undef KS
#define ZL(n, r) "vmovdqu " S(n) "*64(%" r "), %ymm" S(n) "\n"
#define ZS(n, r) "vmovdqu %ymm" S(n) ", 2048+" S(n) "*64(%" r ")\n"
#define ZC ZS
#define KL(n, r)
#define KS(n, r)
#undef Z32
#define Z32(M, r) Z8(M, r) M(8, r) M(9, r) M(10, r) M(11, r) M(12, r) M(13, r) M(14, r) M(15, r)
#endif
#if BITS64
#define ZALL Z32
#define CREG "rbx"
__asm__(".text\n.globl xs_run\nxs_run:\n"
        "push %rbx\n mov %rdi, %rbx\n"
        "stmxcsr 4816(%rbx)\n fninit\n fldcw 4796(%rbx)\n"
        "mov 4804(%rbx), %ecx\n lea 4736(%rbx), %rsi\n"
        "1: test %ecx, %ecx\n jz 2f\n fldt (%rsi)\n add $16, %rsi\n dec %ecx\n jmp 1b\n"
        "2: ldmxcsr 4800(%rbx)\n"
        Z32(ZL, "rbx") Z8(KL, "rbx")
        "mov 4784(%rbx), %eax\n mov 4788(%rbx), %edx\n mov 4808(%rbx), %rcx\n mov 4792(%rbx), %esi\n"
        "cmp $0, %esi\n jne 3f\n xsave (%rcx)\n jmp 5f\n"
        "3: cmp $1, %esi\n jne 4f\n xrstor (%rcx)\n jmp 5f\n"
        "4: cmp $3, %esi\n jne 6f\n ud2\n jmp 5f\n"
        "6: cmp $4, %esi\n jne 5f\n int3\n"
        "5:\n"
        Z32(ZS, "rbx") Z8(KS, "rbx")
        "fxsave 4224(%rbx)\n fninit\n ldmxcsr 4816(%rbx)\n vzeroupper\n"
        "pop %rbx\n ret\n"
        ".globl xs_capture\nxs_capture:\n"
        "push %rbx\n lea xs_entry(%rip), %rbx\n"
        Z32(ZC, "rbx") Z8(KS, "rbx") "fxsave 4224(%rbx)\n pop %rbx\n ret\n");
#else
#define ZALL Z8
__asm__(".text\n.globl xs_run\nxs_run:\n"
        "push %ebx\n push %esi\n mov 12(%esp), %ebx\n"
        "stmxcsr 4816(%ebx)\n fninit\n fldcw 4796(%ebx)\n"
        "mov 4804(%ebx), %ecx\n lea 4736(%ebx), %esi\n"
        "1: test %ecx, %ecx\n jz 2f\n fldt (%esi)\n add $16, %esi\n dec %ecx\n jmp 1b\n"
        "2: ldmxcsr 4800(%ebx)\n"
        Z8(ZL, "ebx") Z8(KL, "ebx")
        "mov 4784(%ebx), %eax\n mov 4788(%ebx), %edx\n mov 4808(%ebx), %ecx\n mov 4792(%ebx), %esi\n"
        "cmp $0, %esi\n jne 3f\n xsave (%ecx)\n jmp 5f\n"
        "3: cmp $1, %esi\n jne 4f\n xrstor (%ecx)\n jmp 5f\n"
        "4: cmp $3, %esi\n jne 6f\n ud2\n jmp 5f\n"
        "6: cmp $4, %esi\n jne 5f\n int3\n"
        "5:\n"
        Z8(ZS, "ebx") Z8(KS, "ebx")
        "fxsave 4224(%ebx)\n fninit\n ldmxcsr 4816(%ebx)\n vzeroupper\n"
        "pop %esi\n pop %ebx\n ret\n"
        ".globl xs_capture\nxs_capture:\n"
        "push %ebx\n call 9f\n9: pop %ebx\n add $xs_entry-9b, %ebx\n"
        Z8(ZC, "ebx") Z8(KS, "ebx") "fxsave 4224(%ebx)\n pop %ebx\n ret\n");
#endif

static uint64_t rng;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void rbytes(void *p, size_t n) { uint8_t *b = p; for (size_t i = 0; i < n; i++) b[i] = (uint8_t) rnd(); }

// Three normal x87 values, a control word, an MXCSR, random vector and
// opmask registers -- some components left zero (their initial
// configuration) so XINUSE has both answers.
static void random_state(struct ctl *c, uint64_t seed) {
    rng = seed * 0x9e3779b97f4a7c15ull + 1;
    memset(c, 0, sizeof(*c));
    unsigned zero = (unsigned) rnd();
    rbytes(c->z_in, sizeof(c->z_in));
    for (int r = 0; r < 32; r++) {
        if (zero & 1) memset(c->z_in[r] + 16, 0, 16);      // the YMM uppers
        if (zero & 2) memset(c->z_in[r] + 32, 0, 32);      // the ZMM uppers
        if ((zero & 4) && r >= 16) memset(c->z_in[r], 0, 64);
        if (zero & 8) memset(c->z_in[r], 0, 16);
    }
    if (!(zero & 16))
        rbytes(c->k_in, sizeof(c->k_in));
#ifdef NO512
    for (int r = 0; r < 32; r++)
        memset(c->z_in[r] + (r < 16 ? 32 : 0), 0, r < 16 ? 32 : 64);
    memset(c->k_in, 0, sizeof(c->k_in));
#endif
    c->nld = (zero & 32) ? 0 : 1 + (unsigned) (rnd() % 3);
    for (int i = 0; i < 3; i++) {
        uint64_t m = rnd() | (1ull << 63);
        uint16_t e = (uint16_t) (0x3fff + (int) (rnd() % 64) - 32) | (rnd() & 1 ? 0x8000 : 0);
        memcpy(c->st_in[i], &m, 8);
        memcpy(c->st_in[i] + 8, &e, 2);
    }
    static const uint16_t cws[] = {0x37f, 0x27f, 0x77f, 0xf7f};
    c->fcw = (zero & 32) ? 0x37f : cws[rnd() % 4];
    static const uint32_t mxs[] = {0x1f80, 0x3f80, 0x5f80, 0x9fc0, 0x1fbf};
    c->mxcsr = mxs[rnd() % 5];
}

// The register-file view of a component's bytes, for XSAVE's offsets.
static const uint8_t *reg_bytes(const uint8_t z[32][64], int reg, int lo) { return z[reg] + lo; }

// What XSAVE leaves in a 0xaa-filled area, byte for byte, from the state
// c loaded (z_in and so on); want_bv and bv_known the XSTATE_BV bits.
static void model_xsave(const struct ctl *c, uint64_t rfbm, uint8_t *exp, uint8_t *known) {
    memset(exp, 0xaa, XSIZE);
    memset(known, 1, XSIZE);
    rfbm &= XCR0;
    if (rfbm & 1) {
        unsigned top = (8 - c->nld) & 7;
        uint16_t fsw = (uint16_t) (top << 11);
        uint8_t ftw = 0;
        for (unsigned i = 0; i < c->nld; i++)
            ftw |= (uint8_t) (1u << ((top + i) & 7));
        memcpy(exp, &c->fcw, 2);
        memcpy(exp + 2, &fsw, 2);
        exp[4] = ftw;
        exp[5] = 0;
        memset(exp + 6, 0, 18);                 // FOP, FIP, FDP: 0 in AOK
        if (sde)
            memset(known + 6, 0, 18);
        memset(exp + 32, 0, 128);
        for (unsigned i = 0; i < c->nld; i++)   // ST(i): the last loaded first
            memcpy(exp + 32 + 16 * i, c->st_in[c->nld - 1 - i], 10);
        for (unsigned i = c->nld; i < 8; i++)  // empty registers: whatever they last held (FNINIT
            memset(known + 32 + 16 * i, 0, 16); // leaves them, on hardware too)
    }
    if (rfbm & 6) {
        memcpy(exp + 24, &c->mxcsr, 4);
        uint32_t mask = 0xffff;                 // MXCSR_MASK (SDE says 0x2ffff: bit 17, nothing AOK has)
        memcpy(exp + 28, &mask, 4);
        if (sde)
            memset(known + 28, 0, 4);
    }
    if (rfbm & 2)
        for (int r = 0; r < NX; r++)
            memcpy(exp + 160 + 16 * r, reg_bytes(c->z_in, r, 0), 16);
    uint64_t bv = 0xaaaaaaaaaaaaaaaaull & ~rfbm, inuse = 0;
    if (c->nld || c->fcw != 0x37f) inuse |= 1;
    int nz;
    nz = 0; for (int r = 0; r < NX; r++) for (int j = 0; j < 16; j++) nz |= c->z_in[r][j]; if (nz) inuse |= 2;
    nz = 0; for (int r = 0; r < NX; r++) for (int j = 16; j < 32; j++) nz |= c->z_in[r][j]; if (nz) inuse |= 4;
    nz = 0; for (int i = 0; i < 8; i++) nz |= c->k_in[i] != 0; if (nz) inuse |= 0x20;
    nz = 0; for (int r = 0; r < NX; r++) for (int j = 32; j < 64; j++) nz |= c->z_in[r][j]; if (nz) inuse |= 0x40;
    nz = 0; for (int r = 16; r < 32; r++) for (int j = 0; j < 64; j++) nz |= c->z_in[r][j]; if (nz) inuse |= 0x80;
    if (!BITS64)
        inuse &= ~0x80ull;                      // (registers 16-31 stay zero in 32-bit code)
    bv |= inuse & rfbm;
    memcpy(exp + 512, &bv, 8);
    if (sde)
        memset(known + 512, 0, 1);              // the low byte: every XCR0 bit
    if (rfbm & 4)
        for (int r = 0; r < NX; r++)
            memcpy(exp + 576 + 16 * r, reg_bytes(c->z_in, r, 16), 16);
    if (rfbm & 0x20)
        memcpy(exp + 1088, c->k_in, 64);
    if (rfbm & 0x40)
        for (int r = 0; r < NX; r++)
            memcpy(exp + 1152 + 32 * r, reg_bytes(c->z_in, r, 32), 32);
    if ((rfbm & 0x80) && BITS64)
        for (int r = 16; r < 32; r++)
            memcpy(exp + 1664 + 64 * (r - 16), c->z_in[r], 64);
}

static uint8_t *pages;                          // two pages, the second PROT_NONE when asked
static volatile int got_sig, skip_len;
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    (void) si;
    got_sig = sig;
    ucontext_t *uc = ucv;
#if BITS64
    uc->uc_mcontext.gregs[REG_RIP] += skip_len;
#else
    uc->uc_mcontext.gregs[REG_EIP] += skip_len;
#endif
}

static void test_xsave(void) {
    static const uint64_t masks[] = {0xe7, 1, 2, 3, 4, 6, 0x20, 0x40, 0x80, 0x63, 0, 0xe4, 0x1ff, ~0ull};
    static const int offs[] = {0, 4096 - 64, 4096 - 512, 4096 - 576, 4096 - 1088, 4096 - 1664, 4096 - 2624, 64};
    static struct ctl c;
    static uint8_t exp[XSIZE], known[XSIZE];
    for (unsigned mi = 0; mi < sizeof masks / sizeof masks[0]; mi++)
        for (unsigned oi = 0; oi < sizeof offs / sizeof offs[0]; oi++)
            for (int seed = 0; seed < 6; seed++) {
                if (sde && (masks[mi] & 0x60200))       // SDE's XCR0 has PKRU and AMX too
                    continue;
#ifdef NO512
                if (masks[mi] & ~0xffull)               // (the host's other components)
                    continue;
#endif
                random_state(&c, mi * 1000 + oi * 10 + (unsigned) seed);
                uint8_t *area = pages + offs[oi];
                memset(pages, 0xaa, 8192);
                c.area = (uintptr_t) area;
                c.mask_lo = (uint32_t) masks[mi];
                c.mask_hi = (uint32_t) (masks[mi] >> 32);
                c.op = 0;
                xs_run(&c);
                model_xsave(&c, masks[mi], exp, known);
                int bad = -1;
                for (int i = 0; i < XSIZE && bad < 0; i++)
                    if (known[i] && area[i] != exp[i])
                        bad = i;
                CHECK(bad < 0, "xsave mask %llx at page offset %d seed %d: byte %d is %02x, want %02x",
                        (unsigned long long) masks[mi], offs[oi], seed, bad, bad < 0 ? 0 : area[bad], bad < 0 ? 0 : exp[bad]);
                int outside = 0;
                for (int i = 0; i < 8192; i++)
                    if ((pages + i < area || pages + i >= area + XSIZE) && pages[i] != 0xaa)
                        outside = 1;
                CHECK(!outside, "xsave mask %llx at %d wrote outside the area", (unsigned long long) masks[mi], offs[oi]);
                // the registers are not changed
                int same = !memcmp(c.z_in, c.z_out, NR * 64) && !memcmp(c.k_in, c.k_out, 64);
                CHECK(same, "xsave mask %llx changed a register", (unsigned long long) masks[mi]);
            }
}

// An XRSTOR image of state s (the registers it holds) with XSTATE_BV bv.
static void build_image(uint8_t *img, const struct ctl *s, uint64_t bv, uint16_t fsw, uint8_t ftw) {
    memset(img, 0, XSIZE);
    memcpy(img, &s->fcw, 2);
    memcpy(img + 2, &fsw, 2);
    img[4] = ftw;
    for (int i = 0; i < 8; i++)
        memcpy(img + 32 + 16 * i, s->z_in[i] + 48, 10);   // x87 registers: some random bytes
    memcpy(img + 24, &s->mxcsr, 4);
    for (int r = 0; r < 16; r++) {
        memcpy(img + 160 + 16 * r, s->z_in[r], 16);
        memcpy(img + 576 + 16 * r, s->z_in[r] + 16, 16);
        memcpy(img + 1152 + 32 * r, s->z_in[r] + 32, 32);
        memcpy(img + 1664 + 64 * r, s->z_in[16 + r], 64);
    }
    memcpy(img + 1088, s->k_in, 64);
    memcpy(img + 512, &bv, 8);
}

static void test_xrstor(void) {
    static const uint64_t masks[] = {0xe7, 1, 2, 4, 6, 0x20, 0x40, 0x80, 0x63, 0, 0x9c};
    static const int offs[] = {0, 4096 - 64, 4096 - 576, 4096 - 1152, 4096 - 2624};
    static struct ctl c, s;
    static const uint16_t fcws[] = {0x37f, 0x27f, 0x1f7f};
    for (unsigned mi = 0; mi < sizeof masks / sizeof masks[0]; mi++)
        for (unsigned oi = 0; oi < sizeof offs / sizeof offs[0]; oi++)
            for (int seed = 0; seed < 8; seed++) {
                random_state(&c, 50000 + mi * 1000 + oi * 10 + (unsigned) seed);
                random_state(&s, 90000 + mi * 1000 + oi * 10 + (unsigned) seed);
                s.fcw = fcws[seed % 3];
                uint64_t bv = rnd() & XCR0;
#ifndef NO512
                if (sde && (masks[mi] & 1))     // SDE loses the registers around an x87 XRSTOR (-DNO512 checks those)
                    continue;
#endif
#ifdef NO512
                if (oi > 2)                     // (the AVX-512 components' places: no use here)
                    continue;
#endif
                uint16_t fsw = (uint16_t) ((rnd() & 7) << 11);
                uint8_t ftw = (uint8_t) rnd();
                uint8_t *area = pages + offs[oi];
                build_image(area, &s, bv, fsw, ftw);
                c.area = (uintptr_t) area;
                c.mask_lo = (uint32_t) masks[mi];
                c.mask_hi = 0;
                c.op = 1;
                xs_run(&c);
                uint64_t m = masks[mi] & XCR0;
                for (int r = 0; r < NR; r++) {
                    uint8_t want[64];
                    memcpy(want, c.z_in[r], 64);
                    if (r < 16 && (m & 2)) { if (bv & 2) memcpy(want, s.z_in[r], 16); else memset(want, 0, 16); }
                    if (r < 16 && (m & 4)) { if (bv & 4) memcpy(want + 16, s.z_in[r] + 16, 16); else memset(want + 16, 0, 16); }
                    if (r < 16 && (m & 0x40)) { if (bv & 0x40) memcpy(want + 32, s.z_in[r] + 32, 32); else memset(want + 32, 0, 32); }
                    if (r >= 16 && (m & 0x80)) { if (bv & 0x80) memcpy(want, s.z_in[r], 64); else memset(want, 0, 64); }
                    if (r >= NX && r < 16)
                        continue;
                    CHECK(!memcmp(c.z_out[r], want, 64), "xrstor mask %llx bv %llx at %d seed %d: zmm%d",
                            (unsigned long long) masks[mi], (unsigned long long) bv, offs[oi], seed, r);
                }
                for (int i = 0; i < 8; i++) {
                    uint64_t want = (m & 0x20) ? ((bv & 0x20) ? s.k_in[i] : 0) : c.k_in[i];
                    CHECK(c.k_out[i] == want, "xrstor mask %llx bv %llx: k%d %llx want %llx", (unsigned long long) masks[mi],
                            (unsigned long long) bv, i, (unsigned long long) c.k_out[i], (unsigned long long) want);
                }
                uint32_t mx;
                memcpy(&mx, c.fx_out + 24, 4);
                uint32_t want_mx = (m & 6) ? s.mxcsr : c.mxcsr;
                CHECK(mx == want_mx, "xrstor mask %llx bv %llx: mxcsr %x want %x", (unsigned long long) masks[mi],
                        (unsigned long long) bv, mx, want_mx);
                uint16_t fcw, fsw2;
                memcpy(&fcw, c.fx_out, 2);
                memcpy(&fsw2, c.fx_out + 2, 2);
                uint8_t ftw2 = c.fx_out[4];
                if (m & 1) {
                    uint16_t wcw = (bv & 1) ? s.fcw : 0x37f, wsw = (bv & 1) ? fsw : 0;
                    uint8_t wtw = (bv & 1) ? ftw : 0;
                    CHECK(fcw == wcw && fsw2 == wsw && ftw2 == wtw, "xrstor mask %llx bv %llx: x87 cw %x sw %x tw %x, want %x %x %x",
                            (unsigned long long) masks[mi], (unsigned long long) bv, fcw, fsw2, ftw2, wcw, wsw, wtw);
                    if (bv & 1)
                        for (int i = 0; i < 8; i++)
                            if ((wtw >> ((((wsw >> 11) & 7) + i) & 7)) & 1)
                                CHECK(!memcmp(c.fx_out + 32 + 16 * i, area + 32 + 16 * i, 10), "xrstor x87 st%d", i);
                }
            }
}

// The #GP cases and a fault on the second page: nothing changes.
static void test_faults(void) {
    static struct ctl c, s;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    skip_len = 3;                               // xrstor (%rcx) / xsave (%rcx): 0f ae 29 / 21
    struct { const char *what; int off; int hdr_byte; uint8_t val; uint32_t mxcsr; uint32_t mask; int faults; } cases[] = {
        {"xstate_bv bit 3", 0, 0, 0x08, 0x1f80, 0xe7, 1},
        {"xstate_bv bit 9", 0, 1, 0x02, 0x1f80, 0xe7, 1},
        {"xstate_bv bit 3, mask 1", 0, 0, 0x08, 0x1f80, 1, 1},
        {"xcomp_bv", 0, 8, 1, 0x1f80, 0xe7, 1},
        {"header byte 20", 0, 20, 1, 0x1f80, 0xe7, 1},
        {"header byte 24", 0, 24, 1, 0x1f80, 0xe7, 0},
        {"header byte 63", 0, 63, 1, 0x1f80, 0xe7, 0},
        {"mxcsr bit 16, mask e7", 0, -1, 0, 0x11f80, 0xe7, 1},
        {"mxcsr bit 16, mask 4", 0, -1, 0, 0x11f80, 4, 1},
        {"mxcsr bit 16, mask 1", 0, -1, 0, 0x11f80, 1, 0},
        {"unaligned", 16, -1, 0, 0x1f80, 0xe7, 1},
        {"unaligned by 32", 32, -1, 0, 0x1f80, 1, 1},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        random_state(&c, 7000 + i);
        random_state(&s, 8000 + i);
        uint8_t *area = pages + 64 + cases[i].off;
        build_image(area, &s, 0xe7, 0, 0);
        memcpy(area + 24, &cases[i].mxcsr, 4);
        if (cases[i].hdr_byte >= 0)
            area[512 + cases[i].hdr_byte] = cases[i].val;
        c.area = (uintptr_t) area;
        c.mask_lo = cases[i].mask;
        c.mask_hi = 0;
        c.op = 1;
        got_sig = 0;
        xs_run(&c);
        CHECK((got_sig != 0) == cases[i].faults, "xrstor %s: signal %d", cases[i].what, got_sig);
        if (cases[i].faults)
            CHECK(!memcmp(c.z_in, c.z_out, NR * 64) && !memcmp(c.k_in, c.k_out, 64) &&
                  !memcmp(c.fx_out + 24, &c.mxcsr, 4), "xrstor %s changed state before its #GP", cases[i].what);
    }
    // XSAVE to an unaligned area
    random_state(&c, 9100);
    c.area = (uintptr_t) (pages + 32);
    c.mask_lo = 0xe7;
    c.op = 0;
    got_sig = 0;
    xs_run(&c);
    CHECK(got_sig == SIGSEGV, "xsave unaligned: signal %d", got_sig);
    // a second page that is not there: the fault comes before anything changes
    mprotect(pages + 4096, 4096, PROT_NONE);
    for (int k = 0; k < 4; k++) {
        static const int offs[] = {4096 - 64, 4096 - 576, 4096 - 1152, 4096 - 2560};
        static const uint32_t ms[] = {0xe7, 4, 0x40, 0x80};
        random_state(&c, 9200 + k);
        random_state(&s, 9300 + k);
        uint8_t *area = pages + offs[k];
        uint8_t img[XSIZE];
        build_image(img, &s, 0xe7, 0, 0);
        memcpy(area, img, (size_t) (4096 - offs[k]));
        c.area = (uintptr_t) area;
        c.mask_lo = ms[k];
        c.op = 1;
        got_sig = 0;
        xs_run(&c);
        int reaches = !(ms[k] == 4 && offs[k] < 4096 - 832) && !(ms[k] == 0x40 && offs[k] < 4096 - 1664) &&
                      !(ms[k] == 0x80 && !BITS64);      // (32-bit code does not reach Hi16_ZMM)
        CHECK((got_sig == SIGSEGV) == reaches, "xrstor mask %x at %d before an unmapped page: signal %d", ms[k], offs[k], got_sig);
        if (got_sig)
            CHECK(!memcmp(c.z_in, c.z_out, NR * 64) && !memcmp(c.k_in, c.k_out, 64),
                  "xrstor mask %x at %d changed state before its page fault", ms[k], offs[k]);
        c.op = 0;
        got_sig = 0;
        memset(area, 0x55, (size_t) (4096 - offs[k]));
        xs_run(&c);
        CHECK(got_sig == SIGSEGV || !reaches, "xsave mask %x at %d before an unmapped page: signal %d", ms[k], offs[k], got_sig);
    }
    mprotect(pages + 4096, 4096, PROT_READ | PROT_WRITE);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

// ---- the signal frame ----
static struct ctl sigc;
static int sig_mode;                            // 0 look, 1 change, 2 drop magic1, 3 clear YMM's bit, 4 bad header
static void on_ud(int sig, siginfo_t *si, void *ucv) __attribute__((used));
static void on_ud(int sig, siginfo_t *si, void *ucv) {
    (void) sig; (void) si;
    ucontext_t *uc = ucv;
#if BITS64
    uint8_t *fp = (uint8_t *) uc->uc_mcontext.fpregs;
    uc->uc_mcontext.gregs[REG_RIP] += 2;
#else
    uint8_t *fp = (uint8_t *) uc->uc_mcontext.fpregs + 112;     // past the FNSAVE header
    uc->uc_mcontext.gregs[REG_EIP] += 2;
#endif
    CHECK(((uintptr_t) fp & 63) == 0, "signal frame: XSAVE image at %p, not 64-byte aligned", (void *) fp);
    uint32_t magic1, ext, size, magic2;
    uint64_t xfeatures, bv;
    memcpy(&magic1, fp + 464, 4);
    memcpy(&ext, fp + 468, 4);
    memcpy(&xfeatures, fp + 472, 8);
    memcpy(&size, fp + 480, 4);
    memcpy(&magic2, fp + XSIZE, 4);
    CHECK(magic1 == 0x46505853 && magic2 == 0x46505845, "signal frame magics %x %x", magic1, magic2);
    CHECK(size == XSIZE && xfeatures == XCR0 && ext == XSIZE + 4 + (BITS64 ? 0 : 112),
          "signal frame sw bytes: size %u xfeatures %llx extended %u", size, (unsigned long long) xfeatures, ext);
#if BITS64
    CHECK(uc->uc_flags & 1, "uc_flags lacks UC_FP_XSTATE");
#endif
    memcpy(&bv, fp + 512, 8);
    for (int r = 0; r < NR; r++) {
        int ok = r < 16 ? !memcmp(fp + 160 + 16 * r, sigc.z_in[r], 16) && !memcmp(fp + 576 + 16 * r, sigc.z_in[r] + 16, 16) &&
                         !memcmp(fp + 1152 + 32 * r, sigc.z_in[r] + 32, 32)
                       : !memcmp(fp + 1664 + 64 * (r - 16), sigc.z_in[r], 64);
        CHECK(ok, "signal frame: zmm%d", r);
    }
    CHECK(!memcmp(fp + 1088, sigc.k_in, 64), "signal frame: k0-k7");
    CHECK(!memcmp(fp + 24, &sigc.mxcsr, 4), "signal frame: mxcsr");
    CHECK((bv & 0x66) == 0x66, "signal frame: xstate_bv %llx", (unsigned long long) bv);
    // what the handler started with: every register zero, MXCSR 1F80H, x87 empty
    int clean = 1;
    for (int r = 0; r < NR; r++)
        for (int j = 0; j < 64; j++)
            clean &= xs_entry.z_out[r][j] == 0;
    for (int i = 0; i < 8; i++)
        clean &= xs_entry.k_out[i] == 0;
    uint32_t emx;
    uint16_t ecw;
    memcpy(&emx, xs_entry.fx_out + 24, 4);
    memcpy(&ecw, xs_entry.fx_out, 2);
    CHECK(clean && emx == 0x1f80 && ecw == 0x37f && xs_entry.fx_out[4] == 0,
          "handler entry state: registers %s, mxcsr %x, fcw %x, ftw %x", clean ? "zero" : "not zero", emx, ecw, xs_entry.fx_out[4]);
    if (sig_mode == 1) {                        // change some of everything
        for (int r = 0; r < 16; r++) {
            fp[160 + 16 * r] ^= 0x5a;
            fp[576 + 16 * r + 3] ^= 0xa5;
            fp[1152 + 32 * r + 31] ^= 0x3c;
        }
        fp[1664 + 64 * 5 + 40] ^= 0xc3;
        fp[1088 + 8 * 2] ^= 0x99;
        uint32_t mx = 0x7f80;
        memcpy(fp + 24, &mx, 4);
    } else if (sig_mode == 2) {
        uint32_t zero = 0;
        memcpy(fp + 464, &zero, 4);
    } else if (sig_mode == 3) {
        bv &= ~4ull;
        memcpy(fp + 512, &bv, 8);
    } else if (sig_mode == 4) {
        fp[520] = 1;                            // XCOMP_BV
    }
}
void ud_stub(int sig, siginfo_t *si, void *uc);          // captures, then on_ud
#if BITS64
__asm__(".text\n.globl ud_stub\nud_stub:\n push %rdi\n push %rsi\n push %rdx\n call xs_capture\n pop %rdx\n pop %rsi\n pop %rdi\n jmp on_ud\n");
#else
__asm__(".text\n.globl ud_stub\nud_stub:\n call xs_capture\n jmp on_ud\n");
#endif

static void run_signal(int mode, uint64_t seed) {
    random_state(&sigc, seed);
    for (int r = 0; r < 16; r++)
        sigc.z_in[r][20] |= 1, sigc.z_in[r][40] |= 1, sigc.z_in[r][1] |= 1;     // every component in use
    sigc.k_in[1] |= 1;
    if (BITS64)
        sigc.z_in[20][3] |= 1;
    sig_mode = mode;
    sigc.op = 3;
    xs_run(&sigc);
}

static void test_signal(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = ud_stub;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    run_signal(0, 11);
    CHECK(!memcmp(sigc.z_in, sigc.z_out, NR * 64) && !memcmp(sigc.k_in, sigc.k_out, 64), "sigreturn did not restore the registers");
    run_signal(1, 12);
    int ok = 1;
    for (int r = 0; r < NX; r++) {
        uint8_t want[64];
        memcpy(want, sigc.z_in[r], 64);
        want[0] ^= 0x5a;
        want[16 + 3] ^= 0xa5;
        want[32 + 31] ^= 0x3c;
        ok &= !memcmp(sigc.z_out[r], want, 64);
    }
    if (BITS64)
        ok &= sigc.z_out[21][40] == (sigc.z_in[21][40] ^ 0xc3);
    ok &= sigc.k_out[2] == (sigc.k_in[2] ^ 0x99);
    uint32_t mx;
    memcpy(&mx, sigc.fx_out + 24, 4);
    CHECK(ok && mx == 0x7f80, "sigreturn did not take the handler's changes (mxcsr %x)", mx);
    run_signal(2, 13);                          // FXSAVE only: x87 and SSE, the rest initialized
    ok = 1;
    for (int r = 0; r < NR; r++) {
        uint8_t want[64] = {0};
        if (r < NX)
            memcpy(want, sigc.z_in[r], 16);
        ok &= !memcmp(sigc.z_out[r], want, 64);
    }
    for (int i = 0; i < 8; i++)
        ok &= sigc.k_out[i] == 0;
    CHECK(ok, "a frame without the magics restored more than x87 and SSE");
    run_signal(3, 14);                          // XSTATE_BV without YMM: the uppers initialized
    ok = 1;
    for (int r = 0; r < NX; r++) {
        uint8_t want[64];
        memcpy(want, sigc.z_in[r], 64);
        memset(want + 16, 0, 16);
        ok &= !memcmp(sigc.z_out[r], want, 64);
    }
    CHECK(ok, "a frame whose XSTATE_BV lacks AVX kept the YMM uppers");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        run_signal(4, 15);                      // XCOMP_BV set: sigreturn is SIGSEGV
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "sigreturn of a bad XSAVE header: status %x", status);
    signal(SIGILL, SIG_DFL);
    // AT_MINSIGSTKSZ, and an altstack that cannot hold the frame
    unsigned long min = getauxval(51);
    CHECK(min >= XSIZE + 4 && min < 8192, "AT_MINSIGSTKSZ %lu", min);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        static uint8_t small[2048] __attribute__((aligned(16)));
        stack_t ss = {.ss_sp = small, .ss_size = sizeof small};
        // the system call itself: musl's sigaltstack() refuses 2048 bytes
        if (syscall(SYS_sigaltstack, &ss, NULL) != 0)
            _exit(3);
        struct sigaction sb = {0};
        sb.sa_sigaction = ud_stub;
        sb.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGILL, &sb, NULL);
        run_signal(0, 16);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "a frame larger than its 2048-byte altstack: status %x", status);
    pid = fork();
    if (pid == 0) {
        static uint8_t big[16384] __attribute__((aligned(16)));
        stack_t ss = {.ss_sp = big, .ss_size = sizeof big};
        sigaltstack(&ss, NULL);
        struct sigaction sb = {0};
        sb.sa_sigaction = ud_stub;
        sb.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGILL, &sb, NULL);
        run_signal(0, 17);
        _exit(memcmp(sigc.z_in, sigc.z_out, NR * 64) != 0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a frame on a 16K altstack: status %x", status);
}

// ---- ptrace ----
static void test_ptrace(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        random_state(&sigc, 21);
        sigc.op = 4;                            // INT3: stop for the tracer
        xs_run(&sigc);
        // the tracer flipped byte 7 of every register's bits 128-511 and k3
        int ok = 1;
        for (int r = 0; r < NX; r++) {
            uint8_t want[64];
            memcpy(want, sigc.z_in[r], 64);
            want[16 + 7] ^= 0xff;
            want[32 + 7] ^= 0xff;
            ok &= !memcmp(sigc.z_out[r], want, 64);
        }
        ok &= sigc.k_out[3] == ~sigc.k_in[3];
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP, "ptrace stop: %x", status);
    static uint8_t img[XSIZE + 64];
    struct iovec iov = {img, sizeof img};
    long r = ptrace(PTRACE_GETREGSET, pid, (void *) 0x202, &iov);
    CHECK(r == 0 && iov.iov_len == XSIZE, "NT_X86_XSTATE: %ld, length %zu", r, (size_t) iov.iov_len);
    uint64_t xcr0;
    memcpy(&xcr0, img + 464, 8);
    CHECK(xcr0 == XCR0, "NT_X86_XSTATE xcr0 %llx", (unsigned long long) xcr0);
    random_state(&sigc, 21);                    // what the child loaded
    int ok = 1;
    for (int r2 = 0; r2 < NX; r2++)
        ok &= !memcmp(img + 576 + 16 * r2, sigc.z_in[r2] + 16, 16) && !memcmp(img + 1152 + 32 * r2, sigc.z_in[r2] + 32, 32);
    ok &= !memcmp(img + 1088, sigc.k_in, 64);
    CHECK(ok, "NT_X86_XSTATE contents");
    for (int r2 = 0; r2 < 16; r2++) {
        img[576 + 16 * r2 + 7] ^= 0xff;
        img[1152 + 32 * r2 + 7] ^= 0xff;
    }
    uint64_t k3;
    memcpy(&k3, img + 1088 + 24, 8);
    k3 = ~k3;
    memcpy(img + 1088 + 24, &k3, 8);
    uint64_t bv;
    memcpy(&bv, img + 512, 8);
    bv |= 0x64;
    memcpy(img + 512, &bv, 8);
    iov.iov_len = XSIZE;
    r = ptrace(PTRACE_SETREGSET, pid, (void *) 0x202, &iov);
    CHECK(r == 0, "NT_X86_XSTATE set: %ld", r);
    img[520] = 1;                               // XCOMP_BV: EINVAL
    r = ptrace(PTRACE_SETREGSET, pid, (void *) 0x202, &iov);
    CHECK(r == -1, "NT_X86_XSTATE set with XCOMP_BV: %ld", r);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the tracee's registers after NT_X86_XSTATE set: %x", status);
}

static void test_cpuid(void) {
#ifdef NO512
    return;
#endif
    unsigned a, b, c, d;
    static const struct { unsigned sub, size, off; } comp[] = {{2, 256, 576}, {5, 64, 1088}, {6, 512, 1152}, {7, 1024, 1664}};
    for (unsigned i = 0; i < 4; i++) {
        __cpuid_count(0xd, comp[i].sub, a, b, c, d);
        CHECK(a == comp[i].size && b == comp[i].off, "cpuid 0xd.%u: size %u offset %u", comp[i].sub, a, b);
    }
    if (!sde) {
        __cpuid_count(0xd, 0, a, b, c, d);
        CHECK(a == XCR0 && b == XSIZE && c == XSIZE && d == 0, "cpuid 0xd.0: %x %u %u %x", a, b, c, d);
        __cpuid_count(0xd, 1, a, b, c, d);
        CHECK(a == 1, "cpuid 0xd.1: %x (XSAVEOPT; no XSAVEC, XSAVES)", a);
    }
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if (!sde)
        CHECK(lo == XCR0 && hi == 0, "xgetbv %x:%x", hi, lo);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sde = argc > 1 && !strcmp(argv[1], "sde");
    pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) { perror("mmap"); return 2; }
    test_cpuid();
    test_xsave();
    test_xrstor();
    if (!sde) {
        test_faults();
        test_signal();
        test_ptrace();
    }
    printf("x86_xsave: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
