// The XSAVE image of the x86 guest's state -- the standard (non-compacted)
// format, with emu/cpuid.h's offsets -- for the kernel: the signal frame's
// math state (kernel/signal.c) and ptrace's NT_X86_XSTATE (kernel/ptrace.c).
// The guest's own XSAVE and XRSTOR instructions are gadgets
// (jit/gadgets-aarch64/x87.S) and keep to the same layout and the same rules:
//
//   - XINUSE, which XSAVE writes into XSTATE_BV for the requested
//     components, is computed from the values: a component is in use unless
//     it is in its initial configuration (x87: FCW 37FH, FSW 0, every
//     register empty; the vector components: all zero). The SDM lets XINUSE
//     be 1 for a component in its initial configuration, which is all that
//     separates this from hardware (Intel tracks modification instead).
//   - XRSTOR initializes each requested component whose XSTATE_BV bit is 0,
//     and loads MXCSR whenever SSE or AVX is requested.
#ifndef EMU_XSAVE_H
#define EMU_XSAVE_H

#include <string.h>
#include "emu/cpu.h"
#include "emu/cpuid.h"
#include "emu/fxsave.h"

#define XSAVE_SIZE_ XSAVE_MAX_SIZE_

static inline bool xsave_nonzero_(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return true;
    return false;
}

// nregs is 16 for a 64-bit task and 8 for a 32-bit one, whose registers
// 8-31 are not its to see: the i386 JIT stages operands in xmm15, which no
// 32-bit code can name, and a 32-bit task's image has zeros there.
static inline qword_t xsave_xinuse(const struct cpu_state *cpu, int nregs) {
    qword_t in_use = 0;
    if (cpu->fcw != 0x37f || cpu->fsw != 0 || cpu->x87_valid != 0)
        in_use |= XCR0_X87_;
    if (xsave_nonzero_(cpu->xmm, nregs * sizeof(cpu->xmm[0])))
        in_use |= XCR0_SSE_;
    if (xsave_nonzero_(cpu->ymm_hi, nregs * sizeof(cpu->ymm_hi[0])))
        in_use |= XCR0_YMM_;
    if (xsave_nonzero_(cpu->avx512_k, sizeof(cpu->avx512_k)))
        in_use |= XCR0_OPMASK_;
    if (xsave_nonzero_(cpu->zmm_hi, nregs * sizeof(cpu->zmm_hi[0])))
        in_use |= XCR0_ZMM_HI256_;
    if (nregs == 16 && (xsave_nonzero_(cpu->xmm_ext, sizeof(cpu->xmm_ext)) ||
            xsave_nonzero_(&cpu->ymm_hi[16], 16 * sizeof(cpu->ymm_hi[0])) ||
            xsave_nonzero_(&cpu->zmm_hi[16], 16 * sizeof(cpu->zmm_hi[0]))))
        in_use |= XCR0_HI16_ZMM_;
    return in_use;
}

// The whole state into buf (XSAVE_SIZE_ bytes), every component, as the
// kernel saves it: the task's registers (nregs, as xsave_xinuse; a 32-bit
// task's others are zero), the header's XSTATE_BV its XINUSE, and bytes the
// format leaves alone (464-511 of the legacy area, the rest of the header,
// the gap where MPX's components would be) zero.
static inline void xsave_fill(struct cpu_state *cpu, uint8_t *buf, int nregs) {
    memset(buf, 0, XSAVE_SIZE_);
    struct fxsave_area fx;
    fxsave_fill(cpu, &fx, nregs);
    memcpy(buf, &fx, 464);
    qword_t bv = xsave_xinuse(cpu, nregs);
    memcpy(buf + XSAVE_LEGACY_SIZE_, &bv, sizeof(bv));
    for (int i = 0; i < nregs; i++)
        memcpy(buf + XSAVE_YMM_OFFSET_ + 16 * i, &cpu->ymm_hi[i], 16);
    memcpy(buf + XSAVE_OPMASK_OFFSET_, cpu->avx512_k, XSAVE_OPMASK_SIZE_);
    for (int i = 0; i < nregs; i++)
        memcpy(buf + XSAVE_ZMM_HI_OFFSET_ + 32 * i, &cpu->zmm_hi[i], 32);
    for (int i = 0; i < 16 && nregs == 16; i++) {
        uint8_t *r = buf + XSAVE_HI16_OFFSET_ + 64 * i;
        memcpy(r, &cpu->xmm_ext[i], 16);
        memcpy(r + 16, &cpu->ymm_hi[16 + i], 16);
        memcpy(r + 32, &cpu->zmm_hi[16 + i], 32);
    }
}

// Every component to its initial configuration (and MXCSR to 1F80H): what
// a signal handler starts with, and what exec leaves.
static inline void xsave_init_state(struct cpu_state *cpu) {
    cpu->fcw = 0x37f;
    cpu->fsw = 0;
    cpu->x87_valid = 0;
    memset(cpu->fp, 0, sizeof(cpu->fp));
    cpu->mxcsr = 0x1f80;
    memset(cpu->xmm, 0, sizeof(cpu->xmm));
    memset(cpu->xmm_ext, 0, sizeof(cpu->xmm_ext));
    memset(cpu->ymm_hi, 0, sizeof(cpu->ymm_hi));
    memset(cpu->zmm_hi, 0, sizeof(cpu->zmm_hi));
    memset(cpu->avx512_k, 0, sizeof(cpu->avx512_k));
}

