// amd64_cmpxchg.c -- CMPXCHG [mem], r32/r64 (0F B1), with and without LOCK,
// against a model of the SDM: equal -> ZF=1, [mem] = src, rax untouched;
// not equal -> ZF=0, rax = [mem] (zero-extended for 32-bit), [mem] untouched;
// the flags are those of CMP rax, [mem]. Aligned operands, misaligned ones
// inside a 16-byte block, straddling blocks and straddling pages. A page that
// is not writable faults even when the compare fails; an unmapped one faults
// at the instruction with rax unchanged. Four threads incrementing one counter
// through a lock cmpxchg loop lose nothing, aligned and misaligned. The gadget
// is jit/gadgets-aarch64/math.S's amd64_cmpxchg_mem32/64.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, long where, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%ld): %#llx, want %#llx\n", what, where,
               (unsigned long long) got, (unsigned long long) want);
}

#define ARITH (0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x800)
// CMP lhs, rhs in `bits`: CF PF AF ZF SF OF
static uint64_t cmp_flags(uint64_t lhs, uint64_t rhs, int bits) {
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1, sb = 1ull << (bits - 1);
    lhs &= m; rhs &= m;
    uint64_t r = (lhs - rhs) & m, f = 0;
    if (lhs < rhs) f |= 0x1;
    if (!__builtin_parity((unsigned) (r & 0xff))) f |= 0x4;
    if ((lhs ^ rhs ^ r) & 0x10) f |= 0x10;
    if (r == 0) f |= 0x40;
    if (r & sb) f |= 0x80;
    if ((lhs ^ rhs) & (lhs ^ r) & sb) f |= 0x800;
    return f;
}

static uint64_t rs = 0x6c8e9cf570932bd5ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

#define CX(insn, p, rax_in, src, rax_out, fout) do { rax_out = rax_in; \
    __asm__ volatile(insn "\n pushfq\n pop %1" : "+a"(rax_out), "=r"(fout) \
                     : "r"(src), "r"(p) : "cc", "memory"); } while (0)

static void one(uint8_t *p, int bits, int lock, int equal, long where) {
    uint64_t old = rnd(), src = rnd(), rax_in = equal ? old : rnd();
    if (bits == 32 && equal)
        rax_in = (rnd() << 32) | (uint32_t) old;   // only eax is compared
    memcpy(p, &old, 8);
    uint8_t before[8];
    memcpy(before, p, 8);
    uint64_t rax = 0, f;
    if (bits == 64) {
        if (lock) CX("lock cmpxchgq %2, (%3)", p, rax_in, src, rax, f);
        else      CX("cmpxchgq %2, (%3)", p, rax_in, src, rax, f);
    } else {
        if (lock) CX("lock cmpxchgl %k2, (%3)", p, rax_in, src, rax, f);
        else      CX("cmpxchgl %k2, (%3)", p, rax_in, src, rax, f);
    }
    uint64_t m = bits == 64 ? ~0ull : 0xffffffffull, cur;
    memcpy(&cur, p, 8);
    int eq = ((rax_in ^ old) & m) == 0;
    check("flags", where, f & ARITH, cmp_flags(rax_in, old, bits));
    if (eq) {
        uint64_t want = (old & ~m) | (src & m);
        check("equal: memory = src", where, cur, want);
        check("equal: rax untouched", where, rax, rax_in);
    } else {
        check("not equal: memory untouched", where, memcmp(p, before, 8) != 0, 0);
        check("not equal: rax = old", where, rax, bits == 64 ? old : (uint32_t) old);
    }
}

static sigjmp_buf jb;
static volatile uintptr_t f_addr, f_pc;
static volatile uint64_t f_rax;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    ucontext_t *uc = ctx;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.gregs[REG_RIP];
    f_rax = uc->uc_mcontext.gregs[REG_RAX];
    siglongjmp(jb, 1);
}
extern char cx_fault_insn[];

