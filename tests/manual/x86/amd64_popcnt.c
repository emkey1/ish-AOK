// amd64_popcnt.c -- POPCNT (F3 0F B8) r, r/m in 16, 32 and 64 bits, register
// and memory sources, against a model of the SDM: the count of set bits; ZF
// set iff the source is zero; CF, OF, SF, AF and PF cleared (each tried from
// all-set and all-clear flags); a 16-bit destination keeps bits 16-63, a
// 32-bit one is zero-extended; a source crossing into an unmapped page faults
// at the instruction with the destination unchanged. The gadgets are
// jit/gadgets-aarch64/math.S's amd64_popcnt_*.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t in, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (src %#llx): %#llx, want %#llx\n", what, (unsigned long long) in,
               (unsigned long long) got, (unsigned long long) want);
}
static unsigned bits(uint64_t v) { unsigned n = 0; for (; v; v >>= 1) n += v & 1; return n; }

#define ARITH (0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x800)    // CF PF AF ZF SF OF
static uint64_t want_flags(uint64_t before, uint64_t src) {
    return (before & ~(uint64_t) ARITH) | (src == 0 ? 0x40 : 0);
}

// The register form: rdx = popcnt(rcx), flags preset to fin.
#define REG(sz, insn, src, dstin, fin, dst, fout) \
    __asm__ volatile("push %4\n popfq\n" insn "\n pushfq\n pop %1" \
                     : "=d"(dst), "=r"(fout) : "c"(src), "0"(dstin), "r"(fin) : "cc")
#define MEM(sz, insn, p, dstin, fin, dst, fout) \
    __asm__ volatile("push %4\n popfq\n" insn "\n pushfq\n pop %1" \
                     : "=d"(dst), "=r"(fout) : "b"(p), "0"(dstin), "r"(fin) : "cc", "memory")

static uint64_t rs = 0x853c49e6748fea9bull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static sigjmp_buf jb;
static volatile uintptr_t f_addr, f_pc;
static volatile uint64_t f_rdx;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    ucontext_t *uc = ctx;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.gregs[REG_RIP];
    f_rdx = uc->uc_mcontext.gregs[REG_RDX];
    siglongjmp(jb, 1);
}
extern char popcnt_fault_insn[];

int main(void) {
    uint8_t *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *edge = page + 4096;
    for (int i = 0; i < 4000; i++) {
        uint64_t src = rnd();
        if (i % 7 == 0) src = 0;
        if (i % 11 == 0) src = ~0ull;
        if (i % 13 == 0) src = 1ull << (rnd() % 64);
        uint64_t dstin = rnd(), fin = (i & 1) ? (0x202 | ARITH) : 0x202;
        uint64_t dst, fout;
        // register sources
        REG(64, "popcnt %%rcx, %%rdx", src, dstin, fin, dst, fout);
        check("popcnt r64", src, dst, bits(src));
        check("popcnt r64 flags", src, fout & 0xfd5, want_flags(fin, src) & 0xfd5);
        REG(32, "popcnt %%ecx, %%edx", src, dstin, fin, dst, fout);
        check("popcnt r32 (zero-extends)", src, dst, bits((uint32_t) src));
        check("popcnt r32 flags", src, fout & 0xfd5, want_flags(fin, (uint32_t) src) & 0xfd5);
        REG(16, "popcnt %%cx, %%dx", src, dstin, fin, dst, fout);
        check("popcnt r16 (keeps 16-63)", src, dst, (dstin & ~0xffffull) | bits((uint16_t) src));
        check("popcnt r16 flags", src, fout & 0xfd5, want_flags(fin, (uint16_t) src) & 0xfd5);
        // memory sources, ending at a page edge and crossing it
        unsigned off = (unsigned) (rnd() % 12);
        uint8_t *p = edge - 8 + off - 4;
        memcpy(p, &src, 8);
        uint64_t m;
        memcpy(&m, p, 8);
        MEM(64, "popcntq (%%rbx), %%rdx", p, dstin, fin, dst, fout);
        check("popcnt m64", m, dst, bits(m));
        check("popcnt m64 flags", m, fout & 0xfd5, want_flags(fin, m) & 0xfd5);
        MEM(32, "popcntl (%%rbx), %%edx", p, dstin, fin, dst, fout);
        check("popcnt m32", m, dst, bits((uint32_t) m));
        check("popcnt m32 flags", m, fout & 0xfd5, want_flags(fin, (uint32_t) m) & 0xfd5);
        MEM(16, "popcntw (%%rbx), %%dx", p, dstin, fin, dst, fout);
        check("popcnt m16", m, dst, (dstin & ~0xffffull) | bits((uint16_t) m));
        check("popcnt m16 flags", m, fout & 0xfd5, want_flags(fin, (uint16_t) m) & 0xfd5);
    }
    // Flags an ALU op left pending (computed lazily from its result) must not
    // survive: xor zeroes (ZF, PF set lazily), sub -1 gives SF and CF. The
    // jump ends the block, so the ALU op's flags are live and left lazy.
    for (int i = 0; i < 64; i++) {
        uint64_t src = rnd() | 1, dst, fout;
        __asm__ volatile("xor %%eax, %%eax\n jmp 1f\n 1: popcnt %2, %0\n pushfq\n pop %1"
                         : "=r"(dst), "=r"(fout) : "r"(src) : "rax", "cc");
        check("after xor: ZF, PF clear", src, fout & 0x44, 0);
        __asm__ volatile("mov $1, %%eax\n sub $2, %%eax\n jmp 1f\n 1: popcnt %2, %0\n pushfq\n pop %1"
                         : "=r"(dst), "=r"(fout) : "r"(src) : "rax", "cc");
        check("after sub: SF, CF, AF clear", src, fout & 0x91, 0);
        __asm__ volatile("mov $1, %%eax\n sub $2, %%eax\n jmp 1f\n 1: popcnt %2, %0\n pushfq\n pop %1"
                         : "=r"(dst), "=r"(fout) : "r"(0ul) : "rax", "cc");
        check("after sub, zero source: ZF only", 0, fout & ARITH, 0x40);
    }
    // a source crossing into an unmapped page
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    munmap(edge, 4096);
    for (int sz = 0; sz < 3; sz++) {
        f_addr = 0;
        uint64_t dst = 0x1234;
        if (!sigsetjmp(jb, 1)) {
            if (sz == 0)
                __asm__ volatile(".globl popcnt_fault_insn\npopcnt_fault_insn: popcntq (%%rbx), %%rdx"
                                 : "+d"(dst) : "b"(edge - 4) : "cc", "memory");
            else if (sz == 1)
                __asm__ volatile("popcntl (%%rbx), %%edx" : "+d"(dst) : "b"(edge - 2) : "cc", "memory");
            else
                __asm__ volatile("popcntw (%%rbx), %%dx" : "+d"(dst) : "b"(edge - 1) : "cc", "memory");
        }
        check("fault: SIGSEGV in the unmapped page", sz, f_addr >= (uintptr_t) edge && f_addr < (uintptr_t) edge + 8, 1);
        check("fault: destination unchanged", sz, f_rdx, 0x1234);
        if (sz == 0)
            check("fault: rip is the instruction", sz, f_pc, (uintptr_t) popcnt_fault_insn);
    }
    printf("amd64_popcnt: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
