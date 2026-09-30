// Privileged x86 instructions from user mode, against
// what Linux does with them (measured on camd, an AMD x86-64 box running
// Linux 6.12, for both -m32 and -m64 builds of this file).
//
// Each case is copied into an executable page, followed by a `ret`, and
// called with the accumulator pointing at a scratch buffer so memory forms
// have somewhere to point. The
// probe records which signal arrived, its si_code, REG_TRAPNO, REG_ERR and
// how far into the case the faulting instruction pointer is -- 0 means the
// fault is reported AT the instruction, as a #GP must be.
//
// Run with -v to print every case; without it the output is one line per
// case whose result differs from the Linux table below, then PASS/FAIL.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__x86_64__)
#define PC REG_RIP
#define ABI "amd64"
#else
#define PC REG_EIP
#define ABI "i386"
#endif

struct kase {
    const char *name;
    unsigned char bytes[8];
    int len;
    int misalign;   // point the accumulator at buffer+1 instead of buffer+0
};

// Memory operands are [eax]/[rax] (ModRM mod=00 rm=000).
static const struct kase cases[] = {
    { "clts",            { 0x0f, 0x06 }, 2, 0 },
    { "invd",            { 0x0f, 0x08 }, 2, 0 },
    { "wbinvd",          { 0x0f, 0x09 }, 2, 0 },
    { "mov eax,cr0",     { 0x0f, 0x20, 0xc0 }, 3, 0 },
    { "mov cr0,eax",     { 0x0f, 0x22, 0xc0 }, 3, 0 },
    { "mov eax,dr0",     { 0x0f, 0x21, 0xc0 }, 3, 0 },
    { "mov dr0,eax",     { 0x0f, 0x23, 0xc0 }, 3, 0 },
    { "wrmsr",           { 0x0f, 0x30 }, 2, 0 },
    { "rdmsr",           { 0x0f, 0x32 }, 2, 0 },
    { "rdpmc",           { 0x0f, 0x33 }, 2, 0 },
    { "lgdt [eax]",      { 0x0f, 0x01, 0x10 }, 3, 0 },
    { "lidt [eax]",      { 0x0f, 0x01, 0x18 }, 3, 0 },
    { "lmsw ax",         { 0x0f, 0x01, 0xf0 }, 3, 0 },
    { "invlpg [eax]",    { 0x0f, 0x01, 0x38 }, 3, 0 },
    { "xsetbv",          { 0x0f, 0x01, 0xd1 }, 3, 0 },
    { "lldt ax",         { 0x0f, 0x00, 0xd0 }, 3, 0 },
    { "ltr ax",          { 0x0f, 0x00, 0xd8 }, 3, 0 },
    { "hlt",             { 0xf4 }, 1, 0 },
    { "cli",             { 0xfa }, 1, 0 },
    { "sti",             { 0xfb }, 1, 0 },
};
#define NCASES (int) (sizeof(cases) / sizeof(cases[0]))

static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile long got_trapno, got_err, got_off;
static unsigned char *code;

static void handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    got_sig = sig;
    got_code = si->si_code;
    got_trapno = uc->uc_mcontext.gregs[REG_TRAPNO];
    got_err = uc->uc_mcontext.gregs[REG_ERR];
    got_off = (long) ((unsigned char *) uc->uc_mcontext.gregs[PC] - code);
    siglongjmp(jb, 1);
}

// The Linux result for each case, per ABI: signal (0 = ran), si_code,
// trapno, err, offset of the faulting instruction pointer.
struct result { int sig, code; long trapno, err, off; };
#include "priv_gp.expected.h"

int main(int argc, char **argv) {
    int verbose = argc > 1;
    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static unsigned char buf[64] __attribute__((aligned(16)));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    int fails = 0;
    for (int i = 0; i < NCASES; i++) {
        const struct kase *k = &cases[i];
        memcpy(code, k->bytes, k->len);
        code[k->len] = 0xc3;   // ret
        got_sig = 0; got_code = 0; got_trapno = got_err = got_off = 0;
        if (sigsetjmp(jb, 1) == 0) {
            void *p = buf + k->misalign;
            __asm__ volatile("call *%1" : "+a" (p) : "r" (code) : "memory",
#if defined(__x86_64__)
                    "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
#else
                    "ecx", "edx",
#endif
#if defined(__SSE__)
                    "xmm0",
#endif
                    "cc");
        }
        struct result r = { got_sig, got_code, got_trapno, got_err, got_off };
        if (verbose)
            printf("    { %d, %d, %ld, %ld, %ld },  // %s\n", r.sig, r.code, r.trapno, r.err, r.off, k->name);
        const struct result *w = &expected[i];
        if (w->sig != r.sig || w->code != r.code || w->trapno != r.trapno ||
                w->err != r.err || w->off != r.off) {
            printf("FAIL %-18s sig %d code %d trap %ld err %ld off %ld; Linux: sig %d code %d trap %ld err %ld off %ld\n",
                    k->name, r.sig, r.code, r.trapno, r.err, r.off,
                    w->sig, w->code, w->trapno, w->err, w->off);
            fails++;
        }
    }
    printf("priv_gp (" ABI "): %d cases\n", NCASES);
    printf("priv_gp: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
