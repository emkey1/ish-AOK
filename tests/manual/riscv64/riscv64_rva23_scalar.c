// riscv64_rva23_scalar.c -- the RVA23 scalar extensions AOK's riscv64 JIT
// implements: every Zba/Zbb/Zbs/Zicond instruction (register forms, and the
// immediate forms at a spread of immediates) and every Zcb compressed
// instruction, against plain-C references built for rv64gc (so the compiler
// cannot lower them to the instructions under test); the MOPs, hints and
// cache-block ops (Zimop/Zcmop, Zawrs, Zihintpause/ntl, Zicbo*); then
// riscv_hwprobe and the cpuinfo isa line, which must advertise them. One line per mismatch,
// PASS/FAIL at the end, exit 1 on failure.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { SH1ADD, SH2ADD, SH3ADD, ADD_UW, SH1ADD_UW, SH2ADD_UW, SH3ADD_UW, SLLI_UW,
       ANDN, ORN, XNOR, CLZ, CLZW, CTZ, CTZW, CPOP, CPOPW, SEXT_B, SEXT_H, ZEXT_H,
       REV8, ORC_B, MAX, MAXU, MIN, MINU, ROL, ROR, ROLW, RORW,
       BCLR, BSET, BINV, BEXT, CZERO_EQZ, CZERO_NEZ, NOPS };

__attribute__((target("arch=rv64gc"))) static uint64_t zw(uint64_t x) { return x & 0xffffffffu; }
__attribute__((target("arch=rv64gc"))) static uint64_t sw(uint64_t x) { return (uint64_t) (int64_t) (int32_t) (uint32_t) x; }
__attribute__((target("arch=rv64gc"))) static uint64_t rotr64(uint64_t x, unsigned n) { n &= 63; return n ? (x >> n) | (x << (64 - n)) : x; }
__attribute__((target("arch=rv64gc"))) static uint32_t rotr32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

__attribute__((target("arch=rv64gc"), noinline))
static uint64_t ref_op(int op, uint64_t a, uint64_t b) {
    unsigned i;
    uint64_t r;
    switch (op) {
    case SH1ADD: return b + (a << 1);
    case SH2ADD: return b + (a << 2);
    case SH3ADD: return b + (a << 3);
    case ADD_UW: return b + zw(a);
    case SH1ADD_UW: return b + (zw(a) << 1);
    case SH2ADD_UW: return b + (zw(a) << 2);
    case SH3ADD_UW: return b + (zw(a) << 3);
    case SLLI_UW: return zw(a) << (b & 63);
    case ANDN: return a & ~b;
    case ORN: return a | ~b;
    case XNOR: return ~(a ^ b);
    case CLZ: for (i = 0; i < 64 && !((a >> (63 - i)) & 1); i++) {} return i;
    case CLZW: for (i = 0; i < 32 && !((a >> (31 - i)) & 1); i++) {} return i;
    case CTZ: for (i = 0; i < 64 && !((a >> i) & 1); i++) {} return i;
    case CTZW: for (i = 0; i < 32 && !((a >> i) & 1); i++) {} return i;
    case CPOP: for (i = 0, r = 0; i < 64; i++) r += (a >> i) & 1; return r;
    case CPOPW: for (i = 0, r = 0; i < 32; i++) r += (a >> i) & 1; return r;
    case SEXT_B: return (uint64_t) (int64_t) (int8_t) (uint8_t) a;
    case SEXT_H: return (uint64_t) (int64_t) (int16_t) (uint16_t) a;
    case ZEXT_H: return a & 0xffff;
    case REV8: for (i = 0, r = 0; i < 8; i++) r |= ((a >> (8 * i)) & 0xff) << (8 * (7 - i)); return r;
    case ORC_B: for (i = 0, r = 0; i < 8; i++) if ((a >> (8 * i)) & 0xff) r |= 0xffull << (8 * i); return r;
    case MAX: return (int64_t) a > (int64_t) b ? a : b;
    case MAXU: return a > b ? a : b;
    case MIN: return (int64_t) a < (int64_t) b ? a : b;
    case MINU: return a < b ? a : b;
    case ROL: return rotr64(a, 64 - (b & 63));
    case ROR: return rotr64(a, b & 63);
    case ROLW: return sw(rotr32((uint32_t) a, 32 - (b & 31)));
    case RORW: return sw(rotr32((uint32_t) a, b & 31));
    case BCLR: return a & ~(1ull << (b & 63));
    case BSET: return a | (1ull << (b & 63));
    case BINV: return a ^ (1ull << (b & 63));
    case BEXT: return (a >> (b & 63)) & 1;
    case CZERO_EQZ: return b == 0 ? 0 : a;
    case CZERO_NEZ: return b != 0 ? 0 : a;
    }
    return 0xdeadbeef;
}

