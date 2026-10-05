// riscv64_amo_align.c -- a misaligned LR, SC or AMO is an address-misaligned
// trap, which Linux does not emulate for atomics: SIGBUS with BUS_ADRALN and
// si_addr the address, at the instruction, with rd and memory unchanged. AOK
// does not claim Zama16b. Every AMO (.w and .d), LR and SC, at each
// misaligned offset; the same instructions aligned must work (the positive
// control: the probe really runs them).
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static sigjmp_buf jb;
static volatile int f_sig, f_code;
static volatile uintptr_t f_addr, f_pc;
static void on_sig(int sig, siginfo_t *si, void *ctx) {
    f_sig = sig;
    f_code = si->si_code;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = ((ucontext_t *) ctx)->uc_mcontext.__gregs[0];   // REG_PC
    siglongjmp(jb, 1);
}

static unsigned long checks, bad;
static void check(const char *what, int off, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s +%d: %#llx, want %#llx\n", what, off,
               (unsigned long long) got, (unsigned long long) want);
}

static uint8_t buf[64] __attribute__((aligned(16)));

// One instruction at a label, rd preset to a marker; reports rd.
#define RUN(insn, p, rs2, rd_out, pc_out) do { \
    register long rd_ __asm__("a0") = 0x5a5a; \
    __asm__ volatile("lla %1, 1f\n1: " insn "\n" \
                     : "+r"(rd_), "=&r"(pc_out) : "r"(p), "r"(rs2) : "memory"); \
    rd_out = rd_; } while (0)

#define CASE(name, insn, bytes, aligned_rd) do { \
    for (int off = 0; off < bytes; off++) { \
        for (int i = 0; i < 64; i++) buf[i] = (uint8_t) (i * 7 + 1); \
        uint8_t before[64]; memcpy(before, buf, 64); \
        uint8_t *p = buf + 16 + off; \
        volatile long rd = -1; uintptr_t pc = 0; \
        f_sig = 0; \
        if (!sigsetjmp(jb, 1)) { \
            if (name[0] == 's' && bytes == 4) /* sc: reserve first */ \
                __asm__ volatile("lr.w zero, (%0)" :: "r"(buf + 16) : "memory"); \
            if (name[0] == 's' && bytes == 8) \
                __asm__ volatile("lr.d zero, (%0)" :: "r"(buf + 16) : "memory"); \
            long r_; uintptr_t pc_; RUN(insn, p, 3L, r_, pc_); rd = r_; pc = pc_; \
        } \
        if (off == 0) { \
            check(name " aligned: no signal", off, f_sig, 0); \
            check(name " aligned: rd", off, (uint64_t) rd, (uint64_t) (aligned_rd)); \
        } else { \
            check(name ": SIGBUS", off, f_sig, SIGBUS); \
            check(name ": BUS_ADRALN", off, f_code, BUS_ADRALN); \
            check(name ": si_addr", off, f_addr, (uintptr_t) p); \
            check(name ": memory unchanged", off, memcmp(buf, before, 64) != 0, 0); \
        } \
    } } while (0)

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    // aligned rd: the old value at buf+16 (bytes 0x71, 0x78, ...), sign-
    // extended for .w; sc gives 0 (success) after the lr in CASE
    int64_t w = (int32_t) (0x71 | 0x78 << 8 | 0x7f << 16 | 0x86u << 24);
    uint64_t d = 0;
    for (int i = 7; i >= 0; i--) d = d << 8 | (uint8_t) ((16 + i) * 7 + 1);
    CASE("amoswap.w", "amoswap.w %0, %3, (%2)", 4, w);
    CASE("amoadd.w", "amoadd.w %0, %3, (%2)", 4, w);
    CASE("amoxor.w", "amoxor.w %0, %3, (%2)", 4, w);
    CASE("amoand.w", "amoand.w %0, %3, (%2)", 4, w);
    CASE("amoor.w", "amoor.w %0, %3, (%2)", 4, w);
    CASE("amomin.w", "amomin.w %0, %3, (%2)", 4, w);
    CASE("amomax.w", "amomax.w %0, %3, (%2)", 4, w);
    CASE("amominu.w", "amominu.w %0, %3, (%2)", 4, w);
    CASE("amomaxu.w", "amomaxu.w %0, %3, (%2)", 4, w);
    CASE("amoswap.d", "amoswap.d %0, %3, (%2)", 8, d);
    CASE("amoadd.d", "amoadd.d %0, %3, (%2)", 8, d);
    CASE("amoxor.d", "amoxor.d %0, %3, (%2)", 8, d);
    CASE("amoand.d", "amoand.d %0, %3, (%2)", 8, d);
    CASE("amoor.d", "amoor.d %0, %3, (%2)", 8, d);
    CASE("amomin.d", "amomin.d %0, %3, (%2)", 8, d);
    CASE("amomax.d", "amomax.d %0, %3, (%2)", 8, d);
    CASE("amominu.d", "amominu.d %0, %3, (%2)", 8, d);
    CASE("amomaxu.d", "amomaxu.d %0, %3, (%2)", 8, d);
    CASE("lr.w", "lr.w %0, (%2)", 4, w);
    CASE("lr.d", "lr.d %0, (%2)", 8, d);
    CASE("sc.w", "sc.w %0, %3, (%2)", 4, 0);
    CASE("sc.d", "sc.d %0, %3, (%2)", 8, 0);
    printf("riscv64_amo_align: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
