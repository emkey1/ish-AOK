// LOCK OR, AND and XOR on 32 bits: the memory result and flags, and a branch on
// the flags they leave.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "atomic_common.h"

enum { VECTOR_LOOPS = 2000 };

static void test_lock_orl_single(uint32_t initial, uint32_t rhs) {
    volatile uint32_t mem = initial;
    unsigned long eflags;
    uint32_t expected = initial | rhs;
    uint32_t expected_flags = logic_flags32(expected);

    asm volatile(
        "lock orl %2, %1\n\t"
        "pushf\n\t"
        "pop %q0\n\t"
        : "=r"(eflags), "+m"(mem)
        : "r"(rhs)
        : "cc", "memory");

    test_logf("lock orl single: mem0=%08" PRIx32 " rhs=%08" PRIx32
              " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              initial, rhs, mem, (uint32_t) (eflags & CC_LOGIC_MASK));

    if (mem != expected || (eflags & CC_LOGIC_MASK) != expected_flags)
        failf("lock orl single", mem, (uint32_t) (eflags & CC_LOGIC_MASK), 0,
              expected, expected_flags, 0);
}

static void test_lock_andl_single(uint32_t initial, uint32_t rhs) {
    volatile uint32_t mem = initial;
    unsigned long eflags;
    uint32_t expected = initial & rhs;
    uint32_t expected_flags = logic_flags32(expected);

    asm volatile(
        "lock andl %2, %1\n\t"
        "pushf\n\t"
        "pop %q0\n\t"
        : "=r"(eflags), "+m"(mem)
        : "r"(rhs)
        : "cc", "memory");

    test_logf("lock andl single: mem0=%08" PRIx32 " rhs=%08" PRIx32
              " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              initial, rhs, mem, (uint32_t) (eflags & CC_LOGIC_MASK));

    if (mem != expected || (eflags & CC_LOGIC_MASK) != expected_flags)
        failf("lock andl single", mem, (uint32_t) (eflags & CC_LOGIC_MASK), 0,
              expected, expected_flags, 0);
}

static void test_lock_xorl_single(uint32_t initial, uint32_t rhs) {
    volatile uint32_t mem = initial;
    unsigned long eflags;
    uint32_t expected = initial ^ rhs;
    uint32_t expected_flags = logic_flags32(expected);

    asm volatile(
        "lock xorl %2, %1\n\t"
        "pushf\n\t"
        "pop %q0\n\t"
        : "=r"(eflags), "+m"(mem)
        : "r"(rhs)
        : "cc", "memory");

    test_logf("lock xorl single: mem0=%08" PRIx32 " rhs=%08" PRIx32
              " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              initial, rhs, mem, (uint32_t) (eflags & CC_LOGIC_MASK));

    if (mem != expected || (eflags & CC_LOGIC_MASK) != expected_flags)
        failf("lock xorl single", mem, (uint32_t) (eflags & CC_LOGIC_MASK), 0,
              expected, expected_flags, 0);
}

static void test_lock_orl_jz_single(uint32_t initial, uint32_t rhs) {
    volatile uint32_t mem = initial;
    uint32_t branch = 0;
    uint32_t expected = initial | rhs;
    uint32_t expected_branch = expected == 0;

    asm volatile(
        "lock orl %2, %1\n\t"
        "jnz 1f\n\t"
        "movl $1, %0\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "movl $0, %0\n"
        "2:\n\t"
        : "=r"(branch), "+m"(mem)
        : "r"(rhs)
        : "cc", "memory");

    test_logf("lock orl jz mem0=%08" PRIx32 " rhs=%08" PRIx32
              " -> branch=%" PRIu32 " mem=%08" PRIx32 "\n",
              initial, rhs, branch, mem);

    if (branch != expected_branch || mem != expected)
        failf("lock orl jz", branch, mem, 0, expected_branch, expected, 0);
}

static void test_lock_logic_vectors(void) {
    uint32_t seed = 0x31415926u;
    unsigned or_failures = 0;
    unsigned and_failures = 0;
    unsigned xor_failures = 0;

    for (unsigned i = 0; i < VECTOR_LOOPS; i++) {
        uint32_t lhs = next_u32(&seed);
        uint32_t rhs = next_u32(&seed);
        volatile uint32_t mem_or = lhs;
        volatile uint32_t mem_and = lhs;
        volatile uint32_t mem_xor = lhs;
        unsigned long flags_or;
        unsigned long flags_and;
        unsigned long flags_xor;
        uint32_t expected_or = lhs | rhs;
        uint32_t expected_and = lhs & rhs;
        uint32_t expected_xor = lhs ^ rhs;

        asm volatile(
            "lock orl %2, %1\n\t"
            "pushf\n\t"
            "pop %q0\n\t"
            : "=r"(flags_or), "+m"(mem_or)
            : "r"(rhs)
            : "cc", "memory");
        asm volatile(
            "lock andl %2, %1\n\t"
            "pushf\n\t"
            "pop %q0\n\t"
            : "=r"(flags_and), "+m"(mem_and)
            : "r"(rhs)
            : "cc", "memory");
        asm volatile(
            "lock xorl %2, %1\n\t"
            "pushf\n\t"
            "pop %q0\n\t"
            : "=r"(flags_xor), "+m"(mem_xor)
            : "r"(rhs)
            : "cc", "memory");

        if (mem_or != expected_or || (flags_or & CC_LOGIC_MASK) != logic_flags32(expected_or))
            or_failures++;
        if (mem_and != expected_and || (flags_and & CC_LOGIC_MASK) != logic_flags32(expected_and))
            and_failures++;
        if (mem_xor != expected_xor || (flags_xor & CC_LOGIC_MASK) != logic_flags32(expected_xor))
            xor_failures++;
    }

    test_log_if(or_failures != 0, "lock orl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, or_failures);
    test_log_if(and_failures != 0, "lock andl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, and_failures);
    test_log_if(xor_failures != 0, "lock xorl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, xor_failures);
    failures_total += or_failures + and_failures + xor_failures;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_lock_orl_single(0x12340000u, 0x00005678u);
    test_lock_orl_single(0x00000000u, 0x00000000u);
    test_lock_orl_single(0x80000000u, 0x00000001u);
    test_lock_andl_single(0xffffffffu, 0x13579bdfu);
    test_lock_andl_single(0x80000001u, 0x7fffffffu);
    test_lock_xorl_single(0x12345678u, 0x12345678u);
    test_lock_xorl_single(0x80000000u, 0x00000001u);
    test_lock_orl_jz_single(0x00000000u, 0x00000000u);
    test_lock_orl_jz_single(0x00000000u, 0x00000001u);
    test_lock_logic_vectors();
    return finish_suite("atomic_logic32");
}