static unsigned long checks, bad;
static void check(const char *what, uint64_t in, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("%s(%#llx) = %#llx, want %#llx\n", what, (unsigned long long) in,
               (unsigned long long) got, (unsigned long long) want);
}
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// ---- Zba, Zbb, Zbs, Zicond ----
#define RR(name, insn) static uint64_t name(uint64_t a, uint64_t b) { uint64_t r; \
    __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " %0, %1, %2\n.option pop" : "=r"(r) : "r"(a), "r"(b)); return r; }
#define UN(name, insn) static uint64_t name(uint64_t a, uint64_t b) { uint64_t r; (void) b; \
    __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " %0, %1\n.option pop" : "=r"(r) : "r"(a)); return r; }

RR(t_sh1add, "sh1add") RR(t_sh2add, "sh2add") RR(t_sh3add, "sh3add")
RR(t_add_uw, "add.uw") RR(t_sh1add_uw, "sh1add.uw") RR(t_sh2add_uw, "sh2add.uw")
RR(t_sh3add_uw, "sh3add.uw")
RR(t_andn, "andn") RR(t_orn, "orn") RR(t_xnor, "xnor")
UN(t_clz, "clz") UN(t_clzw, "clzw") UN(t_ctz, "ctz") UN(t_ctzw, "ctzw")
UN(t_cpop, "cpop") UN(t_cpopw, "cpopw") UN(t_sext_b, "sext.b") UN(t_sext_h, "sext.h")
UN(t_zext_h, "zext.h") UN(t_rev8, "rev8") UN(t_orc_b, "orc.b")
RR(t_max, "max") RR(t_maxu, "maxu") RR(t_min, "min") RR(t_minu, "minu")
RR(t_rol, "rol") RR(t_ror, "ror") RR(t_rolw, "rolw") RR(t_rorw, "rorw")
RR(t_bclr, "bclr") RR(t_bset, "bset") RR(t_binv, "binv") RR(t_bext, "bext")
RR(t_czero_eqz, "czero.eqz") RR(t_czero_nez, "czero.nez")

static uint64_t (*const regform[NOPS])(uint64_t, uint64_t) = {
    [SH1ADD] = t_sh1add, [SH2ADD] = t_sh2add, [SH3ADD] = t_sh3add, [ADD_UW] = t_add_uw,
    [SH1ADD_UW] = t_sh1add_uw, [SH2ADD_UW] = t_sh2add_uw, [SH3ADD_UW] = t_sh3add_uw,
    [ANDN] = t_andn, [ORN] = t_orn, [XNOR] = t_xnor, [CLZ] = t_clz, [CLZW] = t_clzw,
    [CTZ] = t_ctz, [CTZW] = t_ctzw, [CPOP] = t_cpop, [CPOPW] = t_cpopw,
    [SEXT_B] = t_sext_b, [SEXT_H] = t_sext_h, [ZEXT_H] = t_zext_h, [REV8] = t_rev8,
    [ORC_B] = t_orc_b, [MAX] = t_max, [MAXU] = t_maxu, [MIN] = t_min, [MINU] = t_minu,
    [ROL] = t_rol, [ROR] = t_ror, [ROLW] = t_rolw, [RORW] = t_rorw, [BCLR] = t_bclr,
    [BSET] = t_bset, [BINV] = t_binv, [BEXT] = t_bext, [CZERO_EQZ] = t_czero_eqz,
    [CZERO_NEZ] = t_czero_nez,
};
static const char *const names[NOPS] = {
    "sh1add", "sh2add", "sh3add", "add.uw", "sh1add.uw", "sh2add.uw", "sh3add.uw", "slli.uw",
    "andn", "orn", "xnor", "clz", "clzw", "ctz", "ctzw", "cpop", "cpopw", "sext.b", "sext.h",
    "zext.h", "rev8", "orc.b", "max", "maxu", "min", "minu", "rol", "ror", "rolw", "rorw",
    "bclr", "bset", "binv", "bext", "czero.eqz", "czero.nez",
};

