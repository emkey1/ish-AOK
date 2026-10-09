// x86_mmx_x87_alias.c -- the MMX registers are the x87 registers. MMn is the
// significand of PHYSICAL register n; an MMX instruction that writes MMn sets
// its sign and exponent to all ones; every MMX instruction but EMMS sets TOP
// to 0 and tags all eight registers valid; EMMS tags them all empty. So
// FXSAVE after MMX code shows the MMX values in the ST slots, FXRSTOR's ST
// slots are what MMX then reads, x87 code after MMX without EMMS sees them,
// and the signal frame -- an XSAVE image -- carries them: a handler that uses
// MMX does not change what the interrupted code has. And an MMX instruction
// whose memory operand faults changes none of it: the signal frame shows TOP,
// the tags and the registers as they were. AOK kept the MMX registers apart,
// so none of this held. Checked on an AMD Ryzen (camd), 32- and 64-bit builds.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static unsigned char area[512] __attribute__((aligned(16)));
static void fxsave(void) { __asm__ volatile("fxsave %0" : "=m"(area)); }
static void fxrstor(void) { __asm__ volatile("fxrstor %0" :: "m"(area)); }
static unsigned fsw(void) { return area[2] | area[3] << 8; }
static unsigned ftw(void) { return area[4]; }        // the abridged tag
static uint64_t st_sig(int i) { uint64_t v; memcpy(&v, area + 32 + 16 * i, 8); return v; }
static unsigned st_se(int i) { return area[40 + 16 * i] | area[41 + 16 * i] << 8; }

static const uint64_t X[8] = {
    0x0123456789abcdefull, 0xfedcba9876543210ull, 0x1111222233334444ull, 0x8000000000000001ull,
    0x0000000000000000ull, 0xffffffffffffffffull, 0x5555aaaa5555aaaaull, 0x7fff0000ffff0000ull,
};

#define MOVQ_IN(n, v) __asm__ volatile("movq %0, %%mm" #n :: "m"(v))
#define MOVQ_OUT(n, v) __asm__ volatile("movq %%mm" #n ", %0" : "=m"(v))

static void load_all(void) {
    MOVQ_IN(0, X[0]); MOVQ_IN(1, X[1]); MOVQ_IN(2, X[2]); MOVQ_IN(3, X[3]);
    MOVQ_IN(4, X[4]); MOVQ_IN(5, X[5]); MOVQ_IN(6, X[6]); MOVQ_IN(7, X[7]);
}
static void read_all(uint64_t out[8]) {
    MOVQ_OUT(0, out[0]); MOVQ_OUT(1, out[1]); MOVQ_OUT(2, out[2]); MOVQ_OUT(3, out[3]);
    MOVQ_OUT(4, out[4]); MOVQ_OUT(5, out[5]); MOVQ_OUT(6, out[6]); MOVQ_OUT(7, out[7]);
}

static volatile int handled;
static uint64_t in_handler[8];
static void handler(int sig) {
    (void) sig;
    read_all(in_handler);                 // the handler starts from the init state
    uint64_t y = 0xdeaddeaddeaddeadull;
    MOVQ_IN(0, y); MOVQ_IN(5, y);
    __asm__ volatile("emms");
    handled = 1;
}

// The x87 state in the signal frame of a fault: TOP, the tags, and the
// exponent of physical register 3, which is ST((3 - TOP) & 7).
static sigjmp_buf jb;
static void *fault_page;
static volatile int segv_seen, frame_tag_ok;
static volatile unsigned frame_top, frame_tag, frame_exp3;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    segv_seen = 1;
    // The kernel's frame layouts (struct _fpstate_64 / _fpstate_32), spelled
    // out: glibc and musl name them differently.
#if defined(__x86_64__)
    struct { uint16_t cwd, swd, twd, fop; uint64_t rip, rdp; uint32_t mxcsr, mxcsr_mask;
             struct { uint16_t sig[4], exp, pad[3]; } st[8]; } *f = (void *) uc->uc_mcontext.fpregs;
    frame_top = (f->swd >> 11) & 7;
    frame_tag = f->twd;                                // abridged
    frame_tag_ok = f->twd == 0x80;
    frame_exp3 = f->st[(3 - frame_top) & 7].exp;
#else
    struct __attribute__((packed)) { uint32_t cw, sw, tag, ipoff, cssel, dataoff, datasel;
             struct __attribute__((packed)) { uint16_t sig[4], exp; } st[8]; } *f = (void *) uc->uc_mcontext.fpregs;
    frame_top = (f->sw >> 11) & 7;
    frame_tag = f->tag & 0xffff;                       // the full tag word
    frame_tag_ok = (f->tag & 0xffff) == 0x3fff;        // 7 valid (00), the rest empty (11)
    frame_exp3 = f->st[(3 - frame_top) & 7].exp;
