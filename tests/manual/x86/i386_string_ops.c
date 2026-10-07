// i386_string_ops.c -- amd64_string_ops.c for the i386 guest: MOVS STOS LODS
// SCAS CMPS in 1/2/4 bytes, with no prefix, REP/REPE and REPNE, both
// directions, against a model of the SDM: what moves, where ESI/EDI/ECX end,
// EAX for LODS, the CMP flags and where REPE/REPNE stop. Counts reach across
// pages (the page-span path), elements straddle a page, the destination one
// element ahead of the source smears (per element), ECX = 0 does nothing. A
// fault mid-REP leaves ECX/ESI/EDI at the faulting element with EIP at the
// instruction, and returning re-executes it to the end. A 1 ms timer
// interrupts a long REP and the copy still comes out right. The gadgets are
// jit/gadgets-aarch64/string.S's.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned long checks, bad;
static void check(const char *what, long k, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("FAIL %s (%ld): %#llx, want %#llx\n", what, k, (unsigned long long) got, (unsigned long long) want);
}

static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

#define CF 0x1
#define PF 0x4
#define AF 0x10
#define ZF 0x40
#define SF 0x80
#define OF 0x800
#define ARITH (CF | PF | AF | ZF | SF | OF)
static uint64_t cmp_flags(uint64_t a, uint64_t b, int bytes) {
    int bits = bytes * 8;
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1, sb = 1ull << (bits - 1);
    a &= m; b &= m;
    uint64_t r = (a - b) & m, f = 0;
    if (a < b) f |= CF;
    if (!__builtin_parity((unsigned) (r & 0xff))) f |= PF;
    if ((a ^ b ^ r) & 0x10) f |= AF;
    if (r == 0) f |= ZF;
    if (r & sb) f |= SF;
    if ((a ^ b) & (a ^ r) & sb) f |= OF;
    return f;
}
static uint64_t rd(const uint8_t *p, int n) { uint64_t v = 0; memcpy(&v, p, n); return v; }

struct regs { uint32_t rsi, rdi, rcx, rax, flags; };

// One instruction, all five registers in and out, flags out; DF from `df`.
#define STR(name, insn) \
    static struct regs name(struct regs r, int df) { \
        __asm__ volatile("test %5, %5\n jz 1f\n std\n 1:\n" insn "\n pushfl\n pop %4\n cld" \
                         : "+S"(r.rsi), "+D"(r.rdi), "+c"(r.rcx), "+a"(r.rax), "=m"(r.flags) \
                         : "r"((long) df) : "cc", "memory"); \
        return r; }
#define SET(op, s) STR(op##s, #op #s) STR(rep_##op##s, "rep " #op #s) STR(repne_##op##s, "repne " #op #s)
SET(movs, b) SET(movs, w) SET(movs, l)
SET(stos, b) SET(stos, w) SET(stos, l)
SET(lods, b) SET(lods, w) SET(lods, l)
SET(scas, b) SET(scas, w) SET(scas, l)
SET(cmps, b) SET(cmps, w) SET(cmps, l)
typedef struct regs (*str_fn)(struct regs, int);
static const str_fn fns[5][3][3] = {
#define ROW(op) { {op##b, rep_##op##b, repne_##op##b}, {op##w, rep_##op##w, repne_##op##w}, \
                  {op##l, rep_##op##l, repne_##op##l} }
    ROW(movs), ROW(stos), ROW(lods), ROW(scas), ROW(cmps),
};
enum { MOVS, STOS, LODS, SCAS, CMPS };

#define AREA (6 * 4096)
static uint8_t *A, *B, *mA, *mB;      // memory and its model

// The model: run one (REP) string op on mA/mB.
static struct regs model(int op, int n, int rep, int df, struct regs r) {
    int32_t step = df ? -n : n;
    uint32_t count = rep ? r.rcx : 1;
    uint32_t f = r.flags;
    while (count) {
        uint8_t *s = (uint8_t *) (uintptr_t) r.rsi, *d = (uint8_t *) (uintptr_t) r.rdi;
        uint8_t *ms = s >= A && s < A + AREA ? mA + (s - A) : mB + (s - B);
        uint8_t *md = d >= A && d < A + AREA ? mA + (d - A) : mB + (d - B);
        uint64_t m = n == 8 ? ~0ull : (1ull << (8 * n)) - 1;
        if (op == MOVS) { memmove(md, ms, n); r.rsi += step; r.rdi += step; }
        else if (op == STOS) { memcpy(md, &r.rax, n); r.rdi += step; }
        else if (op == LODS) { uint64_t v = rd(ms, n); r.rax = n == 4 ? (uint32_t) v : (uint32_t) ((r.rax & ~m) | v); r.rsi += step; }
        else if (op == SCAS) { f = (f & ~ARITH) | cmp_flags(r.rax, rd(md, n), n); r.rdi += step; }
        else { f = (f & ~ARITH) | cmp_flags(rd(ms, n), rd(md, n), n); r.rsi += step; r.rdi += step; }
        if (!rep) break;
        count--;
        r.rcx = count;
        if (op == SCAS || op == CMPS) {
            if (rep == 1 && !(f & ZF)) break;
            if (rep == 2 && (f & ZF)) break;
        }
    }
    r.flags = f;
    return r;
}

static void fill(uint8_t *p, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t) (i * 31 + seed); }