// Immediate forms: one function per (instruction, immediate).
#define IMM(insn, k) { uint64_t r; __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " %0, %1, " #k "\n.option pop" : "=r"(r) : "r"(a)); return r; }
#define IMM6(name, insn) static uint64_t name(uint64_t a, unsigned k) { switch (k) { \
    case 0: IMM(insn, 0) case 1: IMM(insn, 1) case 7: IMM(insn, 7) case 31: IMM(insn, 31) \
    case 32: IMM(insn, 32) case 33: IMM(insn, 33) default: IMM(insn, 63) } }
#define IMM5(name, insn) static uint64_t name(uint64_t a, unsigned k) { switch (k) { \
    case 0: IMM(insn, 0) case 1: IMM(insn, 1) case 7: IMM(insn, 7) case 16: IMM(insn, 16) \
    default: IMM(insn, 31) } }
IMM6(i_slli_uw, "slli.uw") IMM6(i_rori, "rori") IMM5(i_roriw, "roriw")
IMM6(i_bclri, "bclri") IMM6(i_bseti, "bseti") IMM6(i_binvi, "binvi") IMM6(i_bexti, "bexti")
static const struct { const char *name; uint64_t (*fn)(uint64_t, unsigned); int op; int w; } immform[] = {
    {"slli.uw", i_slli_uw, SLLI_UW, 0}, {"rori", i_rori, ROR, 0}, {"roriw", i_roriw, RORW, 1},
    {"bclri", i_bclri, BCLR, 0}, {"bseti", i_bseti, BSET, 0}, {"binvi", i_binvi, BINV, 0},
    {"bexti", i_bexti, BEXT, 0},
};
static const unsigned k6[] = {0, 1, 7, 31, 32, 33, 63}, k5[] = {0, 1, 7, 16, 31};

static const uint64_t edge[] = {
    0, 1, 2, 63, 64, 0xff, 0x80, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
    0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, 0x0123456789abcdefull,
    0xff00ff0000ff00ffull,
};
#define NEDGE (sizeof(edge) / sizeof(edge[0]))


// ---- Zcb: operands pinned to x8-x15, the registers RVC's 3-bit fields reach ----
#define LOAD(insn, off, p) ({ register uint64_t r_ __asm__("a0"); register const void *b_ __asm__("a1") = (p); \
    __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " a0, " #off "(a1)\n.option pop" : "=r"(r_) : "r"(b_) : "memory"); r_; })
#define STORE(insn, off, p, v) do { register uint64_t v_ __asm__("a2") = (v); register void *b_ __asm__("a3") = (p); \
    __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " a2, " #off "(a3)\n.option pop" : : "r"(v_), "r"(b_) : "memory"); } while (0)
#define UNARY(insn, v) ({ register uint64_t r_ __asm__("s1") = (v); \
    __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" insn " s1\n.option pop" : "+r"(r_)); r_; })


