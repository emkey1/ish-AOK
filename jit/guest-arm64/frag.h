// Gadget bodies as macros, so that one body serves the gadget it always was
// and the fused pairs of pairs.h (alu_spec.S's "Fused pairs").
//
// Under the gadget JIT a guest instruction costs a dispatch -- the load of
// the next gadget's address, the indirect branch, and on the hosts measured
// the two taken branches that bound how fast gadgets can follow each other --
// plus a few host instructions of real work. Two guest instructions that
// always run one after the other can share one dispatch: the pair gadget is
// the first body, then the second, then the dispatch. Nothing else changes.
// Each body still reads its own parameters from the stream and commits its
// result to cpu_state before the next begins, so a fault or a signal in the
// second finds the first already done, as it would between two gadgets.
//
// A family FOO is two macros:
//
//     FOO_body id, last, a1, a2, a3, a4, a5
//     FOO_stub id, last, a1, a2, a3, a4, a5
//
// The body is entered with _ip at its first parameter. With `last` 0 it
// leaves _ip at the word after its parameters and falls through; with `last`
// 1 it ends in the dispatch. The stub is the body's out-of-line code (TLB
// miss, page crossing), placed after everything that falls through; `id`
// makes its labels unique. a1..a5 are the family's own arguments; unused
// ones are passed as `_`.
//
// The standalone gadgets are defined from the same bodies with `last` 1
// (alu_spec.S, memory.S, simd_spec.S, math.S), so a pair cannot drift from
// the instructions it is made of.
//
// A second host. These bodies, alu_spec.S and pairs.h are also the source of
// the arm64 front end of the QEMU plug-in (qemu-aok, target/arm/tcg/aok),
// which assembles copies of them unchanged, as it does guest-riscv64's
// (frag.h there has the longer account). A host that is not iSH-AOK defines
// A64_GADGET_HOST, and before including a shared file supplies what
// gadgets.h supplies here:
//
//   _cpu, _ip     the guest state every slot offset is relative to, and the
//                 stream pointer
//   .gadget NAME  begins a gadget; gret N ends one, N parameter words on;
//                 gret_ahead for a body that has already moved _ip past the
//                 next gadget's word (f_ip_past)
//   load_flags    the guest's NZCV into the host's flags, and store_flags
//   store_flags   back; they may use x16 and x17 and nothing else
//   _addr         the address of an access; read_prep BITS, ID, AHEAD and
//                 write_prep turn it from a guest address into a host one,
//                 read_bullshit/write_bullshit are their out-of-line halves,
//                 write_done BITS, ID follows a store. BITS is the size of
//                 the whole access, both halves of a pair
//   _fofs, _fslot1, _fslot2, _fnewb, _frn, _fval2
//                 what a load or store body keeps across the prep macros
//                 besides x8 and x17: registers the host's prep and its slow
//                 path preserve (here the callee-saved x19 and x21-x25)
//   F_WAY_WORDS, branch_leave OFF, pc_param REG
//                 how a block is left, as in guest-riscv64's frag.h: a way
//                 out is F_WAY_WORDS parameter words, branch_leave OFF leaves
//                 by the one at [_ip, #OFF] and does not come back (give each
//                 its own indirect branch), and pc_param REG turns a parameter
//                 that is a guest code address as the translator wrote it
//                 into the address
//   F_X0          the offset of X0 in the guest state, X1-X30 and SP after it
//                 eight bytes apart: the compare-and-branch bodies' operands
//                 are register numbers
//
// The indirect branches (br_cached, blr_cached, ret_cached) are a host's
// own, since how a host finds the code for an address is: one that has them
// defines f_br_cached_body and f_blr_cached_body (and their _stub), taking a
// forwarded target as the families here take a forwarded operand, and gets
// the pairs that end in them.
//
// A body uses x8-x17 and those, reads its operands through _ip,
// and reaches guest state only as [_cpu, slot]. The last parameter word of a
// load or store belongs to the host (here the instruction's address, for the
// fault paths).

// A body's source operand, into host register x<dst>: loaded from its
// cpu_state slot (whose offset is in `slotreg`), or -- `fw` a register number
// -- moved from the host register the first half of the pair left that very
// guest register in (see fuse2). The bodies that produce a result leave it in
// x12, the loads in x17.
.macro f_src dst, slotreg, fw
.ifc \fw,_
    ldr x\dst, [_cpu, \slotreg]
