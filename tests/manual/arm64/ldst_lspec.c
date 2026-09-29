// LDP/STP with writeback, and single-register loads and stores with
// writeback (post/pre-index) and
// register offsets (LSL/UXTX/SXTX, SXTW, UXTW) on the arm64 JIT: every size,
// sign extension and extend, which have fast gadgets fed slot offsets
// (jit/guest-arm64/memory.S's "lspec" family, /proc/ish/arm64_jit_fuse) as
// well as the generic ones. Rt/Rn/Rm = x3/x17/x26; indices include a
// negative SXTW and a UXTW whose upper half must be ignored. The loaded or
// stored value, the written-back base (relative to the buffer) and the
// buffer are hashed and compared with an Apple Silicon Mac running this file
// natively (-DPRINT_GOLDEN regenerates the table). Then direct checks: a
// faulting post-index load and pre-index store report their own pc and
// address and leave the base register unchanged. Run once with
// `echo lspec=0 > /proc/ish/arm64_jit_fuse` too.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __linux__
#include <ucontext.h>
#endif
#include <unistd.h>

#include "../test_common.h"

#if defined(__aarch64__)

struct st { uint64_t x3, x17, x26; uint8_t *buf; };
typedef void (*case_fn)(struct st *);

#define CASE(i, insn) \
    static void case_##i(struct st *s) { \
        __asm__ volatile("ldr x3, [%0]\n ldr x17, [%0, #8]\n ldr x26, [%0, #16]\n" insn "\n" \
                         "str x3, [%0]\n str x17, [%0, #8]\n str x26, [%0, #16]" \
                         :: "r"(s) : "x3", "x17", "x26", "memory"); \
    }
