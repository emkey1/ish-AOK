// INC (FF /0) and DEC (FF /1) on a REGISTER operand -- the amd64 form that in
// long mode is the ONLY way to spell `inc`/`dec` of a register, because the
// one-byte 0x40+r encodings those mnemonics used in 32-bit mode were reassigned
// as the REX prefixes. So every `incq %rax` a compiler emits is FF /0 mod==3,
// which is why this family is hot (musl's __init_libc basename loop chains
// `incq %rax; movzbl -1(%rax),%ecx; cmp; je`).
//
// It used to bridge to the amd64_jit_ff_group C helper -- the largest single
// amd64 bridge helper at 1.85% self time -- and now has a native gadget
// (amd64_incdec_reg32/64). This test is the semantic oracle for that
// conversion: it must pass identically with the gadget on and off
// (/proc/ish/amd64_jit_fuse incdec_reg=1/0), and on real Linux.
//
// The flag rules are where emulators get this wrong, so they are the bulk of
// what is asserted here:
//
//   * INC and DEC set OF/SF/ZF/AF/PF but leave CF **ALONE**. That is the classic
//     bug -- the natural implementation is an add/sub, and an add/sub writes CF.
//     Checking it needs care, because CF lives in two places in this engine (the
//     authoritative CPU_cf byte that adc reads, and eflags bit 0 that pushfq and
//     the jcc gadgets read); restoring only one leaves a build where `stc; incq;
//     jc` and `stc; incq; adcq` disagree. Every CF case below is therefore read
//     back through THREE consumers -- pushfq, adc, and a conditional branch.
//
//   * OF is set only at the signed boundary (inc of INT64_MAX, dec of INT64_MIN),
//     which is exactly the case a naive `cset vs` on the wrong operand width
//     gets wrong.
//
//   * A 32-bit `incl %eax` zeroes the upper 32 bits of RAX, like every other
//     32-bit write in long mode.
//
// Registers are swept across both halves of the file's register space: ids 0-7
// live in the JIT's host register cache, ids 8-15 do not and are read and
// written straight out of CPU_amd64_regs by a different path in the same gadget.
// A test that only touched rax would exercise half the gadget.
//
// x86_64 only (on i386 these opcodes are the 0x40+r forms and a different path
// entirely; SKIPs there).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../test_common.h"

static const char *suite = "amd64_incdec";

#define CF_BIT (1u << 0)
#define PF_BIT (1u << 2)
#define AF_BIT (1u << 4)
#define ZF_BIT (1u << 6)
#define SF_BIT (1u << 7)
#define OF_BIT (1u << 11)

#if defined(__x86_64__)

static void check_u64(const char *label, uint64_t got, uint64_t expected) {
    if (got != expected)
        failf(label, got, 0, 0, expected, 0, 0);
    else
        test_logf("ok %s = %016" PRIx64 "\n", label, got);
}

// Expected flags, computed independently of the emulator. `width` is 32 or 64.
static uint64_t expect_flags(uint64_t in, uint64_t result, int is_dec,
        int width, int cf_in) {
    uint64_t f = 0;
    uint64_t sign = (width == 64) ? (1ull << 63) : (1ull << 31);
    uint64_t of_edge = is_dec ? sign : (sign - 1);   // dec: INT_MIN, inc: INT_MAX
    uint64_t masked_in = (width == 64) ? in : (in & 0xffffffffu);

    if (cf_in)                      f |= CF_BIT;     // CF is PRESERVED, not computed
    if (masked_in == of_edge)       f |= OF_BIT;
    if (result == 0)                f |= ZF_BIT;
    if (result & sign)              f |= SF_BIT;
    if ((in ^ 1u ^ result) & 0x10)  f |= AF_BIT;     // carry/borrow out of bit 3
    // PF is the even-parity of the low byte.
    unsigned byte = (unsigned) (result & 0xff), ones = 0;
    for (unsigned i = 0; i < 8; i++)
        ones += (byte >> i) & 1;
    if ((ones & 1) == 0)            f |= PF_BIT;
    return f;
}

static const uint64_t FLAG_MASK = CF_BIT | PF_BIT | AF_BIT | ZF_BIT | SF_BIT | OF_BIT;

