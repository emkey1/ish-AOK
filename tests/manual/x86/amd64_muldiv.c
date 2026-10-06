// amd64_muldiv.c -- one-operand MUL, IMUL, DIV and IDIV (F6/F7 /4-/7), 8, 16,
// 32 and 64 bits, register (AH..BH too) and memory operands, against a model
// that multiplies and divides bit by bit (__int128's arithmetic is MUL and
// libgcc's __udivti3, which runs DIV itself -- the instructions under test):
// RDX:RAX (AX for a byte), CF=OF for the multiplies, quotient and remainder
// for the divides -- with dividends past the operand width (RDX not 0, not
// the sign of RAX) -- and #DE for a zero divisor, a quotient that does not
// fit (DIV and IDIV, MIN / -1 among them): SIGFPE, FPE_INTDIV, at the
// instruction, RAX and RDX unchanged. The 8/16-bit forms leave the rest of
// RAX and RDX alone; a byte form leaves RDX alone. The gadgets are math.S's
// amd64_grp3_muldiv and amd64_muldiv_mem*; the slow cases used to be C.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t a, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%#llx): %#llx, want %#llx\n", what, (unsigned long long) a,
               (unsigned long long) got, (unsigned long long) want);
}
static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

// Unsigned 128 / 64 with the quotient's top half dropped, a bit at a time:
// no DIV anywhere (the compiler's division would use one).
static unsigned __int128 udiv_bits(unsigned __int128 n, uint64_t d, uint64_t *rem) {
    unsigned __int128 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (unsigned __int128) 1 << i; }
    }
    *rem = (uint64_t) r;
    return q;
}

// Unsigned 64 x 64 -> 128 by shift-and-add (no MUL).
static unsigned __int128 umul_bits(uint64_t a, uint64_t b) {
    unsigned __int128 p = 0;
    for (int i = 0; i < 64; i++)
        if ((b >> i) & 1) p += (unsigned __int128) a << i;
    return p;
}
static __int128 smul_bits(int64_t a, int64_t b) {
    int neg = (a < 0) != (b < 0);
    unsigned __int128 p = umul_bits(a < 0 ? -(uint64_t) a : (uint64_t) a, b < 0 ? -(uint64_t) b : (uint64_t) b);
    return neg ? -(__int128) p : (__int128) p;
}

static sigjmp_buf jb;
static volatile int f_sig, f_code;
static volatile uintptr_t f_pc;
static volatile uint64_t f_rax, f_rdx;
static void on_fpe(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    f_sig = sig; f_code = si->si_code;
    f_pc = uc->uc_mcontext.gregs[REG_RIP];
    f_rax = uc->uc_mcontext.gregs[REG_RAX];
    f_rdx = uc->uc_mcontext.gregs[REG_RDX];
    siglongjmp(jb, 1);
}

// op 0 mul 1 imul 2 div 3 idiv; mem: operand from memory
#define OP(insn, rax, rdx, src, mem, f) do { \
    if (mem) __asm__ volatile(insn " %4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "m"(src) : "cc"); \
    else     __asm__ volatile(insn " %4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "r"(src) : "cc"); \
    } while (0)
extern char div_fault_r[], div_fault_m[];