static void one(int op, int lg, int rep, int df, uint64_t count, long so, long dof, int k) {
    int n = 1 << lg;
    fill(A, AREA, (unsigned) k); fill(B, AREA, (unsigned) k * 7 + 1);
    // matching runs for CMPS/SCAS, broken at a random point
    if (op == CMPS && (k & 1)) memcpy(B + dof, A + so, (size_t) (count * n < 4096 ? count * n : 4096));
    memcpy(mA, A, AREA); memcpy(mB, B, AREA);
    struct regs in = { (uint32_t) (uintptr_t) (A + so), (uint32_t) (uintptr_t) (B + dof), (uint32_t) count, (uint32_t) rnd(), 0 };
    if (op == SCAS && (k & 1)) { uint64_t v = rd(B + dof, n); in.rax = v; }
    if (df) { in.rsi += 0; }
    struct regs got = fns[op][lg][rep](in, df);
    in.flags = got.flags;   // flags not written by MOVS/STOS/LODS come back as they were
    struct regs want = model(op, n, rep, df, in);
    static const char *names[] = {"movs", "stos", "lods", "scas", "cmps"};
    char what[64];
    snprintf(what, sizeof(what), "%s%d rep%d df%d count %llu", names[op], n, rep, df, (unsigned long long) count);
    check(what, k, memcmp(A, mA, AREA) == 0 && memcmp(B, mB, AREA) == 0, 1);
    check("rsi", k, got.rsi, want.rsi);
    check("rdi", k, got.rdi, want.rdi);
    check("rcx", k, got.rcx, rep ? want.rcx : count);
    if (op == LODS) check("rax", k, got.rax, want.rax);
    if (op == SCAS || op == CMPS) check("flags", k, got.flags & ARITH, want.flags & ARITH);
}

static sigjmp_buf jb;
static volatile uint32_t f_addr, f_pc;
static volatile uint32_t f_rcx, f_rsi, f_rdi;
static uint8_t *hole;
static volatile int remap;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    ucontext_t *uc = ctx;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.gregs[REG_EIP];
    f_rcx = uc->uc_mcontext.gregs[REG_ECX];
    f_rsi = uc->uc_mcontext.gregs[REG_ESI];
    f_rdi = uc->uc_mcontext.gregs[REG_EDI];
    if (remap) {   // map the page and return: the instruction resumes
        mmap(hole, 4096, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        remap = 0;
        return;
    }
    siglongjmp(jb, 1);
}
static volatile int ticks, mid_rep;
extern char str_fault_insn[], timer_rep_movsb[], timer_rep_stosl[];
static void on_alrm(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    ticks++;
    uintptr_t pc = uc->uc_mcontext.gregs[REG_EIP];
    if ((pc == (uintptr_t) timer_rep_movsb || pc == (uintptr_t) timer_rep_stosl) &&
            uc->uc_mcontext.gregs[REG_ECX] != 0)
        mid_rep++;   // delivered in the middle of the REP, which then resumes
}

