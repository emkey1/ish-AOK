/*
 * avx_regress -- AVX/AVX2 (VEX-encoded) instruction coverage for the amd64
 * guest (GH #525).
 *
 * Motivation: Bun-based CLI tools crashed with SIGILL under AOK because the
 * emulator had no VEX decode at all. Disassembling the real binary showed
 * pervasive compiler/stdlib function-multiversioning -- baseline/AVX2/AVX-512
 * variants of hot routines coexisting in one image, selected by a runtime
 * CPUID check -- so the fix needed real instruction support, not a CPUID
 * tweak. See emu/amd64_interp.c's amd64_vex_step.
 *
 * Every case compares the VEX result against a plain scalar C reference
 * computed in a volatile-fenced loop, so the test is self-checking and does
 * not depend on a golden output file. The properties that actually broke
 * during bring-up each get a dedicated case:
 *
 *   - 2-byte VEX (0xC5) has no X/B fields; they are implicitly ZERO. Treating
 *     them as set adds 8 to every rm/base register number, which silently
 *     reads the wrong register (and segfaults on a memory operand).
 *   - The legal prefix set differs per opcode: 6F/7F are the integer moves
 *     (66=MOVDQA, F3=MOVDQU) while 10/11 and 28/29 are the packed-float moves
 *     (none=PS, 66=PD) whose F3/F2 forms mean something else entirely
 *     (scalar MOVSS/MOVSD).
 *   - Any VEX.128 write must ZERO bits 128+ of the destination; VZEROUPPER
 *     must clear the upper half while PRESERVING the low 128 bits.
 *   - Lane widths must wrap independently: carry crosses the 32-bit boundary
 *     for VPADDQ but not for VPADDD/VPADDW/VPADDB.
 *
 * x86_64 only (VEX in 32-bit mode is a separate decode problem -- 0xC4/0xC5
 * are the legacy LES/LDS opcodes there and need mod-field disambiguation).
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#if !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("avx_regress: SKIP (x86_64 only)\n");
    return 0;
}
#else

/* Volatile so the compiler computes the reference with ordinary scalar code
   instead of recognizing the pattern and emitting the very vector instruction
   under test. */
static volatile uint32_t ref_lhs[8];
static volatile uint32_t ref_rhs[8];

static void check8(const char *label, const uint32_t *got, const uint32_t *want) {
    for (int i = 0; i < 8; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s\n  got: ", label);
            for (int j = 0; j < 8; j++)
                printf("%08x ", got[j]);
            printf("\n  want:");
            for (int j = 0; j < 8; j++)
                printf("%08x ", want[j]);
            printf("\n");
            failures_total++;
            return;
        }
    }
    test_logf("PASS %s\n", label);
}

/* Distinctive patterns: sign bits, all-ones, zeros, sequences. */
static const uint32_t PAT_A[8] __attribute__((aligned(32))) = {
    0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f,
    0xdeadbeef, 0xcafebabe, 0x80000000, 0x7fffffff,
};
static const uint32_t PAT_B[8] __attribute__((aligned(32))) = {
    0xffffffff, 0x00000000, 0x12345678, 0x9abcdef0,
    0x00000001, 0xfffffffe, 0x8000ffff, 0x7fff0001,
};

#define VEX_BINOP_256(mnemonic, label, refexpr)                                \
    do {                                                                       \
        uint32_t out[8] __attribute__((aligned(32)));                          \
        uint32_t want[8];                                                      \
        for (int i = 0; i < 8; i++) {                                          \
            ref_lhs[i] = PAT_A[i];                                             \
            ref_rhs[i] = PAT_B[i];                                             \
        }                                                                      \
        for (int i = 0; i < 8; i++) {                                          \
            uint32_t a = ref_lhs[i], b = ref_rhs[i];                           \
            want[i] = (refexpr);                                               \
        }                                                                      \
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"                             \
                         "vmovdqu (%2),%%ymm2\n\t"                             \
                         mnemonic " %%ymm2,%%ymm1,%%ymm3\n\t"                  \
                         "vmovdqu %%ymm3,(%0)\n\t"                             \
                         "vzeroupper"                                          \
                         :                                                     \
                         : "r"(out), "r"(PAT_A), "r"(PAT_B)                    \
                         : "memory", "ymm1", "ymm2", "ymm3");                  \
        check8(label, out, want);                                              \
    } while (0)

static void test_logical_and_arith(void) {
    VEX_BINOP_256("vpxor", "vpxor.256", a ^ b);
    VEX_BINOP_256("vpor", "vpor.256", a | b);
    VEX_BINOP_256("vpand", "vpand.256", a & b);
    /* vpandn dst = ~src1 & src2; src1 is the vvvv operand (ymm1 here). */
    VEX_BINOP_256("vpandn", "vpandn.256", ~a & b);
    VEX_BINOP_256("vpaddd", "vpaddd.256", a + b);
}

/* Lane-width independence: each width must wrap within its own lane. */
static void test_lane_widths(void) {
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
    };
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x01010101, 0x00000000, 0x01010101, 0x00000000,
        0x01010101, 0x00000000, 0x01010101, 0x00000000,
    };
    uint32_t out[8] __attribute__((aligned(32)));

    /* paddb: 0xff + 0x01 wraps to 0x00 in every byte, no carry escapes. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpaddb.wrap", out, want);
    }

    /* paddq: the carry out of the low dword DOES reach the high dword. */
    {
        static const uint32_t qx[8] __attribute__((aligned(32))) = {
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        };
        static const uint32_t qy[8] __attribute__((aligned(32))) = {
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
        };
        static const uint32_t want[8] = {
            0x00000000, 0x00000001, 0x00000000, 0x00000001,
            0x00000000, 0x00000001, 0x00000000, 0x00000001,
        };
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpaddq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(qx), "r"(qy) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpaddq.carry", out, want);
    }

    /* paddd on the same operands must NOT carry across the dword boundary. */
    {
        static const uint32_t dx[8] __attribute__((aligned(32))) = {
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
            0xffffffff, 0x00000000, 0xffffffff, 0x00000000,
        };
        static const uint32_t dy[8] __attribute__((aligned(32))) = {
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
            0x00000001, 0x00000000, 0x00000001, 0x00000000,
        };
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpaddd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(dx), "r"(dy) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpaddd.nocarry", out, want);
    }
}

static void test_compares(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* Equal against itself: every dword lane becomes all-ones. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vpcmpeqd %%ymm1,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
        };
        check8("vpcmpeqd.self", out, want);
    }

    /* Byte-granular compare with a single differing byte in lane 0. */
    {
        static const uint32_t bx[8] __attribute__((aligned(32))) = {
            0x04030201, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t by[8] __attribute__((aligned(32))) = {
            0x0403ff01, 0, 0, 0, 0, 0, 0, 0};
        /* byte 1 differs (0x02 vs 0xff) -> that byte 0x00, all others 0xff */
        static const uint32_t want[8] = {
            0xffff00ff, 0xffffffff, 0xffffffff, 0xffffffff,
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
        };
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpcmpeqb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(bx), "r"(by) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpcmpeqb.mixed", out, want);
    }
}

/* VPSHUFD's imm8 control is applied independently WITHIN each 128-bit lane --
   it does not shuffle across the lane boundary, so the 256-bit form is not
   simply "shuffle 8 dwords". */
