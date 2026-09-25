// AArch64 guest scalar floating-point gadgets. Independently written,
// not adapted from OpenMinis. Same-architecture dispatch does all the
// heavy lifting: every operation is the host's own FP instruction on
// host registers s29-s31/d29-d31, with IEEE semantics, rounding, and
// NaN behavior for free. Values move between the guest V-register file
// (cpu_state.arm64_v, 16-byte slots) and host registers around each op;
// a scalar host write to Sd/Dd architecturally zeroes the rest of the
// vector register, so `str q30` after the op stores the architecturally
// correct full 128-bit result without explicit masking.
//
// Single (S) and double (D) precision are fully implemented; half (H) is
// supported for FCVT precision-conversion only (the baseline storage/
// interchange form, not FEAT_FP16 arithmetic). The fixed-point conversion
// forms still reject at decode (gen_step_arm64) with the usual clean
// INT_UNDEFINED.
//
// FCSEL/FCCMP reuse the cond_* gadget family (dpextra.S): gen emits
// [cond_<cc>][consumer], with the condition result handed over in w10.
//
// No gadget here calls anything, so scratch registers are unconstrained;
// x8-x16 for integers, v29-v31 for FP values.
//
// The guest's FPCR is the host's while guest code runs, and the flags these
// instructions raise are gathered into the guest's FPSR: emu/fpenv.c, via
// the MRS/MSR gadgets in dpextra.S and cpu_run_to_interrupt.

#include "gadgets.h"

.text

// ---- helpers -------------------------------------------------------------
// load slot address of guest V register (index in \idxreg) into \out
.macro vslot out, idxreg
    add \out, _cpu, #CPU_v0
    add \out, \out, \idxreg, lsl #4
.endm

// ---- FP 2-source: FMUL/FDIV/FADD/FSUB/FMAX/FMIN/FMAXNM/FMINNM/FNMUL -----
// Code stream: [gadget][rd | rn<<8 | rm<<16]
.macro fp2src name, op, reg1, reg2, reg3
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    vslot x12, x10
    ldr \reg2, [x12]
    vslot x12, x11
    ldr \reg3, [x12]
    \op \reg1, \reg2, \reg3
    vslot x12, x9
    str q30, [x12]
    gret
.endm

fp2src fmul_s,   fmul,   s30, s30, s31
fp2src fmul_d,   fmul,   d30, d30, d31
fp2src fdiv_s,   fdiv,   s30, s30, s31
fp2src fdiv_d,   fdiv,   d30, d30, d31
fp2src fadd_s,   fadd,   s30, s30, s31
fp2src fadd_d,   fadd,   d30, d30, d31
fp2src fsub_s,   fsub,   s30, s30, s31
fp2src fsub_d,   fsub,   d30, d30, d31
fp2src fmax_s,   fmax,   s30, s30, s31
fp2src fmax_d,   fmax,   d30, d30, d31
fp2src fmin_s,   fmin,   s30, s30, s31
fp2src fmin_d,   fmin,   d30, d30, d31
fp2src fmaxnm_s, fmaxnm, s30, s30, s31
fp2src fmaxnm_d, fmaxnm, d30, d30, d31
fp2src fminnm_s, fminnm, s30, s30, s31
fp2src fminnm_d, fminnm, d30, d30, d31
fp2src fnmul_s,  fnmul,  s30, s30, s31
fp2src fnmul_d,  fnmul,  d30, d30, d31

// ---- FP 1-source: FMOV/FABS/FNEG/FSQRT + FCVT (S<->D) --------------------
// Code stream: [gadget][rd | rn<<8]
.macro fp1src name, op, dstreg, srcreg
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    vslot x12, x10
    ldr \srcreg, [x12]
    \op \dstreg, \srcreg
    vslot x12, x9
    str q30, [x12]
    gret
.endm

fp1src fmov_s,   fmov,  s30, s30
fp1src fmov_d,   fmov,  d30, d30
fp1src fabs_s,   fabs,  s30, s30
fp1src fabs_d,   fabs,  d30, d30
fp1src fneg_s,   fneg,  s30, s30
fp1src fneg_d,   fneg,  d30, d30
fp1src fsqrt_s,  fsqrt, s30, s30
fp1src fsqrt_d,  fsqrt, d30, d30
fp1src fcvt_sd,  fcvt,  d30, s30   // single -> double
fp1src fcvt_ds,  fcvt,  s30, d30   // double -> single
// Half-precision FCVT: this is the baseline ARMv8.0 storage/interchange
// conversion (always available), NOT the FEAT_FP16 arithmetic extension
// (FADD/FMUL/etc on H registers) — no hostcaps gating needed here.
fp1src fcvt_sh,  fcvt,  h30, s30   // single -> half
fp1src fcvt_hs,  fcvt,  s30, h30   // half -> single
fp1src fcvt_dh,  fcvt,  h30, d30   // double -> half
fp1src fcvt_hd,  fcvt,  d30, h30   // half -> double

