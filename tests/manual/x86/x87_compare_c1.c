// x87_compare_c1.c -- what each x87 compare does to C1, against real hardware
// (an AMD Ryzen, camd, -m32 and 64-bit). FCOMI, FCOMIP, FUCOMI and FUCOMIP keep
// it, unordered operands included, and clear it only on a stack fault, though
// the SDM says they clear it. FCOM, FUCOM, FTST, FICOM and their kin clear it.
//
// C0, C2 and C3 are checked with it: the FCOM family sets them (111 for
// unordered or a stack fault), and the FCOMI family leaves them, since it
// reports in ZF, PF and CF.
#include <stdint.h>
#include <stdio.h>

#define C0 0x0100
#define C1 0x0200
#define C2 0x0400
#define C3 0x4000
#define CC (C0 | C1 | C2 | C3)
#define SF 0x0040

static long double one = 1.0L, two = 2.0L, three = 3.0L, nanv;
static float f2 = 2.0f;
static double d2 = 2.0;
static short i16_2 = 2;
static int i32_2 = 2;

static uint16_t sw(void) {
    uint16_t v;
    __asm__ volatile("fnstsw %0" : "=a"(v));
    return v;
}

// A fresh x87 with C1 as asked and the other flags clear; st(0)=a, st(1)=b.
static void setup(long double *a, long double *b, int c1) {
    struct { uint16_t cw, p0, sw, p1, tw, p2; uint32_t ip, op, dp, ds; } env;
    __asm__ volatile("fninit");
    if (b)
        __asm__ volatile("fldt %0" : : "m"(*b));
    if (a)
        __asm__ volatile("fldt %0" : : "m"(*a));
    __asm__ volatile("fnstenv %0" : "=m"(env));
    env.sw = (env.sw & ~(C1 | 0xff)) | (c1 ? C1 : 0);
    __asm__ volatile("fldenv %0" : : "m"(env));
}

#define REG(name, insn) \
    static uint16_t name(void) { \
        uint16_t v; \
        __asm__ volatile(insn "\n\tfnstsw %0" : "=a"(v) : : "cc", "st", "st(1)"); \
        return v; \
    }
#define MEM(name, insn, var) \
    static uint16_t name(void) { \
        uint16_t v; \
        __asm__ volatile(insn " %1\n\tfnstsw %0" : "=a"(v) : "m"(var) : "cc", "st", "st(1)"); \
        return v; \
    }
REG(fcomi_st1, "fcomi %%st(1), %%st")
REG(fcomip_st1, "fcomip %%st(1), %%st")
REG(fucomi_st1, "fucomi %%st(1), %%st")
REG(fucomip_st1, "fucomip %%st(1), %%st")
REG(fcom_st1, "fcom %%st(1)")
REG(fcomp_st1, "fcomp %%st(1)")
REG(fcompp_, "fcompp")
REG(fucom_st1, "fucom %%st(1)")
REG(fucomp_st1, "fucomp %%st(1)")
REG(fucompp_, "fucompp")
REG(ftst_, "ftst")
MEM(fcom_m32, "fcoms", f2)
MEM(fcomp_m32, "fcomps", f2)
MEM(fcom_m64, "fcoml", d2)
MEM(fcomp_m64, "fcompl", d2)
MEM(ficom_m16, "ficoms", i16_2)
MEM(ficomp_m16, "ficomps", i16_2)
MEM(ficom_m32, "ficoml", i32_2)
MEM(ficomp_m32, "ficompl", i32_2)

enum kind { COMI, COM_REGS, COM_MEM, TST };
static const struct {
    const char *name;
    uint16_t (*fn)(void);
    enum kind kind;
} forms[] = {
#define F(n, k) {#n, n, k}
    F(fcomi_st1, COMI), F(fcomip_st1, COMI), F(fucomi_st1, COMI), F(fucomip_st1, COMI),
    F(fcom_st1, COM_REGS), F(fcomp_st1, COM_REGS), F(fcompp_, COM_REGS),
    F(fucom_st1, COM_REGS), F(fucomp_st1, COM_REGS), F(fucompp_, COM_REGS),
    F(ftst_, TST),
    F(fcom_m32, COM_MEM), F(fcomp_m32, COM_MEM), F(fcom_m64, COM_MEM), F(fcomp_m64, COM_MEM),
    F(ficom_m16, COM_MEM), F(ficomp_m16, COM_MEM), F(ficom_m32, COM_MEM), F(ficomp_m32, COM_MEM),
};

enum outcome { GT, LT, EQ, UN, FAULT };
static const struct {
    const char *name;
    long double *a, *b;
    enum outcome regs; // st(0) against st(1)
    enum outcome mem;  // st(0) against the 2 in memory
    enum outcome tst;  // st(0) against 0
} vals[] = {
    {"gt", &three, &two, GT, GT, GT},
    {"lt", &one, &two, LT, LT, GT},
    {"eq", &two, &two, EQ, EQ, GT},
    {"unordered", &nanv, &two, UN, UN, UN},
    {"st1 empty", &two, NULL, FAULT, EQ, GT},
    {"st0 empty", NULL, NULL, FAULT, FAULT, FAULT},
};

static unsigned long checks, bad;

static void check(const char *form, const char *val, int c1, const char *what, unsigned got, unsigned want) {
    checks++;
    if (got != want) {
        bad++;
        printf("%s, %s, C1 was %d: %s %04x, want %04x\n", form, val, c1, what, got, want);
    }
}

int main(void) {
    nanv = __builtin_nanl("");
    for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
        for (unsigned v = 0; v < sizeof(vals) / sizeof(vals[0]); v++) {
            for (int c1 = 0; c1 < 2; c1++) {
                enum kind kind = forms[f].kind;
                enum outcome out = kind == TST ? vals[v].tst : kind == COM_MEM ? vals[v].mem : vals[v].regs;
                unsigned want;
                if (kind == COMI)
                    want = c1 && out != FAULT ? C1 : 0;
                else if (out == LT)
                    want = C0;
                else if (out == EQ)
                    want = C3;
                else if (out == GT)
                    want = 0;
                else
                    want = C3 | C2 | C0;

                setup(vals[v].a, vals[v].b, c1);
                check(forms[f].name, vals[v].name, c1, "before, C1", sw() & C1, c1 ? C1 : 0);
                unsigned got = forms[f].fn();
                check(forms[f].name, vals[v].name, c1, "condition codes", got & CC, want);
                check(forms[f].name, vals[v].name, c1, "stack fault", got & SF, out == FAULT ? SF : 0);
            }
        }
    }
    __asm__ volatile("fninit");
    printf("x87_compare_c1: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
