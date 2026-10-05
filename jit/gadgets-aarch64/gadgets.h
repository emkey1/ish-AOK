#include "../gadgets-generic.h"
#include "cpu-offsets.h"

.if TLB_BITS < 10
.error "the 32-bit TLB index assumes TLB_BITS >= 10"
.endif

# register assignments
eax .req w20
xax .req x20
ebx .req w21
ecx .req w22
xcx .req x22
edx .req w23
xdx .req x23
esi .req w24
edi .req w25
ebp .req w26
esp .req w27
_ip .req x28
eip .req w28
_tmp .req w0
_xtmp .req x0
_cpu .req x1
_tlb .req x2
_addr .req w3
_xaddr .req x3

.extern jit_exit
.extern jit_ret

.macro .gadget name
    .global NAME(gadget_\()\name)
    .align 4
    NAME(gadget_\()\name) :
.endm
// DO NOT "optimize" the load+barrier below into a single `ldar`, however
// obviously equivalent it looks. It was tried (861da1d1) and reverted
// (95140ab7): correct, faster on Apple Silicon (i386 sh-loop 4.3%), and a
// 2.04x REGRESSION on ARMv8.0. Measured on an A9 iPad, i386 chroot sh-loop
// 20k, two builds differing only in that change: 8422ms with ldr+dmb, 17203ms
// with ldar, while the sha256 calibrator got faster and the aarch64 guest
// (whose gadgets were untouched) stayed flat.
//
// jit/guest-arm64/gadgets.h says ldar is "close to free" and dmb ishld "costs
// tens of cycles", and invites keeping the two dispatchers in sync. That
// measurement is Apple Silicon only. Apple's cores retire ldar almost free;
// the A9 evidently implements load-acquire far more conservatively, and this
// sequence runs once per guest instruction, so a per-dispatch acquire is
// brutal there. Old devices are the point of this project, so they win.
// (Open, untested: guest-arm64 still dispatches via ldar, so the arm64 guest
// may be paying that same penalty on ARMv8.0. A/B it on an ARMv8.0 device.)
// gret_nomem: gret without the barrier, for a gadget that reads NO guest
// memory (register ALU and moves, immediates, address arithmetic, lea). The
// `dmb ishld` in every other gret is load-bearing for more than code patching:
// ordering each guest load before every later access is what gives i386/amd64
// guests x86's load->load and load->store ordering -- dropping it from all
// gadgets made an xchg spinlock lose updates across threads
// (tests/manual/x86/atomic_lock_contended.c). A gadget with no guest load has
// nothing of its own to order, and a chain link is only ever loaded by a
// branch gadget, which keeps the full gret -- so skipping the barrier here
// changes no ordering the guest can observe.
.macro gret_nomem pop=0
    ldr x8, [_ip, \pop*8]!
    add _ip, _ip, 8
    cbnz x8, 0f
    b jit_ret
0:  br x8
.endm

.macro gret pop=0
    ldr x8, [_ip, \pop*8]!
    dmb ishld /* Jason Conway's Re Ordering patch (upstream PR #1944 */
    add _ip, _ip, 8 /* TODO get rid of this */
    cbnz x8, 0f   /* null gadget safety: if non-null, execute normally */
    b jit_ret     /* null gadget: bail safely (cbz can't reach external; unconditional branch can) */
0:  br x8
.endm

# jit_ret_chain (entry.S) inlined: enter the block whose code _ip points at.
# Each branch gadget carries its own copy so its final indirect `br` is a
# separate site for the host's branch predictor; with one shared copy every
# chained transition in the engine went through the same `br`, and call/ret
# (whose targets depend on a chain of loads) paid for the mispredictions.
# Conditional branches cannot reach jit_ret/poke in another object file, hence
# the local trampolines.
.macro chain_ip
    cmp _ip, 0
    b.lt 8701f
    ldr x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, 8702f
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le 8702f
    sub x8, _ip, JIT_BLOCK_code
    str x8, [_cpu, LOCAL_last_block]
    gret
