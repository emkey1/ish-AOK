// i386_bcd.c -- DAA DAS AAA AAS AAM AAD for every AL, a spread of AH, CF and AF
// in, the other flags all clear or all set: AX and CF PF AF ZF SF OF against a
// model of what an AMD Ryzen does (camd passes it), the flags the SDM calls
// undefined included:
//   DAA/DAS: AL +/- the correction (6 for the low digit, 0x60 for the high);
//     CF and AF as the SDM says; SF ZF PF OF from that 8-bit add/sub.
//   AAA/AAS: a 16-bit ADD/SUB of 0x106 to AX when the low digit adjusts (of 0
//     when it does not), the result masked with 0xff0f; SF ZF PF OF from the
//     16-bit result, CF = AF = whether it adjusted.
//   AAM: AH = AL / imm, AL = AL % imm; SF ZF PF from AL, CF AF OF clear.
//   AAD: AL += AH * imm, an 8-bit ADD whose flags are all six; AH = 0.
// (bcd_adjust.c has the cases these were first found by.) The gadgets are
// jit/gadgets-aarch64/control.S's aaa .. aad.
#include <stdint.h>
#include <stdio.h>

#define RUN(name, insn) static void name(uint32_t *ax, uint32_t *fl) { \
    __asm__ volatile("push %2\n popfl\n" insn "\n pushfl\n pop %1" : "+a"(*ax), "=r"(*fl) : "r"(*fl) : "cc", "memory"); }
RUN(daa, "daa") RUN(das, "das") RUN(aaa, "aaa") RUN(aas, "aas")
#define B(n) RUN(aam##n, "aam $" #n) RUN(aad##n, "aad $" #n)
B(10) B(16) B(1) B(2) B(7) B(128) B(255) B(3)

static unsigned par(unsigned x) { x &= 0xff; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return ~x & 1; }
static unsigned szp(unsigned v, int bits) {
    unsigned m = (1u << bits) - 1;
    return ((v >> (bits - 1)) & 1) << 7 | ((v & m) == 0) << 6 | par(v) << 2;
}
// an add or sub of `bits`: the result, and its six flags
static unsigned arith(unsigned a, unsigned b, int bits, int sub, unsigned *fl) {
    unsigned m = (1u << bits) - 1, r = (sub ? a - b : a + b) & m;
    unsigned cf = sub ? a < b : a + b > m, af = ((a ^ b ^ r) >> 4) & 1;
    unsigned sa = (a >> (bits - 1)) & 1, sb = (b >> (bits - 1)) & 1, sr = (r >> (bits - 1)) & 1;
    unsigned of = sub ? (sa != sb && sr != sa) : (sa == sb && sr != sa);
    *fl = szp(r, bits) | cf | af << 4 | of << 11;
    return r;
}
static void model(int op, int base, unsigned al, unsigned ah, unsigned fin, unsigned *ax, unsigned *fl) {
    unsigned cf = fin & 1, af = (fin >> 4) & 1, a16 = ah << 8 | al;
    if (op == 2 || op == 3) {                          // aaa, aas
        unsigned adj = (al & 15) > 9 || af;
        unsigned r = arith(a16, adj ? 0x106 : 0, 16, op == 3, fl);
        *fl = (*fl & ~0x11u) | (adj ? 0x11 : 0);
        *ax = r & 0xff0f;
    } else if (op == 0 || op == 1) {                   // daa, das
        unsigned adj1 = (al & 15) > 9 || af, adj2 = al > 0x99 || cf;
        unsigned corr = (adj1 ? 6 : 0) + (adj2 ? 0x60 : 0), f;
        unsigned r = arith(al, corr, 8, op == 1, &f);
        unsigned ncf = adj2 || (op == 1 && adj1 && al < 6);
        *fl = (f & 0x8c4) | ncf | adj1 << 4;
        *ax = ah << 8 | r;
    } else if (op == 4) {                              // aam
        *ax = (al / base) << 8 | al % base;
        *fl = szp(al % base, 8);
    } else {                                           // aad
        *ax = arith(al, (ah * base) & 0xff, 8, 0, fl);
    }
}

typedef void (*fn)(uint32_t *, uint32_t *);
int main(void) {
    static const struct { const char *n; fn f; int op, base; } ops[] = {
        {"daa", daa, 0, 0}, {"das", das, 1, 0}, {"aaa", aaa, 2, 0}, {"aas", aas, 3, 0},
        {"aam 10", aam10, 4, 10}, {"aam 16", aam16, 4, 16}, {"aam 1", aam1, 4, 1}, {"aam 2", aam2, 4, 2},
        {"aam 7", aam7, 4, 7}, {"aam 128", aam128, 4, 128}, {"aam 255", aam255, 4, 255}, {"aam 3", aam3, 4, 3},
        {"aad 10", aad10, 5, 10}, {"aad 16", aad16, 5, 16}, {"aad 1", aad1, 5, 1}, {"aad 2", aad2, 5, 2},
        {"aad 7", aad7, 5, 7}, {"aad 128", aad128, 5, 128}, {"aad 255", aad255, 5, 255}, {"aad 3", aad3, 5, 3},
    };
    static const uint8_t ahs[] = {0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x09, 0x99};
    unsigned long checks = 0, bad = 0;
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
        for (unsigned al = 0; al < 256; al++)
            for (unsigned h = 0; h < sizeof ahs; h++)
                for (unsigned fin = 0; fin < 8; fin++) {
                    uint32_t fl = 0x202 | (fin & 1) | ((fin & 2) << 3) | ((fin & 4) ? 0x8c4 : 0);
                    uint32_t ax = 0x12340000u | (uint32_t) ahs[h] << 8 | al;
                    unsigned f0 = fl, wax, wfl;
                    ops[o].f(&ax, &fl);
                    model(ops[o].op, ops[o].base, al, ahs[h], f0, &wax, &wfl);
                    checks++;
                    if (ax != (0x12340000u | wax) || (fl & 0x8d5) != (wfl & 0x8d5) || (fl & ~0x8d5u) != (f0 & ~0x8d5u)) {
                        if (bad++ < 30)
                            printf("FAIL %s al %02x ah %02x flags %03x: ax %08x flags %03x, want %08x %03x\n", ops[o].n, al,
                                   ahs[h], f0 & 0x8d5, (unsigned) ax, (unsigned) fl & 0x8d5, 0x12340000u | wax, wfl & 0x8d5);
                    }
                }
    printf("i386_bcd: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
