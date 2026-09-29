// RISC-V guest load/store gadgets.
//
// The TLB miss/slow/crosspage helper routines below are riscv64_-prefixed
// copies of jit/guest-arm64/memory.S's (same register convention; that
// file's extensive bug-history comments are inherited verbatim as fixes,
// not re-learned), with the segfault paths writing CPU_riscv64_pc. The
// fault-restart contract is identical: every memory gadget's LAST
// code-stream word is its instruction's guest address, and the segfault
// paths rewind CPU_riscv64_pc from [_ip, #-8] so a kernel-resolved fault
// (CoW after fork) resumes at the FAULTING instruction, not block start.
//
// Load code stream:  [gadget][rd_off][rs1_off][imm][orig_ip]
// Store code stream: [gadget][rs2_off][rs1_off][imm][orig_ip]
// Offsets are byte offsets into cpu_state (gadgets.h header). The store
// value is loaded into callee-saved x19 BEFORE write_prep (prep may bl
// into C, clobbering x0-x18; x27 is the helpers' scratch). Loads keep
// rd_off in x19 across read_prep for the same reason.

#include "gadgets.h"

.text

.extern tlb_handle_miss
.extern tlb_write_ptr_slow
.extern __tlb_read_cross_page
.extern __tlb_write_cross_page

// Both segfault paths also rewind the guest PC to the FAULTING
// INSTRUCTION, read from the fixed [-8] slot of the just-consumed code
// stream: every memory-touching gadget's LAST parameter word is its
// instruction's guest address (emitted by gen_step_riscv64), and every
// gadget advances _ip past all its params before any prep macro can
// fault, so [_ip, #-8] is always that address here. Without this, an
// INT_PF resolved by the kernel (the everyday case: first write to a
// copy-on-write page after fork) resumed at the BLOCK-START pc,
// re-executing every instruction before the faulting one — real bug:
// busybox sh crashed post-fork because musl's __syscall_ret block did
// `mov x0,#-1` before the errno store, the store CoW-faulted, and the
// rerun computed the errno pointer from the clobbered x0. Re-executing
// just the faulting instruction is sound: its inputs are untouched at
// fault time (writeback in the load/store gadgets happens after the
// access).
.global riscv64_segfault_read
riscv64_segfault_read:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_riscv64_pc]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b jit_exit

.global riscv64_segfault_write
riscv64_segfault_write:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_riscv64_pc]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    mov w9, #1
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b jit_exit

// Real bug caught by testing: these routines used x19 as their own scratch
// to stash the C call's return value across restore_c. But x19 (like
// x19-x26 generally) is exactly the register range ldp_gadget/stp_gadget
// (memory.S) hold their own live cross-call state in (rt/rt2/rn/mode/
// offset -- chosen specifically because x19-x28 are callee-saved and
// "guaranteed to survive any well-behaved C function call", per this
// file's own earlier header comment). These hand-written routines are not
// "well-behaved" in that sense: they clobber x19 without saving/restoring
// it, silently corrupting a caller's live offset (e.g. ldp/stp's
// pre/post-index displacement) on every miss/slow-path/crosspage call.
// restore_c only reloads x0-x18+lr, so x19 can't be used as scratch here
// either without permanently losing the caller's value. Fixed by using
// x27 instead -- unused by every gadget in this port so far, genuinely
// free for these routines' own internal use.
.global riscv64_handle_read_miss
riscv64_handle_read_miss:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_READ
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, riscv64_segfault_read
    mov _addr, x27
    ret

.global riscv64_handle_write_miss
riscv64_handle_write_miss:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_WRITE
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, riscv64_segfault_write
    mov _addr, x27
    ret

.global riscv64_resolve_write_ptr
riscv64_resolve_write_ptr:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    bl NAME(tlb_write_ptr_slow)
    mov x27, x0
    restore_c
    cbz x27, riscv64_segfault_write
    mov _addr, x27
    ret

