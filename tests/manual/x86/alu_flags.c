// Arithmetic flags of the i386/amd64 ALU ops against a reference model
// written with plain integer arithmetic (it never reads a flag, so it does
// not share the mechanism under test). Checks OF SF ZF AF PF CF from PUSHF
// after add/sub/adc/sbb/and/or/xor/cmp/test/inc/dec at 8, 16 and 32 bits,
// register-immediate, register-register and memory forms, the fused
// cmp/test+jcc path (the branch taken, and the flags a later reader sees),
// and AF consumed by DAA straight after a logic op.
//
// Written for the lazy-flags deposit in jit/gadgets-aarch64 (setf_ops,
// setf_all_res, setf_logic_res): the logic family now keeps AF in the lazy
// form (op1 = res, op2 = 0) instead of clearing it in EFLAGS.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CF 0x001
#define PF 0x004
#define AF 0x010
#define ZF 0x040
#define SF 0x080
#define OF 0x800
#define MASK (CF | PF | AF | ZF | SF | OF)

static int failures, checks;

static unsigned parity(uint32_t v) {
    v &= 0xff;
    unsigned n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return (n & 1) ? 0 : PF;
}

enum op { ADD, SUB, ADC, SBB, AND, OR, XOR, CMP, TEST, INC, DEC };
static const char *names[] = {"add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test", "inc", "dec"};

// Reference: flags after `op a, b` (a = destination) at `bits`, carry-in cin.
// For inc/dec, b is ignored and CF is carried through from cin.
static unsigned model(enum op op, uint32_t a, uint32_t b, unsigned cin, int bits, uint32_t *res_out) {
    uint64_t m = bits == 32 ? 0xffffffffull : (1ull << bits) - 1;
    uint64_t sign = 1ull << (bits - 1);
    uint64_t x = a & m, y = b & m, r = 0;
    unsigned f = 0;
    int logic = 0;
    switch (op) {
    case ADD: r = x + y; break;
    case ADC: r = x + y + cin; break;
    case SUB: case CMP: r = x - y; break;
    case SBB: r = x - y - cin; break;
    case AND: case TEST: r = x & y; logic = 1; break;
    case OR: r = x | y; logic = 1; break;
    case XOR: r = x ^ y; logic = 1; break;
    case INC: y = 1; r = x + 1; break;
    case DEC: y = 1; r = x - 1; break;
    }
    uint64_t rm = r & m;
    if (rm == 0) f |= ZF;
    if (rm & sign) f |= SF;
    f |= parity((uint32_t) rm);
    if (!logic) {
        if (((x ^ y ^ rm) >> 4) & 1) f |= AF;
        int add = op == ADD || op == ADC || op == INC;
        if (op == INC || op == DEC) {
            if (cin) f |= CF;
        } else if (add) {
            if (r > m) f |= CF;
        } else {
            uint64_t sub = y + ((op == SBB) ? cin : 0);
            if (sub > x) f |= CF;
        }
        uint64_t sx = x & sign, sy = y & sign, sr = rm & sign;
        if (add) { if (sx == sy && sr != sx) f |= OF; }
        else { if (sx != sy && sr != sx) f |= OF; }
    }
    *res_out = (uint32_t) rm;
    return f;
}

static void check(const char *form, enum op op, int bits, uint32_t a, uint32_t b, unsigned cin,
        uint32_t got_res, unsigned long got_flags) {
    uint32_t want_res;
    unsigned want = model(op, a, b, cin, bits, &want_res);
    checks++;
    int writes = op != CMP && op != TEST;
    uint32_t m = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    if ((got_flags & MASK) != want || (writes && (got_res & m) != want_res)) {
        if (failures++ < 20)
            printf("FAIL %s %s%d a=%#x b=%#x cin=%u: flags %#x want %#x res %#x want %#x\n",
                    form, names[op], bits, a, b, cin, (unsigned) (got_flags & MASK), want,
                    got_res & m, want_res);
    }
}

