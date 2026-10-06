// amd64_xadd.c -- XADD [mem], r (0F C0 byte, 0F C1 16/32/64), with and without
// LOCK, against a model of the SDM: [mem] += r, r = the old value (a byte
// register may be AH..BH; 8/16-bit keep the rest of the register, 32
// zero-extends), the flags those of the ADD. Aligned, misaligned inside a
// 16-byte block, straddling blocks and pages. Four threads adding to one
// counter lose nothing, aligned and misaligned. A read-only page faults at
// the instruction with the register unchanged. The gadget is
// jit/gadgets-aarch64/math.S's amd64_xadd_mem*.
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
        printf("FAIL %s (%ld): %#llx, want %#llx\n", what, where, (unsigned long long) got, (unsigned long long) want);
}
#define ARITH (0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x800)
static uint64_t add_flags(uint64_t a, uint64_t b, int bits) {
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1, sb = 1ull << (bits - 1);
    a &= m; b &= m;
    uint64_t r = (a + b) & m, f = 0;
    if (r < a) f |= 0x1;
    if (!__builtin_parity((unsigned) (r & 0xff))) f |= 0x4;
    if ((a ^ b ^ r) & 0x10) f |= 0x10;
    if (r == 0) f |= 0x40;
    if (r & sb) f |= 0x80;
    if (~(a ^ b) & (a ^ r) & sb) f |= 0x800;
    return f;
}
static uint64_t rs = 0x1234567887654321ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

#define XA(insn, cons, p, reg, f) \
    __asm__ volatile(insn "\n pushfq\n pop %1" : "+" cons(reg), "=r"(f) : "r"(p) : "cc", "memory")

static void one(uint8_t *p, int bits, int lock, long where) {
    uint64_t old = rnd(), r = rnd(), r_in = r, f;
    if (where % 5 == 0) r = r_in = -old;           // a zero result
    memcpy(p, &old, 8);
    switch (bits * 2 + lock) {
    case 16: XA("xaddb %b0, (%2)", "q", p, r, f); break;
    case 17: XA("lock xaddb %b0, (%2)", "q", p, r, f); break;
    case 32: XA("xaddw %w0, (%2)", "r", p, r, f); break;
    case 33: XA("lock xaddw %w0, (%2)", "r", p, r, f); break;
    case 64: XA("xaddl %k0, (%2)", "r", p, r, f); break;
    case 65: XA("lock xaddl %k0, (%2)", "r", p, r, f); break;
    case 128: XA("xaddq %0, (%2)", "r", p, r, f); break;
    default: XA("lock xaddq %0, (%2)", "r", p, r, f); break;
    }
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1, cur;
    memcpy(&cur, p, 8);
    check("memory = old + r", where, cur, (old & ~m) | ((old + r_in) & m));
    uint64_t want_r = bits == 32 ? (uint32_t) old : bits == 64 ? old : (r_in & ~m) | (old & m);
    check("r = old", where, r, want_r);
    check("flags", where, f & ARITH, add_flags(old, r_in, bits));
}

static uint8_t *counter;
static void *worker(void *arg) {
    int bits = (int) (intptr_t) arg;
    for (int i = 0; i < 20000; i++) {
        if (bits == 64) { uint64_t one = 1; __asm__ volatile("lock xaddq %0, (%1)" : "+r"(one) : "r"(counter) : "cc", "memory"); }
        else { uint32_t one = 1; __asm__ volatile("lock xaddl %0, (%1)" : "+r"(one) : "r"(counter) : "cc", "memory"); }
    }
    return NULL;
}
static sigjmp_buf jb;
static volatile uintptr_t f_addr, f_pc;
static volatile uint64_t f_rcx;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    ucontext_t *uc = ctx;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.gregs[REG_RIP];
    f_rcx = uc->uc_mcontext.gregs[REG_RCX];
    siglongjmp(jb, 1);
}
extern char xadd_fault_insn[];

int main(void) {
    uint8_t *page = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *edge = page + 4096;
    static const int offs[] = {0, 8, 1, 3, 4, 7, 9, 12, 13, 15};
    for (int i = 0; i < 4000; i++) {
        int bits = 8 << (i & 3), lock = (i >> 2) & 1, o = offs[(i >> 3) % 10];
        one(page + 256 + o, bits, lock, o);
        one(edge - 8 + (o % 8), bits, lock, 1000 + o % 8);
    }
    // a high-byte register
    {
        uint8_t b = 0x10;
        uint64_t rax = 0x1111110511ull;   // ah = 0x05
        __asm__ volatile("lock xaddb %%ah, (%1)" : "+a"(rax) : "S"(&b) : "cc", "memory");
        check("xadd %ah: memory", 0, b, 0x15);
        check("xadd %ah: ah = old, the rest kept", 0, rax, 0x1111111011ull);
    }
    // contention
    static const int coffs[] = {0, 1, 13};
    for (int c = 0; c < 3; c++)
        for (int bits = 32; bits <= 64; bits += 32) {
            counter = page + 512 + coffs[c];
            memset(counter, 0, 8);
            pthread_t t[4];
            for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *) (intptr_t) bits);
            for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
            uint64_t v = 0;
            memcpy(&v, counter, bits / 8);
            check(bits == 64 ? "4 threads, lock xaddq: no lost update" : "4 threads, lock xaddl: no lost update", coffs[c], v, 80000);
        }
    // a read-only page
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    mprotect(edge, 4096, PROT_READ);
    for (int k = 0; k < 2; k++) {
        uint8_t *p = k ? edge - 2 : edge + 64;
        uint64_t rcx = 0x77;
        f_addr = 0;
        if (!sigsetjmp(jb, 1)) {
            if (k == 0)
                __asm__ volatile(".globl xadd_fault_insn\nxadd_fault_insn: lock xaddq %0, (%1)" : "+c"(rcx) : "r"(p) : "cc", "memory");
            else
                __asm__ volatile("lock xaddl %k0, (%1)" : "+c"(rcx) : "r"(p) : "cc", "memory");
        }
        check("read-only: SIGSEGV in the page", k, f_addr >= (uintptr_t) edge && f_addr < (uintptr_t) edge + 4096, 1);
        check("read-only: rcx unchanged", k, f_rcx, 0x77);
        if (k == 0) check("read-only: rip", k, f_pc, (uintptr_t) xadd_fault_insn);
    }
    printf("amd64_xadd: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
