// The amd64 memory-operand macros: effective address from gen.c's meta/disp
// words, and the 64-bit TLB read/write fast paths with their slow-path tails.
// Shared by math.S and x87.S; the slow-path routines they branch to
// (amd64_vmem_read_miss and friends) are in math.S.
# 64-bit TLB read fast path: x9 = guest addr in -> host ptr out (or miss/xpage).
.macro amd64_vread_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vxpage_\id
    and  x8, x9, #0xfffffffffffff000
    ubfx x11, x9, 12, TLB_BITS
    eor  x11, x11, x9, lsr #(12 + TLB_BITS)
    and  x11, x11, #(1 << TLB_BITS) - 1
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page]
    cmp  x8, x13
    b.ne amd64_vmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vback_\id:
.endm

.macro amd64_vread_bullshit size, id
amd64_vmiss_\id:
    bl amd64_vmem_read_miss
    b amd64_vback_\id
amd64_vxpage_\id:
    mov x19, \size
    bl amd64_vmem_crosspage_read
    b amd64_vback_\id
.endm

# Compute the 64-bit effective address from meta(x8)/disp(x9) into x9. Reads base
# and index from cpu->amd64_regs in memory (the reg cache was flushed by gen.c).
.macro amd64_vmem_addr
    tbz  x8, 30, 7001f
    ubfx x0, x8, 20, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    add  x9, x9, x14
7001:
    tbz  x8, 31, 7002f
    ubfx x0, x8, 24, 4
    ubfx x15, x8, 28, 2
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    lsl  x14, x14, x15
    add  x9, x9, x14
7002:
    tbz  x8, 32, 7003f
    ldr  x14, [_ip, 16]
    add  x9, x9, x14
7003:
    /* AMD64_JIT_MEM_ADDR32 (bit 45): a 0x67 address, 32 bits, zero-extended
       before any segment base. */
    tbz  x8, 45, 7009f
    mov  w9, w9
7009:
    /* AMD64_JIT_MEM_FS (bit 33): an %fs-relative address, which in long mode is
       just a flat base added to the effective address. Without this every
       FS-prefixed memory instruction had to bridge -- and that is nearly all of
       them in threaded code, because the stack-protector canary at %fs:0x28 and
       every __thread variable go through it. Measured on a 4-thread pthread
       workload it was 22% of ALL interpreter bridges: mov 8b 12.3%, sub 2b
       5.3%, mov 89 3.0%, xor 33 1.3%. */
    tbz  x8, 33, 7004f
    ldr  x14, [_cpu, CPU_tls_ptr]
    add  x9, x9, x14
7004:
    /* AMD64_JIT_MEM_GS (bit 36): the same for a %gs-relative address, whose
       base ARCH_SET_GS sets. */
    tbz  x8, 36, 7005f
    ldr  x14, [_cpu, CPU_amd64_gs_base]
    add  x9, x9, x14
7005:
    /* AMD64_JIT_MEM_ALIGN16 (bit 37): a legacy-SSE m128 that must be 16-byte
       aligned; misaligned is #GP(0) at the instruction (rip was flushed to it
       before the gadget). */
    tbz  x8, 37, 7006f
    tst  x9, 15
    b.eq 7006f
    b    amd64_vmem_misaligned      /* external to x87.S: no conditional branch reaches it */
7006:
    /* AMD64_JIT_MEM_ALIGN32 (bit 38): a VEX.256 operand that must be 32-byte
       aligned (VMOVAPS ymm and its kind). */
    tbz  x8, 38, 7007f
    tst  x9, 31
    b.eq 7007f
    b    amd64_vmem_misaligned
7007:
    /* AMD64_JIT_MEM_ALIGN64 (bit 39): an EVEX.512 one, 64-byte aligned. */
    tbz  x8, 39, 7008f
    tst  x9, 63
    b.eq 7008f
    b    amd64_vmem_misaligned
7008:
.endm

# The FS or GS base meta x8 names (bits 33, 36), else 0, into \rd.
.macro amd64_vseg_base rd
    mov  \rd, 0
    tbz  x8, 33, 7101f
    ldr  \rd, [_cpu, CPU_tls_ptr]
7101:
    tbz  x8, 36, 7102f
    ldr  \rd, [_cpu, CPU_amd64_gs_base]
7102:
.endm

# A VSIB element address with 0x67 (meta bit 45): x9 came from amd64_vmem_addr
# (base + disp truncated, then the segment base) plus the scaled index; the
# element address is base + disp + index truncated to 32 bits, then the
# segment base. Clobbers x15.
.macro amd64_vsib_addr32
    tbz  x8, 45, 7103f
    amd64_vseg_base x15
    sub  x9, x9, x15
    mov  w9, w9
    add  x9, x9, x15
7103:
.endm

# 64-bit TLB write fast path: x9 = guest addr in -> writable host ptr out (or the
# cross-page staging buffer, flushed by amd64_vwrite_done). Mirrors the 32-bit
# write_prep: a TLB-staleness / host-page-mirroring check routes to the slow C
# resolver; the hit path checks page_if_writable and records the dirty page.
.macro amd64_vwrite_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vwxpage_\id
    ldr  x11, [_tlb, (-TLB_entries+TLB_mmu)]
    ldr  x8,  [_tlb, (-TLB_entries+TLB_mem_changes)]
    ldr  x12, [x11, MMU_changes]
    cmp  x8, x12
    b.ne amd64_vwslow_\id
    ldrb w12, [x11, MMU_requires_write_revalidate]
    cbz  w12, amd64_vwfast_\id
amd64_vwslow_\id:
    bl   amd64_vmem_write_resolve
    b    amd64_vwback_\id
amd64_vwfast_\id:
    and  x8, x9, #0xfffffffffffff000
    str  x8, [_tlb, (-TLB_entries+TLB_dirty_page)]
    ubfx x11, x9, 12, TLB_BITS
    eor  x11, x11, x9, lsr #(12 + TLB_BITS)
    and  x11, x11, #(1 << TLB_BITS) - 1
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page_if_writable]
    cmp  x8, x13
    b.ne amd64_vwmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vwback_\id:
.endm

# After the store, if x9 was the cross-page staging buffer, flush it to memory.
.macro amd64_vwrite_done size, id
    mov x8, LOCAL_value
    add x8, _cpu, x8
    cmp  x8, x9
    b.ne amd64_vwdone_\id
    mov  x19, \size
    bl   amd64_vmem_crosspage_write
amd64_vwdone_\id:
.endm

.macro amd64_vwrite_bullshit size, id
amd64_vwmiss_\id:
    bl   amd64_vmem_write_miss
    b    amd64_vwback_\id
amd64_vwxpage_\id:
    str  x9, [_cpu, LOCAL_value_addr]
    mov x9, LOCAL_value
    add x9, _cpu, x9
    b    amd64_vwback_\id
.endm