// ---- Zimop/Zcmop, Zawrs, Zihintpause/Zihintntl, Zicbop/Zicbom/Zicboz ----
static void check_hints(void) {
    uint64_t r = 0x1234, r2 = 0x5678;
    __asm__ volatile(".option push\n.option arch, +zimop, +zcmop, +zawrs, +zicboz, +zicbom, +zicbop, +zihintntl, +zihintpause\n" "mop.r.5 %0, %1\n.option pop" : "+r"(r) : "r"(r2));
    check("mop.r.5 writes 0", 0x1234, r, 0);
    r = 0x1234;
    __asm__ volatile(".option push\n.option arch, +zimop, +zcmop, +zawrs, +zicboz, +zicbom, +zicbop, +zihintntl, +zihintpause\n" "mop.rr.3 %0, %1, %2\n.option pop" : "+r"(r) : "r"(r2), "r"(r2));
    check("mop.rr.3 writes 0", 0x1234, r, 0);
    register uint64_t t0 __asm__("t0") = 0xaaaa, t2 __asm__("t2") = 0xbbbb;
    __asm__ volatile(".option push\n.option arch, +zimop, +zcmop, +zawrs, +zicboz, +zicbom, +zicbop, +zihintntl, +zihintpause\n" "c.mop.5\nc.mop.7\n.option pop" : "+r"(t0), "+r"(t2));
    check("c.mop.5 leaves t0", 0xaaaa, t0, 0xaaaa);
    check("c.mop.7 leaves t2", 0xbbbb, t2, 0xbbbb);
    static uint8_t blk[256] __attribute__((aligned(64)));
    memset(blk, 0xaa, sizeof(blk));
    void *p = blk + 64 + 13;
    __asm__ volatile(".option push\n.option arch, +zimop, +zcmop, +zawrs, +zicboz, +zicbom, +zicbop, +zihintntl, +zihintpause\n" "wrs.nto\nwrs.sto\npause\nntl.p1\nntl.all\nc.ntl.pall\n"
                     "prefetch.r 0(%0)\nprefetch.w 64(%0)\nprefetch.i 0(%0)\n"
                     "cbo.clean (%0)\ncbo.flush (%0)\ncbo.inval (%0)\ncbo.zero (%0)\n.option pop"
                     : : "r"(p) : "memory");
    unsigned zeroed = 0, kept = 0;
    for (unsigned i = 0; i < sizeof(blk); i++) {
        if (i >= 64 && i < 128)
            zeroed += blk[i] == 0;
        else
            kept += blk[i] == 0xaa;
    }
    check("cbo.zero zeroes its 64-byte block", 0, zeroed, 64);
    check("cbo.zero leaves the rest", 0, kept, sizeof(blk) - 64);
}

// ---- riscv_hwprobe (syscall 258) and /proc/cpuinfo ----
static void check_advertised(void) {
    struct { int64_t key; uint64_t value; } p[] = {{3, 0}, {4, 0}, {12345, 0}, {6, 0}, {12, 0}};
    long r = syscall(258, p, 5, 0, NULL, 0);
    check("hwprobe", 0, (uint64_t) r, 0);
    check("hwprobe base_behavior", 0, p[0].value, 1);          // IMA
    uint64_t want = 1 | 2 | 1 << 3 | 1 << 4 | 1 << 5 | 1ull << 35 | 1ull << 43 | 1ull << 44 | 1ull << 45;
    check("hwprobe ima_ext_0", 0, p[1].value & want, want);  // FD C Zba Zbb Zbs Zicond Zca Zcb Zcd
    check("hwprobe unknown key", 0, (uint64_t) p[2].key, (uint64_t) -1);
    // Zicboz Zihintntl Zihintpause Zimop Zcmop Zawrs Zicntr Zicbom Zicbop Zfa Zfhmin
    uint64_t hints = 1 << 6 | 1 << 29 | 1ull << 36 | 1ull << 42 | 1ull << 47 | 1ull << 48 |
            1ull << 50 | 1ull << 55 | 1ull << 60 | 1ull << 32 | 1 << 28; // ... Zfa Zfhmin
    check("hwprobe ima_ext_0 hints", 0, p[1].value & hints, hints);
    check("hwprobe zicboz block", 0, p[3].value, 64);
    check("hwprobe zicbom block", 0, p[4].value, 64);
    char line[256] = "";
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) { // e.g. a chroot without /proc
        printf("/proc/cpuinfo unreadable: the isa line is not checked\n");
        return;
    }
    while (fgets(line, sizeof(line), f) && strncmp(line, "isa", 3) != 0) {}
    fclose(f);
    const char *exts[] = {"rv64imafdc", "_zicbom", "_zicbop", "_zicboz", "_zicntr", "_zicond",
                          "_zihintntl", "_zihintpause", "_zimop", "_zawrs", "_zfa", "_zfhmin",
                          "_zca", "_zcb", "_zcd",
                          "_zcmop", "_zba", "_zbb", "_zbs", "rv64imafdcv", "_zve64d", "_zvbb",
                          "_zvfhmin", "_zvkb", "_zvkt"};
    for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        check(exts[i], 0, strstr(line, exts[i]) != NULL, 1);
}

