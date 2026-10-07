// The packed/unpacked BCD adjust instructions: aaa (0x37), aas (0x3f),
// daa (0x27), das (0x2f), aam (0xd4 ib) and aad (0xd5 ib).
//
// None of them were in the one-byte decode table, so all six raised SIGILL.
// Reported upstream as ish-app/ish#1650.
//
// Two things the Intel SDM's pseudo-code will get you wrong here, both found by
// differential testing against real silicon rather than by reading the manual:
//
//  * aaa/aas adjust AX, not AL. Writing it as "AL += 6; AH += 1" in separate
//    8-bit steps loses the carry out of AL: hardware gives ax=fe0a for aas with
//    ax=0000 and AF=1, while the 8-bit version gives ff0a.
//
//  * The flags the SDM calls "undefined" are not. Real hardware sets aaa/aas's
//    SF/ZF/PF (from the 16-bit AX +/- 0x106 on AMD), and aad's CF/AF from the
//    implied 8-bit ADD of AL and AH*base. Software has been observed to depend
//    on this, so we match it; i386_bcd.c checks every input against an AMD.
//
// aam with a base of 0 is a divide error; since the base is an immediate that
// is decided at translate time and raises INT_DIV rather than reaching the
// helper.
//
// i386 only: these opcodes are invalid in 64-bit mode, so the test skips itself
// on an x86_64 guest.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__)
int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("bcd_adjust: SKIP (BCD adjusts are invalid in 64-bit mode)\n");
    return 0;
}
#else

#define FLAG_MASK 0xd5u   // CF | PF | AF | ZF | SF

static void check(const char *what, unsigned got, unsigned want) {
    if (got == want) {
        test_logf("  ok   %s = %05x\n", what, got);
        return;
    }
    printf("FAIL: %s = %05x (want %05x)\n", what, got, want);
    failures_total++;
}

// Runs one adjust with AX and the arithmetic flags preloaded, and returns
// (flags & FLAG_MASK) << 16 | resulting AX.
#define MK(name, insn)                                                       \
    static unsigned name(unsigned ax, unsigned flags_in) {                   \
        unsigned out;                                                        \
        __asm__ volatile("movl %1, %%eax\n\t"                                \
                         "pushl %2\n\tpopfl\n\t"                             \
                         insn "\n\t"                                         \
                         "pushfl\n\tpopl %%edx\n\t"                          \
                         "andl $0xd5, %%edx\n\t"                             \
                         "movzwl %%ax, %%eax\n\t"                            \
                         "shll $16, %%edx\n\torl %%edx, %%eax\n\t"           \
                         "movl %%eax, %0"                                    \
                         : "=r"(out) : "r"(ax), "r"(flags_in)                \
                         : "eax", "edx", "cc", "memory");                    \
        return out;                                                          \
    }
MK(r_daa, "daa")
MK(r_das, "das")
MK(r_aaa, "aaa")
MK(r_aas, "aas")
MK(r_aam10, "aam $10")
MK(r_aam16, "aam $16")
MK(r_aad10, "aad $10")
MK(r_aad16, "aad $16")

static sigjmp_buf fpe_jmp;
static volatile sig_atomic_t fpe_signo;
static void on_fpe(int sig) { fpe_signo = sig; siglongjmp(fpe_jmp, 1); }

// Expected values captured from real Intel silicon. flags_in carries bit 1 set
// (the reserved always-one bit), plus CF (0x01) and AF (0x10) as noted.
struct bcd_case {
    unsigned ax, flags_in, want;
    const char *name;
};

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // aaa/aas adjust AX: with ax=0000 and AF set, aas must borrow through AH
    // twice (AX -= 6 then AH -= 1), giving fe0a. This is the case an 8-bit
    // implementation gets wrong.
    check("aas ax=0000 AF=1 (16-bit borrow)", r_aas(0x0000, 0x012) & 0xffff, 0xfe0a);
    check("aaa ax=0000 AF=1", r_aaa(0x0000, 0x012) & 0xffff, 0x0106);

    // aaa/aas set SF/ZF/PF from AL even though the SDM calls them undefined.
    check("aaa ax=0000 sets ZF+PF from AL",
          (r_aaa(0x0000, 0x002) >> 16) & FLAG_MASK, 0x44);   // ZF|PF
    check("aas ax=0000 sets ZF+PF from AL",
          (r_aas(0x0000, 0x002) >> 16) & FLAG_MASK, 0x44);

    // Ordinary in-range adjusts.
    check("aaa ax=0009", r_aaa(0x0009, 0x002) & 0xffff, 0x0009);
    check("aaa ax=000a", r_aaa(0x000a, 0x002) & 0xffff, 0x0100);
    check("aas ax=000a", r_aas(0x000a, 0x002) & 0xffff, 0xff04);

    // daa/das, including the >0x99 and CF-in paths.
    check("daa ax=0015", r_daa(0x0015, 0x002) & 0xffff, 0x0015);
    check("daa ax=009a", r_daa(0x009a, 0x002) & 0xffff, 0x0000);
    check("daa ax=009a CF out", (r_daa(0x009a, 0x002) >> 16) & 0x01, 1);
    check("das ax=0015 AF=1", r_das(0x0015, 0x012) & 0xffff, 0x000f);
    check("das ax=0099", r_das(0x0099, 0x002) & 0xffff, 0x0099);

    // aam splits AL into AH=AL/base, AL=AL%base.
    check("aam $10 ax=00ff", r_aam10(0x00ff, 0x002) & 0xffff, 0x1905);
    check("aam $16 ax=00ff", r_aam16(0x00ff, 0x002) & 0xffff, 0x0f0f);

    // aad folds AH*base into AL, and sets CF/AF from the implied 8-bit ADD --
    // also "undefined" per the SDM, also set by hardware.
    check("aad $10 ax=0abc", r_aad10(0x0abc, 0x002) & 0xffff, 0x0020);
    check("aad $10 ax=0abc CF", (r_aad10(0x0abc, 0x002) >> 16) & 0x01, 1);
    check("aad $10 ax=0abc AF", (r_aad10(0x0abc, 0x002) >> 16) & 0x10, 0x10);
    check("aad $16 ax=0abc", r_aad16(0x0abc, 0x002) & 0xffff, 0x005c);
    check("aad $16 ax=0abc AF clear", (r_aad16(0x0abc, 0x002) >> 16) & 0x10, 0);
    check("aad $10 ax=2799", r_aad10(0x2799, 0x002) & 0xffff, 0x001f);
    check("aad $10 ax=2799 CF", (r_aad10(0x2799, 0x002) >> 16) & 0x01, 1);

    // aam with a zero base is a divide error, not an adjust.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fpe;
        sa.sa_flags = SA_NODEFER;
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        fpe_signo = 0;
        if (sigsetjmp(fpe_jmp, 1) == 0) {
            __asm__ volatile("movl $5,%%eax\n\t.byte 0xd4,0x00" ::: "eax", "cc");
            printf("FAIL: aam $0 did not raise a signal\n");
            failures_total++;
        } else {
            check("aam $0 raises SIGFPE", (unsigned) fpe_signo, SIGFPE);
        }
    }

    if (failures_total != 0) {
        printf("bcd_adjust: FAIL failures=%u\n", failures_total);
        return 1;
    }
    printf("bcd_adjust: PASS\n");
    return 0;
}
#endif