// Each body starts with CF set to cin via stc/clc, runs the op, then pushf.
#define PRE "bt $0, %[cin]\n"
#define RUN_RI(insn, bits, reg) do { \
    uint32_t r = a; unsigned long fl; \
    __asm__ volatile(PRE insn " %[imm], %" reg "[r]\n pushf\n pop %[fl]\n" \
        : [r] "+q" (r), [fl] "=r" (fl) : [imm] "i" (IMM), [cin] "r" (cin) : "cc"); \
    check("ri", OPC, bits, a, IMM, cin, r, fl); } while (0)

static uint32_t vals[] = {0, 1, 2, 0xf, 0x10, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
    0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678, 0x87654321, 0x0f0f0f0f, 0xdeadbeef};
#define NV (sizeof(vals) / sizeof(vals[0]))

// reg,reg and mem forms for one op at one size, via a macro per (insn, suffix).
#define RR_MEM(OPE, INSN, SFX, BITS, R1, R2) \
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) for (unsigned cin = 0; cin < 2; cin++) { \
        uint32_t a = vals[i], b = vals[j], r = a; unsigned long fl; \
        __asm__ volatile(PRE INSN SFX " %" R2 "[b], %" R1 "[r]\n pushf\n pop %[fl]\n" \
            : [r] "+q" (r), [fl] "=r" (fl) : [b] "q" (b), [cin] "r" (cin) : "cc"); \
        check("rr", OPE, BITS, a, b, cin, r, fl); \
        uint32_t mem = a; \
        __asm__ volatile(PRE INSN SFX " %" R2 "[b], %[m]\n pushf\n pop %[fl]\n" \
            : [m] "+m" (mem), [fl] "=r" (fl) : [b] "q" (b), [cin] "r" (cin) : "cc"); \
        check("mr", OPE, BITS, a, b, cin, mem, fl); \
        r = a; \
        __asm__ volatile(PRE INSN SFX " %[m], %" R1 "[r]\n pushf\n pop %[fl]\n" \
            : [r] "+q" (r), [fl] "=r" (fl) : [m] "m" (b), [cin] "r" (cin) : "cc"); \
        check("rm", OPE, BITS, a, b, cin, r, fl); \
    }

// Locked, misaligned memory destination: the i386 JIT runs these through
// math.S's x86_lock_rmw and takes the flags from the old value, which must
// match what the aligned gadgets deposit. Offsets 1 (inside a 16-byte block) and 15 (straddling two).
static unsigned char lockbuf[64] __attribute__((aligned(64)));
#define LOCK_MIS(OPE, INSN, SFX, BITS, R2) \
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) for (unsigned cin = 0; cin < 2; cin++) \
    for (int off = 1; off < 16; off += 14) { \
        uint32_t a = vals[i], b = vals[j], got = 0; unsigned long fl; \
        memcpy(lockbuf + off, &a, BITS / 8); \
        __asm__ volatile(PRE "lock " INSN SFX " %" R2 "[b], (%[p])\n pushf\n pop %[fl]\n" \
            : [fl] "=r" (fl) : [b] "q" (b), [p] "r" (lockbuf + off), [cin] "r" (cin) : "cc", "memory"); \
        memcpy(&got, lockbuf + off, BITS / 8); \
        check(off == 1 ? "lock1" : "lock15", OPE, BITS, a, b, cin, got, fl); \
    }
#define LOCK_ALL(OPE, INSN) LOCK_MIS(OPE, INSN, "l", 32, "k") LOCK_MIS(OPE, INSN, "w", 16, "w")

#define ALL_SIZES(OPE, INSN) \
    RR_MEM(OPE, INSN, "l", 32, "k", "k") \
    RR_MEM(OPE, INSN, "w", 16, "w", "w") \
    RR_MEM(OPE, INSN, "b", 8, "b", "b")

#define UNARY(OPE, INSN, SFX, BITS, R1) \
    for (unsigned i = 0; i < NV; i++) for (unsigned cin = 0; cin < 2; cin++) { \
        uint32_t a = vals[i], r = a; unsigned long fl; \
        __asm__ volatile(PRE INSN SFX " %" R1 "[r]\n pushf\n pop %[fl]\n" \
            : [r] "+q" (r), [fl] "=r" (fl) : [cin] "r" (cin) : "cc"); \
        check("r", OPE, BITS, a, 0, cin, r, fl); \
    }


