// x87_mf.c -- unmasked x87 exceptions and #MF, as Linux on real hardware (an
// AMD Ryzen, camd, -m32 and 64-bit) delivers them:
//
//  * The instruction that raises an unmasked exception completes: it sets
//    the flag and ES (and B), and does not fault itself.
//  * The NEXT waiting x87 instruction faults before it runs: SIGFPE with
//    Linux's code for swd & ~cwd -- FPE_FLTINV (IE, the stack faults too),
//    FPE_FLTDIV (ZE), FPE_FLTOVF (OE), FPE_FLTUND (DE, UE), FPE_FLTRES (PE)
//    -- si_addr and the frame's IP at that waiting instruction, trap 16 and
//    error 0 in the frame. FWAIT is one; FNSTSW, FNSTCW, FNCLEX, FNINIT,
//    FNSTENV and FNSAVE are not.
//  * A handler that clears the flags in the frame's FPU state returns to the
//    waiting instruction, which then runs.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("FAIL %s: %#llx, want %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
}

static volatile int sig, code, trapno, err, swd, resume;
static void *volatile addr;
static void *volatile pc;
static sigjmp_buf jb;
static void on_fpe(int s, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    sig = s; code = si->si_code; addr = si->si_addr;
#if defined(__x86_64__)
    pc = (void *) uc->uc_mcontext.gregs[REG_RIP];
    trapno = (int) uc->uc_mcontext.gregs[REG_TRAPNO];
    err = (int) uc->uc_mcontext.gregs[REG_ERR];
    swd = uc->uc_mcontext.fpregs->swd;
    if (resume)
        uc->uc_mcontext.fpregs->swd &= 0x7f00;          // flags, ES and B cleared
#else
    pc = (void *) uc->uc_mcontext.gregs[REG_EIP];
    trapno = (int) uc->uc_mcontext.gregs[REG_TRAPNO];
    err = (int) uc->uc_mcontext.gregs[REG_ERR];
    swd = uc->uc_mcontext.fpregs->sw & 0xffff;
    if (resume) {
        // the legacy header's status word and the FXSAVE image's both
        uc->uc_mcontext.fpregs->sw &= 0xffff7f00;
        ((uint16_t *) uc->uc_mcontext.fpregs)[(112 + 2) / 2] &= 0x7f00;
    }
#endif
    if (!resume)
        siglongjmp(jb, 1);
}

static uint16_t sw(void) { uint16_t s; __asm__ volatile("fnstsw %0" : "=m"(s)); return s; }

