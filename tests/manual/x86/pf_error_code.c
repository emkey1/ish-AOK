// x86 page-fault frames against Linux (camd, identical for -m32 and -m64):
// REG_ERR's bits -- USER always, WRITE for a store, INSTR for a fetch, PROT
// only when the page is populated with some access (not PROT_NONE, not a
// page never touched) -- si_code MAPERR/ACCERR, trap 14, CR2 and si_addr the
// address. And CR2 is per thread: a later #GP's frame still carries the last
// page fault's address. AOK used to call every PROT_NONE read a fetch, miss
// the fetch bit on a jump to nothing, set PROT for pages never touched, and
// report CR2 0 in any frame but the page fault's own.
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
#else
#define PC REG_EIP
#endif
static sigjmp_buf jb;
static volatile long g_sig, g_code, g_trap, g_err, g_cr2;
static volatile uintptr_t g_addr;
static void h(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    g_sig = sig; g_code = si->si_code; g_trap = uc->uc_mcontext.gregs[REG_TRAPNO];
    g_err = uc->uc_mcontext.gregs[REG_ERR];
#if defined(__x86_64__)
    g_cr2 = uc->uc_mcontext.gregs[REG_CR2];
#else
    g_cr2 = uc->uc_mcontext.cr2;
#endif
    g_addr = (uintptr_t) si->si_addr;
    siglongjmp(jb, 1);
}
static char *base;
static int fails;
static void rep(const char *nm, char *expect_addr, long want_code, long want_trap, long want_err, int addr_is_null) {
    int cr2_ok = g_cr2 == (long) (uintptr_t) expect_addr;
    int addr_ok = addr_is_null ? g_addr == 0 : g_addr == (uintptr_t) expect_addr;
    int ok = g_sig == SIGSEGV && g_code == want_code && g_trap == want_trap && g_err == want_err && cr2_ok && addr_ok;
    printf("%s %-24s code %ld trap %2ld err %#4lx cr2 %s addr %s", ok ? "ok  " : "FAIL", nm, g_code, g_trap, g_err,
           cr2_ok ? "ok" : "WRONG", addr_ok ? "ok" : "WRONG");
    if (!ok)
        printf("  (Linux: code %ld trap %ld err %#lx)", want_code, want_trap, want_err);
    printf("\n");
    fails += !ok;
}
#define READ(p) (*(volatile char *) (p))
#define WRITE(p) (*(volatile char *) (p) = 1)
#define EXEC(p) (((void (*)(void)) (p))())
#define CASE(nm, action, p, code, trap, err, null) do { g_sig = 0; g_cr2 = -1; \
    if (!sigsetjmp(jb, 1)) { action; } rep(nm, (char *) (p), code, trap, err, null); } while (0)
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    base = mmap(NULL, 8 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *unmapped = base;              munmap(unmapped, 4096);
    char *pnone = base + 4096;          // PROT_NONE
    char *ro_fresh = base + 2 * 4096;   mprotect(ro_fresh, 4096, PROT_READ);
    char *ro_touched = base + 3 * 4096; mprotect(ro_touched, 4096, PROT_READ); (void) READ(ro_touched);
    char *rw_noexec = base + 4 * 4096;  mprotect(rw_noexec, 4096, PROT_READ | PROT_WRITE); WRITE(rw_noexec);
    CASE("read unmapped", (void) READ(unmapped + 8), unmapped + 8, 1, 14, 0x4, 0);
    CASE("write unmapped", WRITE(unmapped + 8), unmapped + 8, 1, 14, 0x6, 0);
    CASE("exec unmapped", EXEC(unmapped + 8), unmapped + 8, 1, 14, 0x14, 0);
    CASE("read PROT_NONE", (void) READ(pnone + 8), pnone + 8, 2, 14, 0x4, 0);
    CASE("write PROT_NONE", WRITE(pnone + 8), pnone + 8, 2, 14, 0x6, 0);
    CASE("exec PROT_NONE", EXEC(pnone + 8), pnone + 8, 2, 14, 0x14, 0);
    CASE("write RO untouched", WRITE(ro_fresh + 8), ro_fresh + 8, 2, 14, 0x6, 0);
    CASE("write RO touched", WRITE(ro_touched + 8), ro_touched + 8, 2, 14, 0x7, 0);
    CASE("exec RO touched", EXEC(ro_touched + 8), ro_touched + 8, 2, 14, 0x15, 0);
    CASE("exec RW touched", EXEC(rw_noexec + 8), rw_noexec + 8, 2, 14, 0x15, 0);
    // CR2 kept: a PF at unmapped+0x10, then a #GP (hlt): CR2 still the PF address
    CASE("pf then", (void) READ(unmapped + 0x10), unmapped + 0x10, 1, 14, 0x4, 0);
    CASE("hlt: #GP keeps CR2", __asm__ volatile("hlt"), unmapped + 0x10, 128, 13, 0, 1);
    printf("pf_error_code: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
