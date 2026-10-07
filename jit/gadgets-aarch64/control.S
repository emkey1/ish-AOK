#include "gadgets.h"
#include "emu/interrupt.h"

# An indirect jmp or call to _tmp: enter the target's block straight from
# the dispatch loop's block cache (jit_frame.i386_cache, jit_cache_hash's
# slot: ip * 0x9E3779B97F4A7C15 >> 32, mod 1024) if it holds the block for
# that address and the block is not jetsam, as the loop itself checks;
# chain_ip then does the poke check, the chain budget and last_block. Else
# out to C with eip = _tmp, as before. eip and _ip share x28, so eip is only
# written on the way out.
.macro i386_indir_chain
    mov x8, LOCAL_i386_cache
    ldr x8, [_cpu, x8]
    cbz x8, 9f
    movz x9, 0x7c15
    movk x9, 0x7f4a, lsl 16
    movk x9, 0x79b9, lsl 32
    movk x9, 0x9e37, lsl 48
    mov w10, _tmp
    mul x10, x10, x9
    ubfx x10, x10, 32, 10
    ldr x8, [x8, x10, lsl 3]
    cbz x8, 9f
    ldr x9, [x8, JIT_BLOCK_addr]
    cmp x9, _tmp, uxtw
    b.ne 9f
    ldrb w9, [x8, JIT_BLOCK_is_jetsam]
    cbnz w9, 9f
    add _ip, x8, JIT_BLOCK_code
    chain_ip
9:
    mov eip, _tmp
    b jit_ret
.endm

.gadget call
    // save return address
    sub _addr, esp, 4
    write_prep 32, call
    ldr w8, [_ip, 16]
    str w8, [_xaddr]
    // push stack pointer
    sub esp, esp, 4
    // save ip-to-arguments to return cache
    ubfx w12, w8, 4, 12
    write_done 32, call // clobbers w8
    mov x13, LOCAL_ret_cache
    add x13, _cpu, x13
    str _ip, [x13, x12, lsl 3]
    // jump to target
    ldr _ip, [_ip, 32]
    chain_ip
    write_bullshit 32, call

.gadget call_indir
    // save return address
    sub _addr, esp, 4
    write_prep 32, call_indir
    ldr w8, [_ip, 16]
    str w8, [_xaddr]
    // push stack pointer
    sub esp, esp, 4
    // save ip-to-arguments to return cache
    ubfx w12, w8, 4, 12
    write_done 32, call_indir // clobbers w8
    mov x13, LOCAL_ret_cache
    add x13, _cpu, x13
    str _ip, [x13, x12, lsl 3]
    // jump to target
    i386_indir_chain
    write_bullshit 32, call_indir

.gadget ret
    mov _addr, esp
    // load return address and save to _tmp
    read_prep 32, ret
    ldr _tmp, [_xaddr]
    // pop stack pointer
    ldr w8, [_ip, 8]
    add esp, esp, w8
    // load saved ip in return cache
    ubfx w12, _tmp, 4, 12
    mov x13, LOCAL_ret_cache
    add x13, _cpu, x13
    ldr _ip, [x13, x12, lsl 3]
    // found?
    cbz _ip, 2f
    // check if we jumped to the correct CALL instruction
    ldr w9, [_ip, 16]
    ldr x8, [_ip, 8]
    cmp _tmp, w9
    b.ne 1f
    // good, now do return chaining, the logic is similar to `jit_ret_chain`
    ldr _ip, [_ip, 24]
    cmp _ip, 0
    b.lt 1f
    // check write_wanted before linking, same as jit_ret_chain does
    ldr x9, [_cpu, CPU_poked_ptr]
    ldrb w9, [x9]
    cbnz w9, 4f
    # Same per-entry chain budget jit_ret_chain enforces. The ret cache is a
    # second path that dispatches straight into another block without going
    # through C, and a return edge to a lower guest address is itself a
    # backward edge -- so once the frontend links backward jumps, a cycle can
    # be formed that routes through here. Decrementing the shared budget keeps
    # the "returns to C every few thousand dispatches" guarantee airtight for
    # every chained path, not just the jump ones. x9 is dead after the poke
    # check above; x8 is only live on the 1: (cache-miss) path.
    ldr x9, [_cpu, LOCAL_chain_budget]
    subs x9, x9, 1
    str x9, [_cpu, LOCAL_chain_budget]
    b.gt 3f
    # Both exits share the one unconditional branch: a conditional branch
    # cannot reach `poke`, which is external to this file (same constraint the
    # null-gadget check in entry.S's gret macro works around).
