// ppc64 instruction fields (Power ISA 2.07). The ISA numbers bits from the
// most significant, 0 to 31; these take them by their shift from the least,
// and each says which ISA field it is. Pure extraction: jit/gen_ppc64.c
// decides what an instruction is.
#ifndef EMU_ARCH_PPC64_DECODE_H
#define EMU_ARCH_PPC64_DECODE_H

#include <stdint.h>

static inline unsigned ppc_op(uint32_t w) { return w >> 26; }          // 0:5
static inline unsigned ppc_rt(uint32_t w) { return (w >> 21) & 31; }   // 6:10 (RT, RS, BO, FRT, VRT)
static inline unsigned ppc_ra(uint32_t w) { return (w >> 16) & 31; }   // 11:15 (RA, BI, FRA)
static inline unsigned ppc_rb(uint32_t w) { return (w >> 11) & 31; }   // 16:20 (RB, FRB)
static inline unsigned ppc_rc(uint32_t w) { return (w >> 6) & 31; }    // 21:25 (FRC, MB, VRC)
static inline unsigned ppc_me(uint32_t w) { return (w >> 1) & 31; }    // 26:30
static inline unsigned ppc_bf(uint32_t w) { return (w >> 23) & 7; }    // 6:8
static inline unsigned ppc_bfa(uint32_t w) { return (w >> 18) & 7; }   // 11:13
static inline unsigned ppc_l(uint32_t w) { return (w >> 21) & 1; }     // 10 (a compare's L)
static inline unsigned ppc_rcbit(uint32_t w) { return w & 1; }         // 31 (Rc, LK)
static inline unsigned ppc_aa(uint32_t w) { return (w >> 1) & 1; }     // 30
static inline unsigned ppc_oe(uint32_t w) { return (w >> 10) & 1; }    // 21
static inline unsigned ppc_xo10(uint32_t w) { return (w >> 1) & 0x3ff; } // 21:30 (X, XL, XFX)
static inline unsigned ppc_xo9(uint32_t w) { return (w >> 1) & 0x1ff; }  // 22:30 (XO)
static inline unsigned ppc_xo5(uint32_t w) { return (w >> 1) & 31; }     // 26:30 (A)
static inline unsigned ppc_vxo11(uint32_t w) { return w & 0x7ff; }       // 21:31 (VX)
static inline unsigned ppc_vxo10(uint32_t w) { return w & 0x3ff; }       // 22:31 (VC)
static inline unsigned ppc_vxo6(uint32_t w) { return w & 0x3f; }         // 26:31 (VA)

static inline int64_t ppc_si(uint32_t w) { return (int16_t) w; }       // 16:31
static inline uint64_t ppc_ui(uint32_t w) { return w & 0xffff; }
// DS form: the displacement with its low two bits the opcode's
static inline int64_t ppc_ds(uint32_t w) { return (int16_t) (w & 0xfffc); }
static inline unsigned ppc_ds_xo(uint32_t w) { return w & 3; }
// I form (b): a 26-bit signed word offset
static inline int64_t ppc_li(uint32_t w) {
    return (int64_t) ((int32_t) ((w & 0x03fffffc) << 6) >> 6);
}
// B form (bc): a 16-bit signed word offset
static inline int64_t ppc_bd(uint32_t w) { return (int16_t) (w & 0xfffc); }
// XFX: an SPR's number, its two halves swapped in the encoding
static inline unsigned ppc_spr(uint32_t w) {
    return ((w >> 16) & 31) | ((w >> 11) & 31) << 5;
}
// mtcrf's FXM
static inline unsigned ppc_fxm(uint32_t w) { return (w >> 12) & 0xff; }
// MD form (rld*): sh is sh5 (bit 30) above sh0:4; mb (and me) is its six-bit
// field with the low bit (bit 26) the high one
static inline unsigned ppc_md_sh(uint32_t w) { return ((w >> 11) & 31) | ((w >> 1) & 1) << 5; }
static inline unsigned ppc_md_mb(uint32_t w) { return ((w >> 6) & 31) | ((w >> 5) & 1) << 5; }
static inline unsigned ppc_md_xo(uint32_t w) { return (w >> 2) & 7; }   // 27:29
static inline unsigned ppc_mds_xo(uint32_t w) { return (w >> 1) & 15; } // 27:30
// XS form (sradi): sh as MD's
static inline unsigned ppc_xs_xo(uint32_t w) { return (w >> 2) & 0x1ff; } // 21:29
// VSX register numbers: the five-bit field and its extension bit
static inline unsigned ppc_xt(uint32_t w) { return ppc_rt(w) | (w & 1) << 5; }        // TX: 31
static inline unsigned ppc_xs_x1(uint32_t w) { return ppc_xt(w); }
static inline unsigned ppc_xa(uint32_t w) { return ppc_ra(w) | ((w >> 2) & 1) << 5; } // AX: 29
static inline unsigned ppc_xb(uint32_t w) { return ppc_rb(w) | ((w >> 1) & 1) << 5; } // BX: 30
static inline unsigned ppc_xc(uint32_t w) { return ppc_rc(w) | ((w >> 3) & 1) << 5; } // CX: 28
static inline unsigned ppc_xx3_xo(uint32_t w) { return (w >> 3) & 0xff; }   // 21:28
static inline unsigned ppc_xx2_xo(uint32_t w) { return (w >> 2) & 0x1ff; }  // 21:29

#endif