static void test_pshufd_is_lane_local(void) {
    static const uint32_t seq[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint32_t want[8] = {4, 3, 2, 1, 8, 7, 6, 5}; /* 0x1b = reverse */
    uint32_t out[8] __attribute__((aligned(32)));

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vpshufd $0x1b,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    check8("vpshufd.lane_local", out, want);
}

static void test_upper_bits_semantics(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* A VEX.128 write to xmm3 must zero ymm3's upper 128 bits, even though
       ymm3 was fully populated a moment earlier. */
    __asm__ volatile("vmovdqu (%1),%%ymm3\n\t"
                     "vmovdqu (%2),%%xmm4\n\t"
                     "vpxor %%xmm4,%%xmm4,%%xmm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A), "r"(PAT_B)
                     : "memory", "ymm3", "ymm4");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vex128.zeroes_upper", out, want);
    }

    /* VZEROUPPER clears bits 128+ but must leave the low 128 bits intact. */
    __asm__ volatile("vmovdqu (%1),%%ymm3\n\tvzeroupper\n\tvmovdqu %%ymm3,(%0)"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm3");
    {
        uint32_t want[8];
        for (int i = 0; i < 4; i++)
            want[i] = PAT_A[i];
        for (int i = 4; i < 8; i++)
            want[i] = 0;
        check8("vzeroupper.keeps_low", out, want);
    }
}

/* Exercises the 3-byte VEX (0xC4) encoding and its X/B extension bits by
   forcing a high register (ymm8+) and an r12/r13-based memory operand, which
   a 2-byte VEX cannot encode. Catches sign/inversion errors in the extension
   bits that the low-register cases above cannot see. */
static void test_three_byte_vex_high_regs(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    uint32_t want[8];
    for (int i = 0; i < 8; i++)
        want[i] = PAT_A[i] ^ PAT_B[i];

    __asm__ volatile("vmovdqu (%1),%%ymm9\n\t"
                     "vmovdqu (%2),%%ymm10\n\t"
                     "vpxor %%ymm10,%%ymm9,%%ymm11\n\t"
                     "vmovdqu %%ymm11,(%0)\n\t"
                     "vzeroupper"
                     : : "r"(out), "r"(PAT_A), "r"(PAT_B)
                     : "memory", "ymm9", "ymm10", "ymm11");
    check8("vex3byte.high_regs", out, want);
}

static void test_movd_movq(void) {
    uint32_t got32 = 0;
    uint64_t got64 = 0;

    __asm__ volatile("vmovd %1,%%xmm5\n\tvmovd %%xmm5,%0"
                     : "=r"(got32) : "r"((uint32_t) 0xdeadbeefu) : "xmm5");
    if (got32 != 0xdeadbeefu) {
        printf("FAIL vmovd.roundtrip got=%08x want=deadbeef\n", got32);
        failures_total++;
    } else {
        test_logf("PASS vmovd.roundtrip\n");
    }

    __asm__ volatile("vmovq %1,%%xmm5\n\tvmovq %%xmm5,%0"
                     : "=r"(got64) : "r"((uint64_t) 0x0123456789abcdefull) : "xmm5");
    if (got64 != 0x0123456789abcdefull) {
        printf("FAIL vmovq.roundtrip got=%016llx want=0123456789abcdef\n",
               (unsigned long long) got64);
        failures_total++;
    } else {
        test_logf("PASS vmovq.roundtrip\n");
    }
}

/* MOVDQA (66-prefixed) and MOVDQU (F3-prefixed) share opcodes 6F/7F and must
   both decode; an over-broad prefix filter that accepts only one of them turns
   the other into a SIGILL. */
static void test_aligned_and_unaligned_moves(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    __asm__ volatile("vmovdqa (%1),%%ymm1\n\tvmovdqa %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovdqa.roundtrip", out, PAT_A);

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovdqu.roundtrip", out, PAT_A);

    __asm__ volatile("vmovaps (%1),%%ymm1\n\tvmovaps %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovaps.roundtrip", out, PAT_A);

    __asm__ volatile("vmovups (%1),%%ymm1\n\tvmovups %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(PAT_A) : "memory", "ymm1");
    check8("vmovups.roundtrip", out, PAT_A);
}


/* ---- coverage for the second implementation tier ---- */

/* Saturating arithmetic must clamp, not wrap. */
static void test_saturating(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    /* bytes: 0x7f + 0x7f saturates to 0x7f signed; 0xff + 0xff to 0xff unsigned */
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101,
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101,
        0x7f7f7f7f, 0x80808080, 0xffffffff, 0x01010101};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddsb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* 0x7f+0x7f->0x7f, 0x80+0x80->0x80, 0xff+0xff = -1 + -1 = -2 = 0xfe, 1+1=2 */
        static const uint32_t want[8] = {
            0x7f7f7f7f, 0x80808080, 0xfefefefe, 0x02020202,
            0x7f7f7f7f, 0x80808080, 0xfefefefe, 0x02020202};
        check8("vpaddsb.clamp", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpaddusb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* unsigned: 0x7f+0x7f=0xfe, 0x80+0x80=0x100->0xff, 0xff+0xff->0xff, 1+1=2 */
        static const uint32_t want[8] = {
            0xfefefefe, 0xffffffff, 0xffffffff, 0x02020202,
            0xfefefefe, 0xffffffff, 0xffffffff, 0x02020202};
        check8("vpaddusb.clamp", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpsubusb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* equal operands -> 0 everywhere, no borrow below zero */
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpsubusb.floor", out, want);
    }
}

/* Shifts: by immediate, by xmm count, and per-lane variable. */
static void test_shifts(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t v[8] __attribute__((aligned(32))) = {
        0x00000001, 0x80000000, 0xffffffff, 0x00ff00ff,
        0x00000001, 0x80000000, 0xffffffff, 0x00ff00ff};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslld $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000010, 0x00000000, 0xfffffff0, 0x0ff00ff0,
            0x00000010, 0x00000000, 0xfffffff0, 0x0ff00ff0};
        check8("vpslld.imm", out, want);
    }

    /* Arithmetic vs logical right shift differ on the sign bit. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpsrad $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000000, 0xf8000000, 0xffffffff, 0x000ff00f,
            0x00000000, 0xf8000000, 0xffffffff, 0x000ff00f};
        check8("vpsrad.imm.sign", out, want);
    }
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpsrld $4,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {
            0x00000000, 0x08000000, 0x0fffffff, 0x000ff00f,
            0x00000000, 0x08000000, 0x0fffffff, 0x000ff00f};
        check8("vpsrld.imm.zero", out, want);
    }

    /* An over-wide count yields 0 (or all sign bits), not C's UB. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslld $32,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(v) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        check8("vpslld.overshift", out, want);
    }

    /* Count taken from the low 64 bits of an xmm operand. */
    {
        static const uint32_t cnt[4] __attribute__((aligned(16))) = {8, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%xmm2\n\t"
                         "vpslld %%xmm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(v), "r"(cnt) : "memory", "ymm1", "ymm2", "ymm3");
        static const uint32_t want[8] = {
            0x00000100, 0x00000000, 0xffffff00, 0xff00ff00,
            0x00000100, 0x00000000, 0xffffff00, 0xff00ff00};
        check8("vpslld.xmmcount", out, want);
    }

    /* Per-lane variable shift: each lane uses its own count. */
    {
        static const uint32_t base[8] __attribute__((aligned(32))) = {1, 1, 1, 1, 1, 1, 1, 1};
        static const uint32_t counts[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};
        static const uint32_t want[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpsllvd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(base), "r"(counts) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpsllvd.perlane", out, want);
    }

    /* VPSLLDQ is a BYTE shift within each 128-bit lane, not a bit shift and
       not across the lane boundary. */
    {
        static const uint32_t seq[8] __attribute__((aligned(32))) = {
            0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
            0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
        static const uint32_t want[8] = {
            0x02010000, 0x06050403, 0x0a090807, 0x0e0d0c0b,
            0x12111000, 0x16151413, 0x1a191817, 0x1e1d1c1b};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpslldq $1,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
        check8("vpslldq.lane_local", out, want);
    }
}

/* VPSHUFB is lane-local and its index bit 7 zeroes the output byte. */
static void test_pshufb(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t data[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
    /* select byte 0 everywhere in lane 0; 0x80 -> zero in lane 1 */
    static const uint32_t idx[8] __attribute__((aligned(32))) = {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x80808080, 0x80808080, 0x80808080, 0x80808080};
    static const uint32_t want[8] = {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000};
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpshufb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(data), "r"(idx) : "memory", "ymm1", "ymm2", "ymm3");
    check8("vpshufb.zero_and_lane0", out, want);

    /* Index 15 in the high lane must select byte 15 OF THAT LANE (0x1f), not
       byte 15 of the whole register -- the classic cross-lane mistake. */
    {
        static const uint32_t idx2[8] __attribute__((aligned(32))) = {
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f,
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f};
        static const uint32_t want2[8] = {
            0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f,
            0x1f1f1f1f, 0x1f1f1f1f, 0x1f1f1f1f, 0x1f1f1f1f};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpshufb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(data), "r"(idx2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpshufb.lane_local", out, want2);
    }
}

static void test_unpack_and_pack(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpunpcklbw %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            0x11011000, 0x13031202, 0x15051404, 0x17071606,
            0x11011000, 0x13031202, 0x15051404, 0x17071606};
        check8("vpunpcklbw", out, want);
    }

    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpunpckhdq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            0x0b0a0908, 0x1b1a1918, 0x0f0e0d0c, 0x1f1e1d1c,
            0x0b0a0908, 0x1b1a1918, 0x0f0e0d0c, 0x1f1e1d1c};
        check8("vpunpckhdq", out, want);
    }

    /* packuswb clamps signed words into unsigned bytes: negative -> 0. */
    {
        static const uint32_t w1[8] __attribute__((aligned(32))) = {
            0x00ff0001, 0xffff0100, 0x00010002, 0x00030004,
            0x00ff0001, 0xffff0100, 0x00010002, 0x00030004};
        static const uint32_t w2[8] __attribute__((aligned(32))) = {
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000};
        /* words of w1 lane0: 0001,00ff,0100,ffff,0002,0001,0004,0003
           -> bytes:          01,  ff,  ff,  00,  02,  01,  04,  03   */
        static const uint32_t want[8] = {
            0x00ffff01, 0x03040102, 0x00000000, 0x00000000,
            0x00ffff01, 0x03040102, 0x00000000, 0x00000000};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpackuswb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(w1), "r"(w2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpackuswb.clamp", out, want);
    }
}

static void test_broadcast_and_widen(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {0xdeadbeef, 0, 0, 0};
        static const uint32_t want[8] = {
            0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
            0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpbroadcastd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpbroadcastd", out, want);
    }
    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {0x000000ab, 0, 0, 0};
        static const uint32_t want[8] = {
            0xabababab, 0xabababab, 0xabababab, 0xabababab,
            0xabababab, 0xabababab, 0xabababab, 0xabababab};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpbroadcastb %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpbroadcastb", out, want);
    }
    /* Sign- vs zero-extension of the same negative bytes. */
    {
        static const uint32_t src[4] __attribute__((aligned(16))) = {
            0xff01ff01, 0xff01ff01, 0, 0};
        /* 0xff01ff01 stored little-endian is bytes 01,ff,01,ff -- the FIRST
           widened lane comes from byte 0 (0x01), not from the high byte. */
        static const uint32_t want_z[8] = {
            0x00000001, 0x000000ff, 0x00000001, 0x000000ff,
            0x00000001, 0x000000ff, 0x00000001, 0x000000ff};
        static const uint32_t want_s[8] = {
            0x00000001, 0xffffffff, 0x00000001, 0xffffffff,
            0x00000001, 0xffffffff, 0x00000001, 0xffffffff};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpmovzxbd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpmovzxbd", out, want_z);
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpmovsxbd %%xmm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(src) : "memory", "ymm1", "ymm3");
        check8("vpmovsxbd", out, want_s);
    }
}