CASE(0, "ldrb w3, [x17], #8")
CASE(1, "ldrb w3, [x17, #8]!")
CASE(2, "ldrb w3, [x17], #-8")
CASE(3, "ldrb w3, [x17, #-8]!")
CASE(4, "ldrb w3, [x17], #0")
CASE(5, "ldrb w3, [x17, #0]!")
CASE(6, "ldrb w3, [x17, x26]")
CASE(7, "ldrb w3, [x17, x26, lsl #0]")
CASE(8, "ldrb w3, [x17, w26, sxtw #0]")
CASE(9, "ldrb w3, [x17, w26, uxtw #0]")
CASE(10, "ldrb w3, [x17, x26, sxtx #0]")
CASE(11, "ldrh w3, [x17], #8")
CASE(12, "ldrh w3, [x17, #8]!")
CASE(13, "ldrh w3, [x17], #-8")
CASE(14, "ldrh w3, [x17, #-8]!")
CASE(15, "ldrh w3, [x17], #0")
CASE(16, "ldrh w3, [x17, #0]!")
CASE(17, "ldrh w3, [x17, x26, lsl #0]")
CASE(18, "ldrh w3, [x17, w26, sxtw]")
CASE(19, "ldrh w3, [x17, w26, uxtw]")
CASE(20, "ldrh w3, [x17, x26, sxtx]")
CASE(21, "ldrh w3, [x17, x26, lsl #1]")
CASE(22, "ldrh w3, [x17, w26, sxtw #1]")
CASE(23, "ldrh w3, [x17, w26, uxtw #1]")
CASE(24, "ldrh w3, [x17, x26, sxtx #1]")
CASE(25, "ldr w3, [x17], #8")
CASE(26, "ldr w3, [x17, #8]!")
CASE(27, "ldr w3, [x17], #-8")
CASE(28, "ldr w3, [x17, #-8]!")
CASE(29, "ldr w3, [x17], #0")
CASE(30, "ldr w3, [x17, #0]!")
CASE(31, "ldr w3, [x17, x26, lsl #0]")
CASE(32, "ldr w3, [x17, w26, sxtw]")
CASE(33, "ldr w3, [x17, w26, uxtw]")
CASE(34, "ldr w3, [x17, x26, sxtx]")
CASE(35, "ldr w3, [x17, x26, lsl #2]")
CASE(36, "ldr w3, [x17, w26, sxtw #2]")
CASE(37, "ldr w3, [x17, w26, uxtw #2]")
CASE(38, "ldr w3, [x17, x26, sxtx #2]")
CASE(39, "ldr x3, [x17], #8")
CASE(40, "ldr x3, [x17, #8]!")
CASE(41, "ldr x3, [x17], #-8")
CASE(42, "ldr x3, [x17, #-8]!")
CASE(43, "ldr x3, [x17], #0")
CASE(44, "ldr x3, [x17, #0]!")
CASE(45, "ldr x3, [x17, x26, lsl #0]")
CASE(46, "ldr x3, [x17, w26, sxtw]")
CASE(47, "ldr x3, [x17, w26, uxtw]")
CASE(48, "ldr x3, [x17, x26, sxtx]")
CASE(49, "ldr x3, [x17, x26, lsl #3]")
CASE(50, "ldr x3, [x17, w26, sxtw #3]")
CASE(51, "ldr x3, [x17, w26, uxtw #3]")
CASE(52, "ldr x3, [x17, x26, sxtx #3]")
CASE(53, "ldrsb w3, [x17], #8")
CASE(54, "ldrsb w3, [x17, #8]!")
CASE(55, "ldrsb w3, [x17], #-8")
CASE(56, "ldrsb w3, [x17, #-8]!")
CASE(57, "ldrsb w3, [x17], #0")
CASE(58, "ldrsb w3, [x17, #0]!")
CASE(59, "ldrsb w3, [x17, x26]")
CASE(60, "ldrsb w3, [x17, x26, lsl #0]")
CASE(61, "ldrsb w3, [x17, w26, sxtw #0]")
CASE(62, "ldrsb w3, [x17, w26, uxtw #0]")
CASE(63, "ldrsb w3, [x17, x26, sxtx #0]")
CASE(64, "ldrsb x3, [x17], #8")
CASE(65, "ldrsb x3, [x17, #8]!")
CASE(66, "ldrsb x3, [x17], #-8")
CASE(67, "ldrsb x3, [x17, #-8]!")
CASE(68, "ldrsb x3, [x17], #0")
CASE(69, "ldrsb x3, [x17, #0]!")
CASE(70, "ldrsb x3, [x17, x26]")
CASE(71, "ldrsb x3, [x17, x26, lsl #0]")
CASE(72, "ldrsb x3, [x17, w26, sxtw #0]")
CASE(73, "ldrsb x3, [x17, w26, uxtw #0]")
CASE(74, "ldrsb x3, [x17, x26, sxtx #0]")
CASE(75, "ldrsh w3, [x17], #8")
CASE(76, "ldrsh w3, [x17, #8]!")
CASE(77, "ldrsh w3, [x17], #-8")
CASE(78, "ldrsh w3, [x17, #-8]!")
CASE(79, "ldrsh w3, [x17], #0")
CASE(80, "ldrsh w3, [x17, #0]!")
CASE(81, "ldrsh w3, [x17, x26, lsl #0]")
CASE(82, "ldrsh w3, [x17, w26, sxtw]")
CASE(83, "ldrsh w3, [x17, w26, uxtw]")
CASE(84, "ldrsh w3, [x17, x26, sxtx]")
CASE(85, "ldrsh w3, [x17, x26, lsl #1]")
CASE(86, "ldrsh w3, [x17, w26, sxtw #1]")
CASE(87, "ldrsh w3, [x17, w26, uxtw #1]")
CASE(88, "ldrsh w3, [x17, x26, sxtx #1]")
CASE(89, "ldrsh x3, [x17], #8")
CASE(90, "ldrsh x3, [x17, #8]!")
CASE(91, "ldrsh x3, [x17], #-8")
CASE(92, "ldrsh x3, [x17, #-8]!")
CASE(93, "ldrsh x3, [x17], #0")
CASE(94, "ldrsh x3, [x17, #0]!")
CASE(95, "ldrsh x3, [x17, x26, lsl #0]")
CASE(96, "ldrsh x3, [x17, w26, sxtw]")
CASE(97, "ldrsh x3, [x17, w26, uxtw]")
CASE(98, "ldrsh x3, [x17, x26, sxtx]")
CASE(99, "ldrsh x3, [x17, x26, lsl #1]")
CASE(100, "ldrsh x3, [x17, w26, sxtw #1]")
CASE(101, "ldrsh x3, [x17, w26, uxtw #1]")
CASE(102, "ldrsh x3, [x17, x26, sxtx #1]")
CASE(103, "ldrsw x3, [x17], #8")
CASE(104, "ldrsw x3, [x17, #8]!")
CASE(105, "ldrsw x3, [x17], #-8")
CASE(106, "ldrsw x3, [x17, #-8]!")
CASE(107, "ldrsw x3, [x17], #0")
CASE(108, "ldrsw x3, [x17, #0]!")
CASE(109, "ldrsw x3, [x17, x26, lsl #0]")
CASE(110, "ldrsw x3, [x17, w26, sxtw]")
CASE(111, "ldrsw x3, [x17, w26, uxtw]")
CASE(112, "ldrsw x3, [x17, x26, sxtx]")
CASE(113, "ldrsw x3, [x17, x26, lsl #2]")
CASE(114, "ldrsw x3, [x17, w26, sxtw #2]")
CASE(115, "ldrsw x3, [x17, w26, uxtw #2]")
CASE(116, "ldrsw x3, [x17, x26, sxtx #2]")
CASE(117, "strb w3, [x17], #8")
CASE(118, "strb w3, [x17, #8]!")
CASE(119, "strb w3, [x17], #-8")
CASE(120, "strb w3, [x17, #-8]!")
CASE(121, "strb w3, [x17], #0")
CASE(122, "strb w3, [x17, #0]!")
CASE(123, "strb w3, [x17, x26]")
CASE(124, "strb w3, [x17, x26, lsl #0]")
CASE(125, "strb w3, [x17, w26, sxtw #0]")
CASE(126, "strb w3, [x17, w26, uxtw #0]")
CASE(127, "strb w3, [x17, x26, sxtx #0]")
CASE(128, "strh w3, [x17], #8")
CASE(129, "strh w3, [x17, #8]!")
CASE(130, "strh w3, [x17], #-8")
CASE(131, "strh w3, [x17, #-8]!")
CASE(132, "strh w3, [x17], #0")
CASE(133, "strh w3, [x17, #0]!")
CASE(134, "strh w3, [x17, x26, lsl #0]")
CASE(135, "strh w3, [x17, w26, sxtw]")
CASE(136, "strh w3, [x17, w26, uxtw]")
CASE(137, "strh w3, [x17, x26, sxtx]")
CASE(138, "strh w3, [x17, x26, lsl #1]")
CASE(139, "strh w3, [x17, w26, sxtw #1]")
CASE(140, "strh w3, [x17, w26, uxtw #1]")
CASE(141, "strh w3, [x17, x26, sxtx #1]")
CASE(142, "str w3, [x17], #8")
CASE(143, "str w3, [x17, #8]!")
CASE(144, "str w3, [x17], #-8")
CASE(145, "str w3, [x17, #-8]!")
CASE(146, "str w3, [x17], #0")
CASE(147, "str w3, [x17, #0]!")
CASE(148, "str w3, [x17, x26, lsl #0]")
CASE(149, "str w3, [x17, w26, sxtw]")
CASE(150, "str w3, [x17, w26, uxtw]")
CASE(151, "str w3, [x17, x26, sxtx]")
CASE(152, "str w3, [x17, x26, lsl #2]")
CASE(153, "str w3, [x17, w26, sxtw #2]")
CASE(154, "str w3, [x17, w26, uxtw #2]")
CASE(155, "str w3, [x17, x26, sxtx #2]")
CASE(156, "str x3, [x17], #8")
CASE(157, "str x3, [x17, #8]!")
CASE(158, "str x3, [x17], #-8")
CASE(159, "str x3, [x17, #-8]!")
CASE(160, "str x3, [x17], #0")
CASE(161, "str x3, [x17, #0]!")
CASE(162, "str x3, [x17, x26, lsl #0]")
CASE(163, "str x3, [x17, w26, sxtw]")
CASE(164, "str x3, [x17, w26, uxtw]")
CASE(165, "str x3, [x17, x26, sxtx]")
CASE(166, "str x3, [x17, x26, lsl #3]")
CASE(167, "str x3, [x17, w26, sxtw #3]")
CASE(168, "str x3, [x17, w26, uxtw #3]")
CASE(169, "str x3, [x17, x26, sxtx #3]")
CASE(170, "ldp x3, x26, [x17], #16")
CASE(171, "ldp x3, x26, [x17, #16]!")
CASE(172, "ldp x3, x26, [x17], #-16")
CASE(173, "ldp x3, x26, [x17, #-16]!")
CASE(174, "ldp x3, x26, [x17], #8")
CASE(175, "ldp x3, x26, [x17, #8]!")
CASE(176, "ldp w3, w26, [x17], #16")
CASE(177, "ldp w3, w26, [x17, #16]!")
CASE(178, "ldp w3, w26, [x17], #-16")
CASE(179, "ldp w3, w26, [x17, #-16]!")
CASE(180, "ldp w3, w26, [x17], #8")
CASE(181, "ldp w3, w26, [x17, #8]!")
CASE(182, "stp x3, x26, [x17], #16")
CASE(183, "stp x3, x26, [x17, #16]!")
CASE(184, "stp x3, x26, [x17], #-16")
CASE(185, "stp x3, x26, [x17, #-16]!")
CASE(186, "stp x3, x26, [x17], #8")
CASE(187, "stp x3, x26, [x17, #8]!")
CASE(188, "stp w3, w26, [x17], #16")
CASE(189, "stp w3, w26, [x17, #16]!")
CASE(190, "stp w3, w26, [x17], #-16")
CASE(191, "stp w3, w26, [x17, #-16]!")
CASE(192, "stp w3, w26, [x17], #8")
CASE(193, "stp w3, w26, [x17, #8]!")
#undef CASE