// One case: put `in` in the named register, set CF to cf_in, run the
// instruction, and read the flags back three different ways.
//
//   pushfq -> the architectural eflags image
//   branch -> the conditional-branch path
//   adc    -> the engine's authoritative CF byte (a separate consumer)
//
// A build that restores only one of the two CF locations passes one of these
// and fails another, which is the whole point of reading it three ways.
//
// ORDER MATTERS, and getting it wrong is how the first draft of this test
// failed on real hardware: `adc` WRITES flags (0+0+CF leaves CF clear), so it
// has to come last or it destroys the CF the branch is about to test. The
// branch records its outcome with `mov`, not `inc`, for the same reason --
// `inc`'s effect on CF is the thing under test, so it must not sit in the
// middle of the measurement.
#define RUN_CASE(REGNAME, INSN, CARRY, in_val, res_out, fl_out, adc_out, br_out) \
    do {                                                                        \
        register uint64_t r_ asm(REGNAME) = (in_val);                           \
        uint64_t f_ = 0, adc_ = 0, br_ = 0;                                      \
        __asm__ volatile(                                                        \
            CARRY "\n\t"                 /* set CF in                        */  \
            INSN "\n\t"                  /* the instruction under test       */  \
            "pushfq\n\t"                                                         \
            "popq %1\n\t"                /* consumer 1: eflags image         */  \
            "movq $0, %3\n\t"            /* (mov imm does not touch flags)   */  \
            "jnc 1f\n\t"                 /* consumer 2: branch on CF         */  \
            "movq $1, %3\n"                                                      \
            "1:\t"                                                               \
            "movq $0, %2\n\t"                                                    \
            "adcq $0, %2\n\t"            /* consumer 3: CF byte via adc      */  \
            : "+r"(r_), "=&r"(f_), "=&r"(adc_), "=&r"(br_) : : "cc");            \
        (res_out) = r_; (fl_out) = f_; (adc_out) = adc_; (br_out) = br_;         \
    } while (0)

// Run one (register, instruction, input, carry-in) case and assert result+flags.
#define CASE64(REGNAME, INSN, IS_DEC, CARRY, CF_IN, IN)                          \
    do {                                                                         \
        uint64_t res, fl, adc, br;                                               \
        RUN_CASE(REGNAME, INSN " %0", CARRY, (IN), res, fl, adc, br);            \
        uint64_t want = (IS_DEC) ? (uint64_t)(IN) - 1 : (uint64_t)(IN) + 1;      \
        char lbl[160];                                                           \
        snprintf(lbl, sizeof lbl, "%s %s in=%016" PRIx64 " cf=%d result",        \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, res, want);                                               \
        snprintf(lbl, sizeof lbl, "%s %s in=%016" PRIx64 " cf=%d flags",         \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, fl & FLAG_MASK,                                           \
                  expect_flags((IN), want, (IS_DEC), 64, (CF_IN)));              \
        snprintf(lbl, sizeof lbl, "%s %s in=%016" PRIx64 " cf=%d CF via adc",    \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, adc, (uint64_t)(CF_IN));                                  \
        snprintf(lbl, sizeof lbl, "%s %s in=%016" PRIx64 " cf=%d CF via branch", \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, br, (uint64_t)(CF_IN));                                   \
    } while (0)

// The same, 32-bit (`incl %eax`): the result is the 32-bit value ZERO-EXTENDED
// into the full 64-bit register, so `want` is deliberately a 64-bit compare.
#define CASE32(REGNAME, INSN, IS_DEC, CARRY, CF_IN, IN)                          \
    do {                                                                         \
        uint64_t res, fl, adc, br;                                               \
        RUN_CASE(REGNAME, INSN " %k0", CARRY, (IN), res, fl, adc, br);           \
        uint32_t w32 = (IS_DEC) ? (uint32_t)(IN) - 1 : (uint32_t)(IN) + 1;       \
        uint64_t want = (uint64_t) w32;   /* upper 32 bits must be zero */       \
        char lbl[160];                                                           \
        snprintf(lbl, sizeof lbl, "%s %s(32) in=%016" PRIx64 " cf=%d result",    \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, res, want);                                               \
        snprintf(lbl, sizeof lbl, "%s %s(32) in=%016" PRIx64 " cf=%d flags",     \
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, fl & FLAG_MASK,                                           \
                  expect_flags((IN), want, (IS_DEC), 32, (CF_IN)));              \
        snprintf(lbl, sizeof lbl, "%s %s(32) in=%016" PRIx64 " cf=%d CF via adc",\
                 INSN, REGNAME, (uint64_t)(IN), (CF_IN));                        \
        check_u64(lbl, adc, (uint64_t)(CF_IN));                                  \
    } while (0)

