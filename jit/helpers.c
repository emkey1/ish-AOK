#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include "emu/cpu.h"
#include "emu/cpuid.h"
#include "emu/tlb.h"
#include "kernel/task.h"
#include "util/sync.h"
#include "emu/vec.h"

void helper_cpuid(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    do_cpuid(a, b, c, d);
}

int cpuid_long_mode_override = -1;
struct cpuid_table cpuid_tables[2];

static void cpuid_fill(struct cpuid_answer *ans, dword_t leaf, dword_t subleaf) {
    dword_t a = leaf, b = 0, c = subleaf, d = 0;
    do_cpuid(&a, &b, &c, &d);
    *ans = (struct cpuid_answer) {a, b, c, d};
}

// jit/gadgets-aarch64/misc.S cpuid_lookup picks slots by number.
_Static_assert(CPUID_SLOT_LEAF7_0 == 2 && CPUID_SLOT_LEAFD_0 == 3 && CPUID_SLOT_EXT0 == 9 &&
        CPUID_SLOT_ABOVE_BASIC == 11 && CPUID_SLOT_ZERO == 12 && CPUID_SLOT_ABOVE_EXT == 13 &&
        CPUID_SLOT_LEAF7_1 == 14, "cpuid_lookup's slot numbers");

__attribute__((constructor)) void cpuid_tables_init(void) {
    for (int lm = 0; lm < 2; lm++) {
        cpuid_long_mode_override = lm;
        struct cpuid_answer *s = cpuid_tables[lm].slot;
        cpuid_fill(&s[CPUID_SLOT_LEAF0], 0, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAF1], 1, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAF7_0], 7, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAF7_1], 7, 1);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_0], 0xd, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_1], 0xd, 1);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_2], 0xd, 2);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_5], 0xd, 5);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_6], 0xd, 6);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_7], 0xd, 7);
        cpuid_fill(&s[CPUID_SLOT_EXT0], 0x80000000u, 0);
        cpuid_fill(&s[CPUID_SLOT_EXT1], 0x80000001u, 0);
        cpuid_fill(&s[CPUID_SLOT_ABOVE_BASIC], cpuid_basic_max_leaf() + 1, 0);
        cpuid_fill(&s[CPUID_SLOT_ZERO], 2, 0);
        cpuid_fill(&s[CPUID_SLOT_ABOVE_EXT], 0x80000002u, 0);
    }
    cpuid_long_mode_override = -1;
}

// XGETBV for the i386 guest. Takes the same four-register frame as
// helper_cpuid so it can reuse that gadget's shape exactly: ecx selects the
// register on the way in, eax/edx take the value on the way out, ebx is
// untouched.
//
// Deviation: hardware raises #GP for any index other than 0, and we return
// zero instead. A gadget cannot raise an interrupt without going through the
// interrupt gadget, which is a lot of machinery for a case no real caller
// reaches -- XCR0 is the only register defined here, and glibc, gcc's ifunc
// resolvers and OpenSSL all pass 0. The amd64 interpreter, where raising it
// costs nothing, does return #GP.
void helper_xgetbv(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    (void) b;
    qword_t value = *c == 0 ? xcr0_value() : 0;
    *a = (dword_t) value;
    *d = (dword_t) (value >> 32);
}

void helper_rdtsc(struct cpu_state *cpu) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tsc = now.tv_sec * 1000000000l + now.tv_nsec;
    cpu->eax = tsc & 0xffffffff;
    cpu->edx = tsc >> 32;
    cpu->amd64_regs[amd64_rax] = cpu->eax;
    cpu->amd64_regs[amd64_rdx] = cpu->edx;
}

void helper_expand_flags(struct cpu_state *cpu) {
    expand_flags(cpu);
}

void helper_collapse_flags(struct cpu_state *cpu) {
    collapse_flags(cpu);
}

// LOOP decrements ECX and, unlike DEC, leaves the flags alone. The branch
// itself reuses the jcxz gadget with its two ip slots swapped, since "jump if
// ECX != 0" is exactly the inverse of jcxz -- so this needs no new gadget on
// either host.
void helper_loop_dec_ecx(struct cpu_state *cpu) {
    cpu->ecx--;
}