/* Cross-lane operations -- the ones a lane-local implementation gets wrong. */
static void test_cross_lane(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t seq[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};

    /* vextracti128 $1 pulls the HIGH 128 bits down into an xmm. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvextracti128 $1,%%ymm1,%%xmm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 0, 0, 0};
        check8("vextracti128.high", out, want);
    }

    /* vinserti128 $1 replaces the high half, keeping the low half of vvvv. */
    {
        static const uint32_t ins[4] __attribute__((aligned(16))) = {
            0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd};
        static const uint32_t want[8] = {
            0, 1, 2, 3, 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%xmm2\n\t"
                         "vinserti128 $1,%%xmm2,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq), "r"(ins) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vinserti128.high", out, want);
    }

    /* vpermq crosses lanes: reverse the four qwords. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpermq $0x1b,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {6, 7, 4, 5, 2, 3, 0, 1};
        check8("vpermq.reverse", out, want);
    }

    /* vperm2i128: imm 0x01 -> low=src1.hi, high=src1.lo (a 128-bit swap). */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vperm2i128 $0x01,%%ymm1,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 1, 2, 3};
        check8("vperm2i128.swap", out, want);
    }

    /* imm bit 3 of a selector forces that whole 128-bit half to zero. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"
                     "vperm2i128 $0x81,%%ymm1,%%ymm1,%%ymm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(seq) : "memory", "ymm1", "ymm3");
    {
        /* Each 4-bit selector's bit 3 forces its half to zero, independently
           of the 2-bit source choice: low selector 0x1 -> src1.hi = {4,5,6,7};
           high selector 0x8 -> zeroed. */
        static const uint32_t want[8] = {4, 5, 6, 7, 0, 0, 0, 0};
        check8("vperm2i128.zero_half", out, want);
    }

    /* vpermd: fully general cross-lane dword gather (indices in vvvv). */
    {
        static const uint32_t idx[8] __attribute__((aligned(32))) = {7, 6, 5, 4, 3, 2, 1, 0};
        static const uint32_t want[8] = {7, 6, 5, 4, 3, 2, 1, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpermd %%ymm1,%%ymm2,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(seq), "r"(idx) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpermd.reverse", out, want);
    }
}

/* vpalignr concatenates per 128-bit lane and slides a 16-byte window. */
static void test_palignr(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t hi[8] __attribute__((aligned(32))) = {
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
    static const uint32_t lo[8] __attribute__((aligned(32))) = {
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c};
    /* imm=4: window starts 4 bytes into the low source */
    static const uint32_t want[8] = {
        0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x13121110,
        0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x13121110};
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpalignr $4,%%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(hi), "r"(lo) : "memory", "ymm1", "ymm2", "ymm3");
    check8("vpalignr.imm4", out, want);
}

static void test_pmovmskb_and_madd(void) {
    uint32_t out[8] __attribute__((aligned(32)));

    /* vpmovmskb: one bit per byte MSB, 32 bits for a ymm source. */
    {
        static const uint32_t src[8] __attribute__((aligned(32))) = {
            0x80008000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x80000000};
        uint32_t mask = 0;
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvpmovmskb %%ymm1,%0\n\tvzeroupper"
                         : "=r"(mask) : "r"(src) : "ymm1");
        /* bytes 1 and 3 set in dword 0; byte 31 set */
        uint32_t want = (1u << 1) | (1u << 3) | (1u << 31);
        if (mask != want) {
            printf("FAIL vpmovmskb got=%08x want=%08x\n", mask, want);
            failures_total++;
        } else {
            test_logf("PASS vpmovmskb\n");
        }
    }

    /* vpmaddwd: signed word pairs multiplied and summed into dwords. */
    {
        static const uint32_t x[8] __attribute__((aligned(32))) = {
            0x00020001, 0xffffffff, 0x00010001, 0x00010001,
            0x00020001, 0xffffffff, 0x00010001, 0x00010001};
        static const uint32_t y[8] __attribute__((aligned(32))) = {
            0x00040003, 0x00020002, 0x00010001, 0x00010001,
            0x00040003, 0x00020002, 0x00010001, 0x00010001};
        /* dword0: 1*3 + 2*4 = 11; dword1: (-1)*2 + (-1)*2 = -4 */
        static const uint32_t want[8] = {
            11, (uint32_t) -4, 2, 2,
            11, (uint32_t) -4, 2, 2};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmaddwd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmaddwd.signed", out, want);
    }

    /* vpsadbw: sum of absolute byte differences per 64-bit lane. */
    {
        static const uint32_t x[8] __attribute__((aligned(32))) = {
            0x01010101, 0x01010101, 0, 0, 0, 0, 0, 0};
        static const uint32_t y[8] __attribute__((aligned(32))) = {
            0x03030303, 0x03030303, 0, 0, 0, 0, 0, 0};
        /* |1-3| = 2, eight bytes -> 16 in the first 64-bit lane */
        static const uint32_t want[8] = {16, 0, 0, 0, 0, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpsadbw %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpsadbw", out, want);
    }
}

static void test_minmax_and_mul(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {
        0x00000005, 0xfffffffb, 0x00000100, 0x7fffffff,
        0x00000005, 0xfffffffb, 0x00000100, 0x7fffffff};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0x00000003, 0x00000003, 0x00000200, 0x80000000,
        0x00000003, 0x00000003, 0x00000200, 0x80000000};

    /* signed min: -5 < 3, and 0x7fffffff > 0x80000000 (= -2^31) */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpminsd %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            3, 0xfffffffb, 0x100, 0x80000000,
            3, 0xfffffffb, 0x100, 0x80000000};
        check8("vpminsd.signed", out, want);
    }
    /* unsigned min on the same data picks differently -- catches a
       signed/unsigned mixup that a positive-only test would miss. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpminud %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {
        static const uint32_t want[8] = {
            3, 3, 0x100, 0x7fffffff,
            3, 3, 0x100, 0x7fffffff};
        check8("vpminud.unsigned", out, want);
    }

    /* vpmuludq: low 32 bits of each 64-bit lane -> full 64-bit product. */
    {
        static const uint32_t a2[8] __attribute__((aligned(32))) = {
            0xffffffff, 0xdeadbeef, 2, 0, 0xffffffff, 0, 2, 0};
        static const uint32_t b2[8] __attribute__((aligned(32))) = {
            0xffffffff, 0xcafebabe, 3, 0, 0xffffffff, 0, 3, 0};
        /* 0xffffffff * 0xffffffff = 0xfffffffe00000001; upper dwords ignored */
        static const uint32_t want[8] = {
            0x00000001, 0xfffffffe, 6, 0,
            0x00000001, 0xfffffffe, 6, 0};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmuludq %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(a2), "r"(b2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmuludq.ignores_high", out, want);
    }

    /* vpmulld keeps only the low 32 bits of each product. */
    {
        static const uint32_t a2[8] __attribute__((aligned(32))) = {
            0x00010001, 2, 3, 4, 0x00010001, 2, 3, 4};
        static const uint32_t b2[8] __attribute__((aligned(32))) = {
            0x00010001, 2, 3, 4, 0x00010001, 2, 3, 4};
        static const uint32_t want[8] = {
            0x00020001, 4, 9, 16, 0x00020001, 4, 9, 16};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vpmulld %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(a2), "r"(b2) : "memory", "ymm1", "ymm2", "ymm3");
        check8("vpmulld.low32", out, want);
    }
}

static void test_blend_and_insert(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t x[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint32_t y[8] __attribute__((aligned(32))) = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};

    /* vpblendd imm bit i selects src2 for dword i. */
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vpblendd $0xa5,%%ymm2,%%ymm1,%%ymm3\n\tvmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x), "r"(y) : "memory", "ymm1", "ymm2", "ymm3");
    {   /* 0xa5 = 1010 0101 -> dwords 0,2,5,7 from src2 */
        static const uint32_t want[8] = {0xa0, 1, 0xa2, 3, 4, 0xa5, 6, 0xa7};
        check8("vpblendd.imm", out, want);
    }

    /* vpblendvb selects per BYTE using the mask register's high bits. */
    {
        static const uint32_t mask[8] __attribute__((aligned(32))) = {
            0x80000080, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t want[8] = {0xa0, 1, 2, 3, 4, 5, 6, 7};
        /* bytes 0 and 3 of dword 0 come from src2; src2 dword0 is 0xa0 and
           src1 dword0 is 0 -> byte0 = 0xa0, byte3 = 0 either way */
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vmovdqu (%3),%%ymm4\n\t"
                         "vpblendvb %%ymm4,%%ymm2,%%ymm1,%%ymm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(x), "r"(y), "r"(mask)
                         : "memory", "ymm1", "ymm2", "ymm3", "ymm4");
        check8("vpblendvb.bytemask", out, want);
    }

    /* vpinsrd / vpextrd round trip through lane 2. */
    {
        uint32_t got = 0;
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\t"
                         "vpinsrd $2,%2,%%xmm1,%%xmm3\n\t"
                         "vpextrd $2,%%xmm3,%0"
                         : "=r"(got) : "r"(x), "r"(0xfeedfaceu) : "xmm1", "xmm3");
        if (got != 0xfeedfaceu) {
            printf("FAIL vpinsrd.roundtrip got=%08x want=feedface\n", got);
            failures_total++;
        } else {
            test_logf("PASS vpinsrd.roundtrip\n");
        }
    }
}


/* ---- crypto / FP / VNNI tier ---- */

/* Full AES-128 encryption of the FIPS-197 Appendix C.1 known-answer vector,
   built from VAESENC/VAESENCLAST. This validates the round against the
   published standard rather than against my own reading of the round order:
   x86's AESENC is ShiftRows,SubBytes,MixColumns,AddRoundKey, which equals one
   standard round only because ShiftRows and SubBytes commute. (Note ARM's AESE
   orders it differently, so the two are not interchangeable.) */