4:  b poke
3:
    sub x8, _ip, JIT_BLOCK_code
    str x8, [_cpu, LOCAL_last_block]
    gret
1:
    str x8, [_cpu, LOCAL_last_block]
    // fallthrough
2:
    mov eip, _tmp
    b jit_ret
    read_bullshit 32, ret

.gadget jmp_indir
    i386_indir_chain
.gadget jmp
    ldr _ip, [_ip]
    chain_ip
.gadget amd64_jmp
    # target word is bit-63 tagged; amd64_chain_ip (gadgets.h) handles
    # tagged-exit vs patched-chain uniformly.
    ldr _ip, [_ip]
    amd64_chain_ip
.gadget jcxz
    cbnz ecx, 1f
    ldr _ip, [_ip]
    chain_ip
1:
    ldr _ip, [_ip, 8]
    chain_ip

#define COND_LIST o,c,z,cz,s,p,sxo,sxoz

.macro check_res
    cmpl $0, CPU_res(%_cpu)
.endm
.macro check_cf
    cmpb $0, CPU_cf(%_cpu)
.endm

.macro check_res_or_flag resflag, flag, target, fallthrough
    ldr w8, [_cpu, CPU_flags_res]
    tbnz w8, \resflag, 2f
    ldr w8, [_cpu, CPU_eflags]
    tbnz w8, \flag, \target
    b \fallthrough
2:
.endm

.macro do_jump cond, target
    # please tell me if you know a better way
    .ifc \cond,o
        ldrb w8, [_cpu, CPU_of]
        cbnz w8, \target
    .else N .ifc \cond,c
        ldrb w8, [_cpu, CPU_cf]
        cbnz w8, \target
    .else N .ifc \cond,z
        check_res_or_flag 1/*ZF_RES*/, 6/*ZF_FLAG*/, \target, 3f
        ldr w8, [_cpu, CPU_res]
        cbz w8, \target
    3:
    .else N .ifc \cond,cz
        ldrb w8, [_cpu, CPU_cf]
        cbnz w8, \target
        check_res_or_flag 1/*ZF_RES*/, 6/*ZF_FLAG*/, \target, 3f
        ldr w8, [_cpu, CPU_res]
        cbz w8, \target
    3:
    .else N .ifc \cond,s
        check_res_or_flag 2/*SF_RES*/, 7/*SF_FLAG*/, \target, 3f
        ldr w8, [_cpu, CPU_res]
        cmp w8, 0
        b.lt \target
    3:
    .else N .ifc \cond,p
        check_res_or_flag 0/*PF_RES*/, 2/*PF_FLAG*/, \target, 3f
        ldr w8, [_cpu, CPU_res]
        uxtb w8, w8
        eor w8, w8, w8, lsr #4
        eor w8, w8, w8, lsr #2
        eor w8, w8, w8, lsr #1
        tbz w8, 0, \target
    3:
    .else N .ifc \cond,sxo
        ldr w8, [_cpu, CPU_res]
        cmp w8, 0
        cset w8, lt
        ldrb w9, [_cpu, CPU_of]
        cmp w8, w9
        b.ne \target
    .else N .ifc \cond,sxoz
        ldr w8, [_cpu, CPU_res]
        cmp w8, 0
        b.eq \target
        cset w8, lt
        ldrb w9, [_cpu, CPU_of]
        cmp w8, w9
        b.ne \target
    .endif N .endif N .endif N .endif N .endif N .endif N .endif N .endif
.endm

# LOOPZ/LOOPE (0xe1) and LOOPNZ/LOOPNE (0xe0). gen.c emits a helper that
# decrements ECX first -- LOOP must not touch flags -- so these only branch:
# taken when ECX != 0 and ZF matches. The ZF test goes through do_jump so it
# reads the lazy flag state; a bare CPU_eflags bit test would be wrong
# whenever ZF is still deferred in CPU_res. Labels 4/5 because do_jump and
# check_res_or_flag already use 1-3.
.gadget loopz
    cbz ecx, 4f
    do_jump z, 5f
4:
    ldr _ip, [_ip, 8]
    chain_ip
5:
    ldr _ip, [_ip]
    chain_ip

.gadget loopnz
    cbz ecx, 4f
    do_jump z, 4f
    ldr _ip, [_ip]
    chain_ip
4:
    ldr _ip, [_ip, 8]
    chain_ip

