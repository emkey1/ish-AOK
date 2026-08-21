// The 512-byte FXSAVE/FXRSTOR area, and the conversions between it and
// cpu_state.
//
// This lives in its own header because both guest engines need it and the
// layout is dictated by hardware: a second hand-written copy of a 512-byte
// structure is a drift hazard, and drift here is silent (the guest saves state
// into the wrong offsets and reads back garbage) rather than loud.
//
// The i386 and x86-64 forms of the area differ only in bytes 8-23, which the
// 32-bit form splits into fip/fcs and fdp/fds where the 64-bit form has a flat
// rip and rdp. AOK reports zero for all of them -- the last x87 instruction
// pointer is not modelled -- so one structure serves both, and the field names
// below follow the 64-bit spelling.
//
// The other difference is how many XMM registers are architecturally visible:
// eight in 32-bit mode, sixteen in long mode. The slots for xmm8-15 exist in
// the area either way and are reserved in 32-bit mode, so the count is a
// parameter rather than a second layout.
#ifndef EMU_FXSAVE_H
#define EMU_FXSAVE_H

#include "emu/cpu.h"
#include "emu/fpu.h"

struct fxsave_fpxreg {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct fxsave_xmmreg {
    dword_t element[4];
};

struct fxsave_area {
    word_t fcw;
    word_t fsw;
    byte_t ftw;
    byte_t reserved0;
    word_t fop;
    qword_t rip;
    qword_t rdp;
    dword_t mxcsr;
    dword_t mxcsr_mask;
    struct fxsave_fpxreg st[8];
    struct fxsave_xmmreg xmm[16];
    byte_t reserved1[96];
};

static_assert(sizeof(struct fxsave_area) == 512, "fxsave area size");

// xmm_count is 8 for a 32-bit guest and 16 for a long-mode one. The slots
// above the count are left zeroed, which is what the reserved region of the
// 32-bit area is required to read as.
static inline void fxsave_fill(struct cpu_state *cpu, struct fxsave_area *area,
        int xmm_count) {
    memset(area, 0, sizeof(*area));
    area->fcw = cpu->fcw;
    area->fsw = cpu->fsw;
    area->mxcsr = cpu->mxcsr;
    area->mxcsr_mask = 0xffff;

    for (int i = 0; i < 8; i++) {
        const float80 value = cpu->fp[i];
        for (int j = 0; j < 4; j++)
            area->st[i].significand[j] = (word_t) (value.signif >> (j * 16));
        area->st[i].exponent = value.signExp;
        // The abridged tag word: one bit per register, set when the register
        // is not empty. We do not model the full two-bits-per-register tag, so
        // "has any bits set" stands in for "not empty".
        if (value.signif != 0 || value.signExp != 0)
            area->ftw |= (byte_t) (1u << i);
    }

    for (int i = 0; i < xmm_count; i++)
        for (int j = 0; j < 4; j++)
            area->xmm[i].element[j] = cpu->xmm[i].u32[j];
}

static inline void fxsave_restore(struct cpu_state *cpu,
        const struct fxsave_area *area, int xmm_count) {
    // Through fpu_ldcw16 rather than a plain store: the control word carries
    // the rounding mode and precision, and the live float80 state has to
    // follow it the same way fldcw and frstor make it.
    word_t fcw = area->fcw;
    fpu_ldcw16(cpu, &fcw);
    cpu->fsw = area->fsw;
    cpu->mxcsr = area->mxcsr;

    for (int i = 0; i < 8; i++) {
        float80 value = {0};
        for (int j = 0; j < 4; j++)
            value.signif |= (uint64_t) area->st[i].significand[j] << (j * 16);
        value.signExp = area->st[i].exponent;
        cpu->fp[i] = value;
    }

    for (int i = 0; i < xmm_count; i++)
        for (int j = 0; j < 4; j++)
            cpu->xmm[i].u32[j] = area->xmm[i].element[j];
}

#endif
