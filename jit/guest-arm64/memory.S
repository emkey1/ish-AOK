// AArch64 guest load/store gadgets. Independently written, NOT adapted
// from OpenMinis/ish-arm64's memory.S: their LDP/STP support is split
// across many performance-specialized fast-path gadgets (stp64_imm,
// stp64_imm_preidx_sp, stp64_imm_postidx_sp, stp64_imm_preidx_fast, ...)
// selected by gen.c based on register/addressing-mode combinations. This
// port needs correctness across all three addressing modes (post-index,
// signed offset, pre-index) first; one generic gadget per (size,
// direction) pair here, following the same "generic path first, perf
// specializations later if profiling shows they matter" approach already
// used for ADD/SUB immediate's non-fast-path gadgets. Mirrors
// emu/arm64_interp.c's LDP/STP handler's addressing-mode logic (mode
// values: 1=post-index, 2=signed offset/no writeback, 3=pre-index, same
// as that file's mode field) but computed in assembly instead of C.
//
// read_prep/write_prep (gadgets.h) are this codebase's own TLB macro
// (itself adapted from jit/gadgets-aarch64/gadgets.h, not OpenMinis' —
// see gadgets.h's comment on that macro). They clobber x9-x11 and may
// `bl` into a C helper on a TLB miss/slow-write path, which per AAPCS64
// is free to clobber any of x0-x18. Real bug caught while writing this:
// an earlier version kept rt/rt2/rn/mode/offset live in x9-x13 across
// the prep call — silently wrong (though never actually tested that way;
// caught by re-reading the TLB macro's own clobber list before, not
// after, hitting the bug for real). Fixed by moving those values into
// x19-x26, callee-saved by the AAPCS64 C ABI and therefore guaranteed to
// survive *any* well-behaved C function call, not just this specific
// TLB helper's actual (but unenforced) behavior. x28 is _ip, already
// spoken for.
//
// A second real bug, caught by the assembler itself (not silently wrong
// this time): `str w17, [x9, x20, lsl #3]` is invalid AArch64 -- a 32-bit
// (w-register) store's shifted-register addressing mode only allows
// `lsl #0` or `lsl #2` (matching the access size), not `#3`. Guest
// register slots in cpu_state are always 8-byte-strided regardless of
// whether the *value* being moved is 32 or 64 bits, so `lsl #3` is the
// right stride but not a legal addressing-mode shift for a w-sized
// transfer. Fixed by computing the slot address as a separate `add`
// instruction (no addressing-mode restriction on a plain register-
// register add) and then using unshifted `[xN]` addressing for the
// actual load/store.
//
// Code stream: [gadget][rt | rt2<<8 | rn<<16 | mode<<24 (2 bits)][offset
// (signed, pre-scaled by element size, sign-extended to 64 bits)]
//
// A THIRD real bug, the worst of the three, caught only by an actual
// crash (INT_GPF then SIGSEGV) when this file's gadgets first ran: the
// TLB-miss/slow-path/cross-page routines the read_prep/write_prep macro
// (gadgets.h) calls into on a miss -- handle_read_miss, handle_write_miss,
// resolve_write_ptr, crosspage_load, crosspage_store -- are NOT C
// functions. They're shared HAND-WRITTEN ASSEMBLY (jit/gadgets-aarch64/
// memory.S), compiled once into i386's own object file, with i386's
// register convention baked in at compile time: they read the faulting
// address from i386's `_addr` (w3/x3, per jit/gadgets-aarch64/gadgets.h),
// not this file's `_addr` (x7, matching OpenMinis' convention). Worse,
// their segfault path directly touches i386's `eip`/`esp` fields --
// which live in the SAME PHYSICAL REGISTERS (w28/w27) this port uses for
// _ip/other purposes. `bl handle_read_miss` from an arm64 gadget links
// against that exact global i386 symbol (there's only one in the final
// binary) and silently reads garbage out of x3, corrupting `_ip`/w28 on
// the fault path -- exactly the crash observed. Fixed with this file's
// own arm64_-prefixed copies below, using this file's actual register
// convention and this codebase's REAL C functions (tlb_handle_miss,
// tlb_write_ptr_slow, __tlb_read_cross_page, __tlb_write_cross_page,
// emu/tlb.h) directly rather than i386's assembly wrapper layer around
// them. Also: the guest address passed to tlb_handle_miss is 64-bit here
// (mov x1, _addr), not i386's 32-bit `mov w1, _addr` -- arm64 addresses
// can exceed 32 bits.