int main(void) {
    A = mmap(NULL, AREA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    B = mmap(NULL, AREA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mA = malloc(AREA); mB = malloc(AREA);
    int k = 0;
    static const uint64_t counts[] = {0, 1, 2, 3, 7, 64, 513, 1000, 4097};
    for (int op = 0; op < 5; op++)
        for (int lg = 0; lg < 3; lg++)
            for (int rep = 0; rep < 3; rep++)
                for (int df = 0; df < 2; df++)
                    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++, k++) {
                        uint64_t count = counts[c];
                        if (!rep && c > 1) continue;
                        uint64_t bytes = count << lg;
                        if (bytes > 2 * 4096) count = (2 * 4096) >> lg, bytes = count << lg;
                        // start offsets: forward from near a page edge, backward from the far end
                        long so = df ? 4096 * 3 + 37 + (long) (rnd() % 64) : 4096 - 5 - (long) (rnd() % 64);
                        long dof = df ? 4096 * 3 + 11 + (long) (rnd() % 64) : 4096 - 19 - (long) (rnd() % 64);
                        one(op, lg, rep, df, count, so, dof, k);
                        // aligned to the element, and one byte off it (an element straddles each page edge)
                        if (!df) {
                            one(op, lg, rep, df, count, 4096 - 64, 4096 - 128, k);
                            one(op, lg, rep, df, count, 4096 - 63, 4096 - 125, k);
                        }
                    }
    // MOVS inside one buffer: the destination one element ahead smears, the
    // destination behind copies forward
    for (int lg = 0; lg < 3; lg++) {
        int n = 1 << lg;
        for (int dir = 0; dir < 4; dir++) {
            fill(A, AREA, 3); memcpy(mA, A, AREA); memcpy(mB, B, AREA);
            // ahead by an element, behind by one, ahead by 100 bytes (inside a span), ahead by 5000 (beyond one)
            long so = 4000, dof = dir == 0 ? 4000 - n : dir == 1 ? 4000 + n : dir == 2 ? 4100 : 9000;
            struct regs in = { (uint32_t) (uintptr_t) (A + so), (uint32_t) (uintptr_t) (A + dof), (uint32_t) (9000 / n), 0, 0 };
            struct regs got = fns[MOVS][lg][1](in, 0);
            struct regs want = model(MOVS, n, 1, 0, in);
            check(dir ? "rep movs, dst ahead of src: smears" : "rep movs, dst = src - n", lg * 4 + dir, memcmp(A, mA, AREA), 0);
            check("rep movs overlap: rdi", lg, got.rdi, want.rdi);
        }
    }
    // faults mid-REP: the restart state, then resume to the end
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (int lg = 0; lg < 3; lg++) {
        hole = B + 2 * 4096;
        munmap(hole, 4096);
        fill(A, AREA, 21);
        uint32_t n = 1 << lg, count = (3 * 4096) / n, rsi = (uint32_t) (uintptr_t) A, rdi = (uint32_t) (uintptr_t) B, rcx = count;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile(".globl str_fault_insn\nstr_fault_insn: rep movsb" : "+S"(rsi), "+D"(rdi), "+c"(rcx) :: "memory");
        // only the byte form has the label; the others are checked by state
        check("fault: in the hole", lg, f_addr, (uintptr_t) hole);
        check("fault: rdi at the hole", lg, f_rdi, (uintptr_t) hole);
        check("fault: rsi in step", lg, f_rsi - (uintptr_t) A, f_rdi - (uintptr_t) B);
        check("fault: rcx = what is left", lg, f_rcx, count - (f_rdi - (uintptr_t) B));
        check("fault: rip", lg, f_pc, (uintptr_t) str_fault_insn);
        // the resume: the handler maps the page and returns
        remap = 1;
        rsi = (uint32_t) (uintptr_t) A; rdi = (uint32_t) (uintptr_t) B; rcx = count;
        memset(B, 0, 2 * 4096);
        munmap(hole, 4096);
        __asm__ volatile("rep movsb" : "+S"(rsi), "+D"(rdi), "+c"(rcx) :: "memory");
        check("fault then resume: the whole copy", lg, memcmp(A, B, 3 * 4096), 0);
        check("fault then resume: rcx", lg, rcx, 0);
        break;   // one size is enough for the resume
    }
    // a 1 ms timer through a long backward REP STOSQ and a forward REP MOVSB
    {
        struct sigaction sal;
        memset(&sal, 0, sizeof(sal));
        sal.sa_sigaction = on_alrm;
        sal.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGALRM, &sal, NULL);
        struct itimerval it = { {0, 1000}, {0, 1000} };
        setitimer(ITIMER_REAL, &it, NULL);
        // 1 MB, many times over: after the first pass every page is in the
        // TLB, so nothing but the REP's own poke check lets a signal in
        size_t big = 1 << 20;
        uint8_t *x = malloc(big), *y = malloc(big);
        for (int rep = 0; rep < 400; rep++) {
            memset(x, 0x5a, big);
            uint32_t rsi = (uint32_t) (uintptr_t) x, rdi = (uint32_t) (uintptr_t) y, rcx = big;
            __asm__ volatile(".globl timer_rep_movsb\n timer_rep_movsb: rep movsb"
                             : "+S"(rsi), "+D"(rdi), "+c"(rcx) :: "memory");
            uint32_t rax = 0x11111111u;
            rdi = (uint32_t) (uintptr_t) (x + big - 4); rcx = big / 4;
            __asm__ volatile("std\n .globl timer_rep_stosl\n timer_rep_stosl: rep stosl\n cld"
                             : "+D"(rdi), "+c"(rcx) : "a"(rax) : "memory", "cc");
        }
        struct itimerval off = { {0, 0}, {0, 0} };
        setitimer(ITIMER_REAL, &off, NULL);
        check("timer: the movsb copy", 0, y[0] == 0x5a && y[big - 1] == 0x5a && y[big / 2] == 0x5a, 1);
        check("timer: the stosl fill", 0, x[0] == 0x11 && x[big - 1] == 0x11, 1);
        check("timer: ticks delivered", 0, ticks > 0, 1);
        check("timer: some arrived mid-REP (the REP is interruptible)", 0, mid_rep > 0, 1);
        {
            uint32_t rdi = (uint32_t) (uintptr_t) y, rcx = big / 4, rax = 0x22222222u;
            __asm__ volatile("rep stosl" : "+D"(rdi), "+c"(rcx) : "a"(rax) : "memory");
            check("forward rep stosl: filled", 0, y[0] == 0x22 && y[big - 1] == 0x22 && rcx == 0, 1);
        }
    }
    printf("i386_string_ops: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