8701:
    b jit_ret
8702:
    b poke
.endm

# amd64_chain (control.S) inlined, for the same reason: each amd64 branch
# gadget (jmp, jcc, jrcxz, the fused cmp/test+jcc tails) ends in its own
# `br`. The shared copy put every chained amd64 transition through one
# indirect branch. A tagged (unchained) word and the poked/out-of-budget
# case leave through the shared code, which is out of reach of a
# conditional branch from another object file -- hence the trampolines.
# nomem=1 drops the dmb ishld from the entry: for branch gadgets that read
# no guest memory themselves (every guest load is followed by its own
# gadget's barrier, which is what x86's load ordering needs), as
# gret_nomem does for the i386 gadgets.
.macro amd64_chain_ip nomem=0
    tbnz _ip, 63, 8711f
    ldr x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, 8712f
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le 8712f
    sub x8, _ip, JIT_BLOCK_code
    str x8, [_cpu, LOCAL_last_block]
    ldr x8, [x8, JIT_BLOCK_addr]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    .if \nomem
    gret_nomem
    .else
    gret
    .endif
8711:
    b amd64_branch_dispatch
8712:
    b amd64_chain_poked
.endm

# A misaligned 16/32-bit LOCK operand: helper_atomic_unaligned (jit/helpers.c)
# does it exactly in C, because the ldaxr/stlxr fast path faults the host on a
# misaligned address. \code is the helper's UA_* number; _tmp goes in as the
# operand and comes back as the new _tmp (the old value for xadd/xchg). Only
# cmpxchg reads a guest register (eax), so only it spills and reloads one.
.macro ua_slow code, size, spill_eax=0
    .if \spill_eax
        str eax, [_cpu, CPU_eax]
    .endif
    save_c
    mov w4, _tmp        // first: _tmp is w0, which the cpu argument overwrites
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    mov w2, _addr
    mov w3, (\code | (\size << 8))
    bl NAME(helper_atomic_unaligned)
    mov x14, x0
    restore_c
    .if \spill_eax
        ldr eax, [_cpu, CPU_eax]
    .endif
    tbnz x14, 32, 8703f
    mov _tmp, w14
    gret 1
8703:
    b segfault_write
.endm
# UA_* numbers, as in jit/helpers.c.
#define UA_ADD 0
#define UA_SUB 1
#define UA_ADC 2
#define UA_SBB 3
#define UA_AND 4
#define UA_OR 5
#define UA_XOR 6
#define UA_INC 7
#define UA_DEC 8
#define UA_XADD 9
#define UA_NOT 10
#define UA_NEG 11
#define UA_XCHG 12
#define UA_CMPXCHG 13
#define UA_BTS 14
#define UA_BTR 15
#define UA_BTC 16

# memory reading and writing
.irp type, read,write

.macro \type\()_prep size, id
    and w8, _addr, 0xfff
    cmp x8, (0x1000-(\size/8))
    b.hi crosspage_load_\id
    .ifc \type,write
        # Writes take the slow path (__tlb_write_ptr via resolve_write_ptr)
        # when the TLB is stale or host page mirroring requires revalidating
        # writable hits. IMPORTANT: only x8-x10 may be clobbered here — the
        # rep-string gadgets keep their stride in w12 live across prep, which
        # is what broke the earlier inline check (see fbcb277c).
        ldr x9, [_tlb, (-TLB_entries+TLB_mmu)]
        ldr x8, [_tlb, (-TLB_entries+TLB_mem_changes)]
        ldr x10, [x9, MMU_changes]
        cmp x8, x10
        b.ne slow_write_\id
        ldrb w9, [x9, MMU_requires_write_revalidate]
        cbz w9, fast_write_\id
slow_write_\id :
        bl resolve_write_ptr
        b back_\id
