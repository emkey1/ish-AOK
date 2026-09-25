// null_deref_siginfo -- a load or store at (or just above) address 0 is
// SIGSEGV with si_code SEGV_MAPERR and si_addr the address touched, whatever
// the instruction.
//
// The i386 JIT reports a failed access by storing its address and raising a
// GPF; the kernel's GPF handler took an address of 0 to mean "none reported"
// and fell back to decoding the instruction itself. Its decoder knows the
// plain MOV forms and little else, so a NULL dereference through anything
// else -- `cmp eax, [ecx]` is how HotSpot's interpreter and C1 code do their
// implicit null checks -- came out as SI_KERNEL with si_addr at the PC. HotSpot
// then could not tell it from a real crash, and every Java program on the
// i386 guest died at its first NullPointerException. Addresses above 0 were
// always reported correctly; they are here as the control.
//
// x86 only; builds and runs on both guests. Each probe is inline asm with the
// address in ECX/RCX, so the instruction under test is exactly the one named.
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
    printf("null_deref_siginfo: SKIP (x86 only)\n");
    return 0;
}
#else

static sigjmp_buf env;
static volatile int got_sig, got_code;
static void *volatile got_addr;

static void on_segv(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    got_sig = sig;
    got_code = info->si_code;
    got_addr = info->si_addr;
    siglongjmp(env, 1);
}

// Loads.
static void p_mov_load(uintptr_t a) {           // 8B /r: the decoder knows it
    unsigned long v;
    __asm__ volatile("mov (%1), %0" : "=r"(v) : "c"(a) : "memory");
}
static void p_cmp_load(uintptr_t a) {           // 3B /r: HotSpot's null check
    __asm__ volatile("cmp (%0), %%eax" : : "c"(a), "a"(0) : "cc", "memory");
}
static void p_test_load(uintptr_t a) {          // 85 /r
    __asm__ volatile("test %%eax, (%0)" : : "c"(a), "a"(1) : "cc", "memory");
}
static void p_movzx_load(uintptr_t a) {         // 0F B6 /r
    unsigned v;
    __asm__ volatile("movzbl (%1), %0" : "=r"(v) : "c"(a) : "memory");
}
static void p_movsd_load(uintptr_t a) {         // F2 0F 10 /r (SSE2)
    __asm__ volatile("movsd (%0), %%xmm0" : : "c"(a) : "memory");
}
// Stores.
static void p_mov_store(uintptr_t a) {          // 89 /r: the decoder knows it
    __asm__ volatile("mov %%eax, (%0)" : : "c"(a), "a"(0) : "memory");
}
static void p_add_store(uintptr_t a) {          // 01 /r: read-modify-write
    __asm__ volatile("add %%eax, (%0)" : : "c"(a), "a"(1) : "cc", "memory");
}
static void p_inc_store(uintptr_t a) {          // FF /0
    __asm__ volatile("incl (%0)" : : "c"(a) : "cc", "memory");
}

struct probe {
    const char *name;
    void (*run)(uintptr_t);
};

static const struct probe probes[] = {
    { "mov load",   p_mov_load },
    { "cmp load",   p_cmp_load },
    { "test load",  p_test_load },
    { "movzx load", p_movzx_load },
    { "movsd load", p_movsd_load },
    { "mov store",  p_mov_store },
    { "add store",  p_add_store },
    { "inc store",  p_inc_store },
};

int main(int argc, char **argv) {
    test_init(argc, argv);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    static const uintptr_t addrs[] = { 0, 4, 0x100 };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        for (size_t j = 0; j < sizeof addrs / sizeof addrs[0]; j++) {
            uintptr_t a = addrs[j];
            got_sig = got_code = 0;
            got_addr = (void *) -1;
            if (sigsetjmp(env, 1) == 0) {
                probes[i].run(a);
                printf("FAIL %s at %#lx: no signal\n", probes[i].name, (unsigned long) a);
                failures_total++;
                continue;
            }
            int ok = got_sig == SIGSEGV && got_code == SEGV_MAPERR &&
                     (uintptr_t) got_addr == a;
            test_logf("  %-10s at %#6lx: sig %d code %d addr %p\n", probes[i].name,
                      (unsigned long) a, got_sig, got_code, got_addr);
            if (!ok) {
                printf("FAIL %s at %#lx: sig %d si_code %d si_addr %p "
                       "(want SIGSEGV, SEGV_MAPERR %d, %#lx)\n",
                       probes[i].name, (unsigned long) a, got_sig, got_code,
                       got_addr, SEGV_MAPERR, (unsigned long) a);
                failures_total++;
            }
        }
    }
    return finish_suite("null_deref_siginfo");
}
#endif