static uint8_t *counter_at;
static void *worker(void *arg) {
    int bits = (int) (intptr_t) arg;
    for (int i = 0; i < 20000; i++) {
        if (bits == 64) {
            uint64_t old, neu, ok;
            do {
                memcpy(&old, counter_at, 8);
                neu = old + 1;
                uint64_t rax = old;
                __asm__ volatile("lock cmpxchgq %2, (%3)\n setz %b1"
                                 : "+a"(rax), "=r"(ok) : "r"(neu), "r"(counter_at) : "cc", "memory");
                ok &= 1;
            } while (!ok);
        } else {
            uint32_t old, neu;
            uint64_t ok;
            do {
                memcpy(&old, counter_at, 4);
                neu = old + 1;
                uint32_t rax = old;
                __asm__ volatile("lock cmpxchgl %2, (%3)\n setz %b1"
                                 : "+a"(rax), "=r"(ok) : "r"(neu), "r"(counter_at) : "cc", "memory");
                ok &= 1;
            } while (!ok);
        }
    }
    return NULL;
}

int main(void) {
    uint8_t *page = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *edge = page + 4096;
    // offsets relative to a 16-byte block inside the page, and across the page edge
    static const int offs[] = {0, 8, 1, 3, 4, 7, 9, 12, 13, 15};
    for (int i = 0; i < 3000; i++) {
        int bits = (i & 1) ? 64 : 32, lock = (i >> 1) & 1, equal = (i >> 2) & 1;
        int o = offs[(i >> 3) % 10];
        one(page + 256 + o, bits, lock, equal, o);         // in a block or straddling one
        one(edge - 8 + (o % 8), bits, lock, equal, 1000 + o % 8); // up to straddling the page
    }
    // faults: read-only (even when the compare fails) and unmapped
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    mprotect(edge, 4096, PROT_READ);
    for (int k = 0; k < 4; k++) {
        uint8_t *p = k < 2 ? edge + 64 : edge - 2;   // aligned on the page; straddling into it
        int bits = k & 1 ? 64 : 32;
        uint64_t rax = 0x1111, mem;
        memcpy(&mem, edge + 64, 8);
        f_addr = 0;
        if (!sigsetjmp(jb, 1)) {
            if (bits == 64)
                __asm__ volatile(".globl cx_fault_insn\ncx_fault_insn: lock cmpxchgq %1, (%2)"
                                 : "+a"(rax) : "r"(0ul), "r"(p) : "cc", "memory");
            else
                __asm__ volatile("lock cmpxchgl %k1, (%2)" : "+a"(rax) : "r"(0ul), "r"(p) : "cc", "memory");
        }
        check("read-only page: SIGSEGV in it", k, f_addr >= (uintptr_t) edge && f_addr < (uintptr_t) edge + 4096, 1);
        check("read-only page: rax unchanged", k, f_rax, 0x1111);
        if (k == 1)
            check("read-only page: rip is the instruction", k, f_pc, (uintptr_t) cx_fault_insn);
    }
    munmap(edge, 4096);
    {
        uint64_t rax = 0x2222;
        f_addr = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("cmpxchgq %1, (%2)" : "+a"(rax) : "r"(0ul), "r"(edge - 4) : "cc", "memory");
        check("unmapped: SIGSEGV at the page", 0, f_addr >= (uintptr_t) edge && f_addr < (uintptr_t) edge + 8, 1);
        check("unmapped: rax unchanged", 0, f_rax, 0x2222);
    }
    // contention: aligned, misaligned in a block, straddling blocks
    static const int coffs[] = {0, 1, 13};
    for (int c = 0; c < 3; c++) {
        for (int bits = 32; bits <= 64; bits += 32) {
            counter_at = page + 512 + coffs[c];
            memset(counter_at, 0, 8);
            pthread_t t[4];
            for (int i = 0; i < 4; i++)
                pthread_create(&t[i], NULL, worker, (void *) (intptr_t) bits);
            for (int i = 0; i < 4; i++)
                pthread_join(t[i], NULL);
            uint64_t v = 0;
            memcpy(&v, counter_at, bits / 8);
            check(bits == 64 ? "4 threads, lock cmpxchgq: no lost update"
                             : "4 threads, lock cmpxchgl: no lost update", coffs[c], v, 80000);
        }
    }
    printf("amd64_cmpxchg: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