// Both carry-in states for one (register, insn, input).
#define CASE64_BOTH_CF(REGNAME, INSN, IS_DEC, IN)          \
    do {                                                   \
        CASE64(REGNAME, INSN, IS_DEC, "clc", 0, (IN));     \
        CASE64(REGNAME, INSN, IS_DEC, "stc", 1, (IN));     \
    } while (0)

#define CASE32_BOTH_CF(REGNAME, INSN, IS_DEC, IN)          \
    do {                                                   \
        CASE32(REGNAME, INSN, IS_DEC, "clc", 0, (IN));     \
        CASE32(REGNAME, INSN, IS_DEC, "stc", 1, (IN));     \
    } while (0)

// The inputs that discriminate. Each is run for inc and dec, both carry-ins.
#define SWEEP_INPUTS(REGNAME)                                                    \
    do {                                                                         \
        /* ordinary */                                                           \
        CASE64_BOTH_CF(REGNAME, "incq", 0, 0x0000000000000005ull);               \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x0000000000000005ull);               \
        /* ZF: dec 1 -> 0, and inc of ~0 -> 0 (which must NOT set CF) */         \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x0000000000000001ull);               \
        CASE64_BOTH_CF(REGNAME, "incq", 0, 0xffffffffffffffffull);               \
        /* SF: dec 0 -> -1 (must NOT set CF either) */                           \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x0000000000000000ull);               \
        /* AF: carry out of bit 3 */                                             \
        CASE64_BOTH_CF(REGNAME, "incq", 0, 0x000000000000000full);               \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x0000000000000010ull);               \
        /* OF: the signed boundaries, both directions */                         \
        CASE64_BOTH_CF(REGNAME, "incq", 0, 0x7fffffffffffffffull);               \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x8000000000000000ull);               \
        /* and just off the boundary, where OF must stay clear */                \
        CASE64_BOTH_CF(REGNAME, "incq", 0, 0x7ffffffffffffffeull);               \
        CASE64_BOTH_CF(REGNAME, "decq", 1, 0x8000000000000001ull);               \
        /* 32-bit: upper half must be zeroed, incl of 0xffffffff wraps to 0 */   \
        CASE32_BOTH_CF(REGNAME, "incl", 0, 0xffffffffffffffffull);               \
        CASE32_BOTH_CF(REGNAME, "decl", 1, 0xffffffff00000000ull);               \
        CASE32_BOTH_CF(REGNAME, "incl", 0, 0x000000007fffffffull);               \
        CASE32_BOTH_CF(REGNAME, "decl", 1, 0x0000000080000000ull);               \
        CASE32_BOTH_CF(REGNAME, "incl", 0, 0x00000000deadbeefull);               \
    } while (0)

int main(int argc, char **argv) {
    test_init(argc, argv);

    // ids 0-7: served out of the JIT's host register cache.
    SWEEP_INPUTS("rax");   // 0
    SWEEP_INPUTS("rcx");   // 1
    SWEEP_INPUTS("rdx");   // 2
    SWEEP_INPUTS("rbx");   // 3
    SWEEP_INPUTS("rsi");   // 6
    SWEEP_INPUTS("rdi");   // 7
    // ids 8-15: NOT cached -- a different path through the same gadget, reading
    // and writing CPU_amd64_regs directly. These need REX.B to encode.
    SWEEP_INPUTS("r8");    // 8
    SWEEP_INPUTS("r10");   // 10
    SWEEP_INPUTS("r12");   // 12
    SWEEP_INPUTS("r15");   // 15

    // A tight inc-driven loop: the shape real code takes, and the one that
    // exercises the gadget back-to-back with a deferred rip (nothing in the loop
    // publishes one). Also confirms inc does not disturb the loop's own compare.
    uint64_t n = 0;
    for (int i = 0; i < 1000; i++)
        __asm__ volatile("incq %0" : "+r"(n) : : "cc");
    check_u64("1000x incq accumulates", n, 1000);

    // inc must not clobber a carry the surrounding code is carrying between
    // instructions -- the multi-precision idiom (stc; ...; adc) that a
    // CF-clobbering inc silently corrupts.
    uint64_t lo = 0, carried = 0;
    __asm__ volatile(
        "stc\n\t"
        "incq %0\n\t"       // must preserve the set carry
        "incq %0\n\t"       // ... across more than one of them
        "adcq $0, %1\n\t"
        : "+r"(lo), "+r"(carried) : : "cc");
    check_u64("carry survives a run of incq", carried, 1);
    check_u64("carry-run incq result", lo, 2);

    return finish_suite(suite);
}

#else  /* !__x86_64__ */

int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("%s: SKIP (x86_64 guest only)\n", suite);
    return 0;
}

#endif