#include "gadgets.h"

.text

.extern tlb_handle_miss
.extern tlb_write_ptr_slow
.extern __tlb_read_cross_page
.extern __tlb_write_cross_page

// Both segfault paths also rewind the guest PC to the FAULTING
// INSTRUCTION, read from the fixed [-8] slot of the just-consumed code
// stream: every memory-touching gadget's LAST parameter word is its
// instruction's guest address (emitted by gen_step_arm64), and every
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
.global arm64_segfault_read
arm64_segfault_read:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_pc]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b jit_exit

.global arm64_segfault_write
arm64_segfault_write:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_pc]
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
.global arm64_handle_read_miss
arm64_handle_read_miss:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_READ
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, arm64_segfault_read
    mov _addr, x27
    ret

.global arm64_handle_write_miss
arm64_handle_write_miss:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_WRITE
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, arm64_segfault_write
    mov _addr, x27
    ret

// ISH_ARM64_TRACE_IP debug probe (gen_step_arm64 emits this ahead of the
// instruction at the traced guest pc): calls arm64_trace_probe(cpu, tlb, ip)
// with all guest state committed, then falls through to the real gadget.
// Code stream: [orig_ip].
.gadget trace_probe
    ldr x8, [_ip], #8
    save_c
    mov x0, x1                 // cpu   (x1 = _cpu; copied before x1 is written)
    sub x1, x2, #TLB_entries   // tlb   (consumes x2 before x2 is written)
    mov x2, x8                 // guest ip of the traced instruction
    bl NAME(arm64_trace_probe)
    restore_c
    gret

.extern arm64_trace_probe

