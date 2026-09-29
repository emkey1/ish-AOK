// Scalar integer ALU on the arm64 JIT: add/sub/adds/subs/cmp/cmn immediate
// and register (and LSL-shifted add/sub), and/orr/eor/ands register and
// immediate, mov register, and the SP-source forms, 32 and 64 bit. These have
// offset-fed gadgets (jit/guest-arm64/alu_spec.S, the "ospec" bit of
// /proc/ish/arm64_jit_fuse) beside the index-decoding ones. Registers x3,
// x17, x26 so a wrong offset shows; operand pairs include the carry,
// overflow and sign edges. Each case's result register (SP forms: relative
// to sp) and NZCV are hashed and compared with the value an Apple Silicon
// Mac computes running this file natively (-DPRINT_GOLDEN regenerates the
// table). Run once with `echo ospec=0 > /proc/ish/arm64_jit_fuse` too.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../test_common.h"

#if defined(__aarch64__)

struct st { uint64_t n, m, d, nzcv, sp; };
typedef void (*case_fn)(struct st *);

#define CASE(i, insn) \
    static void case_##i(struct st *s) { \
        __asm__ volatile("ldr x17, [%0]\n ldr x26, [%0, #8]\n ldr x3, [%0, #16]\n msr nzcv, xzr\n" \
                         insn "\n str x3, [%0, #16]\n mrs x9, nzcv\n str x9, [%0, #24]\n" \
                         "mov x9, sp\n str x9, [%0, #32]" \
                         :: "r"(s) : "x3", "x9", "x17", "x26", "cc", "memory"); \
    }
