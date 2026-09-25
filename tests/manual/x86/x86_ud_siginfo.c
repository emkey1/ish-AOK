// x86_ud_siginfo -- an x86 invalid-opcode fault (#UD) is SIGILL with si_code
// ILL_ILLOPN and si_addr the address of the instruction.
//
// Linux x86 raises every user-mode #UD through handle_invalid_op()
// (arch/x86/kernel/traps.c): do_error_trap(..., SIGILL, ILL_ILLOPN,
// error_get_trap_addr(regs)), for 32- and 64-bit tasks alike. AOK reported
// ILL_ILLOPC, which is what arm64 and riscv64 Linux use for an undefined
// instruction, on every guest ABI. Measured on camd (Linux 6.12, Zen+) with
// both -m64 and -m32: si_code 2, si_addr the faulting instruction.
//
// Two instructions, each in its own asm function so its address is a symbol
// rather than label arithmetic (which would need different spellings for
// i386 PIC and x86_64 RIP-relative):
//   - UD2 (0f 0b), the architecturally reserved invalid opcode;
//   - 0f 0d c8, the register form of the prefetch group, which hardware
//     rejects while every memory form runs as a prefetch.
// Each is tested at the start of its function and again after two ordinary
// instructions, so a fault reported at a block start rather than at the
// instruction shows up as a wrong si_addr.
//
// x86 only; builds and runs on both guests.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("x86_ud_siginfo: SKIP (x86 only)\n");
    return 0;
}
#else

#ifndef ILL_ILLOPC
#define ILL_ILLOPC 1
#endif
#ifndef ILL_ILLOPN
#define ILL_ILLOPN 2
#endif

#define HIDDEN __attribute__((visibility("hidden")))

// The function symbols are where the call lands; the *_insn symbols are the
// faulting instruction itself.
extern void ud_ud2(void) HIDDEN;
extern void ud_ud2_mid(void) HIDDEN;
extern const char ud_ud2_mid_insn[] HIDDEN;
extern void ud_prefetch_reg(void) HIDDEN;
extern void ud_prefetch_reg_mid(void) HIDDEN;
extern const char ud_prefetch_reg_mid_insn[] HIDDEN;

#define UD_FUNC(name) \
    ".globl " #name "\n\t.hidden " #name "\n\t.type " #name ",@function\n" #name ":\n\t"
#define UD_LABEL(name) \
    ".globl " #name "\n\t.hidden " #name "\n" #name ":\n\t"

__asm__(
    ".text\n\t"
    UD_FUNC(ud_ud2)
    "ud2\n\t"
    "ret\n\t"

    UD_FUNC(ud_ud2_mid)
    "xorl %eax, %eax\n\t"
    "addl $1, %eax\n\t"
    UD_LABEL(ud_ud2_mid_insn)
    "ud2\n\t"
    "ret\n\t"

    UD_FUNC(ud_prefetch_reg)
    ".byte 0x0f, 0x0d, 0xc8\n\t"
    "ret\n\t"

    UD_FUNC(ud_prefetch_reg_mid)
    "xorl %eax, %eax\n\t"
    "addl $1, %eax\n\t"
    UD_LABEL(ud_prefetch_reg_mid_insn)
    ".byte 0x0f, 0x0d, 0xc8\n\t"
    "ret\n"
);

static sigjmp_buf env;
static volatile int got_sig, got_code;
static void *volatile got_addr;

static void on_fault(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    got_sig = sig;
    got_code = info->si_code;
    got_addr = info->si_addr;
    siglongjmp(env, 1);
}

static const char *code_name(int code) {
    switch (code) {
        case ILL_ILLOPC: return "ILL_ILLOPC";
        case ILL_ILLOPN: return "ILL_ILLOPN";
        default: return "?";
    }
}

// Run fn under SIGILL and SIGSEGV handlers, so an instruction that faults the
// wrong way is reported instead of killing the test.
static void probe(const char *what, void (*fn)(void), const void *want_addr,
                  unsigned char b0, unsigned char b1) {
    struct sigaction sa, old_ill, old_segv;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old_ill);
    sigaction(SIGSEGV, &sa, &old_segv);

    got_sig = 0;
    got_code = 0;
    got_addr = NULL;
    if (sigsetjmp(env, 1) == 0)
        fn();

    sigaction(SIGILL, &old_ill, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);

    if (got_sig != SIGILL) {
        printf("FAIL %s: %s, want SIGILL\n", what,
               got_sig == 0 ? "no signal" : strsignal(got_sig));
        failures_total++;
        return;
    }
    int ok = 1;
    if (got_code != ILL_ILLOPN) {
        printf("FAIL %s: si_code %d (%s), want %d (ILL_ILLOPN)\n", what,
               got_code, code_name(got_code), ILL_ILLOPN);
        failures_total++;
        ok = 0;
    }
    if (got_addr != want_addr) {
        printf("FAIL %s: si_addr %p, want %p (the instruction)\n", what,
               got_addr, want_addr);
        failures_total++;
        ok = 0;
    }
    // The symbol and si_addr agree; make sure both are the instruction and
    // not a neighbour of it.
    const unsigned char *p = want_addr;
    if (p[0] != b0 || p[1] != b1) {
        printf("FAIL %s: bytes at %p are %02x %02x, want %02x %02x\n", what,
               want_addr, p[0], p[1], b0, b1);
        failures_total++;
        ok = 0;
    }
    if (ok)
        test_logf("  ok   %s -> SIGILL si_code=%d si_addr=%p\n", what, got_code,
                  got_addr);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    probe("ud2", ud_ud2, (const void *) ud_ud2, 0x0f, 0x0b);
    probe("ud2 mid-block", ud_ud2_mid, ud_ud2_mid_insn, 0x0f, 0x0b);
    probe("0f 0d c8 (prefetch, register form)", ud_prefetch_reg,
          (const void *) ud_prefetch_reg, 0x0f, 0x0d);
    probe("0f 0d c8 mid-block", ud_prefetch_reg_mid, ud_prefetch_reg_mid_insn,
          0x0f, 0x0d);

    // Four faults in a row must leave the process fully usable.
    probe("ud2 again", ud_ud2, (const void *) ud_ud2, 0x0f, 0x0b);

    return finish_suite("x86_ud_siginfo");
}
#endif