fast_write_\id :
    .endif
    and w8, _addr, 0xfffff000
    .ifc \type,write
        str w8, [_tlb, (-TLB_entries+TLB_dirty_page)]
    .endif
    # TLB_INDEX (emu/tlb.h) for a 32-bit address: the high term has 20 -
    # TLB_BITS bits, never more than TLB_BITS, so no mask is needed.
    ubfx x9, _xaddr, 12, TLB_BITS
    eor x9, x9, _xaddr, lsr #(12 + TLB_BITS)
    mov w10, TLB_ENTRY_SIZE
    madd x9, x9, x10, _tlb
    .ifc \type,read
        ldr w10, [x9, TLB_ENTRY_page]
    .else
        ldr w10, [x9, TLB_ENTRY_page_if_writable]
    .endif
    cmp w8, w10
    b.ne handle_miss_\id
    ldr x10, [x9, TLB_ENTRY_data_minus_addr]
    add _xaddr, x10, _xaddr, uxtx
back_\id:
.endm

.macro \type\()_bullshit size, id
handle_miss_\id :
    bl handle_\type\()_miss
    b back_\id
crosspage_load_\id :
    mov x19, (\size/8)
    bl crosspage_load
    b back_\id
.ifc \type,write
crosspage_store_\id :
    mov x19, (\size/8)
    bl crosspage_store
    b back_write_done_\id
.endif
.endm

.endr
.macro write_done size, id
    mov x8, LOCAL_value
    add x8, _cpu, x8
    cmp x8, _xaddr
    b.eq crosspage_store_\id
back_write_done_\id :
.endm

.macro .each_reg macro:vararg
    \macro reg_a, eax
    \macro reg_b, ebx
    \macro reg_c, ecx
    \macro reg_d, edx
    \macro reg_si, esi
    \macro reg_di, edi
    \macro reg_bp, ebp
    \macro reg_sp, esp
.endm

.macro ss size, macro, args:vararg
    .ifnb \args
        .if \size == 8
            \macro \args, \size, b
        .elseif \size == 16
            \macro \args, \size, h
        .elseif \size == 32
            \macro \args, \size,
        .else
            .error "bad size"
        .endif
    .else
        .if \size == 8
            \macro \size, b
        .elseif \size == 16
            \macro \size, h
        .elseif \size == 32
            \macro \size,
        .else
            .error "bad size"
        .endif
    .endif
.endm

.macro setf_c
    cset w10, cc
    strb w10, [_cpu, CPU_cf]
.endm
.macro setf_oc
    cset w10, vs
    strb w10, [_cpu, CPU_of]
    setf_c
.endm
.macro setf_a src, dst, s=
    movs w10, \src, \s
    str w10, [_cpu, CPU_op1]
    movs w10, \dst, \s
    str w10, [_cpu, CPU_op2]
    ldr w10, [_cpu, CPU_flags_res]
    orr w10, w10, AF_OPS
    str w10, [_cpu, CPU_flags_res]
.endm
.macro clearf_a
    ldr w10, [_cpu, CPU_eflags]
    ldr w11, [_cpu, CPU_flags_res]
    bic w10, w10, AF_FLAG
    bic w11, w11, AF_OPS
    str w10, [_cpu, CPU_eflags]
    str w11, [_cpu, CPU_flags_res]
.endm
.macro clearf_oc
    strb wzr, [_cpu, CPU_of]
    strb wzr, [_cpu, CPU_cf]
.endm
# The deposit an add-family op (add/sub/adc/sbc/inc/dec) leaves: op1 and op2
# for AF, the result for ZF/SF/PF. flags_res holds only those four bits, so
# afterwards it is exactly ZF_RES|SF_RES|PF_RES|AF_OPS whatever it was before:
# a constant byte store, where setf_a + setf_zsp did two load-or-store rounds
# on it. On the A10X those rounds chained through store-to-load forwarding
# from one ALU instruction to the next.
.macro setf_ops src, dst, s=
    movs w10, \src, \s
    str w10, [_cpu, CPU_op1]
    movs w10, \dst, \s
    str w10, [_cpu, CPU_op2]
