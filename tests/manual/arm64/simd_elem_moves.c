// SIMD element moves on the arm64 JIT: UMOV, INS (from a GPR and element to
// element), DUP by element (vector and scalar), the FMOV general<->FP forms
// and vector MOV, at every element size and lane (every pair of a spread of
// lanes for INS element). These have size-specialised gadgets fed
// precomputed offsets (jit/guest-arm64/simd_spec.S, the "vspec" bit of
// /proc/ish/arm64_jit_fuse) alongside the generic ones; each case is checked
// against a byte-level C model of the architecture, including the lanes an
// instruction must keep and the upper half it must clear. The same file
// built natively on an arm64 Mac passes, which is what validates the model.
// Run once with vspec on and once off to cover both gadget sets.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../test_common.h"

#if defined(__aarch64__)

enum { K_TOG, K_INSG, K_INSE, K_DUP, K_DUPS, K_TOV, K_VMOV };

struct st {
    uint8_t v1[16], v2[16];
    uint64_t x9, x10;
};

typedef void (*case_fn)(struct st *);

#define CASE(i, insn, kind, size, a, b) \
    static void case_##i(struct st *s) { \
        __asm__ volatile("ldr q1, [%0]\n ldr q2, [%0, #16]\n ldr x9, [%0, #32]\n ldr x10, [%0, #40]\n" \
                         insn "\n str q2, [%0, #16]\n str x10, [%0, #40]" \
                         :: "r"(s) : "v1", "v2", "x9", "x10", "memory"); \
    }
