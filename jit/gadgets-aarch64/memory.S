#include "gadgets.h"
#include "emu/interrupt.h"

.gadget push
    sub _addr, esp, 4
    write_prep 32, push
    str _tmp, [_xaddr]
    write_done 32, push
    sub esp, esp, 4
    gret 1
    write_bullshit 32, push
.gadget pop
    mov _addr, esp
    read_prep 32, pop
    ldr _tmp, [_xaddr]
    add esp, esp, 4
    gret 1
    read_bullshit 32, pop

.macro x name, reg
    .gadget addr_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        gret 1
.endm
.each_reg x
.purgem x
.gadget addr_none
    ldr _addr, [_ip]
    gret 1
.gadget_list addr, REG_LIST

# Address-computation FUSED into the memory-operand load/store, saving one whole
# gadget dispatch per memory operand. Emitted by gen_try_fuse_addr() in
# jit/gen.c; see there for when it declines. addr_* was 20% of all gadget samples
# in two different profiles (992/4969 on a real gcc compile), and it does two
# instructions of work behind a full gret, so the dispatch cost more than the
# work.
#
# The stream goes from
#     [addr_<base>][disp]  [<op>32_mem][orig_ip]     4 words, 2 dispatches
# to
#     [fused_<op>32_mem_<base>][disp][orig_ip]       3 words, 1 dispatch
#
# The trick that makes this cheap and safe is the tail: rather than duplicating
# the op's body (which would need its own read_prep/read_bullshit label ids and
# is where a copied gadget would rot), we reproduce exactly the register state
# `gret 1` leaves and then branch straight into the existing gadget. On entry
# _ip points at [disp]; the unfused addr gadget's `gret 1` advances _ip past
# disp AND past the op's gadget pointer, landing on [orig_ip], which is where
# the op body expects it. `add _ip, _ip, 8` reproduces that here, so the body
# runs bit-identically. What is removed is the dispatch itself: a load of the
# next gadget pointer, the `dmb ishld`, and an indirect `br`, replaced by one
# direct `b`.
#
# Correctness note: the 32-bit `add` is deliberate -- it wraps the effective
# address at 4G exactly as the addr_* gadget it replaces does.
# load/store x 8/16/32 covers ~90% of the fusable memory operands measured
# (load32 50.2%, store32 24.2%, load8 8.2%, store8 4.3%, load16 3.0%, store16
# 0.4%). The address arithmetic is size-independent -- only the data width in the
# body differs -- so one prologue shape serves all of them.
# Spelled out one macro per (op,size) rather than nesting .irp inside .macro:
# clang's integrated assembler does not substitute .irp parameters inside a
# .macro body ("unexpected token" at instantiation), and this file is not the
# place to be clever -- a malformed gadget array here is a runtime wrong-pointer,
# not a build error.
.macro xf_l8 name, reg
    .gadget fused_load8_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_load8_mem)
.endm
.each_reg xf_l8
.purgem xf_l8

.macro xf_l16 name, reg
    .gadget fused_load16_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_load16_mem)
.endm
.each_reg xf_l16
.purgem xf_l16

.macro xf_l32 name, reg
    .gadget fused_load32_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_load32_mem)
.endm
.each_reg xf_l32
.purgem xf_l32

.macro xf_s8 name, reg
    .gadget fused_store8_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_store8_mem)
.endm
.each_reg xf_s8
.purgem xf_s8

.macro xf_s16 name, reg
    .gadget fused_store16_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_store16_mem)
.endm
.each_reg xf_s16
.purgem xf_s16

.macro xf_s32 name, reg
    .gadget fused_store32_mem_\name
        ldr _addr, [_ip]
        add _addr, _addr, \reg
        add _ip, _ip, 8
        b NAME(gadget_store32_mem)
.endm
.each_reg xf_s32
.purgem xf_s32
# One array per (op,size), each indexed by enum reg32 via REG_LIST. Keep these in
# step with the tables gen_try_fuse_addr() looks up: a short or misordered array
# here is not a build error, it is a wrong function pointer at runtime.
.gadget_list fused_load8_mem, REG_LIST
.gadget_list fused_load16_mem, REG_LIST
.gadget_list fused_load32_mem, REG_LIST
.gadget_list fused_store8_mem, REG_LIST
.gadget_list fused_store16_mem, REG_LIST
.gadget_list fused_store32_mem, REG_LIST

.macro x name, reg
    .irp times, 1,2,4,8
        .gadget si_\name\()_\times
            .ifc \times,1
                add _addr, _addr, \reg
            .else N .ifc \times,2
                add _addr, _addr, \reg, lsl 1
            .else N .ifc \times,4
                add _addr, _addr, \reg, lsl 2
            .else N .ifc \times,8
                add _addr, _addr, \reg, lsl 3
            .endif N .endif N .endif N .endif
            gret
    .endr
.endm
.each_reg x
.purgem x

.pushsection_rodata
.global NAME(si_gadgets)
NAME(si_gadgets):
.irp reg, REG_LIST
    .irp times, 1,2,4,8
        .quad NAME(gadget_si_\reg\()_\times)
    .endr
.endr
.popsection

.gadget seg_gs
    ldr w8, [_cpu, CPU_tls_ptr]
    add _addr, _addr, w8
    gret

.irp type, read,write

.global handle_\type\()_miss
handle_\type\()_miss:
    save_c
    # %tlb actually points to tlb->entries
    sub x0, _tlb, TLB_entries
    mov w1, _addr
    .ifc \type,read
        mov x2, xzr
    .else
        mov x2, 1
    .endif
    bl NAME(tlb_handle_miss)
    mov x19, x0
    restore_c
    cbz x19, segfault_\type
    mov _xaddr, x19
    ret

.global segfault_\type
segfault_\type:
    ldr _addr, [_tlb, -TLB_entries+TLB_segfault_addr]
    str _addr, [_cpu, CPU_segfault_addr]
    .ifc \type,read
        mov w0, 0
    .else
        mov w0, 1
    .endif
    strb w0, [_cpu, CPU_segfault_was_write]
    ldr x0, [_ip]
    mov eip, w0
    sub w8, esp, 4
    tst x0, 1 << 62
    csel esp, esp, w8, eq
    mov x0, INT_GPF
    b jit_exit

.endr

.global resolve_write_ptr
resolve_write_ptr:
    save_c
    sub x0, _tlb, TLB_entries
    mov w1, _addr
    bl NAME(tlb_write_ptr_slow)
    mov x19, x0
    restore_c
    cbz x19, segfault_write
    mov _xaddr, x19
    ret

.global crosspage_load
crosspage_load:
    save_c
    sub x0, _tlb, TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov w1, _addr
    mov x3, x19
    bl NAME(__tlb_read_cross_page)
    mov x19, x0
    restore_c
    cbz x19, segfault_read
    str _addr, [_cpu, LOCAL_value_addr]
    mov _xaddr, LOCAL_value
    add _xaddr, _cpu, _xaddr
    ret

.global crosspage_store
crosspage_store:
    save_c
    sub x0, _tlb, TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    ldr w1, [_cpu, LOCAL_value_addr]
    mov x3, x19
    bl NAME(__tlb_write_cross_page)
    mov x19, x0
    restore_c
    cbz x19, segfault_write
    ret