// ---- FCMP/FCMPE (incl. the compare-with-zero forms) ----------------------
// Code stream: [gadget][rn | rm<<8 | cmp0<<16 | e<<17]. FCMPE is the
// signalling compare -- IOC for a quiet NaN too -- so it runs as fcmpe; the
// NZCV result is the same either way.
.macro fcmp_gadget name, reg1, reg2, zero
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #1
    ubfx x13, x8, #17, #1
    vslot x12, x9
    ldr \reg1, [x12]
    cbnz x11, 1f
    vslot x12, x10
    ldr \reg2, [x12]
    cbnz x13, 3f
    fcmp \reg1, \reg2
    b 2f
3:  fcmpe \reg1, \reg2
    b 2f
1:  cbnz x13, 4f
    fcmp \reg1, \zero
    b 2f
4:  fcmpe \reg1, \zero
2:  store_flags
    gret
.endm

fcmp_gadget fcmp_s, s30, s31, #0.0
fcmp_gadget fcmp_d, d30, d31, #0.0

// ---- FCSEL (consumer; preceded by a cond_* gadget setting w10) -----------
// Code stream: [cond gadget][gadget][rd | rn<<8 | rm<<16]
// Real bug caught by testing (musl atan returning -atan(x) for every
// input): the original parsed the rn field into x10 — the SAME register
// the preceding cond_* gadget hands the condition result over in (w10)
// — then tested `cbnz w10` against its own freshly-parsed parameter.
// The select condition silently became "rn index != 0". The integer
// csel consumer happens to parse its fields into other registers, which
// is why the dpextra CSEL checks all passed. Parameter parsing in any
// w10-consuming gadget must avoid x10.
.gadget fcsel
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x13, x8, #8, #5      // rn (NOT x10 — w10 is the live cond handoff)
    ubfx x11, x8, #16, #5     // rm
    cbnz w10, 1f
    mov x13, x11              // condition false: take rm
1:  vslot x12, x13
    ldr q30, [x12]            // whole slot: FCSEL Sd only zeroes upper, and
                              // the source slot's upper bits are already zero
    vslot x12, x9
    str q30, [x12]
    gret