CASE(0, "umov w10, v1.b[0]", K_TOG, 0, 0, 0)
CASE(1, "umov w10, v1.b[1]", K_TOG, 0, 1, 0)
CASE(2, "umov w10, v1.b[2]", K_TOG, 0, 2, 0)
CASE(3, "umov w10, v1.b[3]", K_TOG, 0, 3, 0)
CASE(4, "umov w10, v1.b[4]", K_TOG, 0, 4, 0)
CASE(5, "umov w10, v1.b[5]", K_TOG, 0, 5, 0)
CASE(6, "umov w10, v1.b[6]", K_TOG, 0, 6, 0)
CASE(7, "umov w10, v1.b[7]", K_TOG, 0, 7, 0)
CASE(8, "umov w10, v1.b[8]", K_TOG, 0, 8, 0)
CASE(9, "umov w10, v1.b[9]", K_TOG, 0, 9, 0)
CASE(10, "umov w10, v1.b[10]", K_TOG, 0, 10, 0)
CASE(11, "umov w10, v1.b[11]", K_TOG, 0, 11, 0)
CASE(12, "umov w10, v1.b[12]", K_TOG, 0, 12, 0)
CASE(13, "umov w10, v1.b[13]", K_TOG, 0, 13, 0)
CASE(14, "umov w10, v1.b[14]", K_TOG, 0, 14, 0)
CASE(15, "umov w10, v1.b[15]", K_TOG, 0, 15, 0)
CASE(16, "umov w10, v1.h[0]", K_TOG, 1, 0, 0)
CASE(17, "umov w10, v1.h[1]", K_TOG, 1, 2, 0)
CASE(18, "umov w10, v1.h[2]", K_TOG, 1, 4, 0)
CASE(19, "umov w10, v1.h[3]", K_TOG, 1, 6, 0)
CASE(20, "umov w10, v1.h[4]", K_TOG, 1, 8, 0)
CASE(21, "umov w10, v1.h[5]", K_TOG, 1, 10, 0)
CASE(22, "umov w10, v1.h[6]", K_TOG, 1, 12, 0)
CASE(23, "umov w10, v1.h[7]", K_TOG, 1, 14, 0)
CASE(24, "umov w10, v1.s[0]", K_TOG, 2, 0, 0)
CASE(25, "umov w10, v1.s[1]", K_TOG, 2, 4, 0)
CASE(26, "umov w10, v1.s[2]", K_TOG, 2, 8, 0)
CASE(27, "umov w10, v1.s[3]", K_TOG, 2, 12, 0)
CASE(28, "umov x10, v1.d[0]", K_TOG, 3, 0, 0)
CASE(29, "umov x10, v1.d[1]", K_TOG, 3, 8, 0)
CASE(30, "mov v2.b[0], w9", K_INSG, 0, 0, 0)
CASE(31, "mov v2.b[1], w9", K_INSG, 0, 1, 0)
CASE(32, "mov v2.b[2], w9", K_INSG, 0, 2, 0)
CASE(33, "mov v2.b[3], w9", K_INSG, 0, 3, 0)
CASE(34, "mov v2.b[4], w9", K_INSG, 0, 4, 0)
CASE(35, "mov v2.b[5], w9", K_INSG, 0, 5, 0)
CASE(36, "mov v2.b[6], w9", K_INSG, 0, 6, 0)
CASE(37, "mov v2.b[7], w9", K_INSG, 0, 7, 0)
CASE(38, "mov v2.b[8], w9", K_INSG, 0, 8, 0)
CASE(39, "mov v2.b[9], w9", K_INSG, 0, 9, 0)
CASE(40, "mov v2.b[10], w9", K_INSG, 0, 10, 0)
CASE(41, "mov v2.b[11], w9", K_INSG, 0, 11, 0)
CASE(42, "mov v2.b[12], w9", K_INSG, 0, 12, 0)
CASE(43, "mov v2.b[13], w9", K_INSG, 0, 13, 0)
CASE(44, "mov v2.b[14], w9", K_INSG, 0, 14, 0)
CASE(45, "mov v2.b[15], w9", K_INSG, 0, 15, 0)
CASE(46, "mov v2.h[0], w9", K_INSG, 1, 0, 0)
CASE(47, "mov v2.h[1], w9", K_INSG, 1, 2, 0)
CASE(48, "mov v2.h[2], w9", K_INSG, 1, 4, 0)
CASE(49, "mov v2.h[3], w9", K_INSG, 1, 6, 0)
CASE(50, "mov v2.h[4], w9", K_INSG, 1, 8, 0)
CASE(51, "mov v2.h[5], w9", K_INSG, 1, 10, 0)
CASE(52, "mov v2.h[6], w9", K_INSG, 1, 12, 0)
CASE(53, "mov v2.h[7], w9", K_INSG, 1, 14, 0)
CASE(54, "mov v2.s[0], w9", K_INSG, 2, 0, 0)
CASE(55, "mov v2.s[1], w9", K_INSG, 2, 4, 0)
CASE(56, "mov v2.s[2], w9", K_INSG, 2, 8, 0)
CASE(57, "mov v2.s[3], w9", K_INSG, 2, 12, 0)
CASE(58, "mov v2.d[0], x9", K_INSG, 3, 0, 0)
CASE(59, "mov v2.d[1], x9", K_INSG, 3, 8, 0)
CASE(60, "mov v2.b[0], v1.b[0]", K_INSE, 0, 0, 0)
CASE(61, "mov v2.b[0], v1.b[1]", K_INSE, 0, 0, 1)
CASE(62, "mov v2.b[0], v1.b[8]", K_INSE, 0, 0, 8)
CASE(63, "mov v2.b[0], v1.b[15]", K_INSE, 0, 0, 15)
CASE(64, "mov v2.b[1], v1.b[0]", K_INSE, 0, 1, 0)
CASE(65, "mov v2.b[1], v1.b[1]", K_INSE, 0, 1, 1)
CASE(66, "mov v2.b[1], v1.b[8]", K_INSE, 0, 1, 8)
CASE(67, "mov v2.b[1], v1.b[15]", K_INSE, 0, 1, 15)
CASE(68, "mov v2.b[8], v1.b[0]", K_INSE, 0, 8, 0)
CASE(69, "mov v2.b[8], v1.b[1]", K_INSE, 0, 8, 1)
CASE(70, "mov v2.b[8], v1.b[8]", K_INSE, 0, 8, 8)
CASE(71, "mov v2.b[8], v1.b[15]", K_INSE, 0, 8, 15)
CASE(72, "mov v2.b[15], v1.b[0]", K_INSE, 0, 15, 0)
CASE(73, "mov v2.b[15], v1.b[1]", K_INSE, 0, 15, 1)
CASE(74, "mov v2.b[15], v1.b[8]", K_INSE, 0, 15, 8)
CASE(75, "mov v2.b[15], v1.b[15]", K_INSE, 0, 15, 15)
CASE(76, "mov v2.h[0], v1.h[0]", K_INSE, 1, 0, 0)
CASE(77, "mov v2.h[0], v1.h[1]", K_INSE, 1, 0, 2)
CASE(78, "mov v2.h[0], v1.h[4]", K_INSE, 1, 0, 8)
CASE(79, "mov v2.h[0], v1.h[7]", K_INSE, 1, 0, 14)
CASE(80, "mov v2.h[1], v1.h[0]", K_INSE, 1, 2, 0)
CASE(81, "mov v2.h[1], v1.h[1]", K_INSE, 1, 2, 2)
CASE(82, "mov v2.h[1], v1.h[4]", K_INSE, 1, 2, 8)
CASE(83, "mov v2.h[1], v1.h[7]", K_INSE, 1, 2, 14)
CASE(84, "mov v2.h[4], v1.h[0]", K_INSE, 1, 8, 0)
CASE(85, "mov v2.h[4], v1.h[1]", K_INSE, 1, 8, 2)
CASE(86, "mov v2.h[4], v1.h[4]", K_INSE, 1, 8, 8)
CASE(87, "mov v2.h[4], v1.h[7]", K_INSE, 1, 8, 14)
CASE(88, "mov v2.h[7], v1.h[0]", K_INSE, 1, 14, 0)
CASE(89, "mov v2.h[7], v1.h[1]", K_INSE, 1, 14, 2)
CASE(90, "mov v2.h[7], v1.h[4]", K_INSE, 1, 14, 8)
CASE(91, "mov v2.h[7], v1.h[7]", K_INSE, 1, 14, 14)
CASE(92, "mov v2.s[0], v1.s[0]", K_INSE, 2, 0, 0)
CASE(93, "mov v2.s[0], v1.s[1]", K_INSE, 2, 0, 4)
CASE(94, "mov v2.s[0], v1.s[2]", K_INSE, 2, 0, 8)
CASE(95, "mov v2.s[0], v1.s[3]", K_INSE, 2, 0, 12)
CASE(96, "mov v2.s[1], v1.s[0]", K_INSE, 2, 4, 0)
CASE(97, "mov v2.s[1], v1.s[1]", K_INSE, 2, 4, 4)
CASE(98, "mov v2.s[1], v1.s[2]", K_INSE, 2, 4, 8)
CASE(99, "mov v2.s[1], v1.s[3]", K_INSE, 2, 4, 12)
CASE(100, "mov v2.s[2], v1.s[0]", K_INSE, 2, 8, 0)
CASE(101, "mov v2.s[2], v1.s[1]", K_INSE, 2, 8, 4)
CASE(102, "mov v2.s[2], v1.s[2]", K_INSE, 2, 8, 8)
CASE(103, "mov v2.s[2], v1.s[3]", K_INSE, 2, 8, 12)
CASE(104, "mov v2.s[3], v1.s[0]", K_INSE, 2, 12, 0)
CASE(105, "mov v2.s[3], v1.s[1]", K_INSE, 2, 12, 4)
CASE(106, "mov v2.s[3], v1.s[2]", K_INSE, 2, 12, 8)
CASE(107, "mov v2.s[3], v1.s[3]", K_INSE, 2, 12, 12)
CASE(108, "mov v2.d[0], v1.d[0]", K_INSE, 3, 0, 0)
CASE(109, "mov v2.d[0], v1.d[1]", K_INSE, 3, 0, 8)
CASE(110, "mov v2.d[1], v1.d[0]", K_INSE, 3, 8, 0)
CASE(111, "mov v2.d[1], v1.d[1]", K_INSE, 3, 8, 8)
CASE(112, "dup v2.16b, v1.b[0]", K_DUP, 0, 0, 1)
CASE(113, "dup v2.16b, v1.b[1]", K_DUP, 0, 1, 1)
CASE(114, "dup v2.16b, v1.b[2]", K_DUP, 0, 2, 1)
CASE(115, "dup v2.16b, v1.b[3]", K_DUP, 0, 3, 1)
CASE(116, "dup v2.16b, v1.b[4]", K_DUP, 0, 4, 1)
CASE(117, "dup v2.16b, v1.b[5]", K_DUP, 0, 5, 1)
CASE(118, "dup v2.16b, v1.b[6]", K_DUP, 0, 6, 1)
CASE(119, "dup v2.16b, v1.b[7]", K_DUP, 0, 7, 1)
CASE(120, "dup v2.16b, v1.b[8]", K_DUP, 0, 8, 1)
CASE(121, "dup v2.16b, v1.b[9]", K_DUP, 0, 9, 1)
CASE(122, "dup v2.16b, v1.b[10]", K_DUP, 0, 10, 1)
CASE(123, "dup v2.16b, v1.b[11]", K_DUP, 0, 11, 1)
CASE(124, "dup v2.16b, v1.b[12]", K_DUP, 0, 12, 1)
CASE(125, "dup v2.16b, v1.b[13]", K_DUP, 0, 13, 1)
CASE(126, "dup v2.16b, v1.b[14]", K_DUP, 0, 14, 1)
CASE(127, "dup v2.16b, v1.b[15]", K_DUP, 0, 15, 1)
CASE(128, "dup v2.8b, v1.b[0]", K_DUP, 0, 0, 0)
CASE(129, "dup v2.8b, v1.b[1]", K_DUP, 0, 1, 0)
CASE(130, "dup v2.8b, v1.b[2]", K_DUP, 0, 2, 0)
CASE(131, "dup v2.8b, v1.b[3]", K_DUP, 0, 3, 0)
CASE(132, "dup v2.8b, v1.b[4]", K_DUP, 0, 4, 0)
CASE(133, "dup v2.8b, v1.b[5]", K_DUP, 0, 5, 0)
CASE(134, "dup v2.8b, v1.b[6]", K_DUP, 0, 6, 0)
CASE(135, "dup v2.8b, v1.b[7]", K_DUP, 0, 7, 0)
CASE(136, "dup v2.8b, v1.b[8]", K_DUP, 0, 8, 0)
CASE(137, "dup v2.8b, v1.b[9]", K_DUP, 0, 9, 0)
CASE(138, "dup v2.8b, v1.b[10]", K_DUP, 0, 10, 0)
CASE(139, "dup v2.8b, v1.b[11]", K_DUP, 0, 11, 0)
CASE(140, "dup v2.8b, v1.b[12]", K_DUP, 0, 12, 0)
CASE(141, "dup v2.8b, v1.b[13]", K_DUP, 0, 13, 0)
CASE(142, "dup v2.8b, v1.b[14]", K_DUP, 0, 14, 0)
CASE(143, "dup v2.8b, v1.b[15]", K_DUP, 0, 15, 0)
CASE(144, "dup v2.8h, v1.h[0]", K_DUP, 1, 0, 1)
CASE(145, "dup v2.8h, v1.h[1]", K_DUP, 1, 2, 1)
CASE(146, "dup v2.8h, v1.h[2]", K_DUP, 1, 4, 1)
CASE(147, "dup v2.8h, v1.h[3]", K_DUP, 1, 6, 1)
CASE(148, "dup v2.8h, v1.h[4]", K_DUP, 1, 8, 1)
CASE(149, "dup v2.8h, v1.h[5]", K_DUP, 1, 10, 1)
CASE(150, "dup v2.8h, v1.h[6]", K_DUP, 1, 12, 1)
CASE(151, "dup v2.8h, v1.h[7]", K_DUP, 1, 14, 1)
CASE(152, "dup v2.4h, v1.h[0]", K_DUP, 1, 0, 0)
CASE(153, "dup v2.4h, v1.h[1]", K_DUP, 1, 2, 0)
CASE(154, "dup v2.4h, v1.h[2]", K_DUP, 1, 4, 0)
CASE(155, "dup v2.4h, v1.h[3]", K_DUP, 1, 6, 0)
CASE(156, "dup v2.4h, v1.h[4]", K_DUP, 1, 8, 0)
CASE(157, "dup v2.4h, v1.h[5]", K_DUP, 1, 10, 0)
CASE(158, "dup v2.4h, v1.h[6]", K_DUP, 1, 12, 0)
CASE(159, "dup v2.4h, v1.h[7]", K_DUP, 1, 14, 0)
CASE(160, "dup v2.4s, v1.s[0]", K_DUP, 2, 0, 1)
CASE(161, "dup v2.4s, v1.s[1]", K_DUP, 2, 4, 1)
CASE(162, "dup v2.4s, v1.s[2]", K_DUP, 2, 8, 1)
CASE(163, "dup v2.4s, v1.s[3]", K_DUP, 2, 12, 1)
CASE(164, "dup v2.2s, v1.s[0]", K_DUP, 2, 0, 0)
CASE(165, "dup v2.2s, v1.s[1]", K_DUP, 2, 4, 0)
CASE(166, "dup v2.2s, v1.s[2]", K_DUP, 2, 8, 0)
CASE(167, "dup v2.2s, v1.s[3]", K_DUP, 2, 12, 0)
CASE(168, "dup v2.2d, v1.d[0]", K_DUP, 3, 0, 1)
CASE(169, "dup v2.2d, v1.d[1]", K_DUP, 3, 8, 1)
CASE(170, "mov b2, v1.b[0]", K_DUPS, 0, 0, 0)
CASE(171, "mov b2, v1.b[1]", K_DUPS, 0, 1, 0)
CASE(172, "mov b2, v1.b[2]", K_DUPS, 0, 2, 0)
CASE(173, "mov b2, v1.b[3]", K_DUPS, 0, 3, 0)
CASE(174, "mov b2, v1.b[4]", K_DUPS, 0, 4, 0)
CASE(175, "mov b2, v1.b[5]", K_DUPS, 0, 5, 0)
CASE(176, "mov b2, v1.b[6]", K_DUPS, 0, 6, 0)
CASE(177, "mov b2, v1.b[7]", K_DUPS, 0, 7, 0)
CASE(178, "mov b2, v1.b[8]", K_DUPS, 0, 8, 0)
CASE(179, "mov b2, v1.b[9]", K_DUPS, 0, 9, 0)
CASE(180, "mov b2, v1.b[10]", K_DUPS, 0, 10, 0)
CASE(181, "mov b2, v1.b[11]", K_DUPS, 0, 11, 0)
CASE(182, "mov b2, v1.b[12]", K_DUPS, 0, 12, 0)
CASE(183, "mov b2, v1.b[13]", K_DUPS, 0, 13, 0)
CASE(184, "mov b2, v1.b[14]", K_DUPS, 0, 14, 0)
CASE(185, "mov b2, v1.b[15]", K_DUPS, 0, 15, 0)
CASE(186, "mov h2, v1.h[0]", K_DUPS, 1, 0, 0)
CASE(187, "mov h2, v1.h[1]", K_DUPS, 1, 2, 0)
CASE(188, "mov h2, v1.h[2]", K_DUPS, 1, 4, 0)
CASE(189, "mov h2, v1.h[3]", K_DUPS, 1, 6, 0)
CASE(190, "mov h2, v1.h[4]", K_DUPS, 1, 8, 0)
CASE(191, "mov h2, v1.h[5]", K_DUPS, 1, 10, 0)
CASE(192, "mov h2, v1.h[6]", K_DUPS, 1, 12, 0)
CASE(193, "mov h2, v1.h[7]", K_DUPS, 1, 14, 0)
CASE(194, "mov s2, v1.s[0]", K_DUPS, 2, 0, 0)
CASE(195, "mov s2, v1.s[1]", K_DUPS, 2, 4, 0)
CASE(196, "mov s2, v1.s[2]", K_DUPS, 2, 8, 0)
CASE(197, "mov s2, v1.s[3]", K_DUPS, 2, 12, 0)
CASE(198, "mov d2, v1.d[0]", K_DUPS, 3, 0, 0)
CASE(199, "mov d2, v1.d[1]", K_DUPS, 3, 8, 0)
CASE(200, "fmov s2, w9", K_TOV, 2, 0, 0)
CASE(201, "fmov d2, x9", K_TOV, 3, 0, 0)
CASE(202, "fmov w10, s1", K_TOG, 2, 0, 0)
CASE(203, "fmov x10, d1", K_TOG, 3, 0, 0)
CASE(204, "fmov x10, v1.d[1]", K_TOG, 3, 8, 0)
CASE(205, "fmov v2.d[1], x9", K_INSG, 3, 8, 0)
CASE(206, "mov v2.16b, v1.16b", K_VMOV, 0, 0, 1)
CASE(207, "mov v2.8b, v1.8b", K_VMOV, 0, 0, 0)
#undef CASE