int main(void) {
        for (int op = 0; op < NOPS; op++) {
        if (regform[op] == NULL)
            continue;
        for (unsigned i = 0; i < NEDGE + 300; i++) {
            for (unsigned j = 0; j < NEDGE + 20; j++) {
                uint64_t a = i < NEDGE ? edge[i] : rnd(), b = j < NEDGE ? edge[j] : rnd();
                if (j >= NEDGE && (rnd() & 1))
                    b &= 127;
                uint64_t got = regform[op](a, b), want = ref_op(op, a, b);
                checks++;
                if (got != want && bad++ < 30)
                    printf("%s(%#llx, %#llx) = %#llx, want %#llx\n", names[op],
                           (unsigned long long) a, (unsigned long long) b,
                           (unsigned long long) got, (unsigned long long) want);
            }
        }
    }
    for (unsigned f = 0; f < sizeof(immform) / sizeof(immform[0]); f++) {
        const unsigned *ks = immform[f].w ? k5 : k6;
        unsigned nk = immform[f].w ? 5 : 7;
        for (unsigned ki = 0; ki < nk; ki++) {
            for (unsigned i = 0; i < NEDGE + 300; i++) {
                uint64_t a = i < NEDGE ? edge[i] : rnd();
                uint64_t got = immform[f].fn(a, ks[ki]), want = ref_op(immform[f].op, a, ks[ki]);
                checks++;
                if (got != want && bad++ < 30)
                    printf("%s(%#llx, %u) = %#llx, want %#llx\n", immform[f].name,
                           (unsigned long long) a, ks[ki],
                           (unsigned long long) got, (unsigned long long) want);
            }
        }
    }
    uint8_t buf[16];
    for (int round = 0; round < 2000; round++) {
        for (int i = 0; i < 16; i++)
            buf[i] = (uint8_t) rnd();
        uint16_t h0, h2;
        memcpy(&h0, buf, 2);
        memcpy(&h2, buf + 2, 2);
        check("c.lbu 0", 0, LOAD("c.lbu", 0, buf), buf[0]);
        check("c.lbu 1", 1, LOAD("c.lbu", 1, buf), buf[1]);
        check("c.lbu 2", 2, LOAD("c.lbu", 2, buf), buf[2]);
        check("c.lbu 3", 3, LOAD("c.lbu", 3, buf), buf[3]);
        check("c.lhu 0", 0, LOAD("c.lhu", 0, buf), h0);
        check("c.lhu 2", 2, LOAD("c.lhu", 2, buf), h2);
        check("c.lh 0", 0, LOAD("c.lh", 0, buf), (uint64_t) (int64_t) (int16_t) h0);
        check("c.lh 2", 2, LOAD("c.lh", 2, buf), (uint64_t) (int64_t) (int16_t) h2);

        uint8_t want[16];
        uint64_t v = rnd();
        memcpy(want, buf, 16);
        want[0] = (uint8_t) v;
        STORE("c.sb", 0, buf, v);
        want[3] = (uint8_t) (v >> 8);
        STORE("c.sb", 3, buf, v >> 8);
        uint16_t hv = (uint16_t) (v >> 16), hw = (uint16_t) (v >> 32);
        memcpy(want + 4, &hv, 2);
        STORE("c.sh", 0, buf + 4, hv);
        memcpy(want + 6, &hw, 2);
        STORE("c.sh", 2, buf + 4, hw);
        check("c.sb/c.sh", v, (uint64_t) memcmp(buf, want, 16), 0);

        uint64_t x = rnd();
        if (round < 8)
            x = (uint64_t[]){0, ~0ull, 0x80, 0x8000, 0x80000000, 0x7f, 0x7fff, 0xffffffff}[round];
        check("c.zext.b", x, UNARY("c.zext.b", x), x & 0xff);
        check("c.sext.b", x, UNARY("c.sext.b", x), (uint64_t) (int64_t) (int8_t) x);
        check("c.zext.h", x, UNARY("c.zext.h", x), x & 0xffff);
        check("c.sext.h", x, UNARY("c.sext.h", x), (uint64_t) (int64_t) (int16_t) x);
        check("c.zext.w", x, UNARY("c.zext.w", x), x & 0xffffffff);
        check("c.not", x, UNARY("c.not", x), ~x);
        uint64_t y = rnd();
        register uint64_t a __asm__("s0") = x;
        register uint64_t b __asm__("a5") = y;
        __asm__ volatile(".option push\n.option arch, +zba, +zbb, +zbs, +zicond, +zcb\n" "c.mul s0, a5\n.option pop" : "+r"(a) : "r"(b));
        check("c.mul", x, a, x * y);
    }
    check_hints();
    check_advertised();
    printf("riscv64_rva23_scalar: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
