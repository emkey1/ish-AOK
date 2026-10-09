// x86_lock_ops.c -- every LOCK read-modify-write at every alignment. Each form
// runs at each byte offset in a 16-byte block (aligned, inside the block,
// straddling two blocks) and straddling a page, with pseudo-random operands
// and incoming flags, and must leave memory (the operand and the bytes around
// it), its registers and EFLAGS exactly as the same instruction without LOCK
// does on a second copy. A straddle onto a PROT_NONE page must fault at that
// page's first byte, leaving the first page's bytes as they were. The JIT
// takes a misaligned LOCK through math.S's x86_lock_rmw (a 16-byte block
// compare-exchange, or the split lock) and the unlocked form through plain
// loads and stores, so the two share nothing but the flag code. The hash of
// every outcome is the same on hardware: checked on an AMD Ryzen (camd), 32-
// and 64-bit builds.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures, checks;
static uint64_t hash = 0xcbf29ce484222325ull;
static void mix(const void *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        hash = (hash ^ ((const uint8_t *) p)[i]) * 0x100000001b3ull;
}
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

typedef void (*op_fn)(void *p, uint64_t *r, uint64_t *a, unsigned long *fl);
struct op { const char *name; op_fn locked, plain; int bytes, bit_index, cmpxchg; };