struct desc { case_fn fn; const char *insn; int kind, size, a, b; };
#define CASE(i, insn, kind, size, a, b) {case_##i, insn, kind, size, a, b},
static const struct desc cases[] = {
CASE(0, "umov w10, v1.b[0]", K_TOG, 0, 0, 0)
CASE(1, "umov w10, v1.b[1]", K_TOG, 0, 1, 0)
CASE(2, "umov w10, v1.b[2]", K_TOG, 0, 2, 0)
CASE(3, "umov w10, v1.b[3]", K_TOG, 0, 3, 0)
CASE(4, "umov w10, v1.b[4]", K_TOG, 0, 4, 0)
CASE(5, "umov w10, v1.b[5]", K_TOG, 0, 5, 0)
CASE(6, "umov w10, v1.b[6]", K_TOG, 0, 6, 0)
CASE(7, "umov w10, v1.b[7]", K_TOG, 0, 7, 0)
CASE(8, "umov w10, v1.b[8]", K_TOG, 0, 8, 0)
CASE(9, "umov w10, v1.b[9]", K_TOG, 0, 9, 0)
CASE(10, "umov w10, v1.b[10]", K_TOG, 0, 10, 0)
CASE(11, "umov w10, v1.b[11]", K_TOG, 0, 11, 0)
CASE(12, "umov w10, v1.b[12]", K_TOG, 0, 12, 0)
CASE(13, "umov w10, v1.b[13]", K_TOG, 0, 13, 0)
CASE(14, "umov w10, v1.b[14]", K_TOG, 0, 14, 0)
CASE(15, "umov w10, v1.b[15]", K_TOG, 0, 15, 0)
CASE(16, "umov w10, v1.h[0]", K_TOG, 1, 0, 0)
CASE(17, "umov w10, v1.h[1]", K_TOG, 1, 2, 0)
CASE(18, "umov w10, v1.h[2]", K_TOG, 1, 4, 0)
CASE(19, "umov w10, v1.h[3]", K_TOG, 1, 6, 0)
CASE(20, "umov w10, v1.h[4]", K_TOG, 1, 8, 0)
CASE(21, "umov w10, v1.h[5]", K_TOG, 1, 10, 0)
CASE(22, "umov w10, v1.h[6]", K_TOG, 1, 12, 0)
CASE(23, "umov w10, v1.h[7]", K_TOG, 1, 14, 0)
CASE(24, "umov w10, v1.s[0]", K_TOG, 2, 0, 0)
CASE(25, "umov w10, v1.s[1]", K_TOG, 2, 4, 0)
CASE(26, "umov w10, v1.s[2]", K_TOG, 2, 8, 0)
CASE(27, "umov w10, v1.s[3]", K_TOG, 2, 12, 0)
CASE(28, "umov x10, v1.d[0]", K_TOG, 3, 0, 0)
CASE(29, "umov x10, v1.d[1]", K_TOG, 3, 8, 0)
CASE(30, "mov v2.b[0], w9", K_INSG, 0, 0, 0)
CASE(31, "mov v2.b[1], w9", K_INSG, 0, 1, 0)
CASE(32, "mov v2.b[2], w9", K_INSG, 0, 2, 0)
CASE(33, "mov v2.b[3], w9", K_INSG, 0, 3, 0)
CASE(34, "mov v2.b[4], w9", K_INSG, 0, 4, 0)
CASE(35, "mov v2.b[5], w9", K_INSG, 0, 5, 0)
CASE(36, "mov v2.b[6], w9", K_INSG, 0, 6, 0)
CASE(37, "mov v2.b[7], w9", K_INSG, 0, 7, 0)
CASE(38, "mov v2.b[8], w9", K_INSG, 0, 8, 0)
CASE(39, "mov v2.b[9], w9", K_INSG, 0, 9, 0)
CASE(40, "mov v2.b[10], w9", K_INSG, 0, 10, 0)
CASE(41, "mov v2.b[11], w9", K_INSG, 0, 11, 0)
CASE(42, "mov v2.b[12], w9", K_INSG, 0, 12, 0)
CASE(43, "mov v2.b[13], w9", K_INSG, 0, 13, 0)
CASE(44, "mov v2.b[14], w9", K_INSG, 0, 14, 0)
CASE(45, "mov v2.b[15], w9", K_INSG, 0, 15, 0)
CASE(46, "mov v2.h[0], w9", K_INSG, 1, 0, 0)
CASE(47, "mov v2.h[1], w9", K_INSG, 1, 2, 0)
CASE(48, "mov v2.h[2], w9", K_INSG, 1, 4, 0)
CASE(49, "mov v2.h[3], w9", K_INSG, 1, 6, 0)
CASE(50, "mov v2.h[4], w9", K_INSG, 1, 8, 0)
CASE(51, "mov v2.h[5], w9", K_INSG, 1, 10, 0)
CASE(52, "mov v2.h[6], w9", K_INSG, 1, 12, 0)
CASE(53, "mov v2.h[7], w9", K_INSG, 1, 14, 0)
CASE(54, "mov v2.s[0], w9", K_INSG, 2, 0, 0)
CASE(55, "mov v2.s[1], w9", K_INSG, 2, 4, 0)
CASE(56, "mov v2.s[2], w9", K_INSG, 2, 8, 0)
CASE(57, "mov v2.s[3], w9", K_INSG, 2, 12, 0)
CASE(58, "mov v2.d[0], x9", K_INSG, 3, 0, 0)
CASE(59, "mov v2.d[1], x9", K_INSG, 3, 8, 0)
CASE(60, "mov v2.b[0], v1.b[0]", K_INSE, 0, 0, 0)
CASE(61, "mov v2.b[0], v1.b[1]", K_INSE, 0, 0, 1)
CASE(62, "mov v2.b[0], v1.b[8]", K_INSE, 0, 0, 8)
CASE(63, "mov v2.b[0], v1.b[15]", K_INSE, 0, 0, 15)
CASE(64, "mov v2.b[1], v1.b[0]", K_INSE, 0, 1, 0)
CASE(65, "mov v2.b[1], v1.b[1]", K_INSE, 0, 1, 1)
CASE(66, "mov v2.b[1], v1.b[8]", K_INSE, 0, 1, 8)
CASE(67, "mov v2.b[1], v1.b[15]", K_INSE, 0, 1, 15)
CASE(68, "mov v2.b[8], v1.b[0]", K_INSE, 0, 8, 0)
CASE(69, "mov v2.b[8], v1.b[1]", K_INSE, 0, 8, 1)
CASE(70, "mov v2.b[8], v1.b[8]", K_INSE, 0, 8, 8)
CASE(71, "mov v2.b[8], v1.b[15]", K_INSE, 0, 8, 15)
CASE(72, "mov v2.b[15], v1.b[0]", K_INSE, 0, 15, 0)
CASE(73, "mov v2.b[15], v1.b[1]", K_INSE, 0, 15, 1)
CASE(74, "mov v2.b[15], v1.b[8]", K_INSE, 0, 15, 8)
CASE(75, "mov v2.b[15], v1.b[15]", K_INSE, 0, 15, 15)
CASE(76, "mov v2.h[0], v1.h[0]", K_INSE, 1, 0, 0)
CASE(77, "mov v2.h[0], v1.h[1]", K_INSE, 1, 0, 2)
CASE(78, "mov v2.h[0], v1.h[4]", K_INSE, 1, 0, 8)
CASE(79, "mov v2.h[0], v1.h[7]", K_INSE, 1, 0, 14)
CASE(80, "mov v2.h[1], v1.h[0]", K_INSE, 1, 2, 0)
CASE(81, "mov v2.h[1], v1.h[1]", K_INSE, 1, 2, 2)
CASE(82, "mov v2.h[1], v1.h[4]", K_INSE, 1, 2, 8)
CASE(83, "mov v2.h[1], v1.h[7]", K_INSE, 1, 2, 14)
CASE(84, "mov v2.h[4], v1.h[0]", K_INSE, 1, 8, 0)
CASE(85, "mov v2.h[4], v1.h[1]", K_INSE, 1, 8, 2)
CASE(86, "mov v2.h[4], v1.h[4]", K_INSE, 1, 8, 8)
CASE(87, "mov v2.h[4], v1.h[7]", K_INSE, 1, 8, 14)
CASE(88, "mov v2.h[7], v1.h[0]", K_INSE, 1, 14, 0)
CASE(89, "mov v2.h[7], v1.h[1]", K_INSE, 1, 14, 2)
CASE(90, "mov v2.h[7], v1.h[4]", K_INSE, 1, 14, 8)
CASE(91, "mov v2.h[7], v1.h[7]", K_INSE, 1, 14, 14)
CASE(92, "mov v2.s[0], v1.s[0]", K_INSE, 2, 0, 0)
CASE(93, "mov v2.s[0], v1.s[1]", K_INSE, 2, 0, 4)
CASE(94, "mov v2.s[0], v1.s[2]", K_INSE, 2, 0, 8)
CASE(95, "mov v2.s[0], v1.s[3]", K_INSE, 2, 0, 12)
CASE(96, "mov v2.s[1], v1.s[0]", K_INSE, 2, 4, 0)
CASE(97, "mov v2.s[1], v1.s[1]", K_INSE, 2, 4, 4)
CASE(98, "mov v2.s[1], v1.s[2]", K_INSE, 2, 4, 8)
CASE(99, "mov v2.s[1], v1.s[3]", K_INSE, 2, 4, 12)
CASE(100, "mov v2.s[2], v1.s[0]", K_INSE, 2, 8, 0)
CASE(101, "mov v2.s[2], v1.s[1]", K_INSE, 2, 8, 4)
CASE(102, "mov v2.s[2], v1.s[2]", K_INSE, 2, 8, 8)
CASE(103, "mov v2.s[2], v1.s[3]", K_INSE, 2, 8, 12)
CASE(104, "mov v2.s[3], v1.s[0]", K_INSE, 2, 12, 0)
CASE(105, "mov v2.s[3], v1.s[1]", K_INSE, 2, 12, 4)
CASE(106, "mov v2.s[3], v1.s[2]", K_INSE, 2, 12, 8)
CASE(107, "mov v2.s[3], v1.s[3]", K_INSE, 2, 12, 12)
CASE(108, "mov v2.d[0], v1.d[0]", K_INSE, 3, 0, 0)
CASE(109, "mov v2.d[0], v1.d[1]", K_INSE, 3, 0, 8)
CASE(110, "mov v2.d[1], v1.d[0]", K_INSE, 3, 8, 0)
CASE(111, "mov v2.d[1], v1.d[1]", K_INSE, 3, 8, 8)
CASE(112, "dup v2.16b, v1.b[0]", K_DUP, 0, 0, 1)
CASE(113, "dup v2.16b, v1.b[1]", K_DUP, 0, 1, 1)
CASE(114, "dup v2.16b, v1.b[2]", K_DUP, 0, 2, 1)
CASE(115, "dup v2.16b, v1.b[3]", K_DUP, 0, 3, 1)
CASE(116, "dup v2.16b, v1.b[4]", K_DUP, 0, 4, 1)
CASE(117, "dup v2.16b, v1.b[5]", K_DUP, 0, 5, 1)
CASE(118, "dup v2.16b, v1.b[6]", K_DUP, 0, 6, 1)
CASE(119, "dup v2.16b, v1.b[7]", K_DUP, 0, 7, 1)
CASE(120, "dup v2.16b, v1.b[8]", K_DUP, 0, 8, 1)
CASE(121, "dup v2.16b, v1.b[9]", K_DUP, 0, 9, 1)
CASE(122, "dup v2.16b, v1.b[10]", K_DUP, 0, 10, 1)
CASE(123, "dup v2.16b, v1.b[11]", K_DUP, 0, 11, 1)
CASE(124, "dup v2.16b, v1.b[12]", K_DUP, 0, 12, 1)
CASE(125, "dup v2.16b, v1.b[13]", K_DUP, 0, 13, 1)
CASE(126, "dup v2.16b, v1.b[14]", K_DUP, 0, 14, 1)
CASE(127, "dup v2.16b, v1.b[15]", K_DUP, 0, 15, 1)
CASE(128, "dup v2.8b, v1.b[0]", K_DUP, 0, 0, 0)
CASE(129, "dup v2.8b, v1.b[1]", K_DUP, 0, 1, 0)
CASE(130, "dup v2.8b, v1.b[2]", K_DUP, 0, 2, 0)
CASE(131, "dup v2.8b, v1.b[3]", K_DUP, 0, 3, 0)
CASE(132, "dup v2.8b, v1.b[4]", K_DUP, 0, 4, 0)
CASE(133, "dup v2.8b, v1.b[5]", K_DUP, 0, 5, 0)
CASE(134, "dup v2.8b, v1.b[6]", K_DUP, 0, 6, 0)
CASE(135, "dup v2.8b, v1.b[7]", K_DUP, 0, 7, 0)
CASE(136, "dup v2.8b, v1.b[8]", K_DUP, 0, 8, 0)
CASE(137, "dup v2.8b, v1.b[9]", K_DUP, 0, 9, 0)
CASE(138, "dup v2.8b, v1.b[10]", K_DUP, 0, 10, 0)
CASE(139, "dup v2.8b, v1.b[11]", K_DUP, 0, 11, 0)
CASE(140, "dup v2.8b, v1.b[12]", K_DUP, 0, 12, 0)
CASE(141, "dup v2.8b, v1.b[13]", K_DUP, 0, 13, 0)
CASE(142, "dup v2.8b, v1.b[14]", K_DUP, 0, 14, 0)
CASE(143, "dup v2.8b, v1.b[15]", K_DUP, 0, 15, 0)
CASE(144, "dup v2.8h, v1.h[0]", K_DUP, 1, 0, 1)
CASE(145, "dup v2.8h, v1.h[1]", K_DUP, 1, 2, 1)
CASE(146, "dup v2.8h, v1.h[2]", K_DUP, 1, 4, 1)
CASE(147, "dup v2.8h, v1.h[3]", K_DUP, 1, 6, 1)
CASE(148, "dup v2.8h, v1.h[4]", K_DUP, 1, 8, 1)
CASE(149, "dup v2.8h, v1.h[5]", K_DUP, 1, 10, 1)
CASE(150, "dup v2.8h, v1.h[6]", K_DUP, 1, 12, 1)
CASE(151, "dup v2.8h, v1.h[7]", K_DUP, 1, 14, 1)
CASE(152, "dup v2.4h, v1.h[0]", K_DUP, 1, 0, 0)
CASE(153, "dup v2.4h, v1.h[1]", K_DUP, 1, 2, 0)
CASE(154, "dup v2.4h, v1.h[2]", K_DUP, 1, 4, 0)
CASE(155, "dup v2.4h, v1.h[3]", K_DUP, 1, 6, 0)
CASE(156, "dup v2.4h, v1.h[4]", K_DUP, 1, 8, 0)
CASE(157, "dup v2.4h, v1.h[5]", K_DUP, 1, 10, 0)
CASE(158, "dup v2.4h, v1.h[6]", K_DUP, 1, 12, 0)
CASE(159, "dup v2.4h, v1.h[7]", K_DUP, 1, 14, 0)
CASE(160, "dup v2.4s, v1.s[0]", K_DUP, 2, 0, 1)
CASE(161, "dup v2.4s, v1.s[1]", K_DUP, 2, 4, 1)
CASE(162, "dup v2.4s, v1.s[2]", K_DUP, 2, 8, 1)
CASE(163, "dup v2.4s, v1.s[3]", K_DUP, 2, 12, 1)
CASE(164, "dup v2.2s, v1.s[0]", K_DUP, 2, 0, 0)
CASE(165, "dup v2.2s, v1.s[1]", K_DUP, 2, 4, 0)
CASE(166, "dup v2.2s, v1.s[2]", K_DUP, 2, 8, 0)
CASE(167, "dup v2.2s, v1.s[3]", K_DUP, 2, 12, 0)
CASE(168, "dup v2.2d, v1.d[0]", K_DUP, 3, 0, 1)
CASE(169, "dup v2.2d, v1.d[1]", K_DUP, 3, 8, 1)
CASE(170, "mov b2, v1.b[0]", K_DUPS, 0, 0, 0)
CASE(171, "mov b2, v1.b[1]", K_DUPS, 0, 1, 0)
CASE(172, "mov b2, v1.b[2]", K_DUPS, 0, 2, 0)
CASE(173, "mov b2, v1.b[3]", K_DUPS, 0, 3, 0)
CASE(174, "mov b2, v1.b[4]", K_DUPS, 0, 4, 0)
CASE(175, "mov b2, v1.b[5]", K_DUPS, 0, 5, 0)
CASE(176, "mov b2, v1.b[6]", K_DUPS, 0, 6, 0)
CASE(177, "mov b2, v1.b[7]", K_DUPS, 0, 7, 0)
CASE(178, "mov b2, v1.b[8]", K_DUPS, 0, 8, 0)
CASE(179, "mov b2, v1.b[9]", K_DUPS, 0, 9, 0)
CASE(180, "mov b2, v1.b[10]", K_DUPS, 0, 10, 0)
CASE(181, "mov b2, v1.b[11]", K_DUPS, 0, 11, 0)
CASE(182, "mov b2, v1.b[12]", K_DUPS, 0, 12, 0)
CASE(183, "mov b2, v1.b[13]", K_DUPS, 0, 13, 0)
CASE(184, "mov b2, v1.b[14]", K_DUPS, 0, 14, 0)
CASE(185, "mov b2, v1.b[15]", K_DUPS, 0, 15, 0)
CASE(186, "mov h2, v1.h[0]", K_DUPS, 1, 0, 0)
CASE(187, "mov h2, v1.h[1]", K_DUPS, 1, 2, 0)
CASE(188, "mov h2, v1.h[2]", K_DUPS, 1, 4, 0)
CASE(189, "mov h2, v1.h[3]", K_DUPS, 1, 6, 0)
CASE(190, "mov h2, v1.h[4]", K_DUPS, 1, 8, 0)
CASE(191, "mov h2, v1.h[5]", K_DUPS, 1, 10, 0)
CASE(192, "mov h2, v1.h[6]", K_DUPS, 1, 12, 0)
CASE(193, "mov h2, v1.h[7]", K_DUPS, 1, 14, 0)
CASE(194, "mov s2, v1.s[0]", K_DUPS, 2, 0, 0)
CASE(195, "mov s2, v1.s[1]", K_DUPS, 2, 4, 0)
CASE(196, "mov s2, v1.s[2]", K_DUPS, 2, 8, 0)
CASE(197, "mov s2, v1.s[3]", K_DUPS, 2, 12, 0)
CASE(198, "mov d2, v1.d[0]", K_DUPS, 3, 0, 0)
CASE(199, "mov d2, v1.d[1]", K_DUPS, 3, 8, 0)
CASE(200, "fmov s2, w9", K_TOV, 2, 0, 0)
CASE(201, "fmov d2, x9", K_TOV, 3, 0, 0)
CASE(202, "fmov w10, s1", K_TOG, 2, 0, 0)
CASE(203, "fmov x10, d1", K_TOG, 3, 0, 0)
CASE(204, "fmov x10, v1.d[1]", K_TOG, 3, 8, 0)
CASE(205, "fmov v2.d[1], x9", K_INSG, 3, 8, 0)
CASE(206, "mov v2.16b, v1.16b", K_VMOV, 0, 0, 1)
CASE(207, "mov v2.8b, v1.8b", K_VMOV, 0, 0, 0)
};
#undef CASE