// IC IVAU: invalidate translated blocks for the page containing the VA in
// Xt. Calls arm64_ic_ivau(tlb, addr) (jit/jit.c). Code stream: [rt].
// Argument order per this file's aliasing discipline: read everything that
// needs x1 (_cpu) and x2 (_tlb) before overwriting them.
.gadget ic_ivau
    ldr x8, [_ip], #8
    add x9, _cpu, #CPU_x0
    ldr x9, [x9, x8, lsl #3]   // addr = regs[rt]
    save_c
    sub x0, x2, #TLB_entries   // tlb
    mov x1, x9                 // addr
    bl NAME(arm64_ic_ivau)
    restore_c
    gret

.extern arm64_ic_ivau

.global arm64_resolve_write_ptr
arm64_resolve_write_ptr:
    // Stash the storing instruction's guest pc for the ISH_ARM64_WATCH_LO16
    // store watchpoint (tlb_write_ptr_slow). [_ip, #-8] is the gadget's
    // trailing orig_ip word, the same convention the segfault paths above
    // rely on; x9 is prep-macro scratch, dead at this point.
    ldur x9, [_ip, #-8]
    str x9, [_tlb, #(-TLB_entries+TLB_watch_ip)]
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    bl NAME(tlb_write_ptr_slow)
    mov x27, x0
    restore_c
    cbz x27, arm64_segfault_write
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
// straddling case in tests/arm64/arm64_dpextra.s, host-crashing on
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
// and arm64_crosspage_store (below) flushes the buffer back to guest
// memory at the stashed address.
.global arm64_crosspage_load
arm64_crosspage_load:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x3, x27             // size in bytes, set by the caller (gadgets.h's bullshit macro)
    mov x1, _addr
    bl NAME(__tlb_read_cross_page)
    mov x27, x0
    restore_c
    cbz x27, arm64_segfault_read
    str _addr, [_cpu, #LOCAL_value_addr]
    mov _addr, LOCAL_value
    add _addr, _cpu, _addr
    ret

// Real bug caught by re-reading i386's flow (not by a failing test --
// nothing exercises crosspage writes yet): this used `mov x1, _addr` for
// the guest address, but by flush time _addr has been REDIRECTED to the
// LOCAL_value buffer by arm64_crosspage_load above -- the guest address
// only survives in LOCAL_value_addr, which is where i386's own
// crosspage_store (jit/gadgets-aarch64/memory.S:138) reloads it from.
// LOCAL_value_addr was also widened to 64 bits (jit/frame.h) since arm64
// guest addresses don't fit the old 32-bit addr_t field.
.global arm64_crosspage_store
arm64_crosspage_store:
    save_c
    // Same argument-order constraint as arm64_crosspage_load above:
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
    cbz x27, arm64_segfault_write
    ret

.text

// DC ZVA, Xt: zero the naturally-aligned ARM64_DCZVA_BYTES-byte block
// containing the VA in Xt. DCZID_EL0 advertises DZP=0 / BS=4 (jit/gen.c),
// so the guest is entitled to issue this and expects exactly this block
// size; it is what glibc's memset and HotSpot's zero_blocks stub emit.
//
// Aligning the address down BEFORE write_prep is what makes that macro's
// crosspage branch dead code: a 64-byte-aligned address has a page offset
// of at most 0xfc0, which is precisely write_prep's `cmp x9, #(0x1000-64)`
// bound, so its `b.hi` can never be taken. That is load-bearing, not a
// nicety — the crosspage buffer (LOCAL_value, jit/frame.h) is 32 bytes,
// and a 64-byte crosspage access would run off the end of it. The TLB
// MISS path is very much live, though, so write_bullshit still has to be
// here.
//
// Xt = 31 is XZR, not SP, in this encoding: DC ZVA of address 0 is
// well-formed and faults like any other unmapped write.
//
// Code stream: [rt][orig_ip] — orig_ip is the fault-restart address the
// segfault paths read from the fixed [_ip, #-8] slot.
.if ARM64_DCZVA_BYTES != 64
.error "dc_zva zeroes four 16-byte pairs; ARM64_DCZVA_BYTES must be 64"
.endif
.gadget dc_zva
    ldr x8, [_ip]
    add _ip, _ip, #16          // rt + trailing orig_ip (fault restart)
    and x8, x8, #0x1f
    cmp x8, #31
    b.eq 1f
    add x9, _cpu, #CPU_x0
    ldr x7, [x9, x8, lsl #3]
    b 2f
1:  mov x7, #0
2:  and x7, x7, #-ARM64_DCZVA_BYTES
    write_prep (ARM64_DCZVA_BYTES*8), dc_zva
    stp xzr, xzr, [_addr]
    stp xzr, xzr, [_addr, #16]
    stp xzr, xzr, [_addr, #32]
    stp xzr, xzr, [_addr, #48]
    write_done (ARM64_DCZVA_BYTES*8), dc_zva
    gret
    write_bullshit (ARM64_DCZVA_BYTES*8), dc_zva

.macro ldp_gadget name, bits, bytes, ldr_reg1, ldr_reg2
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]        // signed scaled offset
    add _ip, _ip, #24         // params + offset + trailing orig_ip (fault restart)
    and x20, x8, #0x1f        // rt
    ubfx x21, x8, #8, #5      // rt2
    ubfx x22, x8, #16, #5     // rn
    ubfx x23, x8, #24, #2     // mode: 1=post,2=offset,3=pre

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]              // base
    b 2f
1:  ldr x24, [_cpu, #CPU_sp]
2:
    cmp x23, #1               // post-index: access at base, writeback base+offset
    b.eq 3f
    add x7, x24, x19           // offset/pre-index: access at base+offset
    b 4f
3:  mov x7, x24
4:
    read_prep \bits, \name\()_1
    ldr \ldr_reg1, [_addr]
    ldr \ldr_reg2, [_addr, #\bytes]

    add x9, _cpu, #CPU_x0      // re-derive: x9 is a TLB-macro scratch reg
    cmp x20, #31
    b.eq 5f
    add x26, x9, x20, lsl #3
    // Always store the full X register: the 32-bit form's `ldr wN`
    // zero-extended it, and `ldp w0, w1` must CLEAR the destination's
    // upper 32 bits. Storing only 4 bytes here left the old upper half
    // in the guest register file — ld computed a pointer from such a
    // register and crashed on garbage high bits.
    str x17, [x26]
5:  cmp x21, #31
    b.eq 6f
    add x26, x9, x21, lsl #3
    str x25, [x26]
6:
    cmp x23, #2                // mode 2 (offset, no writeback): done
    b.eq 8f
    add x25, x24, x19           // new base = old base + offset (post or pre)
    cmp x22, #31
    b.eq 7f
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 8f
7:  str x25, [_cpu, #CPU_sp]
8:  gret
    read_bullshit \bits, \name\()_1
.endm

// LDPSW: load pair of signed words — two 32-bit loads, each sign-extended
// to 64 bits (opc=01 in the LDP/STP encoding; su/busybox login uses it).
// Same structure as ldp_gadget but with ldrsw into the full x-registers.
.macro ldpsw_gadget name
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]
    add _ip, _ip, #24
    and x20, x8, #0x1f
    ubfx x21, x8, #8, #5
    ubfx x22, x8, #16, #5
    ubfx x23, x8, #24, #2

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]
    b 2f
1:  ldr x24, [_cpu, #CPU_sp]
2:
    cmp x23, #1
    b.eq 3f
    add x7, x24, x19
    b 4f
3:  mov x7, x24
4:
    read_prep 64, \name\()_1
    ldrsw x17, [_addr]
    ldrsw x25, [_addr, #4]

    add x9, _cpu, #CPU_x0
    cmp x20, #31
    b.eq 5f
    add x26, x9, x20, lsl #3
    str x17, [x26]
5:  cmp x21, #31
    b.eq 6f
    add x26, x9, x21, lsl #3
    str x25, [x26]
6:
    cmp x23, #2
    b.eq 8f
    add x25, x24, x19
    cmp x22, #31
    b.eq 7f
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 8f
7:  str x25, [_cpu, #CPU_sp]
8:  gret
    read_bullshit 64, \name\()_1
.endm

.macro stp_gadget name, bits, bytes, str_reg1, str_reg2
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]
    add _ip, _ip, #24         // params + offset + trailing orig_ip (fault restart)
    and x20, x8, #0x1f         // rt
    ubfx x21, x8, #8, #5       // rt2
    ubfx x22, x8, #16, #5      // rn
    ubfx x23, x8, #24, #2      // mode

    add x9, _cpu, #CPU_x0
    mov \str_reg1, #0
    cmp x20, #31
    b.eq 1f
    add x26, x9, x20, lsl #3
    ldr \str_reg1, [x26]
1:  mov \str_reg2, #0
    cmp x21, #31
    b.eq 2f
    add x26, x9, x21, lsl #3
    ldr \str_reg2, [x26]
2:
    cmp x22, #31
    b.eq 3f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]               // base
    b 4f
3:  ldr x24, [_cpu, #CPU_sp]
4:
    cmp x23, #1
    b.eq 5f
    add x7, x24, x19
    b 6f
5:  mov x7, x24
6:
    write_prep \bits, \name\()_1
    str \str_reg1, [_addr]
    str \str_reg2, [_addr, #\bytes]
    write_done \bits, \name\()_1

    cmp x23, #2
    b.eq 8f
    add x25, x24, x19
    cmp x22, #31
    b.eq 7f
    add x9, _cpu, #CPU_x0       // re-derive: x9 is a TLB-macro scratch reg
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 8f
7:  str x25, [_cpu, #CPU_sp]
8:  gret
    write_bullshit \bits, \name\()_1
.endm

// `bits` is the TOTAL pair-access size (2x element size), matching how
// OpenMinis' own stp64_imm calls their equivalent TLB macro with size=128
// for a pair of 8-byte stores (`write_prep 128, ... // 16 bytes`) despite
// doing two separate 8-byte str instructions -- the TLB fast-path/cross-
// page check must cover the full range actually written, not just one
// element, or a pair access straddling a page boundary between the two
// elements would silently validate only the first half. Caught by
// comparing against their reference before testing, not after a crash.
ldp_gadget ldp64, 128, 8, x17, x25
ldp_gadget ldp32, 64, 4, w17, w25
ldpsw_gadget ldpsw
stp_gadget stp64, 128, 8, x17, x25
stp_gadget stp32, 64, 4, w17, w25

// ---- Single-register load/store ----------------------------------------
// One generic gadget per (size, direction, extend) covering four guest
// encoding families through a shared parameter format, so gen_step_arm64
// does all the per-encoding decode work once at compile time:
//   - unsigned immediate  (mode 0, offset = imm12 << size)
//   - unscaled LDUR/STUR  (mode 0, offset = sext(imm9))
//   - post-index          (mode 1)
//   - pre-index           (mode 2)
//   - register offset     (mode 3, offset word = rm | option<<8 | shift<<16)
// LDTR/STTR (unprivileged) also map to mode 0: the privilege distinction
// doesn't exist under emulation, so their semantics are identical here.
//
// Code stream: [gadget][rt | rn<<8 | mode<<16][offset word (see above)]
//
// Register discipline matches ldp/stp above: everything live across a
// read_prep/write_prep call sits in x19-x26 (callee-saved, and x27 is the
// miss-helpers' own scratch); the data value rides in x17, which is
// preserved across the helpers' save_c/restore_c C-call bracketing.

// ldinsn/reg pick both the access width and the extend: e.g. `ldrb w17`
// zero-extends a byte, `ldrsb x17` sign-extends it to 64 bits, `ldrsb
// w17` sign-extends to 32 with the upper 32 zeroed -- exactly the
// architected LDR/LDRS semantics, done by the host instruction itself.
.macro load_single name, bits, ldinsn, reg
.gadget \name
    ldst_single_setup
    read_prep \bits, \name
    \ldinsn \reg, [_addr]
    add x9, _cpu, #CPU_x0
    cmp x20, #31
    b.eq 5f
    add x26, x9, x20, lsl #3
    str x17, [x26]
5:  ldst_single_writeback
    read_bullshit \bits, \name
.endm

.macro store_single name, bits, stinsn, reg
.gadget \name
    ldst_single_setup
    add x9, _cpu, #CPU_x0
    mov x17, #0
    cmp x20, #31
    b.eq 6f
    add x26, x9, x20, lsl #3
    ldr x17, [x26]
6:  write_prep \bits, \name
    \stinsn \reg, [_addr]
    write_done \bits, \name
    ldst_single_writeback
    write_bullshit \bits, \name
.endm

load_single load8,      8, ldrb,  w17
load_single load16,    16, ldrh,  w17
load_single load32,    32, ldr,   w17
load_single load64,    64, ldr,   x17
load_single loads8_32,  8, ldrsb, w17
load_single loads8_64,  8, ldrsb, x17
load_single loads16_32, 16, ldrsh, w17
load_single loads16_64, 16, ldrsh, x17
load_single loads32_64, 32, ldrsw, x17
store_single store8,    8, strb, w17
store_single store16,  16, strh, w17
store_single store32,  32, str,  w17
store_single store64,  64, str,  x17

// ---- Fast-path single-register load/store (the specialization pass) -----
// Same idea as math.S's fast_alu_* family: the generic gadgets above
// re-decode rt/rn/mode and resolve the addressing form through a branch
// tree at RUNTIME, on every execution (~20 host instructions and ~8
// branches before the TLB walk even starts, then the full writeback-mode
// compare chain after it). These variants serve the single hottest form —
// offset addressing, no writeback (unsigned-imm, LDUR, LDTR/STTR) — with
// everything but the TLB walk baked at compile time: gen_step_arm64
// precomputes the cpu_state BYTE OFFSETS of rt's and rn's register slots
// (CPU_sp for rn=31: an SP-base access, the dominant case in -O0 stack
// traffic, costs nothing here where the generic path pays a compare+branch)
// and the final scaled/sign-extended offset. rt=31 falls back to the
// generic gadget (LDR-to-XZR and STR-XZR are rare outside prologues).
// Profiled on the iSH_benchmark bmm integer loop (-O0, all traffic
// [sp, #imm]): the generic single load/store path was 68% of total
// runtime; this form covers every load and store in that loop.
//
// Code stream: [gadget][rt_slot | rn_slot<<16][offset][orig_ip]
// — 3 param words, same trailing-orig_ip fault-restart contract at
// [_ip, #-8] as the generic family (see arm64_segfault_read/write).
//
// Register discipline: x8 (the param word) must survive the prep macro —
// the fast path only clobbers x9-x11, and the miss/crosspage helpers
// bracket their C calls with save_c/restore_c (x0-x18 + lr) — the same
// guarantee the generic store gadgets already rely on for x17's ride
// across write_prep.
.macro load_single_fast name, bits, ldinsn, reg
.gadget \name
    ldp x8, x19, [_ip]        // [rt_slot | rn_slot<<16], offset
    add _ip, _ip, #24
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]       // base
    add x7, x10, x19
    read_prep \bits, \name
    \ldinsn \reg, [_addr]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // full X store: 32-bit loads must clear the upper half
    gret
    read_bullshit \bits, \name
.endm

.macro store_single_fast name, bits, stinsn, reg
.gadget \name
    ldp x8, x19, [_ip]        // [rt_slot | rn_slot<<16], offset
    add _ip, _ip, #24
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]       // base
    and x9, x8, #0xffff
    ldr x17, [_cpu, x9]       // value (x17 survives write_prep's helpers)
    add x7, x10, x19
    write_prep \bits, \name
    \stinsn \reg, [_addr]
    write_done \bits, \name   // clobbers x9 — rt_slot already consumed
    gret
    write_bullshit \bits, \name
.endm

load_single_fast load8_fast,      8, ldrb,  w17
load_single_fast load16_fast,    16, ldrh,  w17
load_single_fast load32_fast,    32, ldr,   w17
load_single_fast load64_fast,    64, ldr,   x17
load_single_fast loads8_32_fast,  8, ldrsb, w17
load_single_fast loads8_64_fast,  8, ldrsb, x17
load_single_fast loads16_32_fast, 16, ldrsh, w17
load_single_fast loads16_64_fast, 16, ldrsh, x17
load_single_fast loads32_64_fast, 32, ldrsw, x17
store_single_fast store8_fast,    8, strb, w17
store_single_fast store16_fast,  16, strh, w17
store_single_fast store32_fast,  32, str,  w17
store_single_fast store64_fast,  64, str,  x17

// ---- Fused -O0 idioms (the pattern-fusion pass) --------------------------
// The single-gadget fast path above brought per-gadget cost to parity with
// the amd64 engine, but unoptimized RISC output needs ~2x the instructions
// (= dispatches) of the equivalent -O0 x86 for the same statement: x86
// folds load-op-store into ONE guest instruction (add %rax,-16(%rbp));
// AArch64 spells it out. These gadgets fuse gcc -O0's two signature
// multi-instruction idioms back into one dispatch, chosen by
// gen_step_arm64's lookahead (same consume-the-next-instruction precedent
// as compare+branch fusion):
//
//   rmw_<op>i:   ldr Xt,[B,#o]; add/sub Xt,Xt,#imm; str Xt,[B,#o]
//                (every `i++` / `x -= c` on a stack slot)
//   ldld<op>st:  ldr Xa,[B1,#o1]; ldr Xb,[B2,#o2]; op Xd,Xa,Xb; str Xd,[B3,#o3]
//                (every `x = a OP b` through stack slots)
//
// FAULT-RESTART CONTRACT: a fused gadget has several faultable accesses,
// but arm64_segfault_read/write always report the guest PC from the fixed
// [_ip, #-8] slot. So the code stream carries one orig_ip word per
// faultable access, and _ip advances in STAGES — past each access's param
// group (ending in its orig_ip) right before that access — so [-8] always
// names the sub-instruction that would fault. Every guest-visible register
// result is COMMITTED to its cpu_state slot before the next faultable
// access: a restart at sub-instruction N recompiles a block starting
// there, which reads its inputs from the register file — they must
// already hold everything sub-instructions 0..N-1 wrote. gen.c's guards
// (gen_arm64_try_ldst_fusion) reject register aliasing that would make
// a replayed sub-instruction read a base register the fused group itself
// modified.
//
// Register discipline: x21 carries the first loaded value across the
// second read_prep — callee-saved per AAPCS64 (survives the miss helpers'
// C calls, same argument as the generic ldp/stp gadgets' x19-x26 state)
// and untouched by the prep macros' x9-x11 scratch. x8/x17/x19 usage
// matches the fast single gadgets above.

// Code stream: [gadget][rt_slot | rn_slot<<16][offset][ip_ld][imm][ip_st]
.macro rmw_alu_imm name, bits, op, opreg, immreg
.gadget \name
    ldp x8, x19, [_ip]        // [rt_slot | rn_slot<<16], offset
    add _ip, _ip, #24         // past slots+offset+ip_ld: [-8] = ip_ld
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]       // base
    add x7, x10, x19
    mov x21, x7               // guest address again for the store side
    read_prep \bits, \name\()_rd
    ldr \opreg, [_addr]
    ldr x11, [_ip]            // imm (pre-shifted)
    add _ip, _ip, #16         // past imm+ip_st: [-8] = ip_st
    \op \opreg, \opreg, \immreg
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // commit Xt before the faultable store
    mov x7, x21
    write_prep \bits, \name\()_wr
    str \opreg, [_addr]
    write_done \bits, \name\()_wr
    gret
    read_bullshit \bits, \name\()_rd
    write_bullshit \bits, \name\()_wr
.endm

rmw_alu_imm rmw_addi_fast64, 64, add, x17, x11
rmw_alu_imm rmw_subi_fast64, 64, sub, x17, x11
rmw_alu_imm rmw_addi_fast32, 32, add, w17, w11
rmw_alu_imm rmw_subi_fast32, 32, sub, w17, w11

// Code stream: [gadget][a_slot | b1_slot<<16][off1][ip1]
//                      [b_slot | b2_slot<<16][off2][ip2]
//                      [d_slot | b3_slot<<16][off3][ip3]
.macro ldld_op_st name, bits, op, ldinsn, stinsn, dreg, areg
.gadget \name
    ldp x8, x19, [_ip]
    add _ip, _ip, #24         // [-8] = ip1
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]
    add x7, x10, x19
    read_prep \bits, \name\()_r1
    \ldinsn \dreg, [_addr]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // commit Xa
    mov x21, x17              // Xa value (zero-extended for 32-bit)
    ldp x8, x19, [_ip]
    add _ip, _ip, #24         // [-8] = ip2
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]       // base2 slot read AFTER Xa commit (no aliasing: gen.c guards B2 != Xa anyway)
    add x7, x10, x19
    read_prep \bits, \name\()_r2
    \ldinsn \dreg, [_addr]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // commit Xb
    ldp x8, x19, [_ip]
    add _ip, _ip, #24         // [-8] = ip3
    \op \dreg, \areg, \dreg   // Xd = op(Xa, Xb) — w-form zero-extends x17
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // commit Xd before the faultable store
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]       // base3
    add x7, x10, x19
    write_prep \bits, \name\()_wr
    \stinsn \dreg, [_addr]
    write_done \bits, \name\()_wr
    gret
    read_bullshit \bits, \name\()_r1
    read_bullshit \bits, \name\()_r2
    write_bullshit \bits, \name\()_wr
.endm

ldld_op_st ldldadd_st_fast64, 64, add, ldr, str, x17, x21
ldld_op_st ldldsub_st_fast64, 64, sub, ldr, str, x17, x21
ldld_op_st ldldand_st_fast64, 64, and, ldr, str, x17, x21
ldld_op_st ldldorr_st_fast64, 64, orr, ldr, str, x17, x21
ldld_op_st ldldeor_st_fast64, 64, eor, ldr, str, x17, x21
ldld_op_st ldldadd_st_fast32, 32, add, ldr, str, w17, w21
ldld_op_st ldldsub_st_fast32, 32, sub, ldr, str, w17, w21
ldld_op_st ldldand_st_fast32, 32, and, ldr, str, w17, w21
ldld_op_st ldldorr_st_fast32, 32, orr, ldr, str, w17, w21
ldld_op_st ldldeor_st_fast32, 32, eor, ldr, str, w17, w21

// ---- LDR (literal): PC-relative load. The address is compile-time
// known (PC + imm19*4, computed by gen_step_arm64), so the code stream
// just carries it directly: [gadget][rt][absolute address].
.macro load_literal name, bits, ldinsn, reg
.gadget \name
    ldr x20, [_ip]            // rt
    ldr x7, [_ip, #8]         // absolute guest address
    add _ip, _ip, #24         // rt + address + trailing orig_ip (fault restart)
    read_prep \bits, \name
    \ldinsn \reg, [_addr]
    add x9, _cpu, #CPU_x0
    cmp x20, #31
    b.eq 5f
    add x26, x9, x20, lsl #3
    str x17, [x26]
5:  gret
    read_bullshit \bits, \name
.endm

load_literal load_lit32,   32, ldr,   w17
load_literal load_lit64,   64, ldr,   x17
load_literal load_lit_sw,  32, ldrsw, x17