struct desc { case_fn fn; const char *insn; };
#define CASE(i, insn) {case_##i, insn},
static const struct desc cases[] = {
CASE(0, "ldrb w3, [x17], #8")
CASE(1, "ldrb w3, [x17, #8]!")
CASE(2, "ldrb w3, [x17], #-8")
CASE(3, "ldrb w3, [x17, #-8]!")
CASE(4, "ldrb w3, [x17], #0")
CASE(5, "ldrb w3, [x17, #0]!")
CASE(6, "ldrb w3, [x17, x26]")
CASE(7, "ldrb w3, [x17, x26, lsl #0]")
CASE(8, "ldrb w3, [x17, w26, sxtw #0]")
CASE(9, "ldrb w3, [x17, w26, uxtw #0]")
CASE(10, "ldrb w3, [x17, x26, sxtx #0]")
CASE(11, "ldrh w3, [x17], #8")
CASE(12, "ldrh w3, [x17, #8]!")
CASE(13, "ldrh w3, [x17], #-8")
CASE(14, "ldrh w3, [x17, #-8]!")
CASE(15, "ldrh w3, [x17], #0")
CASE(16, "ldrh w3, [x17, #0]!")
CASE(17, "ldrh w3, [x17, x26, lsl #0]")
CASE(18, "ldrh w3, [x17, w26, sxtw]")
CASE(19, "ldrh w3, [x17, w26, uxtw]")
CASE(20, "ldrh w3, [x17, x26, sxtx]")
CASE(21, "ldrh w3, [x17, x26, lsl #1]")
CASE(22, "ldrh w3, [x17, w26, sxtw #1]")
CASE(23, "ldrh w3, [x17, w26, uxtw #1]")
CASE(24, "ldrh w3, [x17, x26, sxtx #1]")
CASE(25, "ldr w3, [x17], #8")
CASE(26, "ldr w3, [x17, #8]!")
CASE(27, "ldr w3, [x17], #-8")
CASE(28, "ldr w3, [x17, #-8]!")
CASE(29, "ldr w3, [x17], #0")
CASE(30, "ldr w3, [x17, #0]!")
CASE(31, "ldr w3, [x17, x26, lsl #0]")
CASE(32, "ldr w3, [x17, w26, sxtw]")
CASE(33, "ldr w3, [x17, w26, uxtw]")
CASE(34, "ldr w3, [x17, x26, sxtx]")
CASE(35, "ldr w3, [x17, x26, lsl #2]")
CASE(36, "ldr w3, [x17, w26, sxtw #2]")
CASE(37, "ldr w3, [x17, w26, uxtw #2]")
CASE(38, "ldr w3, [x17, x26, sxtx #2]")
CASE(39, "ldr x3, [x17], #8")
CASE(40, "ldr x3, [x17, #8]!")
CASE(41, "ldr x3, [x17], #-8")
CASE(42, "ldr x3, [x17, #-8]!")
CASE(43, "ldr x3, [x17], #0")
CASE(44, "ldr x3, [x17, #0]!")
CASE(45, "ldr x3, [x17, x26, lsl #0]")
CASE(46, "ldr x3, [x17, w26, sxtw]")
CASE(47, "ldr x3, [x17, w26, uxtw]")
CASE(48, "ldr x3, [x17, x26, sxtx]")
CASE(49, "ldr x3, [x17, x26, lsl #3]")
CASE(50, "ldr x3, [x17, w26, sxtw #3]")
CASE(51, "ldr x3, [x17, w26, uxtw #3]")
CASE(52, "ldr x3, [x17, x26, sxtx #3]")
CASE(53, "ldrsb w3, [x17], #8")
CASE(54, "ldrsb w3, [x17, #8]!")
CASE(55, "ldrsb w3, [x17], #-8")
CASE(56, "ldrsb w3, [x17, #-8]!")
CASE(57, "ldrsb w3, [x17], #0")
CASE(58, "ldrsb w3, [x17, #0]!")
CASE(59, "ldrsb w3, [x17, x26]")
CASE(60, "ldrsb w3, [x17, x26, lsl #0]")
CASE(61, "ldrsb w3, [x17, w26, sxtw #0]")
CASE(62, "ldrsb w3, [x17, w26, uxtw #0]")
CASE(63, "ldrsb w3, [x17, x26, sxtx #0]")
CASE(64, "ldrsb x3, [x17], #8")
CASE(65, "ldrsb x3, [x17, #8]!")
CASE(66, "ldrsb x3, [x17], #-8")
CASE(67, "ldrsb x3, [x17, #-8]!")
CASE(68, "ldrsb x3, [x17], #0")
CASE(69, "ldrsb x3, [x17, #0]!")
CASE(70, "ldrsb x3, [x17, x26]")
CASE(71, "ldrsb x3, [x17, x26, lsl #0]")
CASE(72, "ldrsb x3, [x17, w26, sxtw #0]")
CASE(73, "ldrsb x3, [x17, w26, uxtw #0]")
CASE(74, "ldrsb x3, [x17, x26, sxtx #0]")
CASE(75, "ldrsh w3, [x17], #8")
CASE(76, "ldrsh w3, [x17, #8]!")
CASE(77, "ldrsh w3, [x17], #-8")
CASE(78, "ldrsh w3, [x17, #-8]!")
CASE(79, "ldrsh w3, [x17], #0")
CASE(80, "ldrsh w3, [x17, #0]!")
CASE(81, "ldrsh w3, [x17, x26, lsl #0]")
CASE(82, "ldrsh w3, [x17, w26, sxtw]")
CASE(83, "ldrsh w3, [x17, w26, uxtw]")
CASE(84, "ldrsh w3, [x17, x26, sxtx]")
CASE(85, "ldrsh w3, [x17, x26, lsl #1]")
CASE(86, "ldrsh w3, [x17, w26, sxtw #1]")
CASE(87, "ldrsh w3, [x17, w26, uxtw #1]")
CASE(88, "ldrsh w3, [x17, x26, sxtx #1]")
CASE(89, "ldrsh x3, [x17], #8")
CASE(90, "ldrsh x3, [x17, #8]!")
CASE(91, "ldrsh x3, [x17], #-8")
CASE(92, "ldrsh x3, [x17, #-8]!")
CASE(93, "ldrsh x3, [x17], #0")
CASE(94, "ldrsh x3, [x17, #0]!")
CASE(95, "ldrsh x3, [x17, x26, lsl #0]")
CASE(96, "ldrsh x3, [x17, w26, sxtw]")
CASE(97, "ldrsh x3, [x17, w26, uxtw]")
CASE(98, "ldrsh x3, [x17, x26, sxtx]")
CASE(99, "ldrsh x3, [x17, x26, lsl #1]")
CASE(100, "ldrsh x3, [x17, w26, sxtw #1]")
CASE(101, "ldrsh x3, [x17, w26, uxtw #1]")
CASE(102, "ldrsh x3, [x17, x26, sxtx #1]")
CASE(103, "ldrsw x3, [x17], #8")
CASE(104, "ldrsw x3, [x17, #8]!")
CASE(105, "ldrsw x3, [x17], #-8")
CASE(106, "ldrsw x3, [x17, #-8]!")
CASE(107, "ldrsw x3, [x17], #0")
CASE(108, "ldrsw x3, [x17, #0]!")
CASE(109, "ldrsw x3, [x17, x26, lsl #0]")
CASE(110, "ldrsw x3, [x17, w26, sxtw]")
CASE(111, "ldrsw x3, [x17, w26, uxtw]")
CASE(112, "ldrsw x3, [x17, x26, sxtx]")
CASE(113, "ldrsw x3, [x17, x26, lsl #2]")
CASE(114, "ldrsw x3, [x17, w26, sxtw #2]")
CASE(115, "ldrsw x3, [x17, w26, uxtw #2]")
CASE(116, "ldrsw x3, [x17, x26, sxtx #2]")
CASE(117, "strb w3, [x17], #8")
CASE(118, "strb w3, [x17, #8]!")
CASE(119, "strb w3, [x17], #-8")
CASE(120, "strb w3, [x17, #-8]!")
CASE(121, "strb w3, [x17], #0")
CASE(122, "strb w3, [x17, #0]!")
CASE(123, "strb w3, [x17, x26]")
CASE(124, "strb w3, [x17, x26, lsl #0]")
CASE(125, "strb w3, [x17, w26, sxtw #0]")
CASE(126, "strb w3, [x17, w26, uxtw #0]")
CASE(127, "strb w3, [x17, x26, sxtx #0]")
CASE(128, "strh w3, [x17], #8")
CASE(129, "strh w3, [x17, #8]!")
CASE(130, "strh w3, [x17], #-8")
CASE(131, "strh w3, [x17, #-8]!")
CASE(132, "strh w3, [x17], #0")
CASE(133, "strh w3, [x17, #0]!")
CASE(134, "strh w3, [x17, x26, lsl #0]")
CASE(135, "strh w3, [x17, w26, sxtw]")
CASE(136, "strh w3, [x17, w26, uxtw]")
CASE(137, "strh w3, [x17, x26, sxtx]")
CASE(138, "strh w3, [x17, x26, lsl #1]")
CASE(139, "strh w3, [x17, w26, sxtw #1]")
CASE(140, "strh w3, [x17, w26, uxtw #1]")
CASE(141, "strh w3, [x17, x26, sxtx #1]")
CASE(142, "str w3, [x17], #8")
CASE(143, "str w3, [x17, #8]!")
CASE(144, "str w3, [x17], #-8")
CASE(145, "str w3, [x17, #-8]!")
CASE(146, "str w3, [x17], #0")
CASE(147, "str w3, [x17, #0]!")
CASE(148, "str w3, [x17, x26, lsl #0]")
CASE(149, "str w3, [x17, w26, sxtw]")
CASE(150, "str w3, [x17, w26, uxtw]")
CASE(151, "str w3, [x17, x26, sxtx]")
CASE(152, "str w3, [x17, x26, lsl #2]")
CASE(153, "str w3, [x17, w26, sxtw #2]")
CASE(154, "str w3, [x17, w26, uxtw #2]")
CASE(155, "str w3, [x17, x26, sxtx #2]")
CASE(156, "str x3, [x17], #8")
CASE(157, "str x3, [x17, #8]!")
CASE(158, "str x3, [x17], #-8")
CASE(159, "str x3, [x17, #-8]!")
CASE(160, "str x3, [x17], #0")
CASE(161, "str x3, [x17, #0]!")
CASE(162, "str x3, [x17, x26, lsl #0]")
CASE(163, "str x3, [x17, w26, sxtw]")
CASE(164, "str x3, [x17, w26, uxtw]")
CASE(165, "str x3, [x17, x26, sxtx]")
CASE(166, "str x3, [x17, x26, lsl #3]")
CASE(167, "str x3, [x17, w26, sxtw #3]")
CASE(168, "str x3, [x17, w26, uxtw #3]")
CASE(169, "str x3, [x17, x26, sxtx #3]")
CASE(170, "ldp x3, x26, [x17], #16")
CASE(171, "ldp x3, x26, [x17, #16]!")
CASE(172, "ldp x3, x26, [x17], #-16")
CASE(173, "ldp x3, x26, [x17, #-16]!")
CASE(174, "ldp x3, x26, [x17], #8")
CASE(175, "ldp x3, x26, [x17, #8]!")
CASE(176, "ldp w3, w26, [x17], #16")
CASE(177, "ldp w3, w26, [x17, #16]!")
CASE(178, "ldp w3, w26, [x17], #-16")
CASE(179, "ldp w3, w26, [x17, #-16]!")
CASE(180, "ldp w3, w26, [x17], #8")
CASE(181, "ldp w3, w26, [x17, #8]!")
CASE(182, "stp x3, x26, [x17], #16")
CASE(183, "stp x3, x26, [x17, #16]!")
CASE(184, "stp x3, x26, [x17], #-16")
CASE(185, "stp x3, x26, [x17, #-16]!")
CASE(186, "stp x3, x26, [x17], #8")
CASE(187, "stp x3, x26, [x17, #8]!")
CASE(188, "stp w3, w26, [x17], #16")
CASE(189, "stp w3, w26, [x17, #16]!")
CASE(190, "stp w3, w26, [x17], #-16")
CASE(191, "stp w3, w26, [x17, #-16]!")
CASE(192, "stp w3, w26, [x17], #8")
CASE(193, "stp w3, w26, [x17, #8]!")
};
#undef CASE

