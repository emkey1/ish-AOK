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

# ---- `push <reg>` / `pop <reg>` in ONE gadget ----
#
# The two gadgets above move their value through _tmp, so gen.c has to stage it
# with a separate dispatch on either side:
#     [load32_reg_<r>][push][orig_ip]     3 words, 2 dispatches
#     [fused_push_<r>][orig_ip]           2 words, 1 dispatch
#     [pop][orig_ip][store32_reg_<r>]     3 words, 2 dispatches
#     [fused_pop_<r>][orig_ip]            2 words, 1 dispatch
# These are worth their own family because they are everywhere (every call
# sequence) and, unlike the ALU reg,reg case, the table is ONE dimensional:
# 16 gadgets, not 448. In an i386 profile at the time of writing, push+pop were
# 4.7% of gadget samples and the load32_reg/store32_reg staging they drive was a
# further 21.1% shared with other shapes.
#
# The bodies are the generic ones with _tmp replaced by the guest register.
# read_prep/write_prep only clobber x8-x10 and the guest registers live in
# x20-x27, so the register is safe across the prep (including its slow path,
# which is a C call for writes) -- the same reasoning fused_movrm32 above relies
# on. write_done is likewise kept: on a page-straddling push, write_prep aims
# _xaddr at the frame scratch and write_done is what splits it across pages.
.macro fpush name, reg
    .gadget fused_push_\name
        sub _addr, esp, 4
        write_prep 32, fpush_\name
        str \reg, [_xaddr]
        write_done 32, fpush_\name
        sub esp, esp, 4
        gret 1
        write_bullshit 32, fpush_\name
.endm
.each_reg fpush
.purgem fpush
.gadget_list fused_push, REG_LIST

# `push esp` needs no special case: _addr and the stored value are both taken
# from esp BEFORE the decrement, which is what x86 specifies.
#
# `pop esp` DOES, and is deliberately absent from the table below (a 0 entry, so
# gen.c falls back to the two-dispatch form). Loading straight into esp and then
# running the `add esp, esp, 4` would add 4 to the value just popped. Reordering
# the increment before the load would fix that but breaks something subtler:
# gen.c marks POP with orig_ip_extra bit 62, "on segfault the stack pointer
# should be adjusted", so a fault on the load must leave esp un-incremented or
# the handler's adjustment double-counts. Falling back is correct and `pop esp`
# is rare.
.macro fpop name, reg
    .ifnc \name,reg_sp
    .gadget fused_pop_\name
        mov _addr, esp
        read_prep 32, fpop_\name
        ldr \reg, [_xaddr]
        add esp, esp, 4
        gret 1
        read_bullshit 32, fpop_\name
    .endif
.endm
.each_reg fpop
.purgem fpop

.pushsection_rodata
.global.name fused_pop_gadgets
.irp reg, REG_LIST
    .ifc \reg,reg_sp
        .quad 0
    .else
        .quad NAME(gadget_fused_pop_\reg)
    .endif
.endr
.popsection

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
#
# Measured, and it transfers across host generations (which per this file's
# neighbours is not a safe assumption, so it was checked):
#   Apple Silicon, i386 guest: sh-loop 5.24 -> 4.74s (9.5%), gcc 1.71 -> 1.57s
#   ARMv8.0 (A9 iPad), i386:   sh-loop 20k 8166 -> 7449 ms (8.8%), with the
#     arm64 guest flat as a control (3088 -> 3074 ms), which is what attributes
#     the gain to this change rather than to device state.
# Slightly SMALLER on ARMv8.0, not larger, despite dispatch being pricier there.
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

# ---- `mov <reg>, [<base>+disp]` in ONE gadget ----
#
# The single most-emitted shape in the whole instruction stream: 96709 of 207139
# MOVs on a real gcc compile (46.7%), measured by instrumenting gen_mov. Even
# after the address fusion above it still costs two dispatches, because the load
# lands in the _tmp scratch and a separate store32_reg_<dst> moves it out:
#     [fused_load32_mem_<base>][disp][orig_ip][store32_reg_<dst>]   4 words, 2
# becomes
#     [fused_movmr32_<dst>_<base>][disp][orig_ip]                   3 words, 1
#
# Unlike the address fusion, this CANNOT branch into the existing body: the
# destination differs per gadget, so each needs its own read_prep/read_bullshit
# pair (hence a unique label id per (dst,base), and 64 trampolines).
#
# Loading straight into the guest register is safe across the slow paths, which is
# the part worth checking rather than assuming: read_prep clobbers only x8-x10 and
# _xaddr, and both slow paths (handle_read_miss, crosspage_load) are `bl` calls
# that return to `back_<id>` before the load executes -- and the guest registers
# are x20-x27, inside the callee-saved x19-x28 range, so a C call preserves them.
# That is also why the unfused body loads into w8 (caller-saved) and then moves to
# _tmp rather than loading into _tmp directly.
#
# _ip is advanced past the displacement BEFORE read_prep, so a fault sees _ip at
# [orig_ip] exactly as it does in the unfused path.
.macro fmr32 dst_name, dst, base_name, base
    .gadget fused_movmr32_\dst_name\()_\base_name
        ldr _addr, [_ip]
        add _addr, _addr, \base
        add _ip, _ip, 8
        read_prep 32, fmr32_\dst_name\()_\base_name
        ldr \dst, [_xaddr]
        gret 1
        read_bullshit 32, fmr32_\dst_name\()_\base_name
.endm