.else
.if \dst != \fw
    mov x\dst, x\fw
.endif
.endif
.endm

// A body that cannot fault: one update of _ip, in the dispatch when it is last.
.macro f_end last, n
.if \last
    gret \n
.else
    add _ip, _ip, #(\n * 8)
.endif
.endm

// A body that can fault moves _ip before its access, past its own
// parameters (four, or `n`; the fault paths find the instruction's address in
// the last of them) -- and, when it is the last body, past the next gadget's word in the
// same move, dispatching through gret_ahead (gadgets.h).
.macro f_ip_past last, n=4
.if \last
    add _ip, _ip, #(\n * 8 + 8)
.else
    add _ip, _ip, #(\n * 8)
.endif
.endm
.macro f_end_ahead last
.if \last
    gret_ahead
.endif
.endm
.macro f_read_prep last, bits, id
.if \last
    read_prep \bits, \id, 8
.else
    read_prep \bits, \id
.endif
.endm
.macro f_write_prep last, bits, id
.if \last
    write_prep \bits, \id, 8
.else
    write_prep \bits, \id
.endif
.endm
.macro f_read_bullshit last, bits, id
.if \last
    read_bullshit \bits, \id, 8
.else
    read_bullshit \bits, \id
.endif
.endm
.macro f_write_bullshit last, bits, id
.if \last
    write_bullshit \bits, \id, 8
.else
    write_bullshit \bits, \id
.endif
.endm

// ---- Integer ALU, precomputed slots (alu_spec.S) -------------------------

// [d_off][n_off][imm]: op, r, flags
.macro f_alu_imm_body id, last, op, r, flags, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    f_src 12, x10, \fn
    \op \r\()12, \r\()12, \r\()11
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_alu_imm_stub id, last, op, r, flags, a4, a5, fn=_, fm=_
.endm

// [d_off][n_off][m_off]: op, r, flags
.macro f_alu_reg_body id, last, op, r, flags, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    f_src 13, x11, \fm       // (m first: n's register is the one a forward comes in)
    f_src 12, x10, \fn
    \op \r\()12, \r\()12, \r\()13
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_alu_reg_stub id, last, op, r, flags, a4, a5, fn=_, fm=_
.endm

// [d_off][n_off][m_off][shift]: op, r
.macro f_alu_reg_lsl_body id, last, op, r, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
    lslv \r\()13, \r\()13, \r\()14
    \op \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_alu_reg_lsl_stub id, last, op, r, a3, a4, a5, fn=_, fm=_
.endm

// One operation on a register and a register shifted by a constant: the
// logical operations and their inverted forms with any of the four shifts,
// ADD and SUB with LSR and ASR (LSL has f_alu_reg_lsl). The rotates and
// shift-xors of hashing. [d_off][n_off][m_off][amount]: op, r, shv
.macro f_alu_reg_sh_body id, last, op, r, shv, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
    \shv \r\()13, \r\()13, \r\()14
    \op \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_alu_reg_sh_stub id, last, op, r, shv, a4, a5, fn=_, fm=_
.endm

// ADD and SUB with the second register extended from 8, 16 or 32 bits and
// shifted left 0-4: array indexing by a narrower index. Either of rd and rn
// may be SP (their slots say). [d_off][n_off][m_off][shift]: op, r, ext
.macro f_alu_ext_body id, last, op, r, ext, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
.ifc \ext,uxtw
    mov w13, w13
.endif
.ifc \ext,sxtw
    sxtw x13, w13
.endif
.ifc \ext,uxtb
    and x13, x13, #0xff
.endif
.ifc \ext,uxth
    and x13, x13, #0xffff
.endif
.ifc \ext,sxtb
    sxtb x13, w13
.endif
.ifc \ext,sxth
    sxth x13, w13
.endif
    lslv x13, x13, x14
    \op \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_alu_ext_stub id, last, op, r, ext, a4, a5, fn=_, fm=_
.endm

// MVN. [d_off][m_off]: r
.macro f_mvn_body id, last, r, a2, a3, a4, a5, fn=_, fm=_
    ldp x8, x10, [_ip]
    f_src 12, x10, \fn
    mvn \r\()12, \r\()12
    str x12, [_cpu, x8]
    f_end \last, 2
.endm
.macro f_mvn_stub id, last, r, a2, a3, a4, a5, fn=_, fm=_
.endm

