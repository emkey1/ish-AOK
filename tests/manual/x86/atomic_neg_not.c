// LOCK NOT and LOCK NEG: value and flags, at every width.
//
// These are the two group-3 members a LOCK prefix is legal on. The i386
// engine's LOCK table had no group-3 entry at all until build 556, so both
// decoded to UNDEFINED and an i386 guest died with SIGILL on either, while
// Linux runs them. atomic_lock_contended checks that they interlock; this
// checks what one of them computes, which a counter test cannot see:
//
//   not   ~operand, and NO flag changes. That includes a flag an earlier
//         instruction left pending: the JIT computes ZF/SF/PF lazily from a
//         saved result, so a `not` that touched that result would change the
//         flags without touching EFLAGS itself. The lazy case is checked
//         separately from the popf case for exactly that reason.
//   neg   0 - operand with the full sub flag rule: CF set unless the operand
//         is 0, OF only for the most negative value, AF from the borrow out
//         of bit 3, and ZF/SF/PF from the result.
//
// Every width has its own gadget (8, 16 and 32 bits), so every width is run.
// Runs on i386 and amd64, and on real hardware as the oracle.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "atomic_common.h"

static uint32_t mask_of(int bits) {
    return bits == 32 ? 0xffffffffu : (1u << bits) - 1;
}

static uint32_t sub_flags_n(uint32_t lhs, uint32_t rhs, int bits) {
    uint32_t mask = mask_of(bits);
    uint32_t sign = 1u << (bits - 1);
    uint32_t result = (lhs - rhs) & mask;
    uint32_t flags = 0;
    lhs &= mask;
    rhs &= mask;
    if (lhs < rhs)
        flags |= CC_C;
    if (result & sign)
        flags |= CC_S;
    if (result == 0)
        flags |= CC_Z;
    if ((lhs ^ rhs ^ result) & 0x10u)
        flags |= CC_A;
    if (parity_even8(result & 0xffu))
        flags |= CC_P;
    if ((lhs ^ rhs) & (lhs ^ result) & sign)
        flags |= CC_O;
    return flags;
}

// One locked instruction on `mem`, with EFLAGS set to `flags_in` by popf just
// before it and read back by pushf just after, in the same asm block so the
// compiler cannot put anything flag-changing in between.
#define LOCKED(insn, lval, flags_in, flags_out)                                \
    asm volatile("push %2\n\t"                                                \
                 "popf\n\t"                                                   \
                 insn " %1\n\t"                                               \
                 "pushf\n\t"                                                  \
                 "pop %0\n\t"                                                 \
                 : "=&r"(flags_out), "+m"(lval)                               \
                 : "r"(flags_in)                                              \
                 : "cc", "memory")

static void run_one(const char *op, int bits, uint32_t v, unsigned long flags_in) {
    uint32_t mask = mask_of(bits);
    volatile union { uint32_t l; uint16_t w; uint8_t b; } u;
    unsigned long out = 0;
    uint32_t want_val, want_flags;

    u.l = v & mask;
    if (op[0] == 'n' && op[1] == 'o') {
        switch (bits) {
            case 8:  LOCKED("lock notb", u.b, flags_in, out); break;
            case 16: LOCKED("lock notw", u.w, flags_in, out); break;
            default: LOCKED("lock notl", u.l, flags_in, out); break;
        }
        want_val = ~v & mask;
        want_flags = (uint32_t) flags_in & CC_MASK;
    } else {
        switch (bits) {
            case 8:  LOCKED("lock negb", u.b, flags_in, out); break;
            case 16: LOCKED("lock negw", u.w, flags_in, out); break;
            default: LOCKED("lock negl", u.l, flags_in, out); break;
        }
        want_val = (0u - v) & mask;
        want_flags = sub_flags_n(0, v, bits);
    }

    uint32_t mem = bits == 8 ? u.b : bits == 16 ? u.w : u.l;
    uint32_t got_flags = (uint32_t) out & CC_MASK;
    test_logf("lock %s%d v=%08" PRIx32 " flags_in=%03lx -> mem=%08" PRIx32
              " flags=%03" PRIx32 "\n", op, bits, v & mask, flags_in & CC_MASK,
              mem, got_flags);
    if (mem != want_val || got_flags != want_flags) {
        char label[32];
        snprintf(label, sizeof(label), "lock %s%d", op, bits);
        failf(label, v & mask, mem, got_flags, v & mask, want_val, want_flags);
    }
}

// `not` after a flag-setting cmp, with no popf in between: the flags are still
// the lazily computed ones from the cmp when the locked op runs.
static void not_keeps_lazy_flags(uint32_t a, uint32_t b, uint32_t v) {
    volatile uint32_t mem = v;
    unsigned long out;
    asm volatile("cmpl %3, %2\n\t"
                 "lock notl %1\n\t"
                 "pushf\n\t"
                 "pop %0\n\t"
                 : "=&r"(out), "+m"(mem)
                 : "r"(a), "r"(b)
                 : "cc", "memory");
    uint32_t want = sub_flags32(a, b, a - b);
    uint32_t got = (uint32_t) out & CC_MASK;
    test_logf("cmp %08" PRIx32 ",%08" PRIx32 "; lock notl -> flags=%03" PRIx32
              " want=%03" PRIx32 "\n", a, b, got, want);
    if (got != want || mem != ~v)
        failf("lock notl after cmp", a, b, got, mem, ~v, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // Edges for every width: 0 (the one neg leaves CF clear), 1, the most
    // negative value (neg's only OF), the low nibble boundaries AF depends on,
    // and all-ones.
    static const uint32_t values[] = {
        0x00000000u, 0x00000001u, 0x0000000fu, 0x00000010u, 0x00000011u,
        0x0000007fu, 0x00000080u, 0x000000ffu, 0x00007fffu, 0x00008000u,
        0x0000ffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x12345678u,
        0x89abcdefu,
    };
    // All six arithmetic flags set, then all clear. Reserved bit 1 is always
    // set in EFLAGS; popf ignores the rest of what we do not name.
    static const unsigned long flag_patterns[] = { 0x2u | CC_MASK, 0x2u };
    static const int widths[] = { 8, 16, 32 };

    for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            for (unsigned f = 0; f < 2; f++) {
                run_one("not", widths[w], values[i], flag_patterns[f]);
                // neg's flags do not depend on the incoming ones; running it
                // under both patterns proves it overwrites every one.
                run_one("neg", widths[w], values[i], flag_patterns[f]);
            }
        }
    }

    uint32_t seed = 0x2545f491u;
    for (int i = 0; i < 2000; i++) {
        uint32_t v = next_u32(&seed);
        run_one("neg", widths[i % 3], v, flag_patterns[i & 1]);
    }

    not_keeps_lazy_flags(1, 1, 0x12345678u);           // ZF, PF
    not_keeps_lazy_flags(0, 1, 0x00000000u);           // CF, SF, AF, PF
    not_keeps_lazy_flags(0x80000000u, 1, 0xffffffffu); // OF
    not_keeps_lazy_flags(5, 3, 0x0f0f0f0fu);           // none

    return finish_suite("atomic_neg_not");
}