#define FN(id, text) \
static void id(void *p, uint64_t *r, uint64_t *a, unsigned long *fl) { \
    unsigned long rr = (unsigned long) *r, aa = (unsigned long) *a, f = *fl; \
    __asm__ volatile("push %[f]\n popf\n" text "\n pushf\n pop %[f]" \
        : [r] "+r"(rr), [a] "+a"(aa), [f] "+r"(f) : [p] "r"(p) : "memory", "cc"); \
    *r = rr; *a = aa; *fl = f; \
}
#define LIST(X, S, M, B) \
    X(add_##S, "add" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(addi_##S, "add" #S " $0x5a, (%[p])", B, 0, 0) \
    X(or_##S, "or" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(adc_##S, "adc" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(sbb_##S, "sbb" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(and_##S, "and" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(sub_##S, "sub" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(subi_##S, "sub" #S " $-3, (%[p])", B, 0, 0) \
    X(xor_##S, "xor" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(adci_##S, "adc" #S " $0x7f, (%[p])", B, 0, 0) \
    X(inc_##S, "inc" #S " (%[p])", B, 0, 0) \
    X(dec_##S, "dec" #S " (%[p])", B, 0, 0) \
    X(not_##S, "not" #S " (%[p])", B, 0, 0) \
    X(neg_##S, "neg" #S " (%[p])", B, 0, 0) \
    X(xadd_##S, "xadd" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(xchg_##S, "xchg" #S " %" #M "[r], (%[p])", B, 0, 0) \
    X(cmpxchg_##S, "cmpxchg" #S " %" #M "[r], (%[p])", B, 0, 1) \
    X(bts_##S, "bts" #S " %" #M "[r], (%[p])", B, 1, 0) \
    X(btr_##S, "btr" #S " %" #M "[r], (%[p])", B, 1, 0) \
    X(btc_##S, "btc" #S " %" #M "[r], (%[p])", B, 1, 0) \
    X(btsi_##S, "bts" #S " $5, (%[p])", B, 0, 0) \
    X(btri_##S, "btr" #S " $13, (%[p])", B, 0, 0) \
    X(btci_##S, "btc" #S " $2, (%[p])", B, 0, 0)
#define DEF(id, text, B, bi, cx) FN(L_##id, "lock " text) FN(P_##id, text)
#define ROW(id, text, B, bi, cx) { #id, L_##id, P_##id, B, bi, cx },
LIST(DEF, w, w, 2)
LIST(DEF, l, k, 4)
#if defined(__x86_64__)
LIST(DEF, q, q, 8)
#endif

// CMPXCHG8B: EDX:EAX against the operand, ECX:EBX stored; r = ECX:EBX and
// a = EDX:EAX, 64 bits in both ABIs (the other forms use the low long).
#define FN8B(id, text) \
static void id(void *p, uint64_t *r, uint64_t *a, unsigned long *fl) { \
    uint64_t want = *a, put = *r; \
    unsigned lo = (unsigned) want, hi = (unsigned) (want >> 32); \
    unsigned long f = *fl; \
    __asm__ volatile("push %[f]\n popf\n" text "\n pushf\n pop %[f]" \
        : "+a"(lo), "+d"(hi), [f] "+r"(f) \
        : "b"((unsigned) put), "c"((unsigned) (put >> 32)), [p] "S"(p) : "memory", "cc"); \
    *a = (uint64_t) hi << 32 | lo; *fl = f; \
}
FN8B(L_cmpxchg8b, "lock cmpxchg8b (%[p])")
FN8B(P_cmpxchg8b, "cmpxchg8b (%[p])")

static const struct op ops[] = {
    LIST(ROW, w, w, 2)
    LIST(ROW, l, k, 4)
#if defined(__x86_64__)
    LIST(ROW, q, q, 8)
#endif
    { "cmpxchg8b", L_cmpxchg8b, P_cmpxchg8b, 8, 0, 1 },
};
#define NOPS (sizeof ops / sizeof ops[0])

#define FLAG_BITS 0x8d5ul                    // CF PF AF ZF SF OF
#define REGION 48                            // bytes compared around the operand

static void run_one(const struct op *o, uint8_t *a_at, uint8_t *b_at, int trial) {
    // the same bytes around both operands
    for (int i = -16; i < REGION - 16; i++)
        a_at[i] = b_at[i] = (uint8_t) rnd();
    uint64_t r = rnd(), acc = rnd();
    unsigned long fl = (rnd() & FLAG_BITS) | 2;
    if (o->bit_index)
        r %= (uint64_t) o->bytes * 8;        // the operand itself, not a stride away
    if (o->cmpxchg && (trial & 1)) {         // half the trials compare equal
        uint64_t cur = 0;
        memcpy(&cur, a_at, (size_t) o->bytes);
        // the accumulator's bits above the operand are not compared
        acc = o->bytes < 8 ? cur | rnd() << (o->bytes * 8) : cur;
    }
    if (o->bytes <= (int) sizeof(long)) {    // what the register forms see
        r = (unsigned long) r;
        acc = (unsigned long) acc;
    }
    uint64_t ra = r, aa = acc, rb = r, ab = acc;
    unsigned long fa = fl, fb = fl;
    o->locked(a_at, &ra, &aa, &fa);
    o->plain(b_at, &rb, &ab, &fb);
    checks++;
    if (memcmp(a_at - 16, b_at - 16, REGION) != 0 || ra != rb || aa != ab ||
            (fa & FLAG_BITS) != (fb & FLAG_BITS)) {
        if (failures++ < 30) {
            uint64_t ma = 0, mb = 0;
            memcpy(&ma, a_at, (size_t) o->bytes);
            memcpy(&mb, b_at, (size_t) o->bytes);
            printf("FAIL %s at %#lx: mem %#llx/%#llx r %#llx/%#llx a %#llx/%#llx flags %#lx/%#lx%s\n",
                   o->name, (unsigned long) (uintptr_t) a_at & 0xfff,
                   (unsigned long long) ma, (unsigned long long) mb, (unsigned long long) ra,
                   (unsigned long long) rb, (unsigned long long) aa, (unsigned long long) ab,
                   fa & FLAG_BITS, fb & FLAG_BITS,
                   memcmp(a_at - 16, b_at - 16, REGION) ? " (region differs)" : "");
        }
    }
    mix(a_at - 16, REGION);
    mix(&ra, sizeof ra);
    mix(&aa, sizeof aa);
    unsigned long fm = fa & FLAG_BITS;
    mix(&fm, sizeof fm);
}

static sigjmp_buf jb;
static void *volatile fault_addr;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) ctx;
    fault_addr = si->si_addr;
    siglongjmp(jb, 1);
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    // A and B: two pages each, both writable; F: a page, then PROT_NONE
    uint8_t *A = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *B = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *F = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (A == MAP_FAILED || B == MAP_FAILED || F == MAP_FAILED || mprotect(F + page, page, PROT_NONE) != 0) {
        printf("x86_lock_ops: mmap failed\n");
        return 1;
    }
    for (unsigned i = 0; i < NOPS; i++) {
        const struct op *o = &ops[i];
        for (int off = 0; off < 32; off++)       // every block offset, twice
            for (int t = 0; t < 6; t++)
                run_one(o, A + 64 + off, B + 64 + off, t);
        for (int k = 1; k < o->bytes; k++)      // straddling the page
            for (int t = 0; t < 6; t++)
                run_one(o, A + page - k, B + page - k, t);
    }

    // onto a PROT_NONE page: SIGSEGV at that page, the first page untouched
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (unsigned i = 0; i < NOPS; i++) {
        const struct op *o = &ops[i];
        uint8_t *at = F + page - 1;
        memset(F + page - 16, 0x5c, 16);
        uint64_t r = 1, a = 0;
        unsigned long fl = 2;
        fault_addr = NULL;
        int faulted = 0;
        if (sigsetjmp(jb, 1) == 0)
            o->locked(at, &r, &a, &fl);
        else
            faulted = 1;
        checks++;
        int intact = 1;
        for (int j = 0; j < 16; j++)
            intact &= F[page - 16 + j] == 0x5c;
        if (!faulted || fault_addr != F + page || !intact) {
            if (failures++ < 30)
                printf("FAIL %s onto PROT_NONE: faulted %d at %p (want %p), first page %s\n",
                       o->name, faulted, fault_addr, (void *) (F + page), intact ? "intact" : "written");
        }
    }
    printf("x86_lock_ops: %s (%d checks, %d failures) hash %016llx\n",
           failures ? "FAIL" : "PASS", checks, failures, (unsigned long long) hash);
    return failures != 0;
}