// XRSTOR's checks of an image for the components rfbm asks for: 0, or -1
// where XRSTOR would #GP (XSTATE_BV naming a component XCR0 lacks, bytes
// 8-23 of the header not zero -- there is no compacted form -- or, when SSE
// or AVX is asked for, MXCSR's reserved bits set).
static inline int xsave_check(const uint8_t *buf, qword_t rfbm) {
    qword_t bv, comp, rsvd;
    memcpy(&bv, buf + XSAVE_LEGACY_SIZE_, 8);
    memcpy(&comp, buf + XSAVE_LEGACY_SIZE_ + 8, 8);
    memcpy(&rsvd, buf + XSAVE_LEGACY_SIZE_ + 16, 8);
    if ((bv & ~(qword_t) XCR0_SUPPORTED_) || comp || rsvd)
        return -1;
    dword_t mxcsr;
    memcpy(&mxcsr, buf + 24, 4);
    if ((rfbm & (XCR0_SSE_ | XCR0_YMM_)) && (mxcsr & ~0xffffu))
        return -1;
    return 0;
}

// XRSTOR of a checked image, the task's registers (nregs, as xsave_fill):
// each component rfbm asks for loaded if XSTATE_BV has it, else
// initialized; MXCSR loaded if SSE or AVX is asked for.
static inline void xsave_restore(struct cpu_state *cpu, const uint8_t *buf, qword_t rfbm, int nregs) {
    qword_t bv;
    memcpy(&bv, buf + XSAVE_LEGACY_SIZE_, 8);
    rfbm &= XCR0_SUPPORTED_;
    struct fxsave_area fx;
    memcpy(&fx, buf, sizeof(fx));
    if (rfbm & XCR0_X87_) {
        if (bv & XCR0_X87_) {
            word_t fcw = fx.fcw;
            fpu_ldcw16(cpu, &fcw);
            cpu->fsw = fx.fsw;
            cpu->x87_valid = fx.ftw;
            for (int i = 0; i < 8; i++) {
                float80 value = {0};
                for (int j = 0; j < 4; j++)
                    value.signif |= (uint64_t) fx.st[i].significand[j] << (j * 16);
                value.signExp = fx.st[i].exponent;
                cpu->fp[(cpu->top + i) & 7] = value;
            }
        } else {
            word_t fcw = 0x37f;
            fpu_ldcw16(cpu, &fcw);
            cpu->fsw = 0;
            cpu->x87_valid = 0;
            memset(cpu->fp, 0, sizeof(cpu->fp));
        }
    }
    if (rfbm & (XCR0_SSE_ | XCR0_YMM_))
        cpu->mxcsr = fx.mxcsr;
    if (nregs == 8)
        rfbm &= ~(qword_t) XCR0_HI16_ZMM_;
    if (rfbm & XCR0_SSE_) {
        for (int i = 0; i < nregs; i++)
            for (int j = 0; j < 4; j++)
                cpu->xmm[i].u32[j] = (bv & XCR0_SSE_) ? fx.xmm[i].element[j] : 0;
    }
    if (rfbm & XCR0_YMM_) {
        for (int i = 0; i < nregs; i++) {
            if (bv & XCR0_YMM_)
                memcpy(&cpu->ymm_hi[i], buf + XSAVE_YMM_OFFSET_ + 16 * i, 16);
            else
                memset(&cpu->ymm_hi[i], 0, 16);
        }
    }
    if (rfbm & XCR0_OPMASK_) {
        if (bv & XCR0_OPMASK_)
            memcpy(cpu->avx512_k, buf + XSAVE_OPMASK_OFFSET_, XSAVE_OPMASK_SIZE_);
        else
            memset(cpu->avx512_k, 0, sizeof(cpu->avx512_k));
    }
    if (rfbm & XCR0_ZMM_HI256_) {
        for (int i = 0; i < nregs; i++) {
            if (bv & XCR0_ZMM_HI256_)
                memcpy(&cpu->zmm_hi[i], buf + XSAVE_ZMM_HI_OFFSET_ + 32 * i, 32);
            else
                memset(&cpu->zmm_hi[i], 0, 32);
        }
    }
    if (rfbm & XCR0_HI16_ZMM_) {
        for (int i = 0; i < 16; i++) {
            const uint8_t *r = buf + XSAVE_HI16_OFFSET_ + 64 * i;
            if (bv & XCR0_HI16_ZMM_) {
                memcpy(&cpu->xmm_ext[i], r, 16);
                memcpy(&cpu->ymm_hi[16 + i], r + 16, 16);
                memcpy(&cpu->zmm_hi[16 + i], r + 32, 32);
            } else {
                memset(&cpu->xmm_ext[i], 0, 16);
                memset(&cpu->ymm_hi[16 + i], 0, 16);
                memset(&cpu->zmm_hi[16 + i], 0, 32);
            }
        }
    }
}

#endif