#ifndef PRINT_GOLDEN
static const uint64_t golden[] = {
#include "ldst_lspec.golden"
};
#endif

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

#ifdef __linux__
static volatile uintptr_t fault_pc, fault_addr, fault_x17;
static volatile int faults;

static void on_segv(int sig, siginfo_t *info, void *uc_) {
    (void) sig;
    ucontext_t *uc = uc_;
    fault_pc = uc->uc_mcontext.pc;
    fault_addr = (uintptr_t) info->si_addr;
    fault_x17 = uc->uc_mcontext.regs[17];
    faults++;
    uc->uc_mcontext.pc += 4;
}
#endif

static __attribute__((unused)) void ck(const char *label, uint64_t got, uint64_t want) {
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    test_logf("  %-52s got=%#llx want=%#llx\n", label, (unsigned long long) got,
              (unsigned long long) want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    static uint8_t buf[256] __attribute__((aligned(16)));
    // Index values: small positive ones, a negative W for SXTW, and for UXTW
    // one whose upper half must be dropped.
    static const uint64_t idx_x[] = { 0, 3, 5 };
    static const uint64_t idx_sxtw[] = { 0, 3, 0x00000000fffffffdull };
    static const uint64_t idx_uxtw[] = { 0, 3, 0xabcdef0000000004ull };
    unsigned n = sizeof(cases) / sizeof(cases[0]), bad = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t h = 0xcbf29ce484222325ull;
        const char *ins = cases[i].insn;
        const uint64_t *idx = strstr(ins, "sxtw") ? idx_sxtw
                            : strstr(ins, "uxtw") ? idx_uxtw : idx_x;
        unsigned nidx = 3;
        for (unsigned k = 0; k < nidx; k++) {
            for (int rep = 0; rep < 2; rep++) {
                for (int j = 0; j < 256; j++)
                    buf[j] = (uint8_t) (0x80 + j * 37);
                struct st s = { 0x8899aabbccddeeffull, (uint64_t) (uintptr_t) (buf + 128), idx[k], buf };
                cases[i].fn(&s);
                if (rep == 0)
                    continue;
                uint64_t rel = s.x17 - (uint64_t) (uintptr_t) buf;
                h = fnv(h, &s.x3, 8);
                h = fnv(h, &s.x26, 8);
                h = fnv(h, &rel, 8);
                h = fnv(h, buf, sizeof(buf));
            }
        }
#ifdef PRINT_GOLDEN
        printf("0x%016llxull, // %s\n", (unsigned long long) h, ins);
#else
        if (h != golden[i]) {
            bad++;
            test_logf("  %-36s wrong\n", ins);
        }
#endif
    }
#ifdef PRINT_GOLDEN
    return 0;
#else
    if (sizeof(golden) / sizeof(golden[0]) != n)
        failf("golden table size", sizeof(golden) / sizeof(golden[0]), 0, 0, n, 0, 0);
    if (bad)
        failf("loads/stores wrong", bad, 0, 0, 0, 0, 0);
    test_logf("  %u cases, %u wrong\n", n, bad);

#ifdef __linux__ // the fault checks read a Linux ucontext
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(map + pg, pg);
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa, NULL);
    uintptr_t pc;
    uint64_t base = (uint64_t) (uintptr_t) (map + pg), x3 = 0;
    faults = 0;
    {
        register uint64_t r17 __asm__("x17") = base;
        register uint64_t r3 __asm__("x3") = 0;
        __asm__ volatile("adr %2, 1f\n1: ldr x3, [x17], #16" : "+r"(r17), "+r"(r3), "=&r"(pc) :: "memory");
        base = r17;
        x3 = r3;
    }
    ck("post-index load fault: one fault", faults, 1);
    ck("post-index load fault: pc", fault_pc, pc);
    ck("post-index load fault: address", fault_addr, (uintptr_t) (map + pg));
    ck("post-index load fault: base unchanged at the fault", fault_x17, (uintptr_t) (map + pg));
    ck("post-index load fault: base unchanged after", base, (uintptr_t) (map + pg));
    (void) x3;
    base = (uint64_t) (uintptr_t) (map + pg - 8);
    faults = 0;
    {
        register uint64_t r17 __asm__("x17") = base;
        __asm__ volatile("adr %1, 1f\n1: str x17, [x17, #8]!" : "+r"(r17), "=&r"(pc) :: "memory");
        base = r17;
    }
    ck("pre-index store fault: one fault", faults, 1);
    ck("pre-index store fault: pc", fault_pc, pc);
    ck("pre-index store fault: address", fault_addr, (uintptr_t) (map + pg));
    ck("pre-index store fault: base unchanged", base, (uintptr_t) (map + pg - 8));
    base = (uint64_t) (uintptr_t) (map + pg + 16);
    faults = 0;
    {
        register uint64_t r17 __asm__("x17") = base;
        __asm__ volatile("adr %1, 1f\n1: stp x17, x17, [x17, #-16]!" : "+r"(r17), "=&r"(pc) :: "memory");
        base = r17;
    }
    ck("pre-index stp fault: one fault", faults, 1);
    ck("pre-index stp fault: pc", fault_pc, pc);
    ck("pre-index stp fault: address", fault_addr, (uintptr_t) (map + pg));
    ck("pre-index stp fault: base unchanged", base, (uintptr_t) (map + pg + 16));
#endif
    return finish_suite("ldst_lspec");
#endif
}

#else

int main(void) {
    printf("ldst_lspec: SKIP (arm64 only)\n");
    return 0;
}

#endif