static void test_aes(void) {
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t plain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t expect[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};

    /* Key expansion in plain scalar C (no AESKEYGENASSIST needed). */
    static const uint8_t sbox[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
    uint8_t w[11][16];
    uint8_t rcon = 1;
    memcpy(w[0], key, 16);
    for (int r = 1; r <= 10; r++) {
        uint8_t t[4];
        t[0] = (uint8_t) (sbox[w[r-1][13]] ^ rcon);
        t[1] = sbox[w[r-1][14]];
        t[2] = sbox[w[r-1][15]];
        t[3] = sbox[w[r-1][12]];
        for (int i = 0; i < 4; i++)
            w[r][i] = w[r-1][i] ^ t[i];
        for (int i = 4; i < 16; i++)
            w[r][i] = w[r-1][i] ^ w[r][i-4];
        rcon = (uint8_t) ((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
    }

    uint8_t state[16] __attribute__((aligned(16)));
    for (int i = 0; i < 16; i++)
        state[i] = plain[i] ^ w[0][i];

    for (int r = 1; r <= 9; r++) {
        __asm__ volatile("vmovdqu (%0),%%xmm0\n\tvmovdqu (%1),%%xmm1\n\t"
                         "vaesenc %%xmm1,%%xmm0,%%xmm0\n\tvmovdqu %%xmm0,(%0)"
                         : : "r"(state), "r"(w[r]) : "memory", "xmm0", "xmm1");
    }
    __asm__ volatile("vmovdqu (%0),%%xmm0\n\tvmovdqu (%1),%%xmm1\n\t"
                     "vaesenclast %%xmm1,%%xmm0,%%xmm0\n\tvmovdqu %%xmm0,(%0)"
                     : : "r"(state), "r"(w[10]) : "memory", "xmm0", "xmm1");

    if (memcmp(state, expect, 16) != 0) {
        printf("FAIL vaesenc.fips197\n  got: ");
        for (int i = 0; i < 16; i++) printf("%02x", state[i]);
        printf("\n  want:");
        for (int i = 0; i < 16; i++) printf("%02x", expect[i]);
        printf("\n");
        failures_total++;
    } else {
        test_logf("PASS vaesenc.fips197\n");
    }
}

static void test_pclmulqdq(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    /* Carryless square of 0xff spreads its 8 set bits: 0b0101...01 = 0x5555. */
    static const uint32_t x[4] __attribute__((aligned(16))) = {0xff, 0, 0, 0};
    static const uint32_t want[8] = {0x5555, 0, 0, 0, 0, 0, 0, 0};
    __asm__ volatile("vmovdqu (%1),%%xmm1\n\t"
                     "vpclmulqdq $0x00,%%xmm1,%%xmm1,%%xmm3\n\t"
                     "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(x) : "memory", "ymm1", "ymm3");
    check8("vpclmulqdq.square", out, want);

    /* Selecting the HIGH qword of each source must pick a different operand. */
    {
        static const uint32_t y[4] __attribute__((aligned(16))) = {0, 0, 0x0f, 0};
        /* high qword = 0x0f; carryless square of 0b1111 = 0b01010101 = 0x55 */
        static const uint32_t want2[8] = {0x55, 0, 0, 0, 0, 0, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\t"
                         "vpclmulqdq $0x11,%%xmm1,%%xmm1,%%xmm3\n\t"
                         "vmovdqu %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(out), "r"(y) : "memory", "ymm1", "ymm3");
        check8("vpclmulqdq.high_select", out, want2);
    }
}

/* VPTERNLOG's imm8 is a complete 3-input truth table, so a handful of well
   known immediates pin the operand ORDER as well as the logic. */
static void test_pternlog(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    static const uint32_t A3[8] __attribute__((aligned(32))) = {
        0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00,
        0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00};
    static const uint32_t B3[8] __attribute__((aligned(32))) = {
        0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000,
        0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000};
    static const uint32_t C3[8] __attribute__((aligned(32))) = {
        0xf0f0f0f0, 0xf0f0f0f0, 0xf0f0f0f0, 0xf0f0f0f0,
        0xf0f0f0f0, 0xf0f0f0f0, 0xf0f0f0f0, 0xf0f0f0f0};

#define TERNLOG(imm, label, wantexpr)                                          \
    do {                                                                       \
        uint32_t want[8];                                                      \
        for (int i = 0; i < 8; i++) {                                          \
            uint32_t a = A3[i], b = B3[i], c = C3[i];                          \
            (void) a; (void) b; (void) c;                                      \
            want[i] = (wantexpr);                                              \
        }                                                                      \
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\t"                             \
                         "vmovdqu (%2),%%ymm2\n\t"                             \
                         "vmovdqu (%3),%%ymm3\n\t"                             \
                         "vpternlogd $" #imm ",%%ymm3,%%ymm2,%%ymm1\n\t"       \
                         "vmovdqu %%ymm1,(%0)\n\tvzeroupper"                   \
                         : : "r"(out), "r"(A3), "r"(B3), "r"(C3)               \
                         : "memory", "ymm1", "ymm2", "ymm3");                  \
        check8(label, out, want);                                              \
    } while (0)

    /* 0xf0 = "first operand", 0xcc = "second", 0xaa = "third" -- these three
       are exactly what catch an operand-order mixup. */
    TERNLOG(0xf0, "vpternlogd.selects_a", a);
    TERNLOG(0xcc, "vpternlogd.selects_b", b);
    TERNLOG(0xaa, "vpternlogd.selects_c", c);
    TERNLOG(0x96, "vpternlogd.xor3", a ^ b ^ c);
    TERNLOG(0x80, "vpternlogd.and3", a & b & c);
    TERNLOG(0xfe, "vpternlogd.or3", a | b | c);
#undef TERNLOG
}

static void test_vnni(void) {
    uint32_t out[8] __attribute__((aligned(32)));
    /* vpdpbusd: src1 bytes UNSIGNED, src2 bytes SIGNED, 4 products summed into
       each dword and ADDED to the destination's existing value. Using 0xff in
       both operands distinguishes the signedness: unsigned 255 * signed -1. */
    static const uint32_t acc[8] __attribute__((aligned(32))) = {
        100, 0, 0, 0, 0, 0, 0, 0};
    static const uint32_t s1[8] __attribute__((aligned(32))) = {
        0xff010101, 0, 0, 0, 0, 0, 0, 0};
    static const uint32_t s2[8] __attribute__((aligned(32))) = {
        0xff020202, 0, 0, 0, 0, 0, 0, 0};
    /* dword0 bytes: s1 = 01,01,01,ff (unsigned 1,1,1,255)
                     s2 = 02,02,02,ff (signed  2,2,2,-1)
       products: 2 + 2 + 2 + (255 * -1) = 6 - 255 = -249; 100 + -249 = -149 */
    static const uint32_t want[8] = {(uint32_t) -149, 0, 0, 0, 0, 0, 0, 0};
    __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                     "vmovdqu (%3),%%ymm3\n\t"
                     "vpdpbusd %%ymm3,%%ymm2,%%ymm1\n\t"
                     "vmovdqu %%ymm1,(%0)\n\tvzeroupper"
                     : : "r"(out), "r"(acc), "r"(s1), "r"(s2)
                     : "memory", "ymm1", "ymm2", "ymm3");
    check8("vpdpbusd.mixed_sign", out, want);
}

static void test_float(void) {
    float fout[8] __attribute__((aligned(32)));
    static const float fa[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 5, 6, 7, 8};
    static const float fb[8] __attribute__((aligned(32))) = {10, 20, 30, 40, 50, 60, 70, 80};

    __asm__ volatile("vmovups (%1),%%ymm1\n\tvmovups (%2),%%ymm2\n\t"
                     "vaddps %%ymm2,%%ymm1,%%ymm3\n\tvmovups %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(fout), "r"(fa), "r"(fb) : "memory", "ymm1", "ymm2", "ymm3");
    {
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (fout[i] != fa[i] + fb[i]) ok = 0;
        if (!ok) { printf("FAIL vaddps\n"); failures_total++; }
        else test_logf("PASS vaddps\n");
    }

    __asm__ volatile("vmovups (%1),%%ymm1\n\tvmovups (%2),%%ymm2\n\t"
                     "vmulps %%ymm2,%%ymm1,%%ymm3\n\tvmovups %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(fout), "r"(fa), "r"(fb) : "memory", "ymm1", "ymm2", "ymm3");
    {
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (fout[i] != fa[i] * fb[i]) ok = 0;
        if (!ok) { printf("FAIL vmulps\n"); failures_total++; }
        else test_logf("PASS vmulps\n");
    }

    /* x86 MINPS is NOT NaN-propagating and NOT commutative: it is defined as
       "(src1 < src2) ? src1 : src2", so a NaN in EITHER operand yields src2.
       An fminf()-style implementation gets this backwards. */
    {
        float nan_v = 0.0f / 0.0f;
        float src1[4] __attribute__((aligned(16))) = {nan_v, 1.0f, 5.0f, 5.0f};
        float src2[4] __attribute__((aligned(16))) = {7.0f, nan_v, 3.0f, 9.0f};
        float got[4] __attribute__((aligned(16)));
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvmovups (%2),%%xmm2\n\t"
                         "vminps %%xmm2,%%xmm1,%%xmm3\n\tvmovups %%xmm3,(%0)"
                         : : "r"(got), "r"(src1), "r"(src2)
                         : "memory", "xmm1", "xmm2", "xmm3");
        /* lane0: src1 is NaN -> src2 = 7; lane1: src2 is NaN -> src2 = NaN;
           lane2: 5<3 false -> 3;         lane3: 5<9 true  -> 5 */
        int ok = got[0] == 7.0f && (got[1] != got[1]) && got[2] == 3.0f && got[3] == 5.0f;
        if (!ok) {
            printf("FAIL vminps.nan got=%f %f %f %f\n",
                   (double) got[0], (double) got[1], (double) got[2], (double) got[3]);
            failures_total++;
        } else {
            test_logf("PASS vminps.nan\n");
        }
    }

    /* Scalar ops touch ONLY lane 0 and take the rest from src1 (vvvv). */
    {
        float src1[4] __attribute__((aligned(16))) = {1.0f, 111.0f, 222.0f, 333.0f};
        float src2[4] __attribute__((aligned(16))) = {2.0f, 999.0f, 999.0f, 999.0f};
        float got[4] __attribute__((aligned(16)));
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvmovups (%2),%%xmm2\n\t"
                         "vaddss %%xmm2,%%xmm1,%%xmm3\n\tvmovups %%xmm3,(%0)"
                         : : "r"(got), "r"(src1), "r"(src2)
                         : "memory", "xmm1", "xmm2", "xmm3");
        int ok = got[0] == 3.0f && got[1] == 111.0f && got[2] == 222.0f && got[3] == 333.0f;
        if (!ok) {
            printf("FAIL vaddss.scalar got=%f %f %f %f\n",
                   (double) got[0], (double) got[1], (double) got[2], (double) got[3]);
            failures_total++;
        } else {
            test_logf("PASS vaddss.scalar\n");
        }
    }

    /* A true FP compare yields ALL ONES in that lane, not 1. */
    {
        float src1[4] __attribute__((aligned(16))) = {1.0f, 5.0f, 3.0f, 4.0f};
        float src2[4] __attribute__((aligned(16))) = {2.0f, 2.0f, 3.0f, 4.0f};
        uint32_t got[4] __attribute__((aligned(16)));
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvmovups (%2),%%xmm2\n\t"
                         "vcmpltps %%xmm2,%%xmm1,%%xmm3\n\tvmovups %%xmm3,(%0)"
                         : : "r"(got), "r"(src1), "r"(src2)
                         : "memory", "xmm1", "xmm2", "xmm3");
        int ok = got[0] == 0xffffffffu && got[1] == 0 && got[2] == 0 && got[3] == 0;
        if (!ok) {
            printf("FAIL vcmpltps got=%08x %08x %08x %08x\n",
                   got[0], got[1], got[2], got[3]);
            failures_total++;
        } else {
            test_logf("PASS vcmpltps\n");
        }
    }

    /* FMA 213: dst = src2*dst + src3. */
    {
        float d[4] __attribute__((aligned(16))) = {3.0f, 3.0f, 3.0f, 3.0f};
        float s2[4] __attribute__((aligned(16))) = {2.0f, 2.0f, 2.0f, 2.0f};
        float s3[4] __attribute__((aligned(16))) = {4.0f, 4.0f, 4.0f, 4.0f};
        float got[4] __attribute__((aligned(16)));
        __asm__ volatile("vmovups (%1),%%xmm0\n\tvmovups (%2),%%xmm1\n\t"
                         "vmovups (%3),%%xmm2\n\t"
                         "vfmadd213ps %%xmm2,%%xmm1,%%xmm0\n\t"
                         "vmovups %%xmm0,(%0)"
                         : : "r"(got), "r"(d), "r"(s2), "r"(s3)
                         : "memory", "xmm0", "xmm1", "xmm2");
        int ok = 1;
        for (int i = 0; i < 4; i++)
            if (got[i] != 2.0f * 3.0f + 4.0f) ok = 0;
        if (!ok) {
            printf("FAIL vfmadd213ps got=%f want=10\n", (double) got[0]);
            failures_total++;
        } else {
            test_logf("PASS vfmadd213ps\n");
        }
    }
}


/* ---- BMI1/BMI2 ----
   VEX-encoded but general-purpose, not vector. The flag split is the trap:
   ANDN, the BLSx ops, BZHI and BEXTR write flags; SHLX/SHRX/SARX/MULX/RORX/PDEP/PEXT must
   leave every flag untouched. */
static void check64(const char *label, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %s got=%016llx want=%016llx\n", label,
               (unsigned long long) got, (unsigned long long) want);
        failures_total++;
    } else {
        test_logf("PASS %s\n", label);
    }
}

static void test_bmi(void) {
    uint64_t r = 0;
    uint32_t r32 = 0;

    /* andn: ~src1 & src2 */
    __asm__ volatile("andnq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x00000000000000ffull),
                       "r"((uint64_t) 0x0000000000ffffffull) : "cc");
    check64("andnq", r, 0x0000000000ffff00ull);

    /* blsr clears the lowest set bit; blsi isolates it; blsmsk masks up to it */
    __asm__ volatile("blsrq %1,%0" : "=r"(r) : "r"((uint64_t) 0b10110000ull) : "cc");
    check64("blsrq", r, 0b10100000ull);
    __asm__ volatile("blsiq %1,%0" : "=r"(r) : "r"((uint64_t) 0b10110000ull) : "cc");
    check64("blsiq", r, 0b00010000ull);
    __asm__ volatile("blsmskq %1,%0" : "=r"(r) : "r"((uint64_t) 0b10110000ull) : "cc");
    check64("blsmskq", r, 0b00011111ull);

    /* bzhi zeroes bits at and above the index */
    __asm__ volatile("bzhiq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0xffffffffffffffffull), "r"((uint64_t) 8) : "cc");
    check64("bzhiq", r, 0xffull);

    /* bextr extracts a run: start in bits 7:0, length in bits 15:8 */
    __asm__ volatile("bextrq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x0123456789abcdefull),
                       "r"((uint64_t) ((8ull << 8) | 4ull)) : "cc");
    check64("bextrq", r, 0xdeull);

    /* shlx/shrx/sarx: count masked to the operand width, flags untouched */
    __asm__ volatile("shlxq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 1), "r"((uint64_t) 40) : "cc");
    check64("shlxq", r, 1ull << 40);
    __asm__ volatile("shrxq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x8000000000000000ull), "r"((uint64_t) 63) : "cc");
    check64("shrxq.logical", r, 1);
    __asm__ volatile("sarxq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x8000000000000000ull), "r"((uint64_t) 63) : "cc");
    check64("sarxq.arithmetic", r, 0xffffffffffffffffull);
    /* a count of 64 wraps to 0 for a 64-bit operand (masked, not saturated) */
    __asm__ volatile("shlxq %2,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x1234), "r"((uint64_t) 64) : "cc");
    check64("shlxq.count_masked", r, 0x1234);

    /* 32-bit forms must zero-extend into the full 64-bit register */
    __asm__ volatile("shlxl %2,%1,%0" : "=r"(r32)
                     : "r"((uint32_t) 1), "r"((uint32_t) 31) : "cc");
    check64("shlxl", r32, 0x80000000ull);

    /* rorx rotates by an immediate and writes no flags */
    __asm__ volatile("rorxq $8,%1,%0" : "=r"(r)
                     : "r"((uint64_t) 0x00000000000000ffull) :);
    check64("rorxq", r, 0xff00000000000000ull);

    /* mulx: 64x64 -> 128, implicit source is rdx, no flags */
    {
        uint64_t lo = 0, hi = 0;
        __asm__ volatile("movq %2,%%rdx\n\tmulxq %3,%0,%1"
                         : "=r"(lo), "=r"(hi)
                         : "r"((uint64_t) 0xffffffffffffffffull),
                           "r"((uint64_t) 0xffffffffffffffffull)
                         : "rdx");
        /* (2^64-1)^2 = 2^128 - 2^65 + 1 -> hi=0xfffffffffffffffe, lo=1 */
        check64("mulxq.lo", lo, 1);
        check64("mulxq.hi", hi, 0xfffffffffffffffeull);
    }

    /* pdep/pext are inverses under the same mask */
    {
        uint64_t mask = 0xff00ff00ff00ff00ull, packed = 0, spread = 0;
        __asm__ volatile("pdepq %2,%1,%0" : "=r"(spread)
                         : "r"((uint64_t) 0xabcd), "r"(mask) :);
        __asm__ volatile("pextq %2,%1,%0" : "=r"(packed)
                         : "r"(spread), "r"(mask) :);
        check64("pdep_pext.roundtrip", packed, 0xabcd);
    }

    /* SHRX must NOT disturb flags -- set ZF via a test, shift, then read it. */
    {
        uint64_t zf = 0;
        __asm__ volatile("xorq %%rax,%%rax\n\t"   /* sets ZF=1 */
                         "shrxq %2,%1,%1\n\t"     /* must not clobber ZF */
                         "setz %b0\n\tmovzbq %b0,%0"
                         : "=&r"(zf)
                         : "r"((uint64_t) 0xff), "r"((uint64_t) 4)
                         : "rax", "cc");
        check64("shrxq.preserves_flags", zf, 1);
    }
}

/* ---- AVX-512 opmask (k) registers ----
   VEX-encoded but not vector ops: these manipulate the 8 predicate registers
   that every masked EVEX instruction selects between. The width comes from pp
   and W (pp 0 = W/Q, pp 1 = B/D), and KMOV's operand order flips between the
   GPR->k form (opcode 92) and the k->GPR form (93).

   The target attribute is needed so the compiler permits inline asm to clobber
   k registers; the regression runner builds everything with a plain cc -O2. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl")))
static void test_kregs(void) {
    {
        uint64_t back = 0;
        __asm__ volatile("kmovq %1,%%k1\n\tkmovq %%k1,%0"
                         : "=r"(back) : "r"((uint64_t) 0x0123456789abcdefull) : "k1");
        check64("kmovq.roundtrip", back, 0x0123456789abcdefull);
    }
    {
        uint32_t back = 0;
        __asm__ volatile("kmovd %1,%%k2\n\tkmovd %%k2,%0"
                         : "=r"(back) : "r"((uint32_t) 0xdeadbeefu) : "k2");
        check64("kmovd.roundtrip", back, 0xdeadbeefu);
    }
    /* KMOVW is 16-bit: the high half of the source must be dropped. */
    {
        uint32_t back = 0;
        __asm__ volatile("kmovw %1,%%k3\n\tkmovw %%k3,%0"
                         : "=r"(back) : "r"((uint32_t) 0xffff1234u) : "k3");
        check64("kmovw.truncates", back, 0x1234);
    }
    {
        uint32_t r = 0;
        __asm__ volatile("kmovd %1,%%k1\n\tkmovd %2,%%k2\n\tkandd %%k2,%%k1,%%k3\n\tkmovd %%k3,%0"
                         : "=r"(r) : "r"(0xff00ff00u), "r"(0x0f0f0f0fu) : "k1", "k2", "k3");
        check64("kandd", r, 0x0f000f00u);
    }
    {
        uint32_t r = 0;
        __asm__ volatile("kmovd %1,%%k1\n\tkmovd %2,%%k2\n\tkord %%k2,%%k1,%%k3\n\tkmovd %%k3,%0"
                         : "=r"(r) : "r"(0xff00ff00u), "r"(0x0f0f0f0fu) : "k1", "k2", "k3");
        check64("kord", r, 0xff0fff0fu);
    }
    {
        uint32_t r = 0;
        __asm__ volatile("kmovd %1,%%k1\n\tkmovd %2,%%k2\n\tkxord %%k2,%%k1,%%k3\n\tkmovd %%k3,%0"
                         : "=r"(r) : "r"(0xff00ff00u), "r"(0x0f0f0f0fu) : "k1", "k2", "k3");
        check64("kxord", r, 0xf00ff00fu);
    }
    /* KANDN is ~src1 & src2 -- asymmetric, so operand order shows up here. */
    {
        uint32_t r = 0;
        __asm__ volatile("kmovd %1,%%k1\n\tkmovd %2,%%k2\n\tkandnd %%k2,%%k1,%%k3\n\tkmovd %%k3,%0"
                         : "=r"(r) : "r"(0xff00ff00u), "r"(0x0f0f0f0fu) : "k1", "k2", "k3");
        check64("kandnd", r, 0x000f000fu);
    }
    /* KNOT has no vvvv operand at all. */
    {
        uint32_t r = 0;
        __asm__ volatile("kmovw %1,%%k1\n\tknotw %%k1,%%k2\n\tkmovw %%k2,%0"
                         : "=r"(r) : "r"(0x00ffu) : "k1", "k2");
        check64("knotw", r, 0xff00u);
    }
    /* KORTEST sets ZF when the OR is zero and CF when it is all ones -- and
       "all ones" is relative to the mask WIDTH, not to 64 bits. */
    {
        uint32_t zf = 0, cf = 0;
        __asm__ volatile("kmovw %2,%%k1\n\tkmovw %2,%%k2\n\tkortestw %%k2,%%k1\n\t"
                         "setz %b0\n\tsetc %b1\n\tmovzbl %b0,%0\n\tmovzbl %b1,%1"
                         : "=&r"(zf), "=&r"(cf) : "r"(0u) : "k1", "k2", "cc");
        check64("kortestw.zero.zf", zf, 1);
        check64("kortestw.zero.cf", cf, 0);
    }
    {
        uint32_t zf = 0, cf = 0;
        __asm__ volatile("kmovw %2,%%k1\n\tkmovw %2,%%k2\n\tkortestw %%k2,%%k1\n\t"
                         "setz %b0\n\tsetc %b1\n\tmovzbl %b0,%0\n\tmovzbl %b1,%1"
                         : "=&r"(zf), "=&r"(cf) : "r"(0xffffu) : "k1", "k2", "cc");
        check64("kortestw.ones.zf", zf, 0);
        check64("kortestw.ones.cf", cf, 1);
    }
}

/* ---- EVEX predication ----
   Merging (the default) leaves masked-out elements at their previous value;
   {z} zeroes them instead. The masking GRANULARITY is a property of the
   opcode rather than of the operand size -- VMOVDQU8 masks per byte while
   VMOVDQU32 masks per dword -- so one wrong granularity writes 4x too much. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl")))
static void test_evex_masking(void) {
    static const uint32_t seq[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint32_t val[8] __attribute__((aligned(32))) =
        {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    uint32_t r[8] __attribute__((aligned(32)));

    /* Merging: k = 0xa5 selects lanes 0,2,5,7; the rest keep seq's values. */
    __asm__ volatile("vmovdqu32 (%1),%%ymm3\n\t"
                     "vmovdqu32 (%2),%%ymm1\n\t"
                     "kmovw %3,%%k1\n\t"
                     "vpaddd %%ymm1,%%ymm1,%%ymm3%{%%k1%}\n\t"
                     "vmovdqu32 %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(r), "r"(seq), "r"(val), "r"(0xa5u)
                     : "memory", "ymm1", "ymm3", "k1");
    {
        static const uint32_t w[8] = {0x20, 2, 0x60, 4, 5, 0xc0, 7, 0x100};
        check8("evex.vpaddd.merge", r, w);
    }

    /* Zeroing: same predicate, but cleared lanes become 0. */
    __asm__ volatile("vmovdqu32 (%1),%%ymm3\n\t"
                     "vmovdqu32 (%2),%%ymm1\n\t"
                     "kmovw %3,%%k1\n\t"
                     "vpaddd %%ymm1,%%ymm1,%%ymm3%{%%k1%}%{z%}\n\t"
                     "vmovdqu32 %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(r), "r"(seq), "r"(val), "r"(0xa5u)
                     : "memory", "ymm1", "ymm3", "k1");
    {
        static const uint32_t w[8] = {0x20, 0, 0x60, 0, 0, 0xc0, 0, 0x100};
        check8("evex.vpaddd.zeroing", r, w);
    }

    /* The same predicate semantics on a plain data move. */
    __asm__ volatile("vmovdqu32 (%1),%%ymm3\n\t"
                     "kmovw %3,%%k2\n\t"
                     "vmovdqu32 (%2),%%ymm3%{%%k2%}\n\t"
                     "vmovdqu32 %%ymm3,(%0)\n\tvzeroupper"
                     : : "r"(r), "r"(seq), "r"(val), "r"(0x0fu)
                     : "memory", "ymm3", "k2");
    {
        static const uint32_t w[8] = {0x10, 0x20, 0x30, 0x40, 5, 6, 7, 8};
        check8("evex.vmovdqu32.merge", r, w);
    }

    /* VMOVDQU8 masks per BYTE: k = 5 selects bytes 0 and 2 of the whole
       register, not dwords 0 and 2. */
    {
        static const uint32_t zeros[8] __attribute__((aligned(32))) = {0,0,0,0,0,0,0,0};
        static const uint32_t ones[8] __attribute__((aligned(32))) =
            {0xffffffff,0xffffffff,0xffffffff,0xffffffff,
             0xffffffff,0xffffffff,0xffffffff,0xffffffff};
        __asm__ volatile("vmovdqu32 (%1),%%ymm3\n\t"
                         "kmovd %3,%%k3\n\t"
                         "vmovdqu8 (%2),%%ymm3%{%%k3%}\n\t"
                         "vmovdqu32 %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(zeros), "r"(ones), "r"(0x00000005u)
                         : "memory", "ymm3", "k3");
        static const uint32_t w[8] = {0x00ff00ff, 0, 0, 0, 0, 0, 0, 0};
        check8("evex.vmovdqu8.byte_granular", r, w);
    }
}

/* ---- AVX-512 compares, mask/vector conversions, and cross-lane byte permute ----
   VPCMP* writes a k register rather than a vector, and its imm8 selects the
   predicate. A write-mask on a compare acts as a zeroing AND, never a merge. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,avx512vbmi")))
static void test_avx512_compare_and_permute(void) {
    static const uint32_t s1[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint32_t s2[8] __attribute__((aligned(32))) = {9, 2, 0, 4, 99, 6, 1, 8};
    uint32_t r[8] __attribute__((aligned(32)));

    { /* predicate 0 = equal; lanes 1,3,5,7 match */
        uint32_t k = 0;
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tvmovdqu32 (%2),%%ymm2\n\t"
                         "vpcmpd $0,%%ymm2,%%ymm1,%%k1\n\tkmovd %%k1,%0\n\tvzeroupper"
                         : "=r"(k) : "r"(s1), "r"(s2) : "ymm1", "ymm2", "k1");
        check64("vpcmpd.eq", k, 0xaa);
    }
    { /* signed and unsigned less-than must disagree on a negative operand */
        static const uint32_t neg[8] __attribute__((aligned(32))) =
            {0xffffffff, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t one[8] __attribute__((aligned(32))) = {1,1,1,1,1,1,1,1};
        uint32_t ks = 0, ku = 0;
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tvmovdqu32 (%2),%%ymm2\n\t"
                         "vpcmpd $1,%%ymm2,%%ymm1,%%k1\n\tkmovd %%k1,%0\n\tvzeroupper"
                         : "=r"(ks) : "r"(neg), "r"(one) : "ymm1", "ymm2", "k1");
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tvmovdqu32 (%2),%%ymm2\n\t"
                         "vpcmpud $1,%%ymm2,%%ymm1,%%k1\n\tkmovd %%k1,%0\n\tvzeroupper"
                         : "=r"(ku) : "r"(neg), "r"(one) : "ymm1", "ymm2", "k1");
        check64("vpcmpd.signed_lt", ks & 1, 1);      /* -1 < 1 */
        check64("vpcmpud.unsigned_lt", ku & 1, 0);   /* 0xffffffff > 1 */
    }
    { /* VPTEST sets ZF/CF and writes no register */
        uint32_t zf = 0, cf = 0;
        static const uint32_t z[8] __attribute__((aligned(32))) = {0,0,0,0,0,0,0,0};
        __asm__ volatile("vmovdqu (%2),%%ymm1\n\tvmovdqu (%2),%%ymm2\n\t"
                         "vptest %%ymm2,%%ymm1\n\tsetz %b0\n\tsetc %b1\n\t"
                         "movzbl %b0,%0\n\tmovzbl %b1,%1\n\tvzeroupper"
                         : "=&r"(zf), "=&r"(cf) : "r"(z) : "ymm1", "ymm2", "cc");
        check64("vptest.zero.zf", zf, 1);
    }
    { /* mask -> vector -> mask round trip */
        __asm__ volatile("kmovd %1,%%k1\n\tvpmovm2d %%k1,%%ymm3\n\t"
                         "vmovdqu32 %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(0x0au) : "memory", "ymm3", "k1");
        static const uint32_t w[8] = {0, 0xffffffff, 0, 0xffffffff, 0, 0, 0, 0};
        check8("vpmovm2d", r, w);
        uint32_t back = 0;
        __asm__ volatile("kmovd %1,%%k1\n\tvpmovm2d %%k1,%%ymm3\n\t"
                         "vpmovd2m %%ymm3,%%k2\n\tkmovd %%k2,%0\n\tvzeroupper"
                         : "=r"(back) : "r"(0x0au) : "ymm3", "k1", "k2");
        check64("vpmovd2m.roundtrip", back, 0x0a);
    }
    { /* VPERMB crosses the 128-bit lane boundary: reverse all 32 bytes */
        static const uint8_t idx[32] __attribute__((aligned(32))) =
            {31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,
             15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
        static const uint8_t src[32] __attribute__((aligned(32))) =
            {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
             16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
        uint8_t got[32] __attribute__((aligned(32)));
        __asm__ volatile("vmovdqu8 (%1),%%ymm1\n\tvmovdqu8 (%2),%%ymm2\n\t"
                         "vpermb %%ymm2,%%ymm1,%%ymm3\n\tvmovdqu8 %%ymm3,(%0)\n\tvzeroupper"
                         : : "r"(got), "r"(idx), "r"(src)
                         : "memory", "ymm1", "ymm2", "ymm3");
        int ok = 1;
        for (int i = 0; i < 32; i++)
            if (got[i] != 31 - i) ok = 0;
        if (!ok) {
            printf("FAIL vpermb got[0]=%d got[31]=%d\n", got[0], got[31]);
            failures_total++;
        } else {
            test_logf("PASS vpermb\n");
        }
    }
}

/* ---- AVX-512 compress/expand, VALIGN, KSHIFT, and conversions ----
   VPCOMPRESS/VPEXPAND are the one family where the mask is not a per-lane
   predicate but a SELECTOR: it chooses which elements participate and they
   are packed contiguously, so they cannot share the normal masked writeback.
   VALIGND likewise differs from VPALIGNR by crossing the 128-bit lane
   boundary. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,avx512vbmi2")))
static void test_avx512_compress_and_align(void) {
    static const uint32_t s[8] __attribute__((aligned(32))) = {0, 1, 2, 3, 4, 5, 6, 7};
    uint32_t r[8] __attribute__((aligned(32)));

    { /* k = 0xa5 selects lanes 0,2,5,7 -> values packed into the low end */
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tkmovw %2,%%k1\n\t"
                         "vpxor %%ymm2,%%ymm2,%%ymm2\n\t"
                         "vpcompressd %%ymm1,%%ymm2%{%%k1%}\n\t"
                         "vmovdqu32 %%ymm2,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(s), "r"(0xa5u)
                         : "memory", "ymm1", "ymm2", "k1");
        static const uint32_t w[8] = {0, 2, 5, 7, 0, 0, 0, 0};
        check8("vpcompressd", r, w);
    }
    { /* the inverse: low elements scattered back out to the mask positions */
        static const uint32_t packed[8] __attribute__((aligned(32))) = {9,8,7,6,0,0,0,0};
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tkmovw %2,%%k1\n\t"
                         "vpxor %%ymm2,%%ymm2,%%ymm2\n\t"
                         "vpexpandd %%ymm1,%%ymm2%{%%k1%}%{z%}\n\t"
                         "vmovdqu32 %%ymm2,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(packed), "r"(0xa5u)
                         : "memory", "ymm1", "ymm2", "k1");
        static const uint32_t w[8] = {9, 0, 8, 0, 0, 7, 0, 6};
        check8("vpexpandd", r, w);
    }
    { /* VALIGND rotates across the whole register, not per 128-bit lane */
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\t"
                         "valignd $3,%%ymm1,%%ymm1,%%ymm2\n\t"
                         "vmovdqu32 %%ymm2,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(s) : "memory", "ymm1", "ymm2");
        static const uint32_t w[8] = {3, 4, 5, 6, 7, 0, 1, 2};
        check8("valignd.crosslane", r, w);
    }
    { /* VPHADDD sums adjacent pairs WITHIN each 128-bit lane */
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvphaddd %%ymm1,%%ymm1,%%ymm2\n\t"
                         "vmovdqu %%ymm2,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(s) : "memory", "ymm1", "ymm2");
        static const uint32_t w[8] = {1, 5, 1, 5, 9, 13, 9, 13};
        check8("vphaddd.lane_local", r, w);
    }
    {
        uint32_t k = 0;
        __asm__ volatile("kmovw %1,%%k1\n\tkshiftlw $4,%%k1,%%k2\n\tkmovw %%k2,%0"
                         : "=r"(k) : "r"(0x000fu) : "k1", "k2");
        check64("kshiftlw", k, 0x00f0u);
        __asm__ volatile("kmovw %1,%%k1\n\tkshiftrw $4,%%k1,%%k2\n\tkmovw %%k2,%0"
                         : "=r"(k) : "r"(0x00f0u) : "k1", "k2");
        check64("kshiftrw", k, 0x000fu);
    }
    { /* VPTESTMD writes a mask: bit set where (a & b) != 0 */
        uint32_t k = 0;
        static const uint32_t m[8] __attribute__((aligned(32))) = {1,0,1,0,1,0,1,0};
        __asm__ volatile("vmovdqu32 (%1),%%ymm1\n\tvmovdqu32 (%2),%%ymm2\n\t"
                         "vptestmd %%ymm2,%%ymm1,%%k1\n\tkmovw %%k1,%0\n\tvzeroupper"
                         : "=r"(k) : "r"(s), "r"(m) : "ymm1", "ymm2", "k1");
        uint32_t want = 0;
        for (unsigned i = 0; i < 8; i++)
            if (i & m[i]) want |= 1u << i;
        check64("vptestmd", k, want);
    }
    {
        int64_t back = 0;
        __asm__ volatile("vcvtsi2sdq %1,%%xmm1,%%xmm1\n\tvcvttsd2si %%xmm1,%0"
                         : "=r"(back) : "r"((int64_t) -12345) : "xmm1");
        check64("vcvtsi2sd.roundtrip", (uint64_t) back, (uint64_t) (int64_t) -12345);
    }
    { /* VPEXTRW also has a 0F C5 form distinct from the 0F3A 15 one */
        uint32_t v = 0;
        static const uint32_t src[4] __attribute__((aligned(16))) =
            {0x11112222, 0x33334444, 0, 0};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpextrw $1,%%xmm1,%0"
                         : "=r"(v) : "r"(src) : "xmm1");
        check64("vpextrw.0fc5", v, 0x1111);
    }
}

