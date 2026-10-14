// Gadget bodies as macros, so that one body serves the gadget it always was
// and the fused pairs of pairs.h -- jit/guest-arm64/frag.h has the reasoning,
// and this is the same scheme for the riscv64 guest, with one difference in
// the spelling: a gadget FOO's body and its out-of-line code are the macros
//
//     f_FOO_body id, last
//     f_FOO_stub id, last
//
// named for the gadget itself, and defined where the gadget was (the macros
// that used to emit a family of gadgets now define a family of bodies, each
// followed by `f_gadget FOO`). The body is entered with _ip at its first
// parameter; with `last` 0 it leaves _ip after its parameters and falls
// through, with `last` 1 it ends in the dispatch. `id` makes labels unique.

// A second host. The bodies here and in alu.S -- the ALU family, mov_const
// and addi, the loads and stores, the branches -- and the pairs made of them
// were built by another translator as well as by AOK's until 2026-10-14: AOK
// VM's QEMU engine (emkey1/qemu), which took copies then and owns them
// since; nothing keeps the two in step. The contract stays, since it keeps
// the bodies free of this engine's own state. A host that is not AOK defines
// RV_GADGET_HOST, and what the shared code may assume of either host is no
// more than this:
//
//   _cpu, _ip     the guest state and the stream pointer (.req)
//   .gadget NAME  begins a gadget; gret N ends one, past N parameter words
//   parameters    8-byte words after the gadget's own; a register is the byte
//                 offset of its slot from _cpu, x0 a slot that reads as zero
//                 as a source and a slot nothing reads as a destination
//   x0, x9-x15    free for a body's use, and nothing else is; x12 carries a
//                 result from the first body of a pair to the second
//   HOST_NAME     1 for a gadget the host has, 0 for one it lacks, for every
//                 gadget pairs.h names
//
// Guest memory is the host's: how an address is translated, and what a miss
// or a fault does. A load or store body computes the address and makes the
// access itself, between the host's macros:
//
//   _addr         the register the address is in
//   _sav, _savw   a register, and its low half, that the macros below leave
//                 as it was: the body keeps what it needs after the access
//                 there (a load's destination, a store's value)
//   read_prep BITS, ID, AHEAD     _addr holds a guest address on entry and,
//   write_prep BITS, ID, AHEAD    after it, a host address where BITS/8 bytes
//                 can be read or written. It may use x9-x11, may call out,
//                 and may not come back (a fault).
//   write_done BITS, ID           follows the store instruction
//   read_bullshit, write_bullshit BITS, ID, AHEAD
//                 the same access's out-of-line code, placed after the
//                 gadget's dispatch
//   gret_ahead    the dispatch of a body that has already moved _ip past the
//                 next gadget's word
//
// The body moves _ip past its parameters before the access (f_ip_past), and
// AHEAD is how many bytes beyond them _ip is when the macro runs. A load's or
// a store's last parameter is the host's own, whatever its fault path needs
// to name the instruction: it is at [_ip, #-(AHEAD + 8)] there.
//
// How a block is left is the host's too. A body that ends its block names a
// way out by parameter words it does not read:
//
//   F_WAY_WORDS       how many words name one way out
//   branch_leave OFF  leave by the way out whose words begin at [_ip, #OFF].
//                     It does not come back, so nothing of the body's is
//                     live in it; give each its own indirect branch
//                     (branch_dispatch below has the reason).
//   pc_param REG      REG holds a parameter that is a guest code address, as
//                     the translator wrote it; afterwards, the address
//
// Everything else is outside the contract: a shared body does not call out
// on its own account and does not read the host's state. A gadget that must
// is a host's own (between the RV_GADGET_HOST guards), under a name pairs.h
// may still mention; a host that has no such gadget of AOK's can supply the
// body macro itself and take the pairs that end in it.
// tools/check-shared-gadgets.sh builds these files as such a host and fails
// on a body that breaks the contract: run it after changing them.