// ADC/ADCS/SBC/SBCS with XZR as the second operand: adding the carry in.
// [d_off][n_off]: op, r, flags, nf (see f_adc)
.macro f_adc_z_body id, last, op, r, flags, nf, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    f_src 12, x10, \fn
.if \nf == 0
    load_flags
.endif
    \op \r\()12, \r\()12, \r\()zr
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 2
.endm
.macro f_adc_z_stub id, last, op, r, flags, nf, a5, fn=_, fm=_
.endm

// Add/subtract with carry (ADC, ADCS, SBC, SBCS): the guest's carry comes in
// through the host's flags. `nf`: the gadget before this one left the host's
// NZCV equal to the guest's (a flag-setting ALU gadget, or another of these --
// the carry chain of a multi-word add), so the reload, a serialising `msr`, is
// skipped. Either way the host's flags are the guest's when this is done.
// [d_off][n_off][m_off]: op, r, flags, nf
.macro f_adc_body id, last, op, r, flags, nf, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
.if \nf == 0
    load_flags
.endif
    \op \r\()12, \r\()12, \r\()13
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_adc_stub id, last, op, r, flags, nf, a5, fn=_, fm=_
.endm

// Multiply-add and multiply-subtract: [d_off][n_off][m_off][a_off]: op, r.
// (MUL, UMULH and SMULH are f_alu_reg: one operation on two registers.)
.macro f_madd_body id, last, op, r, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    ldr x14, [_cpu, x14]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
    \op \r\()12, \r\()12, \r\()13, \r\()14
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_madd_stub id, last, op, r, a3, a4, a5, fn=_, fm=_
.endm

// UMULL and SMULL: 32 x 32 -> 64. [d_off][n_off][m_off]: op
.macro f_mull_body id, last, op, a2, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    f_src 13, x11, \fm
    f_src 12, x10, \fn
    \op x12, w12, w13
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_mull_stub id, last, op, a2, a3, a4, a5, fn=_, fm=_
.endm

