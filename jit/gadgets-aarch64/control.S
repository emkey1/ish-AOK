#include "gadgets.h"

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
    b jit_ret_chain
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
    mov eip, _tmp
    b jit_ret
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
    cbz w9, 3f
    b poke
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
    mov eip, _tmp
    b jit_ret
.gadget jmp
    ldr _ip, [_ip]
    b jit_ret_chain
.gadget amd64_jmp
    # target word is bit-63 tagged; amd64_branch_dispatch (below, after
    # the jcc family) handles tagged-exit vs patched-chain uniformly.
    ldr _ip, [_ip]
    b amd64_branch_dispatch
.gadget jcxz
    cbnz ecx, 1f
    ldr _ip, [_ip]
    b jit_ret_chain
1:
    ldr _ip, [_ip, 8]
    b jit_ret_chain

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
    b jit_ret_chain
5:
    ldr _ip, [_ip]
    b jit_ret_chain

.gadget loopnz
    cbz ecx, 4f
    do_jump z, 4f
    ldr _ip, [_ip]
    b jit_ret_chain
4:
    ldr _ip, [_ip, 8]
    b jit_ret_chain

.irp cond, COND_LIST
    .gadget jmp_\cond
        do_jump \cond, 1f
        ldr _ip, [_ip, 8]
        b jit_ret_chain
    1:
        ldr _ip, [_ip]
        b jit_ret_chain

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
.macro amd64_do_jump cond, target
    .ifc \cond,o
        ldrb w8, [_cpu, CPU_of]
        cbnz w8, \target
    .endif
    .ifc \cond,c
        ldrb w8, [_cpu, CPU_cf]
        cbnz w8, \target
    .endif
    .ifc \cond,z
        ldr w8, [_cpu, CPU_eflags]
        tbnz w8, 6, \target
    .endif
    .ifc \cond,cz
        ldrb w8, [_cpu, CPU_cf]
        cbnz w8, \target
        ldr w8, [_cpu, CPU_eflags]
        tbnz w8, 6, \target
    .endif
    .ifc \cond,s
        ldr w8, [_cpu, CPU_eflags]
        tbnz w8, 7, \target
    .endif
    .ifc \cond,p
        ldr w8, [_cpu, CPU_eflags]
        tbnz w8, 2, \target
    .endif
    .ifc \cond,sxo
        ldr w8, [_cpu, CPU_eflags]
        ubfx w8, w8, 7, 1
        ldrb w9, [_cpu, CPU_of]
        cmp w8, w9
        b.ne \target
    .endif
    .ifc \cond,sxoz
        ldr w8, [_cpu, CPU_eflags]
        tbnz w8, 6, \target
        ubfx w8, w8, 7, 1
        ldrb w9, [_cpu, CPU_of]
        cmp w8, w9
        b.ne \target
    .endif
.endm

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
.global amd64_branch_dispatch
amd64_branch_dispatch:
    tbz _ip, 63, amd64_chain
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
    # (amd64_jit_ff_group and its siblings in emu/amd64_interp.c) walk prefixes
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
amd64_chain_poked:
    ldur x8, [_ip, (JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    b jit_ret

# amd64 conditional jumps (0x70-0x7f, 0x0f 80-8f). gen.c emits operand 0 =
# taken target, operand 1 = else (swapped for negated condition codes),
# both bit-63 tagged for the chaining dispatch above. Uses only label 1.
.irp cond, COND_LIST
    .gadget amd64_jcc_\cond
        amd64_do_jump \cond, 1f
        ldr _ip, [_ip, 8]
        b amd64_branch_dispatch
    1:
        ldr _ip, [_ip]
        b amd64_branch_dispatch
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

.gadget pushf
    save_c
    mov x0, _cpu
    bl NAME(helper_collapse_flags)
    restore_c

    sub esp, esp, 4
    mov _addr, esp
    write_prep 32, pushf
    ldr w8, [_cpu, CPU_eflags]
    str w8, [_xaddr]
    write_done 32, pushf
    gret
    write_bullshit 32, pushf

.gadget popf
    mov _addr, esp
    read_prep 32, popf
    ldr w8, [_xaddr]
    str w8, [_cpu, CPU_eflags]
    add esp, esp, 4

    save_c
    mov x0, _cpu
    bl NAME(helper_expand_flags)
    restore_c
    gret
    read_bullshit 32, popf

.gadget sahf
    ubfx w8, eax, 8, 8
    strb w8, [_cpu, CPU_eflags]
    save_c
    mov x0, _cpu
    bl NAME(helper_expand_flags)
    restore_c
    gret

# LAHF (0x9f): AH = the low byte of EFLAGS (SF:ZF:0:AF:0:PF:1:CF). collapse_flags
# materializes that byte (sets bit1=1, bits3/5=0); mirror of sahf in reverse. i386
# had no lahf gadget at all -> guest lahf SIGILL'd.
.gadget lahf
    save_c
    mov x0, _cpu
    bl NAME(helper_collapse_flags)
    restore_c
    ldrb w8, [_cpu, CPU_eflags]
    bfi eax, w8, 8, 8
    gret