.irp cond, COND_LIST
    .gadget jmp_\cond
        do_jump \cond, 1f
        ldr _ip, [_ip, 8]
        chain_ip
    1:
        ldr _ip, [_ip]
        chain_ip

    .gadget set_\cond
        do_jump \cond, 1f
        mov _tmp, 0
        gret
    1:  mov _tmp, 1
        gret
    .gadget setn_\cond
        do_jump \cond, 1f
        mov _tmp, 1
        gret
    1:  mov _tmp, 0
        gret

    .gadget skip_\cond
        do_jump \cond, 1f
        gret 1
    1:  ldr x8, [_ip]
        add _ip, _ip, x8
        gret 1
    .gadget skipn_\cond
        do_jump \cond, 1f
        ldr x8, [_ip]
        add _ip, _ip, x8
    1:  gret 1
.endr
.gadget_list jmp, COND_LIST
.gadget_list set, COND_LIST
.gadget_list setn, COND_LIST
.gadget_list skip, COND_LIST
.gadget_list skipn, COND_LIST

# All-eager condition check for amd64. amd64 arith/logic gadgets compute flags
# eagerly into eflags (ZF=bit6, SF=bit7, PF=bit2) plus the CPU_cf/CPU_of bytes,
# and do NOT maintain CPU_res -- so unlike i386 do_jump we must never read
# CPU_res. (do_jump's sxo/sxoz read CPU_res directly with no eager path, which is
# stale garbage on amd64.) Branches to \target if the COND_LIST condition holds.
# amd64_do_jump now lives in gadgets.h so math.S can use it too -- the
# condition-evaluating gadgets (setcc, cmov) need it alongside the memory
# helpers, and the two files are separate translation units.

# amd64 block-chaining dispatch, the riscv64_branch_dispatch design with
# this guest's 64-bit rip fields. Branch gadgets load the chosen target
# word into _ip and jump here. Unchained words carry bit 63 (gen.c tags
# them); the frontend patches a matching word to the successor block's
# host code pointer (bit 63 clear), after which execution continues
# straight into the next block without returning to C. The chain budget
# (jit_frame.chain_budget, reset to 8192 per jit_enter) plus the poke
# check keep chained loops from starving jetsam/quiesce waiters -- the
# same rules that make backward edges safe on the arm64/riscv64 engines.
# (The old forward-chaining experiment measured no win on a -O0 build,
# where the C loop body dwarfed the round trip; at -O2 the round trip
# dominated an amd64 gzip profile.)
# The register cache is loaded at every chained branch and may be newer
# than CPU_amd64_regs (gen.c gen_amd64_sync_reg_cache), so both ways out to
# C from here write it back first.
.macro amd64_chain_exit_spill
    add x9, _cpu, CPU_amd64_regs
    str x20, [x9, 0]
    str x22, [x9, 8]
    str x23, [x9, 16]
    str x21, [x9, 24]
    str x27, [x9, 32]
    str x26, [x9, 40]
    str x24, [x9, 48]
    str x25, [x9, 56]
.endm
.global amd64_branch_dispatch
amd64_branch_dispatch:
    tbz _ip, 63, amd64_chain
    amd64_chain_exit_spill
    and x8, _ip, 0x7fffffffffffffff
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    b jit_ret
amd64_chain:
    ldr x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, amd64_chain_poked
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le amd64_chain_poked
    sub x8, _ip, JIT_BLOCK_code
    str x8, [_cpu, LOCAL_last_block]
    # Publish the entered block's guest rip before running its gadgets. The C
    # frontend loop guarantees CPU_amd64_rip == block->addr on entry to every
    # block, and this engine -- unlike the fully-translated arm64/riscv64 ones
    # this design was lifted from -- has gadgets that RE-DECODE their own
    # instruction out of guest memory at that rip: the helper-call gadgets
    # (the amd64_jit_* bridges in emu/amd64_interp.c) walk prefixes
    # with amd64_fetch_u8, which fetches at CPU_amd64_rip and advances it. A
    # chained entry that left rip stale therefore decoded whatever instruction
    # rip happened to point at -- musl's __init_libc basename loop chains
    # `incq %rax; movzbl -1(%rax),%ecx; cmp; je` to `test; jne` and back, so
    # the ff-group helper re-entered with rip still on the movzbl, fetched its
    # 0f opcode, and took `if (byte != 0xff) return INT_UNDEFINED` -> SIGILL on
    # the first chained iteration of every static amd64 binary.
    ldr x8, [x8, JIT_BLOCK_addr]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    gret