// Packed/unpacked BCD adjust instructions. All of these read and write flags
// that the JIT normally keeps lazily (AF in particular), so each one collapses
// the lazy state first and then works with the materialized bits. Semantics
// follow the Intel SDM; flags the SDM leaves undefined are left alone.
// DAA/DAS set SF/ZF/PF from the result, so they also have to clear the lazy
// bits for those (collapse_flags does) and then write them directly.
static void bcd_set_result_flags(struct cpu_state *cpu, uint8_t al) {
    cpu->zf = al == 0;
    cpu->sf = (al & 0x80) != 0;
    cpu->pf = !__builtin_parity(al);
}

void helper_aaa(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint16_t ax = cpu->eax & 0xffff;
    if ((ax & 0x0f) > 9 || cpu->af) {
        // AX += 6 is a 16-bit add, so a carry out of AL lands in AH before the
        // separate AH increment -- doing this 8-bit on AL loses that carry.
        ax += 6;
        ax = (uint16_t) (ax + 0x100);
        cpu->af = cpu->cf = 1;
    } else {
        cpu->af = cpu->cf = 0;
    }
    ax &= 0xff0f;
    cpu->eax = (cpu->eax & 0xffff0000) | ax;
    // The SDM calls SF/ZF/PF undefined here, but real silicon sets them from
    // AL and software has been observed to rely on it, so match the hardware.
    bcd_set_result_flags(cpu, ax & 0xff);
}

void helper_aas(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint16_t ax = cpu->eax & 0xffff;
    if ((ax & 0x0f) > 9 || cpu->af) {
        ax -= 6;
        ax = (uint16_t) (ax - 0x100);
        cpu->af = cpu->cf = 1;
    } else {
        cpu->af = cpu->cf = 0;
    }
    ax &= 0xff0f;
    cpu->eax = (cpu->eax & 0xffff0000) | ax;
    bcd_set_result_flags(cpu, ax & 0xff);
}

void helper_daa(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t old_al = al;
    bool old_cf = cpu->cf;
    cpu->cf = 0;
    if ((al & 0x0f) > 9 || cpu->af) {
        cpu->cf = old_cf || (al > 0xff - 6);
        al += 6;
        cpu->af = 1;
    } else {
        cpu->af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al += 0x60;
        cpu->cf = 1;
    }
    cpu->eax = (cpu->eax & 0xffffff00) | al;
    bcd_set_result_flags(cpu, al);
}

void helper_das(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t old_al = al;
    bool old_cf = cpu->cf;
    cpu->cf = 0;
    if ((al & 0x0f) > 9 || cpu->af) {
        cpu->cf = old_cf || (al < 6);
        al -= 6;
        cpu->af = 1;
    } else {
        cpu->af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al -= 0x60;
        cpu->cf = 1;
    }
    cpu->eax = (cpu->eax & 0xffffff00) | al;
    bcd_set_result_flags(cpu, al);
}

// AAM's base-0 case is a divide error; gen.c catches that at translate time
// (the base is an immediate) and emits the interrupt instead of calling this.
void helper_aam(struct cpu_state *cpu, uint32_t base) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t ah = al / (uint8_t) base;
    al = al % (uint8_t) base;
    cpu->eax = (cpu->eax & 0xffff0000) | ((uint32_t) ah << 8) | al;
    bcd_set_result_flags(cpu, al);
}

void helper_aad(struct cpu_state *cpu, uint32_t base) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t ah = (cpu->eax >> 8) & 0xff;
    uint8_t prod = (uint8_t) (ah * (uint8_t) base);
    unsigned sum = al + prod;
    // The SDM calls CF/AF undefined, but hardware sets them from the implied
    // 8-bit ADD of AL and AH*base, and matching that is free.
    cpu->cf = sum > 0xff;
    cpu->af = ((al & 0xf) + (prod & 0xf)) > 0xf;
    al = (uint8_t) sum;
    cpu->eax = (cpu->eax & 0xffff0000) | al;
    bcd_set_result_flags(cpu, al);
}