static void init(struct st *s) {
    for (int i = 0; i < 16; i++) {
        s->v1[i] = (uint8_t) (0x80 + i * 7);
        s->v2[i] = (uint8_t) (0xa0 + i);
    }
    s->x9 = 0xf8e7d6c5b4a39281ull;
    s->x10 = 0x1122334455667788ull;
}

// What the architecture says the instruction leaves in v2 and x10.
static void model(const struct desc *d, struct st *s) {
    unsigned esz = 1u << d->size;
    uint8_t x9b[8];
    memcpy(x9b, &s->x9, 8);
    switch (d->kind) {
    case K_TOG: {
        uint64_t v = 0;
        memcpy(&v, s->v1 + d->a, esz);
        s->x10 = v;
        break;
    }
    case K_INSG:
        memcpy(s->v2 + d->a, x9b, esz);
        break;
    case K_INSE:
        memcpy(s->v2 + d->a, s->v1 + d->b, esz);
        break;
    case K_DUP: {
        unsigned bytes = d->b ? 16 : 8;
        uint8_t e[8];
        memcpy(e, s->v1 + d->a, esz);
        memset(s->v2, 0, 16);
        for (unsigned i = 0; i < bytes; i += esz)
            memcpy(s->v2 + i, e, esz);
        break;
    }
    case K_DUPS: {
        uint8_t e[8];
        memcpy(e, s->v1 + d->a, esz);
        memset(s->v2, 0, 16);
        memcpy(s->v2, e, esz);
        break;
    }
    case K_TOV:
        memset(s->v2, 0, 16);
        memcpy(s->v2, x9b, esz);
        break;
    case K_VMOV:
        memset(s->v2, 0, 16);
        memcpy(s->v2, s->v1, d->b ? 16 : 8);
        break;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    unsigned n = sizeof(cases) / sizeof(cases[0]), bad = 0;
    for (unsigned i = 0; i < n; i++) {
        struct st got, want;
        init(&got);
        init(&want);
        // Twice: the second run is the translated block's second execution.
        cases[i].fn(&got);
        init(&got);
        cases[i].fn(&got);
        model(&cases[i], &want);
        if (memcmp(&got, &want, sizeof(got)) != 0) {
            bad++;
            test_logf("  %-28s wrong: v2 ", cases[i].insn);
            for (int j = 15; j >= 0; j--)
                test_logf("%02x", got.v2[j]);
            test_logf(" want ");
            for (int j = 15; j >= 0; j--)
                test_logf("%02x", want.v2[j]);
            test_logf(" x10 %016llx want %016llx\n", (unsigned long long) got.x10,
                      (unsigned long long) want.x10);
        }
    }
    if (bad)
        failf("element moves wrong", bad, 0, 0, 0, 0, 0);
    test_logf("  %u cases, %u wrong\n", n, bad);
    return finish_suite("simd_elem_moves");
}

#else

int main(void) {
    printf("simd_elem_moves: SKIP (arm64 only)\n");
    return 0;
}

#endif