// Real bug caught by testing: _cpu is aliased to x1 (gadgets.h), the same
// TWO real register-aliasing bugs caught here, one by testing, one by
// the fix for the first: the AAPCS64 argument registers x0/x1/x2 ARE the
// gadget convention's _tmp/_cpu/_tlb, so argument setup order is load-
// bearing in both directions. The original wrote x1 (arg1 = addr) first,
// clobbering _cpu before arg2 (= _cpu + LOCAL_value) read it. The first
// "fix" moved the arg2 computation to the top -- which writes x2,
// clobbering _tlb before arg0 (= _tlb - TLB_entries) read it, sending a
// wild tlb pointer into __tlb_read_cross_page (caught by the page-
// straddling case in tests/arm64/riscv64_dpextra.s, host-crashing on
// tlb->mmu). The only safe order consumes x2 first and writes x1 last:
//   x0 <- f(x2)   (reads _tlb; x0/_tmp holds nothing live)
//   x2 <- f(x1)   (reads _cpu, overwrites _tlb -- already consumed)
//   x3 <- x27
//   x1 <- x7      (overwrites _cpu -- already consumed; x7/_addr last-read here)
// Also uses x27 (not x19) for the size argument gadgets.h's bullshit macro
// passes in -- same x19-corrupts-the-caller's-live-offset bug as the miss
// handlers above, just via argument-passing instead of return-stash.
//
// Serves BOTH reads and crosspage WRITES (i386's design, mirrored): for a
// write, this fills LOCAL_value with the current guest bytes, stashes the
// guest address in LOCAL_value_addr, and redirects _addr to the buffer so
// the gadget's store writes into it; write_done then detects the redirect
// and riscv64_crosspage_store (below) flushes the buffer back to guest
// memory at the stashed address.
.global riscv64_crosspage_load
riscv64_crosspage_load:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x3, x27             // size in bytes, set by the caller (gadgets.h's bullshit macro)
    mov x1, _addr
    bl NAME(__tlb_read_cross_page)
    mov x27, x0
    restore_c
    cbz x27, riscv64_segfault_read
    str _addr, [_cpu, #LOCAL_value_addr]
    mov _addr, LOCAL_value
    add _addr, _cpu, _addr
    ret

// Real bug caught by re-reading i386's flow (not by a failing test --
// nothing exercises crosspage writes yet): this used `mov x1, _addr` for
// the guest address, but by flush time _addr has been REDIRECTED to the
// LOCAL_value buffer by riscv64_crosspage_load above -- the guest address
// only survives in LOCAL_value_addr, which is where i386's own
// crosspage_store (jit/gadgets-aarch64/memory.S:138) reloads it from.
// LOCAL_value_addr was also widened to 64 bits (jit/frame.h) since arm64
// guest addresses don't fit the old 32-bit addr_t field.
.global riscv64_crosspage_store
riscv64_crosspage_store:
    save_c
    // Same argument-order constraint as riscv64_crosspage_load above:
    // consume x2 (_tlb) before writing it, write x1 (_cpu) last.
    sub x0, _tlb, #TLB_entries
    ldr x3, [_cpu, #LOCAL_value_addr] // guest addr, staged in x3 while _cpu is intact
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x1, x3
    mov x3, x27             // size in bytes, set by the caller (gadgets.h's write_bullshit)
    bl NAME(__tlb_write_cross_page)
    mov x27, x0
    restore_c
    cbz x27, riscv64_segfault_write
    ret


// ---- Loads ---------------------------------------------------------------
.macro load_gadget name, size, id, ld_insn, reg
.gadget \name
    ldr x19, [_ip]            // rd_off (callee-saved across prep)
    ldr x9, [_ip, #8]         // rs1_off
    ldr x10, [_ip, #16]       // imm
    add _ip, _ip, #32         // 3 params + trailing orig_ip (fault restart)
    ldr x7, [_cpu, x9]
    add x7, x7, x10           // effective address
    read_prep \size, \id
    \ld_insn \reg, [x7]
    str x11, [_cpu, x19]
    gret
    read_bullshit \size, \id
.endm

load_gadget lb,  8,  rvlb,  ldrsb, x11
load_gadget lbu, 8,  rvlbu, ldrb,  w11
load_gadget lh,  16, rvlh,  ldrsh, x11
load_gadget lhu, 16, rvlhu, ldrh,  w11
load_gadget lw,  32, rvlw,  ldrsw, x11
load_gadget lwu, 32, rvlwu, ldr,   w11
load_gadget ld,  64, rvld,  ldr,   x11

// ---- Stores --------------------------------------------------------------
.macro store_gadget name, size, id, st_insn, reg
.gadget \name
    ldr x9, [_ip]             // rs2_off (value)
    ldr x10, [_ip, #8]        // rs1_off (base)
    ldr x11, [_ip, #16]       // imm
    add _ip, _ip, #32
    ldr x19, [_cpu, x9]       // value survives prep in callee-saved reg
    ldr x7, [_cpu, x10]
    add x7, x7, x11
    write_prep \size, \id
    \st_insn \reg, [x7]
    write_done \size, \id
    gret
    write_bullshit \size, \id
.endm

store_gadget sb, 8,  rvsb, strb, w19
store_gadget sh, 16, rvsh, strh, w19
store_gadget sw, 32, rvsw, str,  w19
store_gadget sd, 64, rvsd, str,  x19

// ---- Pairs: two 64-bit loads, or two 64-bit stores, off one base ----------
// RV64 has no ldp/stp, so a function's register saves and restores are runs
// of sd and ld: ld->ld and sd->sd are 12% of adjacent pairs in a gcc compile
// (tools/jitprof-report.py). gen_riscv64_try_pair folds each pair into one
// gadget; fld/fsd take part too, through their f-register slot offsets.
// Code stream: [gadget][r1_off][rs1_off][imm1][r2_off][imm2][pc2][pc1]
// pc1 is last, as for every memory gadget, so a fault in the first access
// restarts there. The second access steps _ip back a word first, so a fault
// in it reports -- and a kernel-resolved one restarts at -- its own pc2, with
// the first access already done, exactly as unfused. The base register is
// read once: gen refuses a pair whose first load replaces it.
.gadget ld_ld
    ldr x19, [_ip]            // r1_off
    ldr x9, [_ip, #8]         // rs1_off
    ldr x10, [_ip, #16]       // imm1
    ldr x20, [_ip, #24]       // r2_off
    ldr x22, [_ip, #32]       // imm2
    add _ip, _ip, #56
    ldr x21, [_cpu, x9]       // base
    add x7, x21, x10
    read_prep 64, rvldld1
    ldr x11, [x7]
    str x11, [_cpu, x19]
    sub _ip, _ip, #8          // [_ip, #-8] = pc2 for the second access
    add x7, x21, x22
    read_prep 64, rvldld2
    ldr x11, [x7]
    str x11, [_cpu, x20]
    add _ip, _ip, #8
    gret
    read_bullshit 64, rvldld1
    read_bullshit 64, rvldld2

.gadget sd_sd
    ldr x9, [_ip]             // r1_off (first value)
    ldr x10, [_ip, #8]        // rs1_off
    ldr x11, [_ip, #16]       // imm1
    ldr x12, [_ip, #24]       // r2_off (second value)
    ldr x22, [_ip, #32]       // imm2
    add _ip, _ip, #56
    ldr x19, [_cpu, x9]       // both values and the base survive prep in
    ldr x20, [_cpu, x12]      // callee-saved registers
    ldr x21, [_cpu, x10]
    add x7, x21, x11
    write_prep 64, rvsdsd1
    str x19, [x7]
    write_done 64, rvsdsd1
    sub _ip, _ip, #8          // [_ip, #-8] = pc2 for the second access
    add x7, x21, x22
    write_prep 64, rvsdsd2
    str x20, [x7]
    write_done 64, rvsdsd2
    add _ip, _ip, #8
    gret
    write_bullshit 64, rvsdsd1
    write_bullshit 64, rvsdsd2

// ---- FP loads/stores (F/D extension) --------------------------------------
// fld/fsd/fsw reuse the integer ld/sd/sw gadgets directly: gen passes the
// f-register slot's byte offset instead of an x-register's, and the byte
// semantics are identical. Only flw needs its own body: a 32-bit FP load
// NaN-boxes into the 64-bit register (high 32 bits all-ones) per the ISA.
.gadget flw
    ldr x19, [_ip]            // fd_off
    ldr x9, [_ip, #8]         // rs1_off
    ldr x10, [_ip, #16]       // imm
    add _ip, _ip, #32
    ldr x7, [_cpu, x9]
    add x7, x7, x10
    read_prep 32, rvflw
    ldr w11, [x7]
    orr x11, x11, #0xffffffff00000000  // NaN-box
    str x11, [_cpu, x19]
    gret
    read_bullshit 32, rvflw

// ISH_JIT_PROFILE (jit/jitprof.c): count one entry of this block; see
// jit/guest-arm64/memory.S. Code stream: [&counter].
.gadget block_count
    ldr x8, [_ip], #8
    ldr x9, [x8]
    add x9, x9, #1
    str x9, [x8]
    gret
