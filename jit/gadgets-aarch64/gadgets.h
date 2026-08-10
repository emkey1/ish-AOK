#include "../gadgets-generic.h"
#include "cpu-offsets.h"

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
// Dispatch. The gadget-pointer load is a LOAD-ACQUIRE (ldar) rather than the
// `ldr + dmb ishld` this used to be (Jason Conway's re-ordering fix, upstream
// PR #1944): acquire ordering gives exactly the same guarantee the trailing
// barrier did -- this load is ordered before every later access, so a
// concurrently-patched chain word cannot be observed before the block contents
// it points into -- and it pairs directly with the __ATOMIC_RELEASE store the
// frontend uses when it patches a jump word.
//
// This is the form jit/guest-arm64/gadgets.h already converted to, where the
// measurement is recorded: the sequence runs once per guest instruction, which
// makes it the single largest constant in the engine (~34 host cycles per guest
// instruction with the dmb, dominated by it), and ldar is close to free by
// comparison. That header says "keep in sync"; the x86 side was never brought
// along, so i386 AND amd64 (both use these gadgets) kept paying the barrier.
//
// ldar takes no addressing mode, so the pop is folded into an explicit add,
// which still comes out one instruction SHORTER than the old sequence at pop=0.
.macro gret pop=0
.if \pop != 0
    add _ip, _ip, \pop*8
.endif
    ldar x8, [_ip]
    add _ip, _ip, 8
    cbnz x8, 0f   /* null gadget safety: if non-null, execute normally */
    b jit_ret     /* null gadget: bail safely (cbz can't reach external; unconditional branch can) */
0:  br x8
.endm

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
    ubfx x9, _xaddr, 12, 10
    eor x9, x9, _xaddr, lsr 22
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