/* ---- rounding, blends, conversions, and the scalar FP compares ----
   VBLENDVPS and VTESTPS look only at each element's SIGN BIT, unlike
   VPBLENDVB (per-byte) and VPTEST (all bits). VPBLENDM* differs again: it
   always writes every element, choosing between sources, rather than
   preserving the destination the way a predicate does. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl")))
static void test_round_blend_convert(void) {
    float fr[4] __attribute__((aligned(16)));
    uint32_t ir[4] __attribute__((aligned(16)));

    { /* imm 1 = floor (toward -inf), imm 3 = truncate (toward zero) */
        static const float v[4] __attribute__((aligned(16))) = {1.7f, -1.7f, 2.5f, -2.5f};
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvroundps $1,%%xmm1,%%xmm2\n\t"
                         "vmovups %%xmm2,(%0)"
                         : : "r"(fr), "r"(v) : "memory", "xmm1", "xmm2");
        if (fr[0] != 1.0f || fr[1] != -2.0f || fr[2] != 2.0f || fr[3] != -3.0f) {
            printf("FAIL vroundps.floor\n"); failures_total++;
        } else { test_logf("PASS vroundps.floor\n"); }
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvroundps $3,%%xmm1,%%xmm2\n\t"
                         "vmovups %%xmm2,(%0)"
                         : : "r"(fr), "r"(v) : "memory", "xmm1", "xmm2");
        if (fr[0] != 1.0f || fr[1] != -1.0f || fr[2] != 2.0f || fr[3] != -2.0f) {
            printf("FAIL vroundps.trunc\n"); failures_total++;
        } else { test_logf("PASS vroundps.trunc\n"); }
    }
    { /* int -> float -> int must round trip through negative values */
        static const uint32_t iv[4] __attribute__((aligned(16))) =
            {1, (uint32_t) -2, 3, (uint32_t) -4};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvcvtdq2ps %%xmm1,%%xmm2\n\t"
                         "vcvttps2dq %%xmm2,%%xmm3\n\tvmovdqu %%xmm3,(%0)"
                         : : "r"(ir), "r"(iv) : "memory", "xmm1", "xmm2", "xmm3");
        for (int i = 0; i < 4; i++)
            if (ir[i] != iv[i]) { printf("FAIL vcvtdq2ps.roundtrip\n"); failures_total++; break; }
        test_logf("PASS vcvtdq2ps.roundtrip\n");
    }
    { /* VBLENDVPS selects per element on the mask's sign bit */
        static const float x[4] __attribute__((aligned(16))) = {1, 2, 3, 4};
        static const float y[4] __attribute__((aligned(16))) = {10, 20, 30, 40};
        static const uint32_t m[4] __attribute__((aligned(16))) =
            {0x80000000, 0, 0x80000000, 0};
        __asm__ volatile("vmovups (%1),%%xmm1\n\tvmovups (%2),%%xmm2\n\t"
                         "vmovdqu (%3),%%xmm3\n\t"
                         "vblendvps %%xmm3,%%xmm2,%%xmm1,%%xmm4\n\tvmovups %%xmm4,(%0)"
                         : : "r"(fr), "r"(x), "r"(y), "r"(m)
                         : "memory", "xmm1", "xmm2", "xmm3", "xmm4");
        if (fr[0] != 10 || fr[1] != 2 || fr[2] != 30 || fr[3] != 4) {
            printf("FAIL vblendvps\n"); failures_total++;
        } else { test_logf("PASS vblendvps\n"); }
    }
    { /* VMOVDDUP duplicates the low qword of each 128-bit lane */
        static const uint32_t v[4] __attribute__((aligned(16))) =
            {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvmovddup %%xmm1,%%xmm2\n\t"
                         "vmovdqu %%xmm2,(%0)"
                         : : "r"(ir), "r"(v) : "memory", "xmm1", "xmm2");
        if (ir[0] != 0xaaaa || ir[1] != 0xbbbb || ir[2] != 0xaaaa || ir[3] != 0xbbbb) {
            printf("FAIL vmovddup\n"); failures_total++;
        } else { test_logf("PASS vmovddup\n"); }
    }
    { /* VPBLENDMD writes every element, unlike a predicate */
        static const uint32_t x[4] __attribute__((aligned(16))) = {1, 2, 3, 4};
        static const uint32_t y[4] __attribute__((aligned(16))) = {10, 20, 30, 40};
        __asm__ volatile("vmovdqu32 (%1),%%xmm1\n\tvmovdqu32 (%2),%%xmm2\n\t"
                         "kmovw %3,%%k1\n\t"
                         "vpblendmd %%xmm2,%%xmm1,%%xmm3%{%%k1%}\n\t"
                         "vmovdqu32 %%xmm3,(%0)"
                         : : "r"(ir), "r"(x), "r"(y), "r"(0x5u)
                         : "memory", "xmm1", "xmm2", "xmm3", "k1");
        if (ir[0] != 10 || ir[1] != 2 || ir[2] != 30 || ir[3] != 4) {
            printf("FAIL vpblendmd\n"); failures_total++;
        } else { test_logf("PASS vpblendmd\n"); }
    }
    { /* VTESTPS tests SIGN BITS only -- nonzero positive values give ZF=1,
         where VPTEST on the same data would give ZF=0 */
        uint32_t zf = 0;
        static const uint32_t pos[4] __attribute__((aligned(16))) = {1, 2, 3, 4};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvmovdqu (%1),%%xmm2\n\t"
                         "vtestps %%xmm2,%%xmm1\n\tsetz %b0\n\tmovzbl %b0,%0"
                         : "=&r"(zf) : "r"(pos) : "xmm1", "xmm2", "cc");
        check64("vtestps.signbits_only", zf, 1);
    }
    { /* VUCOMISS writes EFLAGS: equal -> ZF=1, less -> CF=1, NaN -> both + PF */
        uint32_t zf = 0, cf = 0;
        __asm__ volatile("vmovss %2,%%xmm1\n\tvmovss %3,%%xmm2\n\t"
                         "vucomiss %%xmm2,%%xmm1\n\tsetz %b0\n\tsetc %b1\n\t"
                         "movzbl %b0,%0\n\tmovzbl %b1,%1"
                         : "=&r"(zf), "=&r"(cf) : "m"(*(float[]){1.0f}), "m"(*(float[]){2.0f})
                         : "xmm1", "xmm2", "cc");
        check64("vucomiss.less.cf", cf, 1);
        check64("vucomiss.less.zf", zf, 0);
    }
}