// x86 condition from a flags word, for the jcc checks.
static int cond_true(const char *cc, unsigned f) {
    int c = !!(f & CF), z = !!(f & ZF), sgn = !!(f & SF), o = !!(f & OF), p = !!(f & PF);
    int neg = cc[0] == 'n';
    const char *k = neg ? cc + 1 : cc;
    int v;
    if (!__builtin_strcmp(k, "o")) v = o;
    else if (!__builtin_strcmp(k, "b")) v = c;
    else if (!__builtin_strcmp(k, "e")) v = z;
    else if (!__builtin_strcmp(k, "be")) v = c || z;
    else if (!__builtin_strcmp(k, "s")) v = sgn;
    else if (!__builtin_strcmp(k, "p")) v = p;
    else if (!__builtin_strcmp(k, "l")) v = sgn != o;
    else v = z || (sgn != o); // le
    return neg ? !v : v;
}

// Byte cmp/test + jcc (the fused_cmp8/fused_test8 gadgets): every condition,
// register, immediate and memory sources; the branch and the flags after it.
#define JCC8(CC) do { \
    unsigned long fl; unsigned t; uint8_t mb = b; \
    __asm__ volatile("mov $1, %[t]\n cmpb %b[b], %b[a]\n j" CC " 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n" \
        : [t] "=&r" (t), [fl] "=r" (fl) : [a] "q" (a), [b] "q" (b) : "cc"); \
    jcc_check("cmpb r " CC, CMP, a, b, t, fl, CC); \
    __asm__ volatile("mov $1, %[t]\n cmpb %[m], %b[a]\n j" CC " 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n" \
        : [t] "=&r" (t), [fl] "=r" (fl) : [a] "q" (a), [m] "m" (mb) : "cc"); \
    jcc_check("cmpb m " CC, CMP, a, b, t, fl, CC); \
    __asm__ volatile("mov $1, %[t]\n cmpb $0x81, %b[a]\n j" CC " 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n" \
        : [t] "=&r" (t), [fl] "=r" (fl) : [a] "q" (a) : "cc"); \
    jcc_check("cmpb i " CC, CMP, a, 0x81, t, fl, CC); \
    __asm__ volatile("mov $1, %[t]\n testb %b[b], %b[a]\n j" CC " 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n" \
        : [t] "=&r" (t), [fl] "=r" (fl) : [a] "q" (a), [b] "q" (b) : "cc"); \
    jcc_check("testb r " CC, TEST, a, b, t, fl, CC); \
    __asm__ volatile("mov $1, %[t]\n testb %[m], %b[a]\n j" CC " 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n" \
        : [t] "=&r" (t), [fl] "=r" (fl) : [a] "q" (a), [m] "m" (mb) : "cc"); \
    jcc_check("testb m " CC, TEST, a, b, t, fl, CC); \
} while (0)

static void jcc_check(const char *what, enum op op, uint32_t a, uint32_t b, unsigned taken,
        unsigned long fl, const char *cc) {
    uint32_t res;
    unsigned want = model(op, a, b, 0, 8, &res);
    checks++;
    if ((unsigned) (fl & MASK) != want || taken != (unsigned) cond_true(cc, want)) {
        if (failures++ < 20)
            printf("FAIL %s a=%#x b=%#x: taken %u flags %#x want %#x\n", what, a & 0xff, b & 0xff,
                    taken, (unsigned) (fl & MASK), want);
    }
}