CASE(0, "add x3, x17, #0")
CASE(1, "add x3, x17, #1")
CASE(2, "add x3, x17, #4095")
CASE(3, "add x3, x17, #1, lsl #12")
CASE(4, "add x3, x17, #0xabc, lsl #12")
CASE(5, "sub x3, x17, #0")
CASE(6, "sub x3, x17, #1")
CASE(7, "sub x3, x17, #4095")
CASE(8, "sub x3, x17, #1, lsl #12")
CASE(9, "sub x3, x17, #0xabc, lsl #12")
CASE(10, "adds x3, x17, #0")
CASE(11, "adds x3, x17, #1")
CASE(12, "adds x3, x17, #4095")
CASE(13, "adds x3, x17, #1, lsl #12")
CASE(14, "adds x3, x17, #0xabc, lsl #12")
CASE(15, "subs x3, x17, #0")
CASE(16, "subs x3, x17, #1")
CASE(17, "subs x3, x17, #4095")
CASE(18, "subs x3, x17, #1, lsl #12")
CASE(19, "subs x3, x17, #0xabc, lsl #12")
CASE(20, "cmp x17, #0")
CASE(21, "cmp x17, #1")
CASE(22, "cmp x17, #4095")
CASE(23, "cmp x17, #0x123, lsl #12")
CASE(24, "cmn x17, #0")
CASE(25, "cmn x17, #1")
CASE(26, "cmn x17, #4095")
CASE(27, "cmn x17, #0x123, lsl #12")
CASE(28, "add x3, x17, x26")
CASE(29, "sub x3, x17, x26")
CASE(30, "adds x3, x17, x26")
CASE(31, "subs x3, x17, x26")
CASE(32, "and x3, x17, x26")
CASE(33, "orr x3, x17, x26")
CASE(34, "eor x3, x17, x26")
CASE(35, "ands x3, x17, x26")
CASE(36, "add x3, x17, x26, lsl #1")
CASE(37, "add x3, x17, x26, lsl #3")
CASE(38, "add x3, x17, x26, lsl #32")
CASE(39, "add x3, x17, x26, lsl #63")
CASE(40, "sub x3, x17, x26, lsl #1")
CASE(41, "sub x3, x17, x26, lsl #3")
CASE(42, "sub x3, x17, x26, lsl #32")
CASE(43, "sub x3, x17, x26, lsl #63")
CASE(44, "cmp x17, x26")
CASE(45, "cmn x17, x26")
CASE(46, "and x3, x17, #0xff")
CASE(47, "and x3, x17, #0xffff0000ffff0000")
CASE(48, "and x3, x17, #0x8000000000000001")
CASE(49, "and x3, x17, #0x5555555555555555")
CASE(50, "and x3, x17, #0x7ffffffffffffffe")
CASE(51, "orr x3, x17, #0xff")
CASE(52, "orr x3, x17, #0xffff0000ffff0000")
CASE(53, "orr x3, x17, #0x8000000000000001")
CASE(54, "orr x3, x17, #0x5555555555555555")
CASE(55, "orr x3, x17, #0x7ffffffffffffffe")
CASE(56, "eor x3, x17, #0xff")
CASE(57, "eor x3, x17, #0xffff0000ffff0000")
CASE(58, "eor x3, x17, #0x8000000000000001")
CASE(59, "eor x3, x17, #0x5555555555555555")
CASE(60, "eor x3, x17, #0x7ffffffffffffffe")
CASE(61, "ands x3, x17, #0xff")
CASE(62, "ands x3, x17, #0xffff0000ffff0000")
CASE(63, "ands x3, x17, #0x8000000000000001")
CASE(64, "ands x3, x17, #0x5555555555555555")
CASE(65, "ands x3, x17, #0x7ffffffffffffffe")
CASE(66, "mov x3, x26")
CASE(67, "add w3, w17, #0")
CASE(68, "add w3, w17, #1")
CASE(69, "add w3, w17, #4095")
CASE(70, "add w3, w17, #1, lsl #12")
CASE(71, "add w3, w17, #0xabc, lsl #12")
CASE(72, "sub w3, w17, #0")
CASE(73, "sub w3, w17, #1")
CASE(74, "sub w3, w17, #4095")
CASE(75, "sub w3, w17, #1, lsl #12")
CASE(76, "sub w3, w17, #0xabc, lsl #12")
CASE(77, "adds w3, w17, #0")
CASE(78, "adds w3, w17, #1")
CASE(79, "adds w3, w17, #4095")
CASE(80, "adds w3, w17, #1, lsl #12")
CASE(81, "adds w3, w17, #0xabc, lsl #12")
CASE(82, "subs w3, w17, #0")
CASE(83, "subs w3, w17, #1")
CASE(84, "subs w3, w17, #4095")
CASE(85, "subs w3, w17, #1, lsl #12")
CASE(86, "subs w3, w17, #0xabc, lsl #12")
CASE(87, "cmp w17, #0")
CASE(88, "cmp w17, #1")
CASE(89, "cmp w17, #4095")
CASE(90, "cmp w17, #0x123, lsl #12")
CASE(91, "cmn w17, #0")
CASE(92, "cmn w17, #1")
CASE(93, "cmn w17, #4095")
CASE(94, "cmn w17, #0x123, lsl #12")
CASE(95, "add w3, w17, w26")
CASE(96, "sub w3, w17, w26")
CASE(97, "adds w3, w17, w26")
CASE(98, "subs w3, w17, w26")
CASE(99, "and w3, w17, w26")
CASE(100, "orr w3, w17, w26")
CASE(101, "eor w3, w17, w26")
CASE(102, "ands w3, w17, w26")
CASE(103, "add w3, w17, w26, lsl #1")
CASE(104, "add w3, w17, w26, lsl #3")
CASE(105, "add w3, w17, w26, lsl #17")
CASE(106, "add w3, w17, w26, lsl #31")
CASE(107, "sub w3, w17, w26, lsl #1")
CASE(108, "sub w3, w17, w26, lsl #3")
CASE(109, "sub w3, w17, w26, lsl #17")
CASE(110, "sub w3, w17, w26, lsl #31")
CASE(111, "cmp w17, w26")
CASE(112, "cmn w17, w26")
CASE(113, "and w3, w17, #0xff")
CASE(114, "and w3, w17, #0xff00")
CASE(115, "and w3, w17, #0x80000001")
CASE(116, "and w3, w17, #0x55555555")
CASE(117, "and w3, w17, #0x7ffffffe")
CASE(118, "orr w3, w17, #0xff")
CASE(119, "orr w3, w17, #0xff00")
CASE(120, "orr w3, w17, #0x80000001")
CASE(121, "orr w3, w17, #0x55555555")
CASE(122, "orr w3, w17, #0x7ffffffe")
CASE(123, "eor w3, w17, #0xff")
CASE(124, "eor w3, w17, #0xff00")
CASE(125, "eor w3, w17, #0x80000001")
CASE(126, "eor w3, w17, #0x55555555")
CASE(127, "eor w3, w17, #0x7ffffffe")
CASE(128, "ands w3, w17, #0xff")
CASE(129, "ands w3, w17, #0xff00")
CASE(130, "ands w3, w17, #0x80000001")
CASE(131, "ands w3, w17, #0x55555555")
CASE(132, "ands w3, w17, #0x7ffffffe")
CASE(133, "mov w3, w26")
CASE(134, "add x3, sp, #16")
CASE(135, "mov x3, sp")
CASE(136, "add w3, wsp, #0x40")
CASE(137, "sub x3, sp, #0x10, lsl #12")
#undef CASE