// Forwarding (jit/guest-arm64/frag.h's fuse2 has the reasoning). A body that
// produces a register leaves the value in x12 as well when the body after
// it will take it from there (`res`, f_res), and a body's source operand comes from its cpu_state
// slot or -- `fw` 12 -- from there (f_src). No body touches x12 before it has
// taken its operands.
.macro f_res res, reg
.if \res
    mov x12, \reg
.endif
.endm
.macro f_src dst, slotreg, fw
.ifc \fw,_
    ldr x\dst, [_cpu, \slotreg]
.else
    mov x\dst, x\fw
.endif
.endm
// (the same, for a destination that is a name: _addr, _sav)
.macro f_src_r dst, slotreg, fw
.ifc \fw,_
    ldr \dst, [_cpu, \slotreg]
.else
    mov \dst, x\fw
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

// A body that can fault moves _ip before its access, past its `n` parameters
// (the fault paths find the instruction's address in the last of them) and,
// when it is the last body, past the next gadget's word in the same move,
// dispatching through gret_ahead (gadgets.h).
.macro f_ip_past last, n
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
// `more`: parameter words of this body that lie beyond the access's own
// address word (a gadget with two accesses: the second's).
.macro f_read_prep last, bits, id, more=0
.if \last
    read_prep \bits, \id, (\more + 8)
.else
    read_prep \bits, \id, \more
.endif
.endm
.macro f_write_prep last, bits, id, more=0
.if \last
    write_prep \bits, \id, (\more + 8)
.else
    write_prep \bits, \id, \more
.endif
.endm
.macro f_read_bullshit last, bits, id, more=0
.if \last
    read_bullshit \bits, \id, (\more + 8)
.else
    read_bullshit \bits, \id, \more
.endif
.endm
.macro f_write_bullshit last, bits, id, more=0
.if \last
    write_bullshit \bits, \id, (\more + 8)
.else
    write_bullshit \bits, \id, \more
.endif
.endm

// A gadget with no out-of-line code.
.macro f_nostub name
.macro f_\name\()_stub id, last
.endm
.endm

// The standalone gadget, and a fused pair of two.
.macro f_gadget name
.gadget \name
    f_\name\()_body \name, 1
    f_\name\()_stub \name, 1
.endm

.macro fuse2_one sym, a, b, fn, fm, res=0
.gadget \sym
    f_\a\()_body \sym\()_a, 0, _, _, \res
    f_\b\()_body \sym\()_b, 1, \fn, \fm
    f_\a\()_stub \sym\()_a, 0
    f_\b\()_stub \sym\()_b, 1
.endm
// `hr`: the first leaves a result in x12; `kb`: how many operands the second
// can take from there. f2n_ forwards the second's first operand, f2m_ its
// second.
.macro fuse2 a, b, hr, kb
    fuse2_one f2_\a\()__\b, \a, \b, _, _
.if \hr
.if \kb >= 1
    fuse2_one f2n_\a\()__\b, \a, \b, 12, _, 1
.endif
.if \kb >= 2
    fuse2_one f2m_\a\()__\b, \a, \b, _, 12, 1
.endif
.endif
.endm

// (a second host: only where it has both gadgets)
.macro fuse2_host ha, hb, a, b, hr, kb
.if \ha && \hb
    fuse2 \a, \b, \hr, \kb
.endif
.endm