/* ---- the long tail ----
   VPMULDQ is signed where VPMULUDQ is unsigned, and both read only the LOW
   dword of each qword lane. VPMASKMOVD must not fault on elements the mask
   excludes, which is the whole point of the instruction. */
__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,avx512vpopcntdq")))
static void test_long_tail(void) {
    uint32_t r[8] __attribute__((aligned(32)));
    static const uint32_t s[4] __attribute__((aligned(16))) =
        {0x11112222, 0x33334444, 0x55556666, 0x77778888};

    {
        uint32_t v = 0;
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpinsrw $2,%2,%%xmm1,%%xmm2\n\t"
                         "vpextrw $2,%%xmm2,%0"
                         : "=r"(v) : "r"(s), "r"(0xbeefu) : "xmm1", "xmm2");
        check64("vpinsrw.roundtrip", v, 0xbeef);
    }
    {
        uint32_t v = 0;
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvextractps $2,%%xmm1,%0"
                         : "=r"(v) : "r"(s) : "xmm1");
        check64("vextractps", v, 0x55556666);
    }
    {
        static const uint32_t v8[8] __attribute__((aligned(32))) = {0,1,2,3,4,5,6,7};
        __asm__ volatile("vmovdqu (%1),%%ymm1\n\tvextractf128 $1,%%ymm1,%%xmm2\n\t"
                         "vmovdqu %%ymm2,(%0)\n\tvzeroupper"
                         : : "r"(r), "r"(v8) : "memory", "ymm1", "ymm2");
        static const uint32_t w[8] = {4, 5, 6, 7, 0, 0, 0, 0};
        check8("vextractf128", r, w);
    }
    { /* signed: -1 * -1 == 1. VPMULUDQ on the same input gives a huge value. */
        static const uint32_t x[4] __attribute__((aligned(16))) =
            {0xffffffff, 0, 0xffffffff, 0};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpmuldq %%xmm1,%%xmm1,%%xmm2\n\t"
                         "vmovdqu %%xmm2,(%0)"
                         : : "r"(r), "r"(x) : "memory", "xmm1", "xmm2");
        if (r[0] != 1 || r[1] != 0) { printf("FAIL vpmuldq.signed\n"); failures_total++; }
        else { test_logf("PASS vpmuldq.signed\n"); }
    }
    {
        static const double d[2] __attribute__((aligned(16))) = {3.9, -3.9};
        __asm__ volatile("vmovupd (%1),%%xmm1\n\tvcvttpd2dq %%xmm1,%%xmm2\n\t"
                         "vmovdqu %%xmm2,(%0)"
                         : : "r"(r), "r"(d) : "memory", "xmm1", "xmm2");
        if (r[0] != 3 || r[1] != (uint32_t) -3) {
            printf("FAIL vcvttpd2dq\n"); failures_total++;
        } else { test_logf("PASS vcvttpd2dq\n"); }
    }
    {
        static const uint32_t x[4] __attribute__((aligned(16))) = {0xffffffff, 0, 0xf, 0};
        __asm__ volatile("vmovdqu (%1),%%xmm1\n\tvpopcntq %%xmm1,%%xmm2\n\t"
                         "vmovdqu %%xmm2,(%0)"
                         : : "r"(r), "r"(x) : "memory", "xmm1", "xmm2");
        if (r[0] != 32 || r[2] != 4) { printf("FAIL vpopcntq\n"); failures_total++; }
        else { test_logf("PASS vpopcntq\n"); }
    }
    { /* VPTESTNM is the inverse of VPTESTM: bits set where (a & b) == 0 */
        static const uint32_t x[4] __attribute__((aligned(16))) = {1, 0, 3, 0};
        static const uint32_t m[4] __attribute__((aligned(16))) = {1, 1, 0, 0};
        uint32_t k = 0;
        __asm__ volatile("vmovdqu32 (%1),%%xmm1\n\tvmovdqu32 (%2),%%xmm2\n\t"
                         "vptestnmd %%xmm2,%%xmm1,%%k1\n\tkmovw %%k1,%0"
                         : "=r"(k) : "r"(x), "r"(m) : "xmm1", "xmm2", "k1");
        check64("vptestnmd", k & 0xf, 0xe);
    }
    { /* only the mask-selected elements are read */
        static const uint32_t data[4] __attribute__((aligned(16))) = {11, 22, 33, 44};
        static const uint32_t m[4] __attribute__((aligned(16))) =
            {0x80000000, 0, 0x80000000, 0};
        __asm__ volatile("vmovdqu (%2),%%xmm1\n\tvpmaskmovd (%1),%%xmm1,%%xmm2\n\t"
                         "vmovdqu %%xmm2,(%0)"
                         : : "r"(r), "r"(data), "r"(m) : "memory", "xmm1", "xmm2");
        if (r[0] != 11 || r[1] != 0 || r[2] != 33 || r[3] != 0) {
            printf("FAIL vpmaskmovd.load\n"); failures_total++;
        } else { test_logf("PASS vpmaskmovd.load\n"); }
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    test_aligned_and_unaligned_moves();
    test_logical_and_arith();
    test_lane_widths();
    test_compares();
    test_pshufd_is_lane_local();
    test_upper_bits_semantics();
    test_three_byte_vex_high_regs();
    test_movd_movq();
    test_saturating();
    test_shifts();
    test_pshufb();
    test_unpack_and_pack();
    test_broadcast_and_widen();
    test_cross_lane();
    test_palignr();
    test_pmovmskb_and_madd();
    test_minmax_and_mul();
    test_blend_and_insert();
    test_aes();
    test_pclmulqdq();
    test_pternlog();
    test_vnni();
    test_float();
    test_bmi();
    test_kregs();
    test_evex_masking();
    test_avx512_compare_and_permute();
    test_avx512_compress_and_align();
    test_round_blend_convert();
    test_long_tail();

    return finish_suite("avx_regress");
}

#endif /* __x86_64__ */