struct desc { case_fn fn; const char *insn; };
#define CASE(i, insn) {case_##i, insn},
static const struct desc cases[] = {
CASE(0, "add x3, x17, #0")
CASE(1, "add x3, x17, #1")
CASE(2, "add x3, x17, #4095")
CASE(3, "add x3, x17, #1, lsl #12")
CASE(4, "add x3, x17, #0xabc, lsl #12")
CASE(5, "sub x3, x17, #0")
CASE(6, "sub x3, x17, #1")
CASE(7, "sub x3, x17, #4095")
CASE(8, "sub x3, x17, #1, lsl #12")
CASE(9, "sub x3, x17, #0xabc, lsl #12")
CASE(10, "adds x3, x17, #0")
CASE(11, "adds x3, x17, #1")
CASE(12, "adds x3, x17, #4095")
CASE(13, "adds x3, x17, #1, lsl #12")
CASE(14, "adds x3, x17, #0xabc, lsl #12")
CASE(15, "subs x3, x17, #0")
CASE(16, "subs x3, x17, #1")
CASE(17, "subs x3, x17, #4095")
CASE(18, "subs x3, x17, #1, lsl #12")
CASE(19, "subs x3, x17, #0xabc, lsl #12")
CASE(20, "cmp x17, #0")
CASE(21, "cmp x17, #1")
CASE(22, "cmp x17, #4095")
CASE(23, "cmp x17, #0x123, lsl #12")
CASE(24, "cmn x17, #0")
CASE(25, "cmn x17, #1")
CASE(26, "cmn x17, #4095")
CASE(27, "cmn x17, #0x123, lsl #12")
CASE(28, "add x3, x17, x26")
CASE(29, "sub x3, x17, x26")
CASE(30, "adds x3, x17, x26")
CASE(31, "subs x3, x17, x26")
CASE(32, "and x3, x17, x26")
CASE(33, "orr x3, x17, x26")
CASE(34, "eor x3, x17, x26")
CASE(35, "ands x3, x17, x26")
CASE(36, "add x3, x17, x26, lsl #1")
CASE(37, "add x3, x17, x26, lsl #3")
CASE(38, "add x3, x17, x26, lsl #32")
CASE(39, "add x3, x17, x26, lsl #63")
CASE(40, "sub x3, x17, x26, lsl #1")
CASE(41, "sub x3, x17, x26, lsl #3")
CASE(42, "sub x3, x17, x26, lsl #32")
CASE(43, "sub x3, x17, x26, lsl #63")
CASE(44, "cmp x17, x26")
CASE(45, "cmn x17, x26")
CASE(46, "and x3, x17, #0xff")
CASE(47, "and x3, x17, #0xffff0000ffff0000")
CASE(48, "and x3, x17, #0x8000000000000001")
CASE(49, "and x3, x17, #0x5555555555555555")
CASE(50, "and x3, x17, #0x7ffffffffffffffe")
CASE(51, "orr x3, x17, #0xff")
CASE(52, "orr x3, x17, #0xffff0000ffff0000")
CASE(53, "orr x3, x17, #0x8000000000000001")
CASE(54, "orr x3, x17, #0x5555555555555555")
CASE(55, "orr x3, x17, #0x7ffffffffffffffe")
CASE(56, "eor x3, x17, #0xff")
CASE(57, "eor x3, x17, #0xffff0000ffff0000")
CASE(58, "eor x3, x17, #0x8000000000000001")
CASE(59, "eor x3, x17, #0x5555555555555555")
CASE(60, "eor x3, x17, #0x7ffffffffffffffe")
CASE(61, "ands x3, x17, #0xff")
CASE(62, "ands x3, x17, #0xffff0000ffff0000")
CASE(63, "ands x3, x17, #0x8000000000000001")
CASE(64, "ands x3, x17, #0x5555555555555555")
CASE(65, "ands x3, x17, #0x7ffffffffffffffe")
CASE(66, "mov x3, x26")
CASE(67, "add w3, w17, #0")
CASE(68, "add w3, w17, #1")
CASE(69, "add w3, w17, #4095")
CASE(70, "add w3, w17, #1, lsl #12")
CASE(71, "add w3, w17, #0xabc, lsl #12")
CASE(72, "sub w3, w17, #0")
CASE(73, "sub w3, w17, #1")
CASE(74, "sub w3, w17, #4095")
CASE(75, "sub w3, w17, #1, lsl #12")
CASE(76, "sub w3, w17, #0xabc, lsl #12")
CASE(77, "adds w3, w17, #0")
CASE(78, "adds w3, w17, #1")
CASE(79, "adds w3, w17, #4095")
CASE(80, "adds w3, w17, #1, lsl #12")
CASE(81, "adds w3, w17, #0xabc, lsl #12")
CASE(82, "subs w3, w17, #0")
CASE(83, "subs w3, w17, #1")
CASE(84, "subs w3, w17, #4095")
CASE(85, "subs w3, w17, #1, lsl #12")
CASE(86, "subs w3, w17, #0xabc, lsl #12")
CASE(87, "cmp w17, #0")
CASE(88, "cmp w17, #1")
CASE(89, "cmp w17, #4095")
CASE(90, "cmp w17, #0x123, lsl #12")
CASE(91, "cmn w17, #0")
CASE(92, "cmn w17, #1")
CASE(93, "cmn w17, #4095")
CASE(94, "cmn w17, #0x123, lsl #12")
CASE(95, "add w3, w17, w26")
CASE(96, "sub w3, w17, w26")
CASE(97, "adds w3, w17, w26")
CASE(98, "subs w3, w17, w26")
CASE(99, "and w3, w17, w26")
CASE(100, "orr w3, w17, w26")
CASE(101, "eor w3, w17, w26")
CASE(102, "ands w3, w17, w26")
CASE(103, "add w3, w17, w26, lsl #1")
CASE(104, "add w3, w17, w26, lsl #3")
CASE(105, "add w3, w17, w26, lsl #17")
CASE(106, "add w3, w17, w26, lsl #31")
CASE(107, "sub w3, w17, w26, lsl #1")
CASE(108, "sub w3, w17, w26, lsl #3")
CASE(109, "sub w3, w17, w26, lsl #17")
CASE(110, "sub w3, w17, w26, lsl #31")
CASE(111, "cmp w17, w26")
CASE(112, "cmn w17, w26")
CASE(113, "and w3, w17, #0xff")
CASE(114, "and w3, w17, #0xff00")
CASE(115, "and w3, w17, #0x80000001")
CASE(116, "and w3, w17, #0x55555555")
CASE(117, "and w3, w17, #0x7ffffffe")
CASE(118, "orr w3, w17, #0xff")
CASE(119, "orr w3, w17, #0xff00")
CASE(120, "orr w3, w17, #0x80000001")
CASE(121, "orr w3, w17, #0x55555555")
CASE(122, "orr w3, w17, #0x7ffffffe")
CASE(123, "eor w3, w17, #0xff")
CASE(124, "eor w3, w17, #0xff00")
CASE(125, "eor w3, w17, #0x80000001")
CASE(126, "eor w3, w17, #0x55555555")
CASE(127, "eor w3, w17, #0x7ffffffe")
CASE(128, "ands w3, w17, #0xff")
CASE(129, "ands w3, w17, #0xff00")
CASE(130, "ands w3, w17, #0x80000001")
CASE(131, "ands w3, w17, #0x55555555")
CASE(132, "ands w3, w17, #0x7ffffffe")
CASE(133, "mov w3, w26")
CASE(134, "add x3, sp, #16")
CASE(135, "mov x3, sp")
CASE(136, "add w3, wsp, #0x40")
CASE(137, "sub x3, sp, #0x10, lsl #12")
};
#undef CASE