int main(void) {
    ALL_SIZES(ADD, "add")
    ALL_SIZES(SUB, "sub")
    ALL_SIZES(ADC, "adc")
    ALL_SIZES(SBB, "sbb")
    ALL_SIZES(AND, "and")
    ALL_SIZES(OR, "or")
    ALL_SIZES(XOR, "xor")
    ALL_SIZES(CMP, "cmp")
    ALL_SIZES(TEST, "test")
    LOCK_ALL(ADD, "add") LOCK_ALL(SUB, "sub") LOCK_ALL(ADC, "adc") LOCK_ALL(SBB, "sbb")
    LOCK_ALL(AND, "and") LOCK_ALL(OR, "or") LOCK_ALL(XOR, "xor")
    UNARY(INC, "inc", "l", 32, "k") UNARY(INC, "inc", "w", 16, "w") UNARY(INC, "inc", "b", 8, "b")
    UNARY(DEC, "dec", "l", 32, "k") UNARY(DEC, "dec", "w", 16, "w") UNARY(DEC, "dec", "b", 8, "b")

    // reg,imm 32-bit (the fused reg,imm ALU gadgets).
    for (unsigned i = 0; i < NV; i++) for (unsigned cin = 0; cin < 2; cin++) {
        uint32_t a = vals[i];
#define IMM 0x11
#define OPC ADD
        RUN_RI("addl", 32, "k");
#undef OPC
#define OPC SUB
        RUN_RI("subl", 32, "k");
#undef OPC
#define OPC AND
        RUN_RI("andl", 32, "k");
#undef OPC
#define OPC XOR
        RUN_RI("xorl", 32, "k");
#undef OPC
#define OPC OR
        RUN_RI("orl", 32, "k");
#undef OPC
#define OPC CMP
        RUN_RI("cmpl", 32, "k");
#undef OPC
#define OPC TEST
        RUN_RI("testl", 32, "k");
#undef OPC
#undef IMM
    }

    // Fused cmp/test + jcc: the branch, and the flags the next reader sees.
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) {
        uint32_t a = vals[i], b = vals[j];
        unsigned long fl; unsigned taken;
        __asm__ volatile("mov $1, %[t]\n cmpl %[b], %[a]\n jl 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n"
            : [t] "=&r" (taken), [fl] "=r" (fl) : [a] "r" (a), [b] "r" (b) : "cc");
        check("jcc", CMP, 32, a, b, 0, 0, fl);
        checks++;
        if (taken != ((int32_t) a < (int32_t) b) && failures++ < 20)
            printf("FAIL cmp+jl a=%#x b=%#x taken=%u\n", a, b, taken);
        __asm__ volatile("mov $1, %[t]\n testl %[b], %[a]\n je 1f\n mov $0, %[t]\n1: pushf\n pop %[fl]\n"
            : [t] "=&r" (taken), [fl] "=r" (fl) : [a] "r" (a), [b] "r" (b) : "cc");
        check("jcc", TEST, 32, a, b, 0, 0, fl);
        checks++;
        if (taken != ((a & b) == 0) && failures++ < 20)
            printf("FAIL test+je a=%#x b=%#x taken=%u\n", a, b, taken);
    }


    {
        static const uint8_t bv[] = {0, 1, 0x0f, 0x10, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x55, 0xaa};
        for (unsigned i = 0; i < sizeof(bv); i++) for (unsigned j = 0; j < sizeof(bv); j++) {
            uint32_t a = bv[i], b = bv[j];
            JCC8("o"); JCC8("no"); JCC8("b"); JCC8("nb"); JCC8("e"); JCC8("ne");
            JCC8("be"); JCC8("nbe"); JCC8("s"); JCC8("ns"); JCC8("p"); JCC8("np");
            JCC8("l"); JCC8("nl"); JCC8("le"); JCC8("nle");
        }
    }
#ifdef __i386__
    // AF consumed by DAA right after a logic op (AF must read 0): with
    // AL = 0x0f, DAA adds 6 only if AF or the low nibble > 9 -- the nibble
    // decides here, so use AL = 0x09 where only AF could trigger the +6.
    // Positive control: after `add` with a nibble carry (0x09 + 0x09 = 0x12,
    // AF=1) DAA must give 0x18.
    {
        unsigned al;
        __asm__ volatile("mov $0x08, %%eax\n add $0x08, %%al\n and $0xff, %%al\n daa\n movzbl %%al, %[r]\n"
            : [r] "=r" (al) : : "eax", "cc");
        checks++;
        if (al != 0x10 && failures++ < 20)
            printf("FAIL daa after and: al=%#x want 0x10\n", al);
        __asm__ volatile("mov $0x09, %%eax\n add $0x09, %%al\n daa\n movzbl %%al, %[r]\n"
            : [r] "=r" (al) : : "eax", "cc");
        checks++;
        if (al != 0x18 && failures++ < 20)
            printf("FAIL daa after add (positive control): al=%#x want 0x18\n", al);
    }
#endif

    printf("%d checks\n", checks);
    printf("alu_flags: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