# Two dimensions need nested macros, not nested .irp: clang's integrated assembler
# will not substitute .irp parameters inside a .macro body. Same shape as
# mov32_reg_reg_row/_col in math.S.
.macro fmr32_row dst_name, dst
    .macro fmr32_col base_name, base
        fmr32 \dst_name, \dst, \base_name, \base
    .endm
    .each_reg fmr32_col
    .purgem fmr32_col
.endm
.each_reg fmr32_row
.purgem fmr32_row

# 64-entry table, dst-major, indexed [(dst_reg - arg_reg_a) * 8 + modrm->base].
# Built from REG_LIST (enum arg / enum reg32 order), NOT .each_reg order which
# instantiated the bodies above. Emitted manually like mov32_reg_reg_gadgets
# rather than via .gadget_list, since that helper only walks one dimension.
.pushsection_rodata
.global.name fused_movmr32_gadgets
.irp dst, REG_LIST
    .irp base, REG_LIST
        .quad NAME(gadget_fused_movmr32_\dst\()_\base)
    .endr
.endr
.popsection

# ---- `mov [<base>+disp], <reg>` in ONE gadget ----
#
# The mirror of the above, and the next most-emitted shape: 39815 of 207139 MOVs
# (19.2%). Two dispatches become one:
#     [load32_reg_<src>][fused_store32_mem_<base>][disp][orig_ip]   4 words, 2
#     [fused_movrm32_<src>_<base>][disp][orig_ip]                   3 words, 1
#
# Stores need more of the surrounding machinery than loads, and all of it is kept:
#   - write_prep validates writability and may take slow_write (bl resolve_write_ptr),
#     which is a C call -- safe, because the source is a guest register (x20-x27,
#     inside callee-saved x19-x28) and write_prep itself only clobbers x8-x10.
#   - write_done is NOT optional: on a page-straddling store, write_prep points
#     _xaddr at the frame's scratch (LOCAL_value) instead of at guest memory, and
#     write_done is what notices that and calls crosspage_store to split the value
#     across the two pages. Omitting it would silently drop every crosspage store.
#
# The generic body loads the old value into w8 before overwriting it, because
# do_op's sub-32-bit store path writes only w8's low bits. At 32 bits the whole word
# is replaced and `str` writes all of it, so this stores the guest register directly
# and skips that dead load.
.macro fmw32 src_name, src, base_name, base
    .gadget fused_movrm32_\src_name\()_\base_name
        ldr _addr, [_ip]
        add _addr, _addr, \base
        add _ip, _ip, 8
        write_prep 32, fmw32_\src_name\()_\base_name
        str \src, [_xaddr]
        write_done 32, fmw32_\src_name\()_\base_name
        gret 1
        write_bullshit 32, fmw32_\src_name\()_\base_name
.endm

.macro fmw32_row src_name, src
    .macro fmw32_col base_name, base
        fmw32 \src_name, \src, \base_name, \base
    .endm
    .each_reg fmw32_col
    .purgem fmw32_col
.endm
.each_reg fmw32_row
.purgem fmw32_row

# src-major, indexed [(src_reg - arg_reg_a) * 8 + modrm->base], REG_LIST order.
.pushsection_rodata
.global.name fused_movrm32_gadgets
.irp src, REG_LIST
    .irp base, REG_LIST
        .quad NAME(gadget_fused_movrm32_\src\()_\base)
    .endr
.endr
.popsection

# ---- `lea <reg>, [<base>+disp]` in ONE gadget ----
#
# LEA is 18.4% of the instructions that reach gen_mov in this repo's own i386
# Devuan rootfs, and it was the mystery "src=other" 14.3% bucket in the MOV
# emission histogram -- it is the only i386 MOV site whose src is arg_addr
# (emu/decode.h:1171, `MOV(addr, modrm_reg, oz)`), which is exactly the case the
# movmr fusion above declines.
#
# It is also the worst work-to-dispatch ratio in the engine. LEA is pure
# arithmetic -- it computes an address and never touches memory -- yet it costs
# THREE dispatches:
#     [addr_<base>][disp][load32_addr][store32_reg_<dst>]     4 words, 3 dispatches
# where gadget_load32_addr's entire body is `mov w0, w3`: ONE instruction of work
# behind a full gret. Fused:
#     [fused_lea32_<dst>_<base>][disp]                        2 words, 1 dispatch
#
# And because there is no memory access there is no read_prep, no orig_ip word and
# no fault path -- the whole gadget is three instructions. Compare the mov<->mem
# fusions above, which each needed their own TLB trampoline.
#
# dst == base (`lea eax, [eax+8]`) is fine: one add that reads and writes the same
# register. 32-bit only -- at oz=16 LEA writes just the low half of the
# destination, which a full-width add would not reproduce.
.macro flea32 dst_name, dst, base_name, base
    .gadget fused_lea32_\dst_name\()_\base_name
        ldr w8, [_ip]
        add \dst, w8, \base
        gret 1
.endm

.macro flea32_row dst_name, dst
    .macro flea32_col base_name, base
        flea32 \dst_name, \dst, \base_name, \base
    .endm
    .each_reg flea32_col
    .purgem flea32_col
.endm
.each_reg flea32_row
.purgem flea32_row

# dst-major, indexed [(dst_reg - arg_reg_a) * 8 + modrm->base], REG_LIST order.
.pushsection_rodata
.global.name fused_lea32_gadgets
.irp dst, REG_LIST
    .irp base, REG_LIST
        .quad NAME(gadget_fused_lea32_\dst\()_\base)
    .endr
.endr
.popsection

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