#endif
    siglongjmp(jb, 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // 1. MMX writes: the ST slots hold them, exponent ones, TOP 0, all valid
    __asm__ volatile("fninit\n fld1\n fld1\n fld1");      // TOP 5 to start from
    load_all();
    fxsave();
    CHECK(((fsw() >> 11) & 7) == 0, "after MMX: TOP %u", (fsw() >> 11) & 7);
    CHECK(ftw() == 0xff, "after MMX: abridged tag %#x", ftw());
    for (int i = 0; i < 8; i++)
        CHECK(st_sig(i) == X[i] && st_se(i) == 0xffff, "after MMX: ST%d %016llx/%04x, want MM%d %016llx/ffff",
              i, (unsigned long long) st_sig(i), st_se(i), i, (unsigned long long) X[i]);

    // 2. EMMS: all empty, TOP and the registers as they were
    __asm__ volatile("emms");
    fxsave();
    CHECK(ftw() == 0 && ((fsw() >> 11) & 7) == 0, "after EMMS: tag %#x TOP %u", ftw(), (fsw() >> 11) & 7);
    CHECK(st_sig(3) == X[3], "after EMMS: ST3 %016llx", (unsigned long long) st_sig(3));

    // 3. FXRSTOR with TOP 5: MMn reads physical register n, ST((n - 5) & 7);
    // a read leaves the exponent, and makes TOP 0 and all valid
    memset(area + 32, 0, 128);
    for (int i = 0; i < 8; i++) {
        uint64_t v = 0x1000000000000000ull * (unsigned) (i + 1) + 0x77;
        memcpy(area + 32 + 16 * i, &v, 8);
        area[40 + 16 * i] = (unsigned char) (0x10 + i);
        area[41 + 16 * i] = 0x40;
    }
    area[2] = 0; area[3] = 5 << 3;                       // FSW: TOP 5
    area[4] = 0xff;
    fxrstor();
    uint64_t got[8];
    read_all(got);
    for (int n = 0; n < 8; n++) {
        int slot = (n - 5) & 7;
        uint64_t want = 0x1000000000000000ull * (unsigned) (slot + 1) + 0x77;
        CHECK(got[n] == want, "after FXRSTOR (TOP 5): MM%d %016llx, want ST%d's %016llx",
              n, (unsigned long long) got[n], slot, (unsigned long long) want);
    }
    fxsave();
    CHECK(((fsw() >> 11) & 7) == 0 && ftw() == 0xff, "after MMX reads: TOP %u tag %#x", (fsw() >> 11) & 7, ftw());
    // TOP is 0 now, so ST(i) is physical i, which was slot (i - 5) & 7
    for (int i = 0; i < 8; i++) {
        int slot = (i - 5) & 7;
        CHECK(st_se(i) == (0x4000u | (0x10u + (unsigned) slot)), "after MMX reads: ST%d exponent %04x", i, st_se(i));
    }

    // 4. x87 after MMX, no EMMS: ST0 is MM0 with an all-ones exponent
    load_all();
    unsigned char t[10];
    __asm__ volatile("fstpt %0" : "=m"(t));
    uint64_t tsig; memcpy(&tsig, t, 8);
    CHECK(tsig == X[0] && t[8] == 0xff && t[9] == 0xff, "fstp after MMX: %016llx/%02x%02x", (unsigned long long) tsig, t[9], t[8]);
    __asm__ volatile("fninit");

    // 5. a signal handler that uses MMX: the interrupted code's MMX is kept;
    // the handler starts from the init state (all zero)
    load_all();
    signal(SIGUSR1, handler);
    raise(SIGUSR1);
    CHECK(handled, "the handler did not run");
    read_all(got);
    for (int i = 0; i < 8; i++)
        CHECK(got[i] == X[i], "after the handler: MM%d %016llx, want %016llx", i, (unsigned long long) got[i],
              (unsigned long long) X[i]);
    for (int i = 0; i < 8; i++)
        CHECK(in_handler[i] == 0, "in the handler: MM%d %016llx, want 0", i, (unsigned long long) in_handler[i]);
    __asm__ volatile("emms");

    // 6. FNSAVE / FRSTOR carry them too (the 80-bit registers, ST order)
    unsigned char sv[108];
    load_all();
    __asm__ volatile("fnsave %0" : "=m"(sv));          // also re-initialises: tags, not contents
    uint64_t z[8];
    read_all(z);
    CHECK(z[2] == X[2], "after FNSAVE: MM2 %016llx", (unsigned long long) z[2]);
    uint64_t y = 0x2222333344445555ull;
    MOVQ_IN(2, y);
    uint64_t s2; memcpy(&s2, sv + 28 + 10 * 2, 8);
    CHECK(s2 == X[2] && sv[28 + 10 * 2 + 8] == 0xff, "FNSAVE ST2 %016llx", (unsigned long long) s2);
    __asm__ volatile("frstor %0" :: "m"(sv));
    read_all(z);
    CHECK(z[2] == X[2] && z[7] == X[7], "after FRSTOR: MM2 %016llx MM7 %016llx", (unsigned long long) z[2],
          (unsigned long long) z[7]);
    __asm__ volatile("emms");

    // 7. an MMX load that faults changes nothing in the x87: TOP 7, only
    // physical register 7 valid, and physical 3 (ST4) keeps its exponent
    fault_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(area, 0, sizeof area);
    area[0] = 0x7f; area[1] = 0x03;                    // FCW
    area[2] = 0; area[3] = 7 << 3;                     // FSW: TOP 7
    area[4] = 0x80;                                    // only physical 7 valid
    area[24] = 0x80; area[25] = 0x1f;                  // MXCSR
    for (int i = 0; i < 8; i++) {
        uint64_t sig = 0x8000000000000000ull | (unsigned) i;
        memcpy(area + 32 + 16 * i, &sig, 8);
        area[40 + 16 * i] = (unsigned char) (0x34 + i);
        area[41 + 16 * i] = 0x12;
    }
    fxrstor();
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    segv_seen = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile("movq (%0), %%mm3" :: "r"(fault_page) : "memory");
    CHECK(segv_seen, "the faulting MMX load did not fault");
    CHECK(frame_top == 7, "after a faulting MMX load: TOP %u (want 7, unchanged)", frame_top);
    CHECK(frame_tag_ok, "after a faulting MMX load: tags %#x (want only physical 7 valid)", frame_tag);
    CHECK(frame_exp3 == 0x1238, "after a faulting MMX load: physical 3 exponent %#x (want 0x1238)", frame_exp3);
    __asm__ volatile("fninit");

    printf("x86_mmx_x87_alias: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