#ifndef PRINT_GOLDEN
static const uint64_t golden[] = {
#include "alu_ospec.golden"
};
#endif

static const uint64_t vals[][2] = {
    {0, 0}, {1, 1}, {0xffffffffffffffffull, 1}, {0x7fffffffffffffffull, 1},
    {0x8000000000000000ull, 0x8000000000000000ull}, {0x00000000ffffffffull, 1},
    {0x000000007fffffffull, 0x7fffffff}, {0x0123456789abcdefull, 0xfedcba9876543210ull},
    {5, 7}, {0xdeadbeef00000000ull, 0x00000000cafef00dull},
};

int main(int argc, char **argv) {
    test_init(argc, argv);
    unsigned n = sizeof(cases) / sizeof(cases[0]), bad = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t h = 0xcbf29ce484222325ull;
        int sp_rel = strstr(cases[i].insn, "sp") != NULL; // "add x3, sp", "mov x3, sp"...
        for (unsigned v = 0; v < sizeof(vals) / sizeof(vals[0]); v++) {
            for (int rep = 0; rep < 2; rep++) {
                struct st s = { vals[v][0], vals[v][1], 0x5a5a5a5a5a5a5a5aull, 0, 0 };
                cases[i].fn(&s);
                if (rep == 0)
                    continue;
                uint64_t d = s.d;
                // "x3, sp" / "w3, wsp": the result relative to sp, which differs
                // between runs and hosts. The 32-bit form keeps only the low half.
                if (sp_rel)
                    d -= s.sp;
                if (sp_rel && strstr(cases[i].insn, "wsp") != NULL)
                    d = (uint32_t) d;
                h = (h ^ d) * 0x100000001b3ull;
                h = (h ^ (s.nzcv >> 28)) * 0x100000001b3ull;
            }
        }
#ifdef PRINT_GOLDEN
        printf("0x%016llxull, // %s\n", (unsigned long long) h, cases[i].insn);
#else
        if (h != golden[i]) {
            bad++;
            test_logf("  %-36s wrong\n", cases[i].insn);
        }
#endif
    }
#ifndef PRINT_GOLDEN
    if (sizeof(golden) / sizeof(golden[0]) != n)
        failf("golden table size", sizeof(golden) / sizeof(golden[0]), 0, 0, n, 0, 0);
    if (bad)
        failf("ALU results wrong", bad, 0, 0, 0, 0, 0);
    test_logf("  %u cases, %u wrong\n", n, bad);
    return finish_suite("alu_ospec");
#else
    return 0;
#endif
}

#else

int main(void) {
    printf("alu_ospec: SKIP (arm64 only)\n");
    return 0;
}

#endif