// Raise an exception with its mask clear, then run `fld1` (a waiting
// instruction) at a known address.
extern char fld1_at[], fwait_at[];
__attribute__((noinline)) static void raise_then_fld1(uint16_t cw, int which) {
    __asm__ volatile("fninit\n fldcw %0" :: "m"(cw));
    switch (which) {
    case 0: __asm__ volatile("fldz\n fldz\n fdivrp" ::: "memory"); break;              // 0/0: IE
    case 1: __asm__ volatile("fld1\n fldz\n fdivrp" ::: "memory"); break;              // 1/0: ZE
    case 2: { static const uint16_t big[5] = {0, 0, 0, 0x8000, 0x7ffe};               // max*max: OE
              __asm__ volatile("fldt %0\n fldt %0\n fmulp" :: "m"(big) : "memory"); break; }
    case 3: { static const uint16_t tiny[5] = {0, 0, 0, 0x8000, 0x0001};             // 2^-16382 squared: UE, exact
              __asm__ volatile("fldt %0\n fldt %0\n fmulp" :: "m"(tiny) : "memory"); break; }
    case 4: __asm__ volatile("fld1\n fldpi\n fdivrp" ::: "memory"); break;             // 1/pi: PE
    case 5: { static const uint16_t den[5] = {1, 0, 0, 0, 0};                         // denormal + 1: DE
              __asm__ volatile("fld1\n fldt %0\n faddp" :: "m"(den) : "memory"); break; }
    default: __asm__ volatile("fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1" ::: "memory"); break;  // overflow: IE|SF
    }
    __asm__ volatile(".globl fld1_at\nfld1_at: fld1" ::: "memory");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fpe;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGFPE, &sa, NULL);

    static const struct { const char *name; uint16_t cw; int which, code; uint16_t flags; } cases[] = {
        {"IE", 0x037e, 0, FPE_FLTINV, 0x01}, {"ZE", 0x037b, 1, FPE_FLTDIV, 0x04},
        {"OE", 0x0377, 2, FPE_FLTOVF, 0x08}, {"UE", 0x036f, 3, FPE_FLTUND, 0x10},
        {"PE", 0x035f, 4, FPE_FLTRES, 0x20}, {"DE", 0x037d, 5, FPE_FLTUND, 0x02},
        {"stack overflow", 0x037e, 6, FPE_FLTINV, 0x41},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sig = 0; resume = 0;
        if (!sigsetjmp(jb, 1))
            raise_then_fld1(cases[i].cw, cases[i].which);
        char what[64];
        snprintf(what, sizeof what, "%s: SIGFPE", cases[i].name); check(what, sig, SIGFPE);
        snprintf(what, sizeof what, "%s: si_code", cases[i].name); check(what, code, cases[i].code);
        snprintf(what, sizeof what, "%s: si_addr at the waiting fld1", cases[i].name); check(what, (uintptr_t) addr, (uintptr_t) fld1_at);
        snprintf(what, sizeof what, "%s: the frame's IP", cases[i].name); check(what, (uintptr_t) pc, (uintptr_t) fld1_at);
        snprintf(what, sizeof what, "%s: trap 16", cases[i].name); check(what, trapno, 16);
        snprintf(what, sizeof what, "%s: error 0", cases[i].name); check(what, err, 0);
        snprintf(what, sizeof what, "%s: the frame's status: the flag, ES, B", cases[i].name);
        check(what, swd & 0x80ff, 0x8080 | cases[i].flags);
        __asm__ volatile("fnclex\n fninit");
    }

    // the no-wait forms do not fault; FWAIT does
    sig = 0; resume = 0;
    if (!sigsetjmp(jb, 1)) {
        uint16_t c = 0x037b, s, c2;
        uint8_t env[28], save[108];
        __asm__ volatile("fninit\n fldcw %0\n fld1\n fldz\n fdivrp" :: "m"(c) : "memory");
        __asm__ volatile("fnstsw %0\n fnstcw %1" : "=m"(s), "=m"(c2));
        check("fnstsw sees ES", s & 0x8084, 0x8084);
        __asm__ volatile("fnstenv %0" : "=m"(env));       // no fault; masks everything, so ES clears
        check("fnstenv: ES clear", sw() & 0x8080, 0);
        __asm__ volatile("fnclex\n fldcw %0\n fld1\n fldz\n fdivrp" :: "m"(c) : "memory");
        __asm__ volatile("fnsave %0" : "=m"(save));      // no fault, and FNINIT after it
        check("fnsave cleared it", sw(), 0);
        __asm__ volatile("fldcw %0\n fld1\n fldz\n fdivrp\n fnclex\n fld1" :: "m"(c) : "memory");
        check("fnclex cleared it", sw() & 0x80ff, 0);
        __asm__ volatile("fldcw %0\n fld1\n fldz\n fdivrp\n fninit\n fld1" :: "m"(c) : "memory");
        check("fninit cleared it", sw() & 0x80ff, 0);
        __asm__ volatile("fldcw %0\n fld1\n fldz\n fdivrp" :: "m"(c) : "memory");
        __asm__ volatile(".globl fwait_at\nfwait_at: fwait" ::: "memory");
    }
    check("no fault before fwait", sig ? (uintptr_t) addr : 0, (uintptr_t) fwait_at);
    check("fwait: FPE_FLTDIV", code, FPE_FLTDIV);
    __asm__ volatile("fnclex\n fninit");

    // a handler that clears the flags in the frame returns to the waiting
    // instruction, which then runs: fld1 leaves 1.0 on top
    sig = 0; resume = 1;
    long double top = 0;
    {
        uint16_t c = 0x037b;
        __asm__ volatile("fninit\n fldcw %1\n fld1\n fldz\n fdivrp\n fstp %%st(0)\n fld1\n fstpt %0"
                         : "=m"(top) : "m"(c) : "memory");
    }
    check("resumed: one fault", sig, SIGFPE);
    check("resumed: fld1 ran", top == 1.0L, 1);
    __asm__ volatile("fnclex\n fninit");

    printf("x87_mf: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