.endm
.macro setf_all_res s, val=_tmp
    .ifnb \s
        sxt\s \val, \val
    .endif
    str \val, [_cpu, CPU_res]
    mov w10, (ZF_RES|SF_RES|PF_RES|AF_OPS)
    strb w10, [_cpu, CPU_flags_res]
.endm
# The logic family (and/or/xor/test) clears AF. Rather than clear it in
# eflags (a load-and-store of eflags plus one of flags_res), AF is left in the
# lazy form with op1 = res and op2 = 0, so op1^op2^res is 0 -- the same three
# plain stores and constant flags_res byte as the add family.
.macro setf_logic_res s, val=_tmp
    .ifnb \s
        sxt\s \val, \val
    .endif
    str \val, [_cpu, CPU_res]
    str \val, [_cpu, CPU_op1]
    str wzr, [_cpu, CPU_op2]
    mov w10, (ZF_RES|SF_RES|PF_RES|AF_OPS)
    strb w10, [_cpu, CPU_flags_res]
.endm

.macro setf_zsp s, val=_tmp
    .ifnb \s
        sxt\s \val, \val
    .endif
    str \val, [_cpu, CPU_res]
    ldr w10, [_cpu, CPU_flags_res]
    orr w10, w10, (ZF_RES|SF_RES|PF_RES)
    str w10, [_cpu, CPU_flags_res]
.endm

.macro save_c
    stp x0, x1, [sp, -0x60]!
    stp x2, x3, [sp, 0x10]
    stp x8, x9, [sp, 0x20]
    stp x10, x11, [sp, 0x30]
    stp x12, x13, [sp, 0x40]
    str lr, [sp, 0x50]
.endm
.macro restore_c
    ldr lr, [sp, 0x50]
    ldp x12, x13, [sp, 0x40]
    ldp x10, x11, [sp, 0x30]
    ldp x8, x9, [sp, 0x20]
    ldp x2, x3, [sp, 0x10]
    ldp x0, x1, [sp], 0x60
.endm

.macro movs dst, src, s
    .ifc \s,h
        bfxil \dst, \src, 0, 16
    .else N .ifc \s,b
        bfxil \dst, \src, 0, 8
    .else
        mov \dst, \src
    .endif N .endif
.endm
.macro op_s op, dst, src1, src2, s
    .ifb \s
        \op \dst, \src1, \src2
    .else
        movs w10, \dst, \s
        \op w10, \src1, \src2
        movs \dst, w10, \s
    .endif
.endm
.macro ldrs src, dst, s
    ldr\s w10, \dst
    movs \src, w10, \s
.endm

.macro uxts dst, src, s=
    .ifnb \s
        uxt\s \dst, \src
        .exitm
    .endif
    .ifnc \dst,\src
        mov \dst, \src
    .endif
.endm

.macro load_regs
    ldr eax, [_cpu, CPU_eax]
    ldr ebx, [_cpu, CPU_ebx]
    ldr ecx, [_cpu, CPU_ecx]
    ldr edx, [_cpu, CPU_edx]
    ldr esi, [_cpu, CPU_esi]
    ldr edi, [_cpu, CPU_edi]
    ldr ebp, [_cpu, CPU_ebp]
    ldr esp, [_cpu, CPU_esp]
.endm

.macro save_regs
    str eax, [_cpu, CPU_eax]
    str ebx, [_cpu, CPU_ebx]
    str ecx, [_cpu, CPU_ecx]
    str edx, [_cpu, CPU_edx]
    str edi, [_cpu, CPU_edi]
    str esi, [_cpu, CPU_esi]
    str ebp, [_cpu, CPU_ebp]
    str esp, [_cpu, CPU_esp]
    str eip, [_cpu, CPU_eip]
.endm

# vim: ft=gas

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

