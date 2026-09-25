// AArch64 guest integer data-processing gadgets, batch 2: bitfield
// (SBFM/BFM/UBFM and all their aliases -- LSL/LSR/ASR immediate, UBFX/
// SBFX/BFI/BFXIL, SXTB/SXTH/SXTW/UXTB/UXTH), EXTR, conditional select
// (CSEL/CSINC/CSINV/CSNEG and the CSET/CINC/CINV/CNEG aliases),
// conditional compare (CCMP/CCMN, immediate and register), 2-source
// (UDIV/SDIV/LSLV/LSRV/ASRV/RORV), 1-source (RBIT/REV16/REV32/REV/CLZ/
// CLS), 3-source (MADD/MSUB/SMADDL/SMSUBL/SMULH/UMADDL/UMSUBL/UMULH),
// barriers (DMB/DSB/ISB), and MRS/MSR for TPIDR_EL0 plus constant system
// registers. Independently written, not adapted from OpenMinis.
//
// None of these touch guest memory, so no TLB-macro register discipline
// applies -- everything uses x8-x17 scratch freely, with one deliberate
// exception: the cond_* gadgets communicate their result to the NEXT
// gadget in the block through w10, which persists across gret (gret is
// just a load + indirect branch; registers survive between gadgets
// within a block). gen_step_arm64 emits [cond gadget][consumer gadget]
// pairs for CSEL/CCMP-family instructions -- the cond gadget evaluates
// the (compile-time-selected) condition against guest NZCV via the
// host's own flags, the consumer reads w10. This two-gadget split keeps
// the per-condition-code specialization pattern already proven by
// control.S's bcond_* family while letting one consumer body serve all
// 16 conditions.
//
// SBFM/UBFM use the classic double-shift lowering: with R = register
// width, L = R-1-imms, S = (L+immr) mod R, the architected bitfield-move
// result is exactly `(src << L) >>(u/s) S` for BOTH the extract
// (imms >= immr) and insert-into-zeros (imms < immr) halves of the
// encoding -- verified against the ARM ARM's DecodeBitMasks-based
// definition for the extract, insert, and width-alias (SXTW/LSL-imm/
// LSR-imm) cases before writing this. L and S are computed at compile
// time by gen_step_arm64; the gadget just performs two register-amount
// shifts (lslv + lsrv/asrv), so the host does the actual semantics.
// BFM additionally needs the insert mask M (which destination bits are
// replaced), also compile-time computed and passed as a second code-
// stream word.

#include "gadgets.h"

.text

// ---- Bitfield: SBFM/UBFM ------------------------------------------------
// Code stream: [gadget][rd | rn<<8 | sf<<16 | L<<24 | S<<32]
.macro bfm_shift_gadget name, shiftop
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn
    ubfx x11, x8, #16, #1     // sf
    ubfx x12, x8, #24, #6     // L (left shift)
    ubfx x13, x8, #32, #6     // S (right shift)

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x15, x14, x10, lsl #3
    ldr x16, [x15]
    b 2f
1:  mov x16, #0
2:  cbz x11, 3f
    lsl x16, x16, x12
    \shiftop x16, x16, x13
    b 4f
3:  lsl w16, w16, w12
    \shiftop w16, w16, w13
4:  cmp x9, #31
    b.eq 5f
    add x15, x14, x9, lsl #3
    str x16, [x15]
5:  gret
.endm

bfm_shift_gadget ubfm, lsr
bfm_shift_gadget sbfm, asr