static void run64(int op, int mem, uint64_t rax0, uint64_t rdx0, uint64_t src) {
    uint64_t rax = rax0, rdx = rdx0, f = 0;
    unsigned __int128 n = ((unsigned __int128) rdx0 << 64) | rax0;
    int de = 0;
    uint64_t wq = 0, wr = 0;
    if (op == 2) {
        if (src == 0 || rdx0 >= src) de = 1;
        else wq = (uint64_t) udiv_bits(n, src, &wr);
    } else if (op == 3) {
        __int128 sn = (__int128) n;
        int64_t sd = (int64_t) src;
        if (sd == 0) de = 1;
        else {
            int nneg = sn < 0, dneg = sd < 0;
            unsigned __int128 un = nneg ? -(unsigned __int128) sn : (unsigned __int128) sn;
            uint64_t ud = dneg ? -(uint64_t) sd : (uint64_t) sd, ur;
            unsigned __int128 uq = udiv_bits(un, ud, &ur);
            // the quotient must fit int64 (MIN / -1 does not)
            if (nneg != dneg ? uq > ((unsigned __int128) 1 << 63) : uq >= ((unsigned __int128) 1 << 63)) de = 1;
            else {
                wq = (uint64_t) (nneg != dneg ? -(uint64_t) uq : (uint64_t) uq);
                wr = nneg ? -ur : ur;
            }
        }
    }
    f_sig = 0;
    if (!sigsetjmp(jb, 1)) {
        switch (op * 2 + mem) {
        case 0: OP("mulq", rax, rdx, src, 0, f); break;
        case 1: OP("mulq", rax, rdx, src, 1, f); break;
        case 2: OP("imulq", rax, rdx, src, 0, f); break;
        case 3: OP("imulq", rax, rdx, src, 1, f); break;
        case 4: __asm__ volatile(".globl div_fault_r\ndiv_fault_r: divq %2" : "+a"(rax), "+d"(rdx) : "r"(src) : "cc"); break;
        case 5: __asm__ volatile(".globl div_fault_m\ndiv_fault_m: divq %2" : "+a"(rax), "+d"(rdx) : "m"(src) : "cc"); break;
        case 6: __asm__ volatile("idivq %2" : "+a"(rax), "+d"(rdx) : "r"(src) : "cc"); break;
        default: __asm__ volatile("idivq %2" : "+a"(rax), "+d"(rdx) : "m"(src) : "cc"); break;
        }
    }
    static const char *names[] = {"mul64", "mul64 m", "imul64", "imul64 m", "div64", "div64 m", "idiv64", "idiv64 m"};
    const char *nm = names[op * 2 + mem];
    if (op == 0) {
        unsigned __int128 p = umul_bits(rax0, src);
        check(nm, src, rax, (uint64_t) p);
        check(nm, src, rdx, (uint64_t) (p >> 64));
        check(nm, src, (f >> 0) & 1, (p >> 64) != 0);
        check(nm, src, (f >> 11) & 1, (p >> 64) != 0);
    } else if (op == 1) {
        __int128 p = smul_bits((int64_t) rax0, (int64_t) src);
        check(nm, src, rax, (uint64_t) p);
        check(nm, src, rdx, (uint64_t) ((unsigned __int128) p >> 64));
        check(nm, src, f & 1, p != (int64_t) p);
    } else if (de) {
        check(nm, src, f_sig, SIGFPE);
        check(nm, src, f_code, FPE_INTDIV);
        check(nm, src, f_rax, rax0);
        check(nm, src, f_rdx, rdx0);
        if (op == 2) check(nm, src, f_pc, (uintptr_t) (mem ? div_fault_m : div_fault_r));
    } else {
        check(nm, src, f_sig, 0);
        check(nm, src, rax, wq);
        check(nm, src, rdx, wr);
    }
}