#ifndef RV_GADGET_HOST   // AOK's own way out of a block
// A branch gadget's way out, its own copy (jit/guest-arm64/control.S's
// branch_dispatch has the reason: through the shared riscv64_chain every
// transition between blocks left by ONE indirect branch). The rare ways out
// stay shared (core.S).
.macro branch_dispatch
    tbnz _ip, #63, 9f       // (a test-and-branch reaches 32K: hence the stub)
    ldr x8, [_cpu, #CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, 8f
    ldr x8, [_cpu, #LOCAL_chain_budget]
    subs x8, x8, #1
    str x8, [_cpu, #LOCAL_chain_budget]
    b.le 8f
    sub x8, _ip, #JIT_BLOCK_code
    str x8, [_cpu, #LOCAL_last_block]
#if !defined(ISH_ARM64_GRET_LDAR)
    dmb ishld
#endif
    gret
    // (a conditional branch cannot leave the object file: the branch gadgets
    // are in alu.S and these two are in core.S)
8:  b riscv64_chain_poked
9:  b riscv64_branch_unchained
.endm
// One tagged word names a way out (core.S's riscv64_branch_dispatch has the
// scheme), and a guest address in the stream is the address.
#define F_WAY_WORDS 1
.macro branch_leave off
    ldr _ip, [_ip, #\off]
    branch_dispatch
.endm
.macro pc_param reg
.endm
#endif // !RV_GADGET_HOST

// ---- Bodies defined here because more than one file needs them ------------
// alu.S builds the pairs, and holds its own gadgets' bodies; these are the
// bodies of core.S's and memory.S's gadgets that take part.

// rd = constant. [rd_off][value]
// rd = rs1 + imm. [rd_off][rs1_off][imm]
// Unconditional branch. [way out]
.macro f_mov_const_body id, last, fn=_, fm=_, res=0
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    str x10, [_cpu, x9]
    f_res \res, x10
    f_end \last, 2
.endm
f_nostub mov_const
.macro f_addi_body id, last, fn=_, fm=_, res=0
    ldr x9, [_ip]           // rd_off
    ldr x10, [_ip, #8]      // rs1_off
    ldr x11, [_ip, #16]     // imm
    f_src 10, x10, \fn
    add x10, x10, x11
    str x10, [_cpu, x9]
    f_res \res, x10
    f_end \last, 3
.endm
f_nostub addi
.macro f_b_body id, last, fn=_, fm=_, res=0
    branch_leave 0
.endm
f_nostub b

// Loads and stores. [rd_off|rs2_off][rs1_off][imm][the host's word]
// (the host's word: AOK's is the instruction's address, where a fault
// restarts)
// (`lid`: the old label id, unused -- the labels take the body's own `id`)
// (the bodies only: memory.S's load_gadget emits the gadgets)
.macro def_load name, size, lid, ld_insn, reg
.macro f_\name\()_body id, last, fn=_, fm=_, res=0
    ldr _sav, [_ip]           // rd_off (kept across prep)
    ldr x9, [_ip, #8]         // rs1_off
    ldr x10, [_ip, #16]       // imm
    f_ip_past \last, 4       // 3 params + the host's word
    f_src_r _addr, x9, \fn
    add _addr, _addr, x10     // effective address
    f_read_prep \last, \size, \id
    \ld_insn \reg, [_addr]
    str x11, [_cpu, _sav]
    f_res \res, x11
    f_end_ahead \last
.endm
.macro f_\name\()_stub id, last
    f_read_bullshit \last, \size, \id
.endm
.endm
def_load lb,  8,  rvlb,  ldrsb, x11
def_load lbu, 8,  rvlbu, ldrb,  w11
def_load lh,  16, rvlh,  ldrsh, x11
def_load lhu, 16, rvlhu, ldrh,  w11
def_load lw,  32, rvlw,  ldrsw, x11
def_load lwu, 32, rvlwu, ldr,   w11
def_load ld,  64, rvld,  ldr,   x11

// (`lid`: the old label id, unused -- the labels take the body's own `id`)
.macro def_store name, size, lid, st_insn, reg
.macro f_\name\()_body id, last, fn=_, fm=_, res=0
    ldr x9, [_ip]             // rs2_off (value)
    ldr x10, [_ip, #8]        // rs1_off (base)
    ldr x11, [_ip, #16]       // imm
    f_ip_past \last, 4
    f_src_r _sav, x9, \fn    // the value, kept across prep
    f_src_r _addr, x10, \fm
    add _addr, _addr, x11
    f_write_prep \last, \size, \id
    \st_insn \reg, [_addr]
    write_done \size, \id
    f_end_ahead \last
.endm
.macro f_\name\()_stub id, last
    f_write_bullshit \last, \size, \id
.endm
.endm
def_store sb, 8,  rvsb, strb, _savw
def_store sh, 16, rvsh, strh, _savw
def_store sw, 32, rvsw, str,  _savw
def_store sd, 64, rvsd, str,  _sav