// Bitfield extract and the immediate shifts (UBFM, SBFM and their aliases):
// (src << L) >> S, logical or arithmetic -- dpextra.S has the lowering.
// [d_off][n_off][L][S]: shiftop, r
.macro f_bfm_body id, last, shiftop, r, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    ldp x11, x13, [_ip, #16]
    f_src 12, x10, \fn
    lslv \r\()12, \r\()12, \r\()11
    \shiftop \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_bfm_stub id, last, shiftop, r, a3, a4, a5, fn=_, fm=_
.endm

// ---- Moves ----------------------------------------------------------------

// Register to register, zero-extending (simd_spec.S's vspec_umov*, which is
// also what `mov Xd, Xm` and `mov Wd, Wm` become). [d_off][n_off]: ld, reg
// (x12 or w12 for a pair's forwarding; any w register otherwise)
.macro f_umov_body id, last, ld, reg, a3, a4, a5, fn=_, fm=_
    ldp x8, x10, [_ip]
.ifc \fn,_
    \ld \reg, [_cpu, x10]
.else
.ifc \reg,x12
    f_src 12, x10, \fn
.else
    mov w12, w\fn            // the 32-bit move: the low half, zero-extended
.endif
.endif
    str x12, [_cpu, x8]
    f_end \last, 2
.endm
.macro f_umov_stub id, last, ld, reg, a3, a4, a5, fn=_, fm=_
.endm

// A constant (math.S's mov_const). [d_off][value]
.macro f_mov_const_body id, last, a1, a2, a3, a4, a5, fn=_, fm=_
    ldp x8, x12, [_ip]
    str x12, [_cpu, x8]
    f_end \last, 2
.endm
.macro f_mov_const_stub id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.endm

// ---- Loads and stores, offset form (memory.S) ----------------------------

// [rt_slot][rn_slot][offset][orig_ip]: bits, ldinsn, reg
.macro f_load_body id, last, bits, ldinsn, reg, a4, a5, fn=_, fm=_
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldr _fofs, [_ip, #16]       // offset
    f_ip_past \last
    f_src 10, x9, \fn        // base
    add _addr, x10, _fofs
    f_read_prep \last, \bits, \id
    \ldinsn \reg, [_addr]
    str x17, [_cpu, x8]       // full X store: 32-bit loads must clear the upper half
    f_end_ahead \last
.endm
.macro f_load_stub id, last, bits, ldinsn, reg, a4, a5, fn=_, fm=_
    f_read_bullshit \last, \bits, \id
.endm

// [rt_slot][rn_slot][offset][orig_ip]: bits, stinsn, reg
.macro f_store_body id, last, bits, stinsn, reg, a4, a5, fn=_, fm=_
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldr _fofs, [_ip, #16]       // offset
    f_ip_past \last
    f_src 10, x9, \fm        // base (before the value: a forward may come in x17)
    f_src 17, x8, \fn        // value (x17 survives write_prep's helpers)
    add _addr, x10, _fofs
    f_write_prep \last, \bits, \id
    \stinsn \reg, [_addr]
    write_done \bits, \id     // clobbers x9 — rt_slot already consumed
    f_end_ahead \last
.endm
.macro f_store_stub id, last, bits, stinsn, reg, a4, a5, fn=_, fm=_
    f_write_bullshit \last, \bits, \id
.endm

// Register-offset form: the index register extended (0: none, 1: sxtw,
// 2: uxtw) and shifted. [rt_slot][rn_slot][rm_slot][shift][orig_ip]:
// bits, insn, reg, ext
// (`sh`: the register the shift count is in. The bodies here use x13: x12 is
// where a pair's first half leaves its result.)
.macro rx_index ext, fw=_, sh=x12
    f_src 11, x11, \fw
.if \ext == 1
    sxtw x11, w11
.elseif \ext == 2
    mov w11, w11
.endif
    lslv x11, x11, \sh
.endm

.macro f_load_rx_body id, last, bits, ldinsn, reg, ext, a5, fn=_, fm=_
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldp x11, x13, [_ip, #16]  // rm_slot, shift
    f_ip_past \last, 5
    f_src 10, x9, \fn
    rx_index \ext, \fm, x13
    add _addr, x10, x11
    f_read_prep \last, \bits, \id
    \ldinsn \reg, [_addr]
    str x17, [_cpu, x8]
    f_end_ahead \last
.endm
.macro f_load_rx_stub id, last, bits, ldinsn, reg, ext, a5, fn=_, fm=_
    f_read_bullshit \last, \bits, \id
.endm

.macro f_store_rx_body id, last, bits, stinsn, reg, ext, a5, fn=_, fm=_
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldp x11, x13, [_ip, #16]  // rm_slot, shift
    f_ip_past \last, 5
    f_src 10, x9, \fm        // base; the index is not forwarded
    rx_index \ext, _, x13
    add _addr, x10, x11
    f_src 17, x8, \fn        // value
    f_write_prep \last, \bits, \id
    \stinsn \reg, [_addr]
    write_done \bits, \id
    f_end_ahead \last
.endm
.macro f_store_rx_stub id, last, bits, stinsn, reg, ext, a5, fn=_, fm=_
    f_write_bullshit \last, \bits, \id
.endm

// Pairs, offset form. [rt_slot | rt2_slot<<16][rn_slot][offset][orig_ip]:
// bits, bytes, reg1, reg2
.macro f_ldp_body id, last, bits, bytes, ldr_reg1, ldr_reg2, a5, fn=_, fm=_
    ldp x8, x9, [_ip]          // [rt_slot | rt2_slot<<16], [rn_slot]
    ldr _fofs, [_ip, #16]        // offset
    f_ip_past \last
    f_src 10, x9, \fn         // base from precomputed rn_slot
    add _addr, x10, _fofs
    f_read_prep \last, \bits, \id
    ldr \ldr_reg1, [_addr]
    ldr \ldr_reg2, [_addr, #\bytes]
    and x9, x8, #0xffff        // rt_slot (low 16 bits)
    str x17, [_cpu, x9]        // full X store: 32-bit loads must clear upper half
    ubfx x9, x8, #16, 16      // rt2_slot (high 16 bits)
    str _fval2, [_cpu, x9]        // full X store: 32-bit loads must clear upper half
    f_end_ahead \last
.endm
.macro f_ldp_stub id, last, bits, bytes, ldr_reg1, ldr_reg2, a5, fn=_, fm=_
    f_read_bullshit \last, \bits, \id
.endm

.macro f_stp_body id, last, bits, bytes, str_reg1, str_reg2, a5, fn=_, fm=_
    ldp x8, x9, [_ip]          // [rt_slot | rt2_slot<<16], [rn_slot]
    ldr _fofs, [_ip, #16]        // offset
    f_ip_past \last
    and _fslot1, x8, #0xffff       // rt_slot (callee-saved, survives write_prep)
    ubfx _fslot2, x8, #16, #16    // rt2_slot (callee-saved, survives write_prep)
    f_src 10, x9, \fn         // base from precomputed rn_slot (before x17 is a value)
    ldr \str_reg1, [_cpu, _fslot1] // load value from rt slot
    ldr \str_reg2, [_cpu, _fslot2] // load value from rt2 slot
    add _addr, x10, _fofs
    f_write_prep \last, \bits, \id
    str \str_reg1, [_addr]
    str \str_reg2, [_addr, #\bytes]
    write_done \bits, \id
    f_end_ahead \last
.endm
.macro f_stp_stub id, last, bits, bytes, str_reg1, str_reg2, a5, fn=_, fm=_
    f_write_bullshit \last, \bits, \id
.endm

// Pairs with writeback: the new base (base + offset) is computed before the
// access and written to rn after it, so a fault leaves rn untouched. _fnewb
// holds the new base and _frn rn's slot across the prep helpers. Same stream
// as the offset form: bits, bytes, reg1, reg2, pre
.macro f_ldp_wb_body id, last, bits, bytes, ldr_reg1, ldr_reg2, pre, fn=_, fm=_
    ldp x8, _frn, [_ip]
    ldr _fofs, [_ip, #16]
    f_ip_past \last
    f_src 10, _frn, \fn
    add _fnewb, x10, _fofs
.if \pre
    mov _addr, _fnewb
.else
    mov _addr, x10
.endif
    f_read_prep \last, \bits, \id
    ldr \ldr_reg1, [_addr]
    ldr \ldr_reg2, [_addr, #\bytes]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]
    ubfx x9, x8, #16, 16
    str _fval2, [_cpu, x9]
    str _fnewb, [_cpu, _frn]
    f_end_ahead \last
.endm
.macro f_ldp_wb_stub id, last, bits, bytes, ldr_reg1, ldr_reg2, pre, fn=_, fm=_
    f_read_bullshit \last, \bits, \id
.endm

.macro f_stp_wb_body id, last, bits, bytes, str_reg1, str_reg2, pre, fn=_, fm=_
    ldp x8, _frn, [_ip]
    ldr _fofs, [_ip, #16]
    f_ip_past \last
    and _fslot1, x8, #0xffff
    ubfx _fslot2, x8, #16, #16
    f_src 10, _frn, \fn
    ldr \str_reg1, [_cpu, _fslot1]
    ldr \str_reg2, [_cpu, _fslot2]
    add _fnewb, x10, _fofs
.if \pre
    mov _addr, _fnewb
.else
    mov _addr, x10
.endif
    f_write_prep \last, \bits, \id
    str \str_reg1, [_addr]
    str \str_reg2, [_addr, #\bytes]
    write_done \bits, \id
    str _fnewb, [_cpu, _frn]
    f_end_ahead \last
.endm
.macro f_stp_wb_stub id, last, bits, bytes, str_reg1, str_reg2, pre, fn=_, fm=_
    f_write_bullshit \last, \bits, \id
.endm

// ---- Branches (control.S) ------------------------------------------------
// A branch ends its block, so it is only ever the second of a pair, and its
// `last` is always 1. The ways out are parameters it does not read itself
// (branch_leave's). Each conditional form has two layouts, picked by the
// translator from the branch's direction: the plain one keeps the
// fallthrough straight-line (forward branches, mostly not taken), _bk the
// taken path (backward ones, loops). Whichever path jumps inside the gadget
// costs ~1.3 ns more on the A10X (measured on guest-riscv64's twins).

#ifndef A64_GADGET_HOST
// iSH-AOK: a way out is one word, the next block's code (or the dispatch's
// tagged word), and a guest address in the stream is the address.
#define F_WAY_WORDS 1
#define F_X0 CPU_x0
.macro branch_leave off
    ldr _ip, [_ip, #\off]
    branch_dispatch
.endm
.macro pc_param reg
.endm

// The indirect branches, by the return cache (control.S's
// ret_cache_dispatch).
// BR, and RET, which is BR X30 with a different hint. [n_off]
.macro f_br_cached_body id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.ifc \fn,_
    ldr x8, [_ip]             // the register's offset (the translator: never XZR)
    ldr x10, [_cpu, x8]
.else
    mov x10, x\fn            // the target, from the pair's first half
.endif
    ret_cache_dispatch x10
.endm
.macro f_br_cached_stub id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.endm

// [n_off][return address]
.macro f_blr_cached_body id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.ifc \fn,_
    ldr x8, [_ip]
    ldr x10, [_cpu, x8]
.else
    mov x10, x\fn
.endif
    ldr x11, [_ip, #8]      // return address
    pc_param x11
    str x11, [_cpu, #(F_X0 + 30*8)]  // X30 = return address, AFTER the target is read
    ret_cache_dispatch x10
.endm
.macro f_blr_cached_stub id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.endm
#endif

// [way]
.macro f_b_body id, last, a1, a2, a3, a4, a5, fn=_, fm=_
    branch_leave 0
.endm
.macro f_b_stub id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.endm

// [way][return address]
.macro f_bl_body id, last, a1, a2, a3, a4, a5, fn=_, fm=_
    ldr x9, [_ip, #(F_WAY_WORDS * 8)]
    pc_param x9
    add x10, _cpu, #F_X0
    str x9, [x10, #(30*8)]  // X30 = return address
    branch_leave 0
.endm
.macro f_bl_stub id, last, a1, a2, a3, a4, a5, fn=_, fm=_
.endm

// CBZ/CBNZ: [rt_off][taken][fallthrough]; reg, jmp (jmp_bk for _bk)
.macro f_cb_body id, last, reg, jmp, a3, a4, a5, fn=_, fm=_
.ifc \fn,_
    ldr x9, [_ip]
    ldr x9, [_cpu, x9]
.else
    mov x9, x\fn
.endif
    \jmp \reg, 1f
    branch_leave (8+F_WAY_WORDS*8)      // fallthrough
1:  branch_leave 8                          // taken
.endm
.macro f_cb_stub id, last, reg, jmp, a3, a4, a5, fn=_, fm=_
.endm
.macro f_cb_bk_body id, last, reg, jmp_bk, a3, a4, a5, fn=_, fm=_
.ifc \fn,_
    ldr x9, [_ip]
    ldr x9, [_cpu, x9]
.else
    mov x9, x\fn
.endif
    \jmp_bk \reg, 1f
    branch_leave 8                          // taken
1:  branch_leave (8+F_WAY_WORDS*8)      // fallthrough
.endm
.macro f_cb_bk_stub id, last, reg, jmp_bk, a3, a4, a5, fn=_, fm=_
.endm

// TBZ/TBNZ: [rt_off][bit mask][taken][fallthrough]; jmp (jmp_bk for _bk)
.macro f_tb_body id, last, jmp, a2, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]       // rt_off, mask
    f_src 9, x9, \fn
    and x9, x9, x10
    \jmp x9, 1f
    branch_leave (16+F_WAY_WORDS*8)     // fallthrough
1:  branch_leave 16                         // taken
.endm
.macro f_tb_stub id, last, jmp, a2, a3, a4, a5, fn=_, fm=_
.endm
.macro f_tb_bk_body id, last, jmp_bk, a2, a3, a4, a5, fn=_, fm=_
    ldp x9, x10, [_ip]
    f_src 9, x9, \fn
    and x9, x9, x10
    \jmp_bk x9, 1f
    branch_leave 16                         // taken
1:  branch_leave (16+F_WAY_WORDS*8)     // fallthrough
.endm
.macro f_tb_bk_stub id, last, jmp_bk, a2, a3, a4, a5, fn=_, fm=_
.endm

// B.cond: [taken][fallthrough]; cc
.macro f_bcond_body id, last, cc, a2, a3, a4, a5, fn=_, fm=_
    load_flags
    b.\cc 1f
    branch_leave (F_WAY_WORDS*8)
1:  branch_leave 0
.endm
.macro f_bcond_stub id, last, cc, a2, a3, a4, a5, fn=_, fm=_
.endm

// B.cond without the flags' reload, straight after a gadget that set them:
// the host's NZCV is still live from that gadget's adds/subs/ands, because
// nothing in its tail (store_flags) nor in a dispatch writes flags. `msr
// nzcv` is serializing on Apple cores, so skipping it (and the flags' load)
// is the biggest single saving in a compare-and-branch loop. The translator
// picks these only when the instruction before, in the same block, set the
// flags with such a gadget.
.macro f_bcond_nf_body id, last, cc, a2, a3, a4, a5, fn=_, fm=_
    b.\cc 1f
    branch_leave (F_WAY_WORDS*8)
1:  branch_leave 0
.endm
.macro f_bcond_nf_stub id, last, cc, a2, a3, a4, a5, fn=_, fm=_
.endm

// A compare and the B.cond after it, one gadget: the compare runs natively,
// the guest's NZCV is still stored (a later instruction may read it), and
// the branch tests the live host flags. Operands are register numbers (F_X0).
//   fused_cmpi: [rn][imm][taken][fallthrough]; cmpop, zr, r15, r11, cc
//   fused_cmpr: [rn | rm<<8][taken][fallthrough]; cmpop, zr, r15, r13, cc
.macro f_fused_cmpi_body id, last, cmpop, zr, r15, r11, cc, fn=_, fm=_
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
.ifc \fn,_
    add x14, _cpu, #F_X0
    ldr x15, [x14, x10, lsl #3]
.else
    mov x15, x\fn
.endif
    \cmpop \zr, \r15, \r11
    store_flags
    b.\cc 1f
    branch_leave (F_WAY_WORDS*8)
1:  branch_leave 0
.endm
.macro f_fused_cmpi_stub id, last, cmpop, zr, r15, r11, cc, fn=_, fm=_
.endm

.macro f_fused_cmpr_body id, last, cmpop, zr, r15, r13, cc, fn=_, fm=_
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x16, x8, #8, #5      // (not x12: a pair's first half may have left its result there)
    add x14, _cpu, #F_X0
.ifc \fm,_
    ldr x13, [x14, x16, lsl #3]
.else
    mov x13, x\fm
.endif
.ifc \fn,_
    ldr x15, [x14, x10, lsl #3]
.else
    mov x15, x\fn
.endif
    \cmpop \zr, \r15, \r13
    store_flags
    b.\cc 1f
    branch_leave (F_WAY_WORDS*8)
1:  branch_leave 0
.endm
.macro f_fused_cmpr_stub id, last, cmpop, zr, r15, r13, cc, fn=_, fm=_
.endm

// A standalone gadget from a family, and a fused pair of two -- and, when the
// first half leaves a result in a host register (`ra`: its number, 12 or 17;
// `_` for none) and the second has operands it could take from one (`kb` of
// them), the pair again with the second half's first operand (f2n_) or its
// second (f2m_) FORWARDED: taken from that register instead of loaded from
// the slot the first half has just stored it to. A guest instruction that
// uses what the one before it produced is the common case, and through the
// slot each such step waits out a store and a load of the same address. The
// translator picks the forwarding gadget when the slots are the same one
// (gen.c's jit_pair_fuse; the `fwd` bit of /proc/ish/arm64_jit_fuse). The
// first half still stores its result: the register file is always current.
.macro f_gadget name, fam, a1, a2, a3, a4, a5
.gadget \name
    \fam\()_body \name, 1, \a1, \a2, \a3, \a4, \a5
    \fam\()_stub \name, 1, \a1, \a2, \a3, \a4, \a5
.endm

.macro fuse2_one sym, fa, a1, a2, a3, a4, a5, fb, b1, b2, b3, b4, b5, fn, fm
.gadget \sym
    \fa\()_body \sym\()_a, 0, \a1, \a2, \a3, \a4, \a5
    \fb\()_body \sym\()_b, 1, \b1, \b2, \b3, \b4, \b5, \fn, \fm
    \fa\()_stub \sym\()_a, 0, \a1, \a2, \a3, \a4, \a5
    \fb\()_stub \sym\()_b, 1, \b1, \b2, \b3, \b4, \b5
.endm
.macro fuse2 name, ra, kb, fa, a1, a2, a3, a4, a5, fb, b1, b2, b3, b4, b5
    fuse2_one f2_\name, \fa, \a1, \a2, \a3, \a4, \a5, \fb, \b1, \b2, \b3, \b4, \b5, _, _
.ifnc \ra,_
.if \kb >= 1
    fuse2_one f2n_\name, \fa, \a1, \a2, \a3, \a4, \a5, \fb, \b1, \b2, \b3, \b4, \b5, \ra, _
.endif
.if \kb >= 2
    fuse2_one f2m_\name, \fa, \a1, \a2, \a3, \a4, \a5, \fb, \b1, \b2, \b3, \b4, \b5, _, \ra
.endif
.endif
.endm