// ---- FCCMP (consumer): cond true -> flags from the compare, else nzcv ----
// Code stream: [cond gadget][gadget][rn | rm<<8 | is_d<<16 | nzcv<<17 | e<<21]
// e: FCCMPE, the signalling form (see FCMPE above).
.gadget fccmp
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x13, x8, #16, #1
    ubfx x14, x8, #17, #4
    ubfx x15, x8, #21, #1
    cbnz w10, 1f
    lsl w14, w14, #28
    str w14, [_cpu, #CPU_nzcv]
    gret
1:  vslot x12, x9
    ldr q30, [x12]
    vslot x12, x11
    ldr q31, [x12]
    cbnz x13, 2f
    cbnz x15, 4f
    fcmp s30, s31
    b 3f
4:  fcmpe s30, s31
    b 3f
2:  cbnz x15, 5f
    fcmp d30, d31
    b 3f
5:  fcmpe d30, d31
3:  store_flags
    gret

// ---- Integer -> FP: SCVTF/UCVTF from a general register ------------------
// Code stream: [gadget][rd | rn<<8]. rn=31 is ZR (never SP) in this form.
.macro cvtf_gadget name, op, fpreg, gpreg
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    mov x16, #0
    cmp x10, #31
    b.eq 1f
    add x12, _cpu, #CPU_x0
    add x12, x12, x10, lsl #3
    ldr x16, [x12]
1:  \op \fpreg, \gpreg
    vslot x12, x9
    str q30, [x12]
    gret
.endm

cvtf_gadget scvtf_ws, scvtf, s30, w16
cvtf_gadget scvtf_xs, scvtf, s30, x16
cvtf_gadget scvtf_wd, scvtf, d30, w16
cvtf_gadget scvtf_xd, scvtf, d30, x16
cvtf_gadget ucvtf_ws, ucvtf, s30, w16
cvtf_gadget ucvtf_xs, ucvtf, s30, x16
cvtf_gadget ucvtf_wd, ucvtf, d30, w16
cvtf_gadget ucvtf_xd, ucvtf, d30, x16

// ---- FP -> integer: FCVT{Z,N,M,P,A}{S,U} to a general register -----------
// Code stream: [gadget][rd | rn<<8]. rd=31 discards (ZR).
.macro fcvti_gadget name, op, gpreg, fpreg
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    vslot x12, x10
    ldr q30, [x12]
    \op \gpreg, \fpreg
    cmp x9, #31
    b.eq 1f
    add x12, _cpu, #CPU_x0
    add x12, x12, x9, lsl #3
    str x16, [x12]            // w-form ops zero-extended into x16 already
1:  gret
.endm

fcvti_gadget fcvtzs_sw, fcvtzs, w16, s30
fcvti_gadget fcvtzs_sx, fcvtzs, x16, s30
fcvti_gadget fcvtzs_dw, fcvtzs, w16, d30
fcvti_gadget fcvtzs_dx, fcvtzs, x16, d30
fcvti_gadget fcvtzu_sw, fcvtzu, w16, s30
fcvti_gadget fcvtzu_sx, fcvtzu, x16, s30
fcvti_gadget fcvtzu_dw, fcvtzu, w16, d30
fcvti_gadget fcvtzu_dx, fcvtzu, x16, d30
fcvti_gadget fcvtns_sw, fcvtns, w16, s30
fcvti_gadget fcvtns_sx, fcvtns, x16, s30
fcvti_gadget fcvtns_dw, fcvtns, w16, d30
fcvti_gadget fcvtns_dx, fcvtns, x16, d30
fcvti_gadget fcvtms_sw, fcvtms, w16, s30
fcvti_gadget fcvtms_sx, fcvtms, x16, s30
fcvti_gadget fcvtms_dw, fcvtms, w16, d30
fcvti_gadget fcvtms_dx, fcvtms, x16, d30
fcvti_gadget fcvtps_sw, fcvtps, w16, s30
fcvti_gadget fcvtps_sx, fcvtps, x16, s30
fcvti_gadget fcvtps_dw, fcvtps, w16, d30
fcvti_gadget fcvtps_dx, fcvtps, x16, d30
fcvti_gadget fcvtas_sw, fcvtas, w16, s30
fcvti_gadget fcvtas_sx, fcvtas, x16, s30
fcvti_gadget fcvtas_dw, fcvtas, w16, d30
fcvti_gadget fcvtas_dx, fcvtas, x16, d30
// Unsigned rounding variants (rust: `f64 as u64` after floor/ceil lowers
// to FCVTMU/FCVTPU; a cargo-built coordinator SIGILL'd on fcvtmu x1, d28).
fcvti_gadget fcvtnu_sw, fcvtnu, w16, s30
fcvti_gadget fcvtnu_sx, fcvtnu, x16, s30
fcvti_gadget fcvtnu_dw, fcvtnu, w16, d30
fcvti_gadget fcvtnu_dx, fcvtnu, x16, d30
fcvti_gadget fcvtpu_sw, fcvtpu, w16, s30
fcvti_gadget fcvtpu_sx, fcvtpu, x16, s30
fcvti_gadget fcvtpu_dw, fcvtpu, w16, d30
fcvti_gadget fcvtpu_dx, fcvtpu, x16, d30
fcvti_gadget fcvtmu_sw, fcvtmu, w16, s30
fcvti_gadget fcvtmu_sx, fcvtmu, x16, s30
fcvti_gadget fcvtmu_dw, fcvtmu, w16, d30
fcvti_gadget fcvtmu_dx, fcvtmu, x16, d30
fcvti_gadget fcvtau_sw, fcvtau, w16, s30
fcvti_gadget fcvtau_sx, fcvtau, x16, s30
fcvti_gadget fcvtau_dw, fcvtau, w16, d30
fcvti_gadget fcvtau_dx, fcvtau, x16, d30

// ---- FP <-> fixed-point (the bit21=0 conversion family) ------------------
// SCVTF/UCVTF Fd, Rn, #fbits and FCVTZS/FCVTZU Rd, Fn, #fbits (tmux's
// server SIGILL'd on `scvtf d0, w0, #2`). The host instruction's #fbits
// is an assemble-time immediate, so instead of 64 gadgets per form the
// scale rides the code stream as a compile-time FP CONSTANT (2^-fbits or
// 2^+fbits in the operand precision, built by gen_step_arm64) and the
// gadget applies the register-form conversion plus one fmul. Bit-exact
// vs the architected single-step operation: scaling by a power of two
// only shifts the exponent, never the significand, so the rounding
// decision is identical — and an fmul overflow to infinity saturates
// FCVTZ* exactly like the direct out-of-range result (NaN -> 0 likewise).
// The scalar fmul zeroes the upper vector lanes just like the convert,
// matching the architected Vd write.
// Code stream: [gadget][rd | rn<<8][scale constant bits]

.macro cvtf_fix_gadget name, op, fpreg, gpreg, sclreg, sclgpr
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    mov x16, #0
    cmp x10, #31
    b.eq 1f
    add x12, _cpu, #CPU_x0
    add x12, x12, x10, lsl #3
    ldr x16, [x12]
1:  \op \fpreg, \gpreg
    fmov \sclreg, \sclgpr
    fmul \fpreg, \fpreg, \sclreg
    vslot x12, x9
    str q30, [x12]
    gret
.endm

cvtf_fix_gadget scvtf_fix_ws, scvtf, s30, w16, s31, w11
cvtf_fix_gadget scvtf_fix_xs, scvtf, s30, x16, s31, w11
cvtf_fix_gadget scvtf_fix_wd, scvtf, d30, w16, d31, x11
cvtf_fix_gadget scvtf_fix_xd, scvtf, d30, x16, d31, x11
cvtf_fix_gadget ucvtf_fix_ws, ucvtf, s30, w16, s31, w11
cvtf_fix_gadget ucvtf_fix_xs, ucvtf, s30, x16, s31, w11
cvtf_fix_gadget ucvtf_fix_wd, ucvtf, d30, w16, d31, x11
cvtf_fix_gadget ucvtf_fix_xd, ucvtf, d30, x16, d31, x11

.macro fcvti_fix_gadget name, op, gpreg, fpreg, sclreg, sclgpr
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    vslot x12, x10
    ldr q30, [x12]
    fmov \sclreg, \sclgpr
    fmul \fpreg, \fpreg, \sclreg
    \op \gpreg, \fpreg
    cmp x9, #31
    b.eq 1f
    add x12, _cpu, #CPU_x0
    add x12, x12, x9, lsl #3
    str x16, [x12]            // w-form ops zero-extended into x16 already
1:  gret
.endm

fcvti_fix_gadget fcvtzs_fix_sw, fcvtzs, w16, s30, s31, w11
fcvti_fix_gadget fcvtzs_fix_sx, fcvtzs, x16, s30, s31, w11
fcvti_fix_gadget fcvtzs_fix_dw, fcvtzs, w16, d30, d31, x11
fcvti_fix_gadget fcvtzs_fix_dx, fcvtzs, x16, d30, d31, x11
fcvti_fix_gadget fcvtzu_fix_sw, fcvtzu, w16, s30, s31, w11
fcvti_fix_gadget fcvtzu_fix_sx, fcvtzu, x16, s30, s31, w11
fcvti_fix_gadget fcvtzu_fix_dw, fcvtzu, w16, d30, d31, x11
fcvti_fix_gadget fcvtzu_fix_dx, fcvtzu, x16, d30, d31, x11

// ---- FP 3-source: FMADD/FMSUB/FNMADD/FNMSUB ------------------------------
// Code stream: [gadget][rd | rn<<8 | rm<<16 | ra<<24]
.macro fp3src name, op, r1, r2, r3, r4
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x13, x8, #24, #5
    vslot x12, x10
    ldr \r2, [x12]
    vslot x12, x11
    ldr \r3, [x12]
    vslot x12, x13
    ldr \r4, [x12]
    \op \r1, \r2, \r3, \r4
    vslot x12, x9
    str q30, [x12]
    gret
.endm

fp3src fmadd_s,  fmadd,  s30, s30, s31, s29
fp3src fmadd_d,  fmadd,  d30, d30, d31, d29
fp3src fmsub_s,  fmsub,  s30, s30, s31, s29
fp3src fmsub_d,  fmsub,  d30, d30, d31, d29
fp3src fnmadd_s, fnmadd, s30, s30, s31, s29
fp3src fnmadd_d, fnmadd, d30, d30, d31, d29
fp3src fnmsub_s, fnmsub, s30, s30, s31, s29
fp3src fnmsub_d, fnmsub, d30, d30, d31, d29

// ---- FRINT* (round to integral, FP -> FP) ---------------------------------
// Code stream: [gadget][rd | rn<<8]
fp1src frintz_s, frintz, s30, s30
fp1src frintz_d, frintz, d30, d30
fp1src frintn_s, frintn, s30, s30
fp1src frintn_d, frintn, d30, d30
fp1src frintm_s, frintm, s30, s30
fp1src frintm_d, frintm, d30, d30
fp1src frintp_s, frintp, s30, s30
fp1src frintp_d, frintp, d30, d30
fp1src frinta_s, frinta, s30, s30
fp1src frinta_d, frinta, d30, d30
fp1src frintx_s, frintx, s30, s30
fp1src frintx_d, frintx, d30, d30
fp1src frinti_s, frinti, s30, s30
fp1src frinti_d, frinti, d30, d30

# vim: ft=gas