static void run32(int op, int mem, uint32_t eax0, uint32_t edx0, uint32_t src) {
    uint64_t rax = 0xdead000000000000ull | eax0, rdx = 0xbeef000000000000ull | edx0, f = 0;
    uint64_t n = ((uint64_t) edx0 << 32) | eax0;
    int de = 0; uint32_t wq = 0, wr = 0;
    if (op == 2) {
        uint64_t r;
        if (!src) de = 1;
        else { uint64_t q = (uint64_t) udiv_bits(n, src, &r); if (q > 0xffffffffu) de = 1; else { wq = (uint32_t) q; wr = (uint32_t) r; } }
    }
    if (op == 3) {
        int64_t sn = (int64_t) n, sd = (int32_t) src;
        if (!sd) de = 1;
        else {
            int nneg = sn < 0, dneg = sd < 0;
            uint64_t un = nneg ? -(uint64_t) sn : (uint64_t) sn, ud = dneg ? -(uint64_t) sd : (uint64_t) sd, ur;
            uint64_t uq = (uint64_t) udiv_bits(un, ud, &ur);
            if (nneg != dneg ? uq > 0x80000000u : uq >= 0x80000000u) de = 1;
            else { wq = (uint32_t) (nneg != dneg ? -uq : uq); wr = (uint32_t) (nneg ? -ur : ur); }
        }
    }
    f_sig = 0;
    if (!sigsetjmp(jb, 1)) {
        switch (op * 2 + mem) {
        case 0: __asm__ volatile("mull %k4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "r"(src) : "cc"); break;
        case 1: __asm__ volatile("mull %4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "m"(src) : "cc"); break;
        case 2: __asm__ volatile("imull %k4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "r"(src) : "cc"); break;
        case 3: __asm__ volatile("imull %4\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "0"(rax), "m"(src) : "cc"); break;
        case 4: __asm__ volatile("divl %k2" : "+a"(rax), "+d"(rdx) : "r"(src) : "cc"); break;
        case 5: __asm__ volatile("divl %2" : "+a"(rax), "+d"(rdx) : "m"(src) : "cc"); break;
        case 6: __asm__ volatile("idivl %k2" : "+a"(rax), "+d"(rdx) : "r"(src) : "cc"); break;
        default: __asm__ volatile("idivl %2" : "+a"(rax), "+d"(rdx) : "m"(src) : "cc"); break;
        }
    }
    if (op == 0) {
        uint64_t p = (uint64_t) umul_bits(eax0, src);
        check("mul32", src, rax, (uint32_t) p); check("mul32 edx", src, rdx, p >> 32);
        check("mul32 cf", src, f & 1, (p >> 32) != 0);
    } else if (op == 1) {
        int64_t p = (int64_t) smul_bits((int32_t) eax0, (int32_t) src);
        check("imul32", src, rax, (uint32_t) p); check("imul32 edx", src, rdx, (uint32_t) ((uint64_t) p >> 32));
        check("imul32 cf", src, f & 1, p != (int32_t) p);
    } else if (de) {
        check("div32 #DE", src, f_sig, SIGFPE);
    } else {
        check("div32 q", src, rax, wq); check("div32 r", src, rdx, wr);
    }
}

// 8 and 16 bits: DX:AX (AX for a byte) and the rest of RAX/RDX kept.
// form: 0 a register, 1 memory, 2 a high-byte register (CH; bytes only)
static void run_small(int bits, int op, int form, uint64_t rax0, uint64_t rdx0, uint64_t s) {
    uint64_t m = bits == 8 ? 0xff : 0xffff, rax = rax0, rdx = rdx0, f = 0;
    uint64_t src = s & m, rcx = form == 2 ? (s << 8) | 0x5a : s;
    uint64_t lo = rax0 & m, hi = bits == 8 ? (rax0 >> 8) & 0xff : rdx0 & 0xffff;
    uint64_t n = bits == 8 ? rax0 & 0xffff : (hi << 16) | lo;
    uint16_t m16 = (uint16_t) src; uint8_t m8 = (uint8_t) src;
    int de = 0;
    uint64_t wlo = 0, whi = 0, wcf = 0;
    if (op == 0) {
        uint64_t p = (uint64_t) umul_bits(lo, src);
        wlo = p & m; whi = (p >> bits) & m; wcf = whi != 0;
    } else if (op == 1) {
        int64_t a = bits == 8 ? (int8_t) lo : (int16_t) lo, b = bits == 8 ? (int8_t) src : (int16_t) src;
        int64_t p = (int64_t) smul_bits(a, b);
        wlo = (uint64_t) p & m; whi = ((uint64_t) p >> bits) & m;
        int64_t back = bits == 8 ? (int8_t) wlo : (int16_t) wlo;
        wcf = back != p;
    } else if (op == 2) {
        uint64_t r;
        if (!src) de = 1;
        else { uint64_t q = (uint64_t) udiv_bits(n, src, &r); if (q > m) de = 1; else { wlo = q; whi = r; } }
    } else {
        int64_t sn = bits == 8 ? (int16_t) n : (int32_t) n, sd = bits == 8 ? (int8_t) src : (int16_t) src;
        if (!sd) de = 1;
        else {
            int nneg = sn < 0, dneg = sd < 0;
            uint64_t un = nneg ? -(uint64_t) sn : (uint64_t) sn, ud = dneg ? -(uint64_t) sd : (uint64_t) sd, ur;
            uint64_t uq = (uint64_t) udiv_bits(un, ud, &ur);
            uint64_t lim = (m >> 1) + 1;
            if (nneg != dneg ? uq > lim : uq >= lim) de = 1;
            else { wlo = (nneg != dneg ? -uq : uq) & m; whi = (nneg ? -ur : ur) & m; }
        }
    }
    f_sig = 0;
    if (!sigsetjmp(jb, 1)) {
#define S8(i) do { \
        if (form == 0) __asm__ volatile(i " %%cl\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "c"(rcx) : "cc"); \
        else if (form == 2) __asm__ volatile(i " %%ch\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "c"(rcx) : "cc"); \
        else __asm__ volatile(i "b %3\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "m"(m8) : "cc"); } while (0)
#define S16(i) do { \
        if (form == 0) __asm__ volatile(i " %%cx\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "c"(rcx) : "cc"); \
        else __asm__ volatile(i "w %3\n pushfq\n pop %2" : "+a"(rax), "+d"(rdx), "=r"(f) : "m"(m16) : "cc"); } while (0)
        if (bits == 8) {
            switch (op) { case 0: S8("mul"); break; case 1: S8("imul"); break; case 2: S8("div"); break; default: S8("idiv"); }
        } else {
            switch (op) { case 0: S16("mul"); break; case 1: S16("imul"); break; case 2: S16("div"); break; default: S16("idiv"); }
        }
    }
    static const char *names[2][4][3] = {
        {{"mulb cl", "mulb m", "mulb ch"}, {"imulb cl", "imulb m", "imulb ch"}, {"divb cl", "divb m", "divb ch"}, {"idivb cl", "idivb m", "idivb ch"}},
        {{"mulw cx", "mulw m", ""}, {"imulw cx", "imulw m", ""}, {"divw cx", "divw m", ""}, {"idivw cx", "idivw m", ""}}};
    const char *nm = names[bits == 16][op][form];
    if (de) {
        check(nm, s, f_sig, SIGFPE);
        check(nm, s, f_code, FPE_INTDIV);
        check(nm, s, f_rax, rax0);
        check(nm, s, f_rdx, rdx0);
        return;
    }
    check(nm, s, f_sig, 0);
    uint64_t want_rax, want_rdx;
    if (bits == 8) {
        want_rax = (rax0 & ~0xffffull) | (whi << 8) | wlo;
        want_rdx = rdx0;
    } else {
        want_rax = (rax0 & ~0xffffull) | wlo;
        want_rdx = (rdx0 & ~0xffffull) | whi;
    }
    check(nm, s, rax, want_rax);
    check(nm, s, rdx, want_rdx);
    if (op < 2) {
        check(nm, s, f & 1, wcf);
        check(nm, s, (f >> 11) & 1, wcf);
    }
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fpe;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGFPE, &sa, NULL);
    for (int i = 0; i < 6000; i++) {
        uint64_t a = rnd(), d = rnd(), s = rnd();
        switch (i % 9) {
        case 0: d = 0; break;                        // a 64-bit dividend
        case 1: d = (int64_t) a < 0 ? ~0ull : 0; break;  // the cqo idiom
        case 2: d = rnd() % (s ? s : 1); break;      // a real 128-bit dividend that fits
        case 3: s >>= (rnd() % 64); break;           // small divisors
        case 4: s = 0; break;                        // #DE
        case 5: s = ~0ull; a = 1ull << 63; d = ~0ull; break;   // MIN / -1
        case 6: d = (int64_t) s < 0 ? -(int64_t) (rnd() % (-(int64_t) s ? -(int64_t) s : 1)) : rnd() % (s | 1); break;
        }
        for (int op = 0; op < 4; op++)
            for (int mem = 0; mem < 2; mem++) {
                run64(op, mem, a, d, s);
                run32(op, mem, (uint32_t) a, (uint32_t) d, (uint32_t) s);
            }
        for (int op = 0; op < 4; op++)
            for (int form = 0; form < 3; form++) {
                uint64_t ss = s, aa = a;
                if (i % 9 == 3) ss = (s >> (rnd() % 16)) | 1;
                if (i % 9 == 5) { ss = 0xff; aa = (a & ~0xffffull) | 0x8000; }   // -32768 / -1
                run_small(8, op, form, aa, d, ss);
                if (form < 2) {
                    if (i % 9 == 5) { ss = 0xffff; d = (d & ~0xffffull) | 0x8000; aa &= ~0xffffull; }
                    run_small(16, op, form, aa, d, ss);
                }
            }
    }
    printf("amd64_muldiv: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