// ---- Bitfield: BFM (insert, preserving untouched destination bits) -----
// Code stream: [gadget][rd | rn<<8 | sf<<16 | L<<24 | S<<32][M]
.gadget bfm
    ldr x8, [_ip]
    ldr x17, [_ip, #8]        // M: insert mask
    add _ip, _ip, #16
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn
    ubfx x11, x8, #16, #1     // sf
    ubfx x12, x8, #24, #6     // L
    ubfx x13, x8, #32, #6     // S

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x15, x14, x10, lsl #3
    ldr x16, [x15]
    b 2f
1:  mov x16, #0
2:  mov x10, #0               // current rd value (rd=31: ZR, result discarded anyway)
    cmp x9, #31
    b.eq 3f
    add x15, x14, x9, lsl #3
    ldr x10, [x15]
3:  cbz x11, 4f
    lsl x16, x16, x12
    lsr x16, x16, x13
    b 5f
4:  lsl w16, w16, w12
    lsr w16, w16, w13
5:  and x16, x16, x17         // field bits
    bic x10, x10, x17         // hole in the destination
    orr x16, x16, x10
    cbnz x11, 6f
    mov w16, w16              // 32-bit form: upper 32 of the result are zero
6:  cmp x9, #31
    b.eq 7f
    add x15, x14, x9, lsl #3
    str x16, [x15]
7:  gret

// ---- EXTR (and its ROR-immediate alias, Rn==Rm) -------------------------
// Code stream: [gadget][rd | rn<<8 | rm<<16 | sf<<24 | lsb<<32]
.gadget extr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn (high half)
    ubfx x11, x8, #16, #5     // rm (low half)
    ubfx x12, x8, #24, #1     // sf
    ubfx x13, x8, #32, #6     // lsb

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x15, x14, x10, lsl #3
    ldr x16, [x15]
    b 2f
1:  mov x16, #0
2:  cmp x11, #31
    b.eq 3f
    add x15, x14, x11, lsl #3
    ldr x17, [x15]
    b 4f
3:  mov x17, #0
4:  cbz x13, extr_lsb0        // lsb=0: result is just rm (an x<<R shift would wrap)
    cbz x12, 5f
    lsr x17, x17, x13
    mov x15, #64
    sub x15, x15, x13
    lsl x16, x16, x15
    orr x17, x17, x16
    b extr_store
5:  lsr w17, w17, w13
    mov w15, #32
    sub w15, w15, w13
    lsl w16, w16, w15
    orr w17, w17, w16
    b extr_store
extr_lsb0:
    cbnz x12, extr_store
    mov w17, w17
extr_store:
    cmp x9, #31
    b.eq 6f
    add x15, x14, x9, lsl #3
    str x17, [x15]
6:  gret

// ---- Condition evaluation gadgets ---------------------------------------
// Each sets w10 = 1 if the (gadget-selected) condition holds against the
// guest NZCV, else 0, then falls through to the next gadget in the block
// (the CSEL-family or CCMP-family consumer). See the file header.
.macro cond_gadget name, cc
.gadget cond_\name
    load_flags
    cset w10, \cc
    gret
.endm

cond_gadget eq, eq
cond_gadget ne, ne
cond_gadget cs, cs
cond_gadget cc, cc
cond_gadget mi, mi
cond_gadget pl, pl
cond_gadget vs, vs
cond_gadget vc, vc
cond_gadget hi, hi
cond_gadget ls, ls
cond_gadget ge, ge
cond_gadget lt, lt
cond_gadget gt, gt
cond_gadget le, le

.gadget cond_al               // AL and NV: architecturally always true
    mov w10, #1
    gret

// ---- Conditional select consumer: CSEL/CSINC/CSINV/CSNEG ---------------
// Code stream: [gadget][rd | rn<<8 | rm<<16 | sf<<24 | op<<25]
// op: 0=CSEL, 1=CSINC, 2=CSINV, 3=CSNEG. Reads w10 from the preceding
// cond_* gadget: condition true -> rn; false -> f(rm).
.gadget csel
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x11, x8, #8, #5      // rn
    ubfx x12, x8, #16, #5     // rm
    ubfx x13, x8, #24, #1     // sf
    ubfx x14, x8, #25, #2     // op

    add x15, _cpu, #CPU_x0
    cbz w10, csel_nottaken
    cmp x11, #31
    b.eq 1f
    add x16, x15, x11, lsl #3
    ldr x17, [x16]
    b csel_mask
1:  mov x17, #0
    b csel_mask
csel_nottaken:
    cmp x12, #31
    b.eq 2f
    add x16, x15, x12, lsl #3
    ldr x17, [x16]
    b 3f
2:  mov x17, #0
3:  cmp x14, #1
    b.lo csel_mask            // op 0: rm as-is
    b.eq 4f
    cmp x14, #2
    b.eq 5f
    neg x17, x17              // op 3: CSNEG
    b csel_mask
4:  add x17, x17, #1          // op 1: CSINC
    b csel_mask
5:  mvn x17, x17              // op 2: CSINV
csel_mask:
    cbnz x13, 6f
    mov w17, w17              // 32-bit form: truncate (inc/inv/neg computed in 64 is
                              // correct in the low 32 bits; upper 32 must read zero)
6:  cmp x9, #31
    b.eq 7f
    add x16, x15, x9, lsl #3
    str x17, [x16]
7:  gret

// ---- Conditional compare consumer: CCMP/CCMN ----------------------------
// Code stream: [gadget][rn | val<<8 | is_imm<<16 | is_cmn<<17 | sf<<18 | nzcv<<19]
// val: imm5 (is_imm=1) or rm (is_imm=0). Condition true -> flags from the
// compare; false -> flags = the immediate nzcv field.
.gadget ccmp
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rn
    ubfx x11, x8, #8, #5      // val
    ubfx x12, x8, #16, #1     // is_imm
    ubfx x13, x8, #17, #1     // is_cmn
    ubfx x14, x8, #18, #1     // sf
    ubfx x15, x8, #19, #4     // nzcv (condition-false result)

    cbnz w10, ccmp_taken
    lsl w15, w15, #28
    str w15, [_cpu, #CPU_nzcv]
    gret
ccmp_taken:
    add x16, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    add x17, x16, x9, lsl #3
    ldr x17, [x17]
    b 2f
1:  mov x17, #0
2:  cbnz x12, 4f              // immediate: val is the operand itself
    cmp x11, #31
    b.eq 3f
    add x11, x16, x11, lsl #3
    ldr x11, [x11]
    b 4f
3:  mov x11, #0
4:  cbnz x13, ccmp_cmn
    cbz x14, 5f
    subs xzr, x17, x11
    b ccmp_flags
5:  subs wzr, w17, w11
    b ccmp_flags
ccmp_cmn:
    cbz x14, 6f
    adds xzr, x17, x11
    b ccmp_flags
6:  adds wzr, w17, w11
ccmp_flags:
    store_flags
    gret

// ---- 2-source: UDIV/SDIV/LSLV/LSRV/ASRV/RORV ----------------------------
// Code stream: [gadget][rd | rn<<8 | rm<<16 | sf<<24 | op<<25]
// op: 0=UDIV, 1=SDIV, 2=LSLV, 3=LSRV, 4=ASRV, 5=RORV. Host udiv/sdiv
// already implement the architected divide-by-zero-yields-zero and
// INT_MIN/-1 overflow-wraps semantics; the variable shifts take their
// amount mod the register width in hardware, exactly the guest semantics.
.gadget dp2src
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn
    ubfx x11, x8, #16, #5     // rm
    ubfx x12, x8, #24, #1     // sf
    ubfx x13, x8, #25, #3     // op

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x15, x14, x10, lsl #3
    ldr x16, [x15]
    b 2f
1:  mov x16, #0
2:  cmp x11, #31
    b.eq 3f
    add x15, x14, x11, lsl #3
    ldr x17, [x15]
    b 4f
3:  mov x17, #0
4:
    cmp x13, #0
    b.eq dp2_udiv
    cmp x13, #1
    b.eq dp2_sdiv
    cmp x13, #2
    b.eq dp2_lslv
    cmp x13, #3
    b.eq dp2_lsrv
    cmp x13, #4
    b.eq dp2_asrv
    cbz x12, 5f
    ror x16, x16, x17
    b dp2_store
5:  ror w16, w16, w17
    b dp2_store
dp2_udiv:
    cbz x12, 6f
    udiv x16, x16, x17
    b dp2_store
6:  udiv w16, w16, w17
    b dp2_store
dp2_sdiv:
    cbz x12, 7f
    sdiv x16, x16, x17
    b dp2_store
7:  sdiv w16, w16, w17
    b dp2_store
dp2_lslv:
    cbz x12, 8f
    lsl x16, x16, x17
    b dp2_store
8:  lsl w16, w16, w17
    b dp2_store
dp2_lsrv:
    cbz x12, 9f
    lsr x16, x16, x17
    b dp2_store
9:  lsr w16, w16, w17
    b dp2_store
dp2_asrv:
    cbz x12, 10f
    asr x16, x16, x17
    b dp2_store
10: asr w16, w16, w17
dp2_store:
    cmp x9, #31
    b.eq 11f
    add x15, x14, x9, lsl #3
    str x16, [x15]
11: gret

// ---- 1-source: RBIT/REV16/REV32(REV w)/REV(x)/CLZ/CLS -------------------
// Code stream: [gadget][rd | rn<<8 | sf<<16 | op<<17]
// op: 0=RBIT, 1=REV16, 2=REV32-or-REV(w), 3=REV(x), 4=CLZ, 5=CLS
.gadget dp1src
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn
    ubfx x11, x8, #16, #1     // sf
    ubfx x12, x8, #17, #3     // op

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x15, x14, x10, lsl #3
    ldr x16, [x15]
    b 2f
1:  mov x16, #0
2:
    cmp x12, #0
    b.eq dp1_rbit
    cmp x12, #1
    b.eq dp1_rev16
    cmp x12, #2
    b.eq dp1_rev32
    cmp x12, #3
    b.eq dp1_rev64
    cmp x12, #4
    b.eq dp1_clz
    cbz x11, 3f
    cls x16, x16
    b dp1_store
3:  cls w16, w16
    b dp1_store
dp1_rbit:
    cbz x11, 4f
    rbit x16, x16
    b dp1_store
4:  rbit w16, w16
    b dp1_store
dp1_rev16:
    cbz x11, 5f
    rev16 x16, x16
    b dp1_store
5:  rev16 w16, w16
    b dp1_store
dp1_rev32:
    cbz x11, 6f
    rev32 x16, x16
    b dp1_store
6:  rev w16, w16
    b dp1_store
dp1_rev64:
    rev x16, x16              // sf=1 only (gen rejects the sf=0 encoding)
    b dp1_store
dp1_clz:
    cbz x11, 7f
    clz x16, x16
    b dp1_store
7:  clz w16, w16
dp1_store:
    cmp x9, #31
    b.eq 8f
    add x15, x14, x9, lsl #3
    str x16, [x15]
8:  gret

// ---- 3-source: MADD/MSUB/widening multiplies/high multiplies ------------
// Code stream: [gadget][rd | rn<<8 | rm<<16 | ra<<24 | sf<<32 | op<<33]
// op: 0=MADD, 1=MSUB, 2=SMADDL, 3=SMSUBL, 4=SMULH, 5=UMADDL, 6=UMSUBL,
// 7=UMULH. The widening/high forms are 64-bit-only (gen enforces sf=1).
.gadget dp3src
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd
    ubfx x10, x8, #8, #5      // rn
    ubfx x11, x8, #16, #5     // rm
    ubfx x12, x8, #24, #5     // ra
    ubfx x13, x8, #32, #1     // sf
    ubfx x14, x8, #33, #3     // op

    add x15, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x16, x15, x10, lsl #3
    ldr x16, [x16]
    b 2f
1:  mov x16, #0
2:  cmp x11, #31
    b.eq 3f
    add x17, x15, x11, lsl #3
    ldr x17, [x17]
    b 4f
3:  mov x17, #0
4:  cmp x12, #31
    b.eq 5f
    add x8, x15, x12, lsl #3
    ldr x8, [x8]              // ra (x8's packed params fully consumed above)
    b 6f
5:  mov x8, #0
6:
    cmp x14, #0
    b.eq dp3_madd
    cmp x14, #1
    b.eq dp3_msub
    cmp x14, #2
    b.eq dp3_smaddl
    cmp x14, #3
    b.eq dp3_smsubl
    cmp x14, #4
    b.eq dp3_smulh
    cmp x14, #5
    b.eq dp3_umaddl
    cmp x14, #6
    b.eq dp3_umsubl
    umulh x16, x16, x17
    b dp3_store
dp3_madd:
    cbz x13, 7f
    madd x16, x16, x17, x8
    b dp3_store
7:  madd w16, w16, w17, w8
    b dp3_store
dp3_msub:
    cbz x13, 8f
    msub x16, x16, x17, x8
    b dp3_store
8:  msub w16, w16, w17, w8
    b dp3_store
dp3_smaddl:
    smaddl x16, w16, w17, x8
    b dp3_store
dp3_smsubl:
    smsubl x16, w16, w17, x8
    b dp3_store
dp3_smulh:
    smulh x16, x16, x17
    b dp3_store
dp3_umaddl:
    umaddl x16, w16, w17, x8
    b dp3_store
dp3_umsubl:
    umsubl x16, w16, w17, x8
dp3_store:
    cmp x9, #31
    b.eq 9f
    add x15, _cpu, #CPU_x0
    add x15, x15, x9, lsl #3
    str x16, [x15]
9:  gret

// ---- Barriers ------------------------------------------------------------
// One gadget for the whole DSB/DMB/ISB space: a full `dmb ish` is at
// least as strong as any of them needs on this single-emulated-thread
// execution model, and is cheap.
.gadget barrier
    dmb ish
    gret

// ---- System registers ----------------------------------------------------
// MRS Rt, TPIDR_EL0 / MSR TPIDR_EL0, Rt — the TLS base register.
// Code stream: [gadget][rt]
.gadget mrs_tpidr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    cmp x8, #31
    b.eq 1f
    ldr x9, [_cpu, #CPU_tpidr]
    add x10, _cpu, #CPU_x0
    add x10, x10, x8, lsl #3
    str x9, [x10]
1:  gret

.gadget msr_tpidr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    mov x9, #0
    cmp x8, #31
    b.eq 1f
    add x10, _cpu, #CPU_x0
    add x10, x10, x8, lsl #3
    ldr x9, [x10]
1:  str x9, [_cpu, #CPU_tpidr]
    gret

// MRS Rt, <constant system register> (CTR_EL0, DCZID_EL0, ...): the value
// is a compile-time constant chosen by gen_step_arm64.
// Code stream: [gadget][rt][value]
.gadget mrs_const
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x8, x8, #0x1f
    cmp x8, #31
    b.eq 1f
    add x10, _cpu, #CPU_x0
    add x10, x10, x8, lsl #3
    str x9, [x10]
1:  gret

// MRS Xt, NZCV / MSR NZCV, Xt: the guest flags live at CPU_nzcv in the
// architectural bit positions (31:28), so this is a plain 32-bit
// load/store. Ported from OpenMinis' d53b4200/d51b4200 branches.
// [gadget][rt]
.gadget mrs_nzcv
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    cmp x8, #31
    b.eq 1f
    ldr w9, [_cpu, #CPU_nzcv]
    add x10, _cpu, #CPU_x0
    str x9, [x10, x8, lsl #3]
1:  gret

.gadget msr_nzcv
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    mov x9, #0
    cmp x8, #31
    b.eq 1f
    add x10, _cpu, #CPU_x0
    ldr x9, [x10, x8, lsl #3]
1:  and w9, w9, #0xf0000000
    str w9, [_cpu, #CPU_nzcv]
    gret

// MRS Xt, CNTVCT_EL0: virtual counter read at runtime from the host
// monotonic clock (nanoseconds; CNTFRQ_EL0 reports 1 GHz to match).
// [gadget][rt]
.gadget mrs_cntvct
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x19, x8, #0x1f         // rt, callee-saved across the C call
    save_c
    bl NAME(arm64_cntvct)
    mov x20, x0                // result; x0 itself is restored by restore_c
    restore_c
    cmp x19, #31
    b.eq 1f
    add x10, _cpu, #CPU_x0
    str x20, [x10, x19, lsl #3]
1:  gret

// CRC32{B,H,W,X} / CRC32C{B,H,W,X}: Wd = crc(Wn, Rm<sz>). FEAT_CRC32 is
// optional in v8.0 and Apple only added it with A10 — gen.c hands this
// native gadget out only when the host has it (crc32_soft below covers
// the rest). Xcode's default -arch arm64 assembler target doesn't
// advertise the extension — enable it explicitly for this file.
// [gadget][rd | rn<<8 | rm<<16 | sz<<24 | c<<26]
.arch_extension crc
.gadget crc32
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2      // size: 0=b 1=h 2=w 3=x
    ubfx x13, x8, #26, #1      // 1 = CRC32C
    add x14, _cpu, #CPU_x0
    mov x15, #0
    cmp x10, #31
    b.eq 1f
    add x17, x14, x10, lsl #3
    ldr w15, [x17]             // Wn (acc)
1:  mov x16, #0
    cmp x11, #31
    b.eq 2f
    ldr x16, [x14, x11, lsl #3] // Rm
2:  cbnz x13, crc32_c
    cmp x12, #0
    b.eq 3f
    cmp x12, #1
    b.eq 4f
    cmp x12, #2
    b.eq 5f
    crc32x w15, w15, x16
    b crc32_st
3:  crc32b w15, w15, w16
    b crc32_st
4:  crc32h w15, w15, w16
    b crc32_st
5:  crc32w w15, w15, w16
    b crc32_st
crc32_c:
    cmp x12, #0
    b.eq 6f
    cmp x12, #1
    b.eq 7f
    cmp x12, #2
    b.eq 8f
    crc32cx w15, w15, x16
    b crc32_st
6:  crc32cb w15, w15, w16
    b crc32_st
7:  crc32ch w15, w15, w16
    b crc32_st
8:  crc32cw w15, w15, w16
crc32_st:
    cmp x9, #31
    b.eq 9f
    str x15, [x14, x9, lsl #3] // Wd zero-extends
9:  gret

// Soft CRC32 fallback for hosts without FEAT_CRC32 (pre-A10): computes
// in C (arm64_crc32_soft, emu/tlb.c). Same code stream as crc32 above:
// [gadget][rd | rn<<8 | rm<<16 | sz<<24 | c<<26]
.gadget crc32_soft
    ldr x8, [_ip]
    add _ip, _ip, #8
    save_c
    and x19, x8, #0x1f          // rd, callee-saved across the C call
    ubfx x9, x8, #8, #5         // rn
    ubfx x10, x8, #16, #5       // rm
    add x11, _cpu, #CPU_x0
    mov x0, #0
    cmp x9, #31
    b.eq 1f
    // w-sized load can't use `lsl #3` addressing (see memory.S header,
    // bug #2): compute the 8-byte-strided slot address separately.
    add x12, x11, x9, lsl #3
    ldr w0, [x12]               // Wn (acc)
1:  mov x1, #0
    cmp x10, #31
    b.eq 2f
    ldr x1, [x11, x10, lsl #3]  // Rm (value)
2:  ubfx x2, x8, #24, #2        // size: 0=b 1=h 2=w 3=x
    ubfx x3, x8, #26, #1        // 1 = CRC32C
    bl NAME(arm64_crc32_soft)
    mov w20, w0                 // zero-extend: Wd write clears the top
    restore_c
    cmp x19, #31
    b.eq 3f
    add x10, _cpu, #CPU_x0
    str x20, [x10, x19, lsl #3]
3:  gret

# vim: ft=gas

// MRS/MSR FPCR and FPSR, through emu/fpenv.c: a written FPCR goes onto the
// host FPU at once, so the guest's rounding mode, FZ and DN govern the very
// next instruction; a read of FPSR gathers the flags the host FPU has raised
// since (IOC..IXC, IDC, and QC from the saturating ops). They were plain
// cpu_state loads and stores, which no FP operation ever looked at.
// Code stream: [gadget][rt]
.macro fpsysreg_gadget name, op, is_read
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    save_c
    and x19, x8, #0x1f          // rt, callee-saved across the C call
    mov x2, #0
.if !\is_read
    cmp x19, #31
    b.eq 1f
    add x9, _cpu, #CPU_x0
    ldr x2, [x9, x19, lsl #3]   // Xt (XZR writes 0)
1:
.endif
    mov x0, _cpu                // _cpu IS x1: read it before x1 is loaded
    mov x1, #\op
    bl NAME(fpenv_arm64_sysreg)
    mov x20, x0
    restore_c
.if \is_read
    cmp x19, #31
    b.eq 2f
    add x9, _cpu, #CPU_x0
    str x20, [x9, x19, lsl #3]
2:
.endif
    gret
.endm

fpsysreg_gadget mrs_fpcr, 0, 1
fpsysreg_gadget msr_fpcr, 1, 0
fpsysreg_gadget mrs_fpsr, 2, 1
fpsysreg_gadget msr_fpsr, 3, 0