.global amd64_chain_poked
amd64_chain_poked:
    amd64_chain_exit_spill
    ldur x8, [_ip, (JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    b jit_ret

# amd64 conditional jumps (0x70-0x7f, 0x0f 80-8f). gen.c emits operand 0 =
# taken target, operand 1 = else (swapped for negated condition codes),
# both bit-63 tagged for the chaining dispatch above (inlined per gadget:
# amd64_chain_ip). Uses labels 1 and 2.
.irp cond, COND_LIST
    .gadget amd64_jcc_\cond
        amd64_do_jump \cond, 1f
        ldr _ip, [_ip, 8]
        b 2f
    1:
        ldr _ip, [_ip]
    2:
        amd64_chain_ip
.endr

# amd64 LOOP (0xe2) / LOOPE/LOOPZ (0xe1) / LOOPNE/LOOPNZ (0xe0), no address-size prefix
# (RCX counter; the 0x67/ECX form bridges). Decrement RCX (regs[1], 64-bit) -- LOOP does
# NOT touch flags -- then branch to the taken target if RCX!=0 [LOOPE: && ZF==1; LOOPNE:
# && ZF==0], else fall through. Like the jcc gadgets: set rip + exit to the main loop (no
# static link). gen.c flushes the reg cache, so RCX is read/written straight from
# CPU_amd64_regs. operand 0 = taken target, operand 1 = else (fall-through). Label 1.
.macro amd64_gen_loop name, ztest
    .gadget amd64_\name
        ldr x8, [_cpu, (CPU_amd64_regs + 8)]   /* RCX */
        sub x8, x8, 1
        str x8, [_cpu, (CPU_amd64_regs + 8)]   /* write decremented count back */
        cbz x8, 1f                              /* count==0 -> fall through */
        /* ZF via do_jump, not a bare CPU_eflags bit test: ZF is lazy in
         * general (deferred in CPU_res with ZF_RES set), and while the amd64
         * frontend happens to materialize it everywhere today -- its native
         * flag gadgets clear ZF_RES, and the interp bridge calls
         * collapse_flags -- nothing states that as an invariant. A bare read
         * would silently take the wrong arm the day a deferring amd64 gadget
         * is added. Same reasoning as the i386 loopz/loopnz gadgets. Label 5
         * because do_jump/check_res_or_flag use 2-3 and this gadget uses 1. */
        .ifc \ztest,e
            do_jump z, 5f                        /* LOOPE: ZF==1 -> taken */
            b 1f
        5:
        .endif
        .ifc \ztest,ne
            do_jump z, 1f                        /* LOOPNE: ZF==1 -> fall through */
        .endif
        ldr x10, [_ip]                           /* taken target */
        str x10, [_cpu, CPU_amd64_rip]
        str w10, [_cpu, CPU_eip]
        b jit_ret
    1:
        ldr x10, [_ip, 8]                        /* else (fall-through) target */
        str x10, [_cpu, CPU_amd64_rip]
        str w10, [_cpu, CPU_eip]
        b jit_ret
.endm
amd64_gen_loop loop, none
amd64_gen_loop loope, e
amd64_gen_loop loopne, ne

# The 0x67 forms (67 E0-E3) count in ECX: a 32-bit write, so RCX's upper half
# is cleared, as Linux's hardware does it. JECXZ tests ECX alone.
# Operands: [taken][else].
.macro amd64_gen_loop32 name, ztest
    .gadget amd64_\name
        ldr w8, [_cpu, (CPU_amd64_regs + 8)]   /* ECX */
        sub w8, w8, 1
        str x8, [_cpu, (CPU_amd64_regs + 8)]   /* zero-extended into RCX */
        cbz w8, 1f
        .ifc \ztest,e
            do_jump z, 5f
            b 1f
        5:
        .endif
        .ifc \ztest,ne
            do_jump z, 1f
        .endif
        ldr x10, [_ip]
        str x10, [_cpu, CPU_amd64_rip]
        str w10, [_cpu, CPU_eip]
        b jit_ret
    1:
        ldr x10, [_ip, 8]
        str x10, [_cpu, CPU_amd64_rip]
        str w10, [_cpu, CPU_eip]
        b jit_ret
.endm
amd64_gen_loop32 loop32, none
amd64_gen_loop32 loope32, e
amd64_gen_loop32 loopne32, ne
.gadget amd64_jecxz
    ldr w8, [_cpu, (CPU_amd64_regs + 8)]
    cbz w8, 1f
    ldr x10, [_ip, 8]
    b 2f
1:  ldr x10, [_ip]
2:  str x10, [_cpu, CPU_amd64_rip]
    str w10, [_cpu, CPU_eip]
    b jit_ret

# amd64 JRCXZ (0xe3), rel8: branch if RCX == 0. Unlike LOOP it neither
# decrements the counter nor consults ZF, so it needs no do_jump and no flag
# reasoning at all -- just a register test. Chained like the jcc gadgets rather
# than exiting like LOOP: operand 0 = taken, operand 1 = else, both bit-63
# tagged for amd64_branch_dispatch, so the frontend can patch either arm to a
# successor block. gen.c flushes the reg cache, so RCX is read straight out of
# CPU_amd64_regs.
# 0F 90+cc SETcc r/m8, register form (mod==3). Writes 1 or 0 into a byte
# register and touches no flags.
#
# SETE/SETNE alone were 5.3% of every amd64 interpreter bridge measured on a
# threaded workload, and unlike the lock-prefixed atomics they need no
# read-modify-write reasoning at all -- just the condition the jcc gadgets
# already evaluate, and the byte writeback the byte-ALU family already does.
#
# The operand packing deliberately matches amd64_byte_alu_reg_imm --
# dst_id(4-7) | is_high(8) -- so amd64_byte_writeback is reused verbatim,
# including its handling of AH/CH/DH/BH (which, with no REX prefix, is what
# encodings 4-7 mean).
#
# Order matters: amd64_do_jump clobbers w8/w9, and both the packed operand and
# the destination's current value live there, so the condition is evaluated
# FIRST into x15 and the operand is only loaded afterwards.
# amd64_do_jump lives here in control.S while the byte-register helpers live in
# math.S, and the two are separate translation units -- so the write is inlined
# rather than reusing amd64_byte_writeback. gen.c FLUSHES the register cache
# before this gadget, so every guest register is in CPU_amd64_regs and a direct
# load/store is the correct way to reach it.
#
# Order matters: amd64_do_jump clobbers w8/w9, so the condition is evaluated
# into x15 BEFORE the operand word is loaded.
.macro amd64_gen_setcc name, cond
    .gadget amd64_set_\name
        amd64_do_jump \cond, 1f
        mov  x15, 0
        b    2f
    1:  mov  x15, 1
    2:  amd64_setcc_store
.endm

.macro amd64_gen_setncc name, cond
    .gadget amd64_setn_\name
        amd64_do_jump \cond, 1f
        mov  x15, 1
        b    2f
    1:  mov  x15, 0
    2:  amd64_setcc_store
.endm

# x15 holds the 0/1 result. Operand: dst_id(4-7) | is_high(8) -- the same
# packing the byte-ALU family uses, so AH/CH/DH/BH (encodings 4-7 with no REX)
# land in the right half of the register.
.macro amd64_setcc_store
    ldr  x8, [_ip]
    ubfx x13, x8, 4, 4
    ubfx x11, x8, 8, 1
    lsl  x11, x11, 3
    add  x12, _cpu, CPU_amd64_regs
    ldr  x9, [x12, x13, lsl 3]
    mov  x14, 0xff
    lsl  x14, x14, x11
    bic  x9, x9, x14
    lsl  x15, x15, x11
    and  x15, x15, x14
    orr  x9, x9, x15
    str  x9, [x12, x13, lsl 3]
    gret 1
.endm

.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_gen_setcc \cond, \cond
    amd64_gen_setncc \cond, \cond
.endr

# 0F 40+cc CMOVcc r, r/m -- register source (mod==3).
#
# The destination is written EVEN WHEN THE CONDITION IS FALSE: on real x86-64
# CMOVcc always writes, and a 32-bit write zero-extends, so a not-taken 32-bit
# cmov CLEARS the upper half. Verified on hardware; AOK's interpreter had this
# wrong in both of its copies until now. Writing the current value back is a
# no-op at 16 and 64 bits, so the store below is unconditional at every size.
#
# gen.c flushes the register cache, so guest registers are in CPU_amd64_regs.
# amd64_do_jump clobbers w8/w9, so it runs before anything is loaded.
#
# Operand: src_id(0-3) | dst_id(4-7) | size(8-15).
.macro amd64_gen_cmov name, cond, pol
    .gadget amd64_cmov\pol\()_\name
        amd64_do_jump \cond, 1f
        .ifc \pol,n
            mov  x16, 1
        .else
            mov  x16, 0
        .endif
        b    2f
    1:
        .ifc \pol,n
            mov  x16, 0
        .else
            mov  x16, 1
        .endif
    2:  ldr  x8, [_ip]
        ubfx x11, x8, 0, 4
        ubfx x12, x8, 4, 4
        ubfx x13, x8, 8, 8
        add  x14, _cpu, CPU_amd64_regs
        ldr  x9,  [x14, x11, lsl 3]      /* src */
        ldr  x10, [x14, x12, lsl 3]      /* current dst */
        cmp  x16, 0
        csel x15, x9, x10, ne            /* taken ? src : dst */
        cmp  x13, 64
        b.eq 3f
        cmp  x13, 32
        b.eq 4f
        bfi  x10, x15, 0, 16             /* 16-bit: upper 48 preserved */
        mov  x15, x10
        b    3f
    4:  mov  w15, w15                    /* 32-bit: zero-extend */
    3:  str  x15, [x14, x12, lsl 3]
        gret 1
.endm

.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_gen_cmov \cond, \cond,
    amd64_gen_cmov \cond, \cond, n
.endr



.gadget amd64_jrcxz
    ldr x8, [_cpu, (CPU_amd64_regs + 8)]    /* RCX */
    cbz x8, 1f
    ldr _ip, [_ip, 8]                       /* RCX != 0 -> else / fall through */
    b 2f
1:
    ldr _ip, [_ip]                          /* RCX == 0 -> taken */
2:
    amd64_chain_ip

# PUSHF/POPF, in both operand sizes. [gadget][orig_ip]: a fault reports the
# instruction, so the stack-growth retry re-runs it.
#
# These used to be one 32-bit pair that the 0x66 prefix never reached, so
# PUSHFW/POPFW moved ESP by 4, and POPF stored all 32 popped bits into EFLAGS.
# Linux lets a user-mode POPF change only CF PF AF ZF SF TF DF OF NT AC ID;
# IF, IOPL, VM, RF and the reserved bits stay put, so they read back as they
# were. POPFW changes only the ones in the low word. TF stays writable, as it
# always was here and is on Linux, where setting it single-steps the process.
# The mask is the same number jit/gadgets-x86_64/control.S uses.
#
# The push computes its address before moving ESP (as `push` does) because
# the fault path leaves ESP where it finds it, and the old gadget had already
# moved it; nor did it carry an orig_ip, so a fault took EIP from whatever
# word followed it in the block.
# collapse_flags (emu/cpu.h) in place: ZF, SF and PF from CPU_res and AF from
# op1 ^ op2 ^ res where the lazy bits say so, CF and OF from their bytes (set
# when nonzero), bit 1 set, bits 3 and 5 clear, IF set; the lazy bits cleared.
# Clobbers w8-w12.
.macro i386_collapse_flags
    ldr  w8, [_cpu, CPU_eflags]
    ldrb w9, [_cpu, CPU_flags_res]
    ldr  w10, [_cpu, CPU_res]
    tbz  w9, 1, .Lcf_zf_\@             /* ZF_RES */
    bic  w8, w8, ZF_FLAG
    cmp  w10, 0
    cset w11, eq
    orr  w8, w8, w11, lsl 6
.Lcf_zf_\@:
    tbz  w9, 2, .Lcf_sf_\@             /* SF_RES */
    bic  w8, w8, SF_FLAG
    lsr  w11, w10, 31
    orr  w8, w8, w11, lsl 7
.Lcf_sf_\@:
    tbz  w9, 0, .Lcf_pf_\@             /* PF_RES: even parity of the low byte */
    bic  w8, w8, PF_FLAG
    eor  w11, w10, w10, lsr 4
    eor  w11, w11, w11, lsr 2
    eor  w11, w11, w11, lsr 1
    mvn  w11, w11
    and  w11, w11, 1
    orr  w8, w8, w11, lsl 2
.Lcf_pf_\@:
    tbz  w9, 3, .Lcf_af_\@             /* AF_OPS */
    ldr  w11, [_cpu, CPU_op1]
    ldr  w12, [_cpu, CPU_op2]
    eor  w11, w11, w12
    eor  w11, w11, w10
    ubfx w11, w11, 4, 1
    bic  w8, w8, AF_FLAG
    orr  w8, w8, w11, lsl 4
.Lcf_af_\@:
    strb wzr, [_cpu, CPU_flags_res]
    ldrb w11, [_cpu, CPU_cf]
    cmp  w11, 0
    cset w11, ne
    bic  w8, w8, 1
    orr  w8, w8, w11
    ldrb w11, [_cpu, CPU_of]
    cmp  w11, 0
    cset w11, ne
    bic  w8, w8, (1 << 11)
    orr  w8, w8, w11, lsl 11
    orr  w8, w8, 2
    bic  w8, w8, 8
    bic  w8, w8, 32
    orr  w8, w8, (1 << 9)
    str  w8, [_cpu, CPU_eflags]
.endm

# expand_flags: CF and OF bytes from eflags, no lazy bits. Clobbers w8, w9.
.macro i386_expand_flags
    ldr  w8, [_cpu, CPU_eflags]
    and  w9, w8, 1
    strb w9, [_cpu, CPU_cf]
    ubfx w9, w8, 11, 1
    strb w9, [_cpu, CPU_of]
    strb wzr, [_cpu, CPU_flags_res]
.endm

.macro do_pushf size, s
    .gadget pushf\size
        i386_collapse_flags

        sub _addr, esp, (\size/8)
        write_prep \size, pushf\size
        ldr w8, [_cpu, CPU_eflags]
        str\s w8, [_xaddr]
        write_done \size, pushf\size
        sub esp, esp, (\size/8)
        gret 1
        write_bullshit \size, pushf\size
.endm
do_pushf 32
do_pushf 16, h
.purgem do_pushf

# [gadget][orig_ip][next_ip]. A POPF that leaves TF set leaves the JIT at the
# next instruction (as a yield), so cpu_single_step takes over from there and
# the trap comes after it, as on hardware.
.macro do_popf size, s, mask_hi
    .gadget popf\size
        mov _addr, esp
        read_prep \size, popf\size
        ldr\s w8, [_xaddr]
        movz w9, 0x4dd5
        movk w9, \mask_hi, lsl 16
        and w8, w8, w9
        ldr w10, [_cpu, CPU_eflags]
        bic w10, w10, w9
        orr w10, w10, w8
        str w10, [_cpu, CPU_eflags]
        add esp, esp, (\size/8)

        i386_expand_flags
        tbz w10, 8, 1f
        ldr eip, [_ip, 8]
        mov _tmp, INT_TIMER
        b jit_exit
1:      gret 2
        read_bullshit \size, popf\size
.endm
do_popf 32, , 0x24
do_popf 16, h, 0
.purgem do_popf

.gadget sahf
    ubfx w8, eax, 8, 8
    strb w8, [_cpu, CPU_eflags]
    i386_expand_flags
    gret

# LAHF (0x9f): AH = the low byte of EFLAGS (SF:ZF:0:AF:0:PF:1:CF). collapse_flags
# materializes that byte (sets bit1=1, bits3/5=0); mirror of sahf in reverse. i386
# had no lahf gadget at all -> guest lahf SIGILL'd.
.gadget lahf
    i386_collapse_flags
    ldrb w8, [_cpu, CPU_eflags]
    bfi eax, w8, 8, 8
    gret

# The BCD adjusts, exact to an AMD Ryzen for every AL, AF and CF in (camd,
# tests/manual/x86/i386_bcd.c), the flags the SDM calls undefined included:
#   DAA/DAS: AL +/- the correction (6 for the low digit, 0x60 for the high),
#     CF and AF as the SDM says, SF ZF PF OF from that 8-bit add/sub.
#   AAA/AAS: a 16-bit ADD/SUB of 0x106 to AX when the low digit needs it (of 0
#     when not), AX's result masked with 0xff0f: SF ZF PF OF from the 16-bit
#     result, CF = AF = whether it adjusted.
#   AAM imm8: AH = AL / imm, AL = AL % imm; SF ZF PF from AL, CF AF OF clear.
#   AAD imm8: AL = AL + AH * imm, an 8-bit ADD whose flags are all of them; AH 0.
# Each collapses the lazy flags first (AF and CF are inputs) and leaves them
# collapsed. AAM/AAD: [imm], gret 1 (AAM 0 is #DE, gen.c's); the rest no operand.
.macro bcd_szp r, bits                  /* SF ZF PF of \r's low \bits into w8 */
    ubfx w9, \r, (\bits - 1), 1
    orr  w8, w8, w9, lsl 7
    tst  \r, ((1 << \bits) - 1)
    cset w9, eq
    orr  w8, w8, w9, lsl 6
    eor  w9, \r, \r, lsr 4
    eor  w9, w9, w9, lsr 2
    eor  w9, w9, w9, lsr 1
    mvn  w9, w9
    and  w9, w9, 1
    orr  w8, w8, w9, lsl 2
.endm
.macro bcd_begin                        /* w8 = EFLAGS with CF PF AF ZF SF OF clear; w16 CF, w17 AF */
    i386_collapse_flags
    and  w16, w8, 1
    ubfx w17, w8, 4, 1
    mov  w9, 0x8d5
    bic  w8, w8, w9
.endm
.macro bcd_end
    str  w8, [_cpu, CPU_eflags]
    i386_expand_flags
.endm
.macro bcd_ascii name, op                /* AAA (add) / AAS (sub) */
.gadget \name
    bcd_begin
    and  w10, eax, 0xffff
    and  w11, eax, 0xf
    cmp  w11, 9
    cset w12, hi
    orr  w12, w12, w17                   /* adjust */
    mov  w13, 0x106
    cmp  w12, 0
    csel w13, w13, wzr, ne
    \op  w14, w10, w13                   /* the 16-bit result */
    orr  w8, w8, w12                     /* CF */
    orr  w8, w8, w12, lsl 4              /* AF */
    bcd_szp w14, 16
.ifc \op, add
    eor  w9, w10, w14
    eor  w15, w13, w14
    and  w9, w9, w15
.else
    eor  w9, w10, w13
    eor  w15, w10, w14
    and  w9, w9, w15
.endif
    ubfx w9, w9, 15, 1
    orr  w8, w8, w9, lsl 11              /* OF */
    mov  w15, 0xff0f
    and  w14, w14, w15
    bfi  eax, w14, 0, 16
    bcd_end
    gret
.endm
bcd_ascii aaa, add
bcd_ascii aas, sub
.macro bcd_decimal name, op              /* DAA (add) / DAS (sub) */
.gadget \name
    bcd_begin
    and  w10, eax, 0xff                  /* AL */
    and  w11, eax, 0xf
    cmp  w11, 9
    cset w12, hi
    orr  w12, w12, w17                   /* the low digit adjusts */
    cmp  w10, 0x99
    cset w13, hi
    orr  w13, w13, w16                   /* the high digit adjusts: CF */
    add  w14, w12, w12, lsl 1
    lsl  w14, w14, 1                     /* 6 */
    add  w15, w13, w13, lsl 1
    add  w14, w14, w15, lsl 5            /* + 0x60 */
    \op  w15, w10, w14                   /* the result */
.ifc \op, sub
    cmp  w10, 6                          /* DAS: a borrow out of the low digit's adjust */
    cset w11, lo
    and  w11, w11, w12
    orr  w13, w13, w11
.endif
    orr  w8, w8, w13                     /* CF */
    orr  w8, w8, w12, lsl 4              /* AF */
    bcd_szp w15, 8
.ifc \op, add
    eor  w9, w10, w15
    eor  w11, w14, w15
    and  w9, w9, w11
.else
    eor  w9, w10, w14
    eor  w11, w10, w15
    and  w9, w9, w11
.endif
    ubfx w9, w9, 7, 1
    orr  w8, w8, w9, lsl 11              /* OF */
    bfi  eax, w15, 0, 8
    bcd_end
    gret
.endm
bcd_decimal daa, add
bcd_decimal das, sub
.gadget aam
    bcd_begin
    ldr  w13, [_ip]                      /* the base, not 0 */
    and  w10, eax, 0xff
    udiv w11, w10, w13
    msub w12, w11, w13, w10              /* AL % base */
    bcd_szp w12, 8
    bfi  w12, w11, 8, 8
    bfi  eax, w12, 0, 16
    bcd_end
    gret 1
.gadget aad
    bcd_begin
    ldr  w13, [_ip]
    and  w10, eax, 0xff                  /* AL */
    ubfx w11, eax, 8, 8
    mul  w11, w11, w13
    and  w11, w11, 0xff                  /* AH * base, 8 bits */
    add  w12, w10, w11
    ubfx w9, w12, 8, 1
    orr  w8, w8, w9                      /* CF */
    eor  w9, w10, w11
    eor  w9, w9, w12
    ubfx w9, w9, 4, 1
    orr  w8, w8, w9, lsl 4               /* AF */
    eor  w9, w10, w12
    eor  w14, w11, w12
    and  w9, w9, w14
    ubfx w9, w9, 7, 1
    orr  w8, w8, w9, lsl 11              /* OF */
    bcd_szp w12, 8
    and  w12, w12, 0xff
    bfi  eax, w12, 0, 16                 /* AH = 0 */
    bcd_end
    gret 1

# LOOP/LOOPZ/LOOPNZ's decrement: ECX - 1, the flags untouched (the branch is
# jcxz's or loopz's). No operand.
.gadget loop_dec_ecx
    sub  ecx, ecx, 1
    gret
