// AArch64 GUEST gadgets (native ARM64 guest code on ARM64 host). Distinct
// from jit/gadgets-aarch64/, which is the ARM64 HOST gadget set used by the
// i386/amd64 guest JIT — unrelated, see aarch64_guest_plan.md's naming note.
//
// Reuses jit/gadgets-aarch64/entry.S's jit_enter/gret/jit_exit completely
// unmodified: that entry point is guest-ABI-agnostic at the level that
// matters (x0=block, x1=cpu, x2=tlb -> _ip/_cpu/_tlb setup, then dispatch
// into whichever gadget array was compiled). Its unconditional i386
// load_regs/save_regs calls are inert for arm64 tasks — confirmed by
// checking how the existing amd64 frontend already relies on exactly this
// (jit/jit.c:1467 explicitly re-syncs frame->cpu.eip from amd64_rip right
// after jit_enter returns, because save_regs's i386-field write is known
// garbage for a non-i386 task; every downstream consumer is abi-gated and
// never reads those fields for a non-i386 task). No separate entry.S
// needed — do not create one.
//
// Register convention (adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gadgets-aarch64/gadgets.h, GPLv3, see
// /CREDITS-aarch64.md): _cpu=x1, _tlb=x2, _ip=x28 match iSH-AOK's own
// i386-guest convention exactly (same physical registers, verified above),
// so entry.S's setup is correct unmodified. Guest GPR access is
// memory-based (ldr/str against cpu_state per touch) rather than cached in
// host registers the way i386/amd64 cache their <=16 GPRs in x20-x27 —
// arm64 has 31 GPRs, too many to cache that way, and this is the same
// choice OpenMinis made. x20-x27 are left untouched by every arm64 guest
// gadget; whatever i386's load_regs put there is simply never read.

#include "../gadgets-generic.h"
#include "cpu-offsets.h"
#include "emu/interrupt.h"

// CPU_x0/CPU_sp/CPU_pc/CPU_nzcv alias this codebase's actual generated
// offset names (CPU_arm64_regs etc — see jit/offsets.c's arm64() function)
// to the bare names OpenMinis' ported gadget bodies use, so those bodies
// don't need per-line renaming. Defined once, here, not per-file.
//
// CPU_sp needs an #undef first: cpu-offsets.h already defines it for
// i386's own `sp` field (the 16-bit view of esp, see jit/offsets.c's
// OFFSET(CPU, cpu_state, sp)) — a real, caught-by-the-compiler collision,
// not a hypothetical one. Safe to shadow here: each .S file is a separate
// compilation unit, and only arm64-guest gadget files include this header,
// never i386's — this #undef has no effect on i386's own gadgets.h/files.
#define CPU_x0 CPU_arm64_regs
#undef CPU_sp
#define CPU_sp CPU_arm64_sp
#define CPU_pc CPU_arm64_pc
#define CPU_nzcv CPU_arm64_nzcv
#define CPU_excl_addr CPU_arm64_excl_addr
#define CPU_excl_val CPU_arm64_excl_val
#define CPU_excl_val_hi CPU_arm64_excl_val_hi
#define CPU_tpidr CPU_arm64_tpidr
#define CPU_v0 CPU_arm64_v
#define CPU_fpsr CPU_arm64_fpsr
#define CPU_fpcr CPU_arm64_fpcr

_cpu    .req x1
_tlb    .req x2
_ip     .req x28

// Scratch registers for gadget use. Distinct from OpenMinis' _tmp/_tmp2/
// _addr naming (x0/x8/x7) only where needed to avoid clashing with this
// file's _ip/_cpu/_tlb assignments above — x0/x7/x8 don't collide with
// x1/x2/x28, so their choices are kept as-is for easier comparison against
// their source when porting further gadgets.
_tmp    .req x0
_tmp2   .req x8
_addr   .req x7

// RULE: never hold live gadget state in x18. It's the platform-reserved
// register on Apple targets (iOS/macOS) -- the OS is allowed to trash it
// asynchronously, so a value parked there can silently corrupt between
// any two instructions. (save_c/restore_c still spill/reload the x18
// SLOT around C calls, which is fine -- that's saving whatever garbage
// is there, not relying on it.) An early revision of the ldp/stp and
// dpreg gadgets used x18 as a data register; fixed by audit, not by a
// crash -- this would have been a rare, unreproducible on-device
// corruption.

.extern jit_ret
.extern jit_exit
.extern arm64_handle_read_miss
.extern arm64_handle_write_miss
.extern arm64_resolve_write_ptr
.extern arm64_crosspage_load
.extern arm64_crosspage_store

.macro .gadget name
    .global NAME(gadget_arm64_\()\name)
    .align 4
    NAME(gadget_arm64_\()\name) :
.endm

// Identical to jit/gadgets-aarch64/gadgets.h's gret (same entry.S/jit_ret
// contract) — reproduced here rather than shared via a common header
// because the i386 one is textually entangled with i386-specific
// PROFILE/debug hooks not relevant here. Keep in sync if entry.S's
// contract changes.
// Dispatch. Two spellings of the same ordering guarantee, selected by
// -Darm64_gret= (default ldar, so this is unchanged unless asked for). Either
// way the gadget-pointer load must be ordered before every later access, so a
// concurrently-patched chain word cannot be observed before the block contents
// it points into; `ldar` gets that from acquire semantics, `ldr + dmb ishld`
// from the trailing barrier.
//
// WHICH IS FASTER DEPENDS ON THE HOST GENERATION, by a lot, and this sequence
// runs once per guest instruction so it is the single largest constant in the
// engine. MEASURED, both directions, same benchmark shape:
//   - ARMv8.0 (A9 iPad): dmb wins hugely. arm64-guest sh-loop 50k
//     **13255ms with ldar -> 7652ms with dmb, a 1.73x speedup**, run-to-run
//     spread 0.13%. The x86 gadgets said the same thing independently: going
//     the other way there was a 2.04x REGRESSION (8422 -> 17203ms), which is
//     why 861da1d1 was reverted by 95140ab7.
//   - Apple Silicon: ldar wins, but only ~6% (arm64-guest sh-loop min 1.66s
//     vs 1.77s, 4 of 4 pairs). The "~34 host cycles per guest instruction,
//     dominated by the dmb" figure this file used to cite came from here, and
//     it does NOT generalize -- Apple's cores retire ldar nearly free while
//     the A9 evidently implements load-acquire far more conservatively.
// So dmb is the DEFAULT: giving up ~6% on new hardware to gain ~73% on old
// hardware is the trade this project wants, and iSH-AOK ships one binary for
// both. -Darm64_gret=ldar restores the other spelling; a build using it says
// " gret=ldar" in `uname -v`.
// A future refinement could pick per host at startup, but that needs two full
// gadget tables, and 1.7x on old devices is worth having now.
.macro gret pop=0
#if defined(ISH_ARM64_GRET_LDAR)
.if \pop != 0
    add _ip, _ip, \pop*8
.endif
    ldar x9, [_ip]
#else
    ldr x9, [_ip, \pop*8]!
    dmb ishld
#endif
    add _ip, _ip, 8
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm

// ---- Guest register access (memory-based; see file header) -------------

.macro load_guest_reg host_reg, guest_idx
    ldr \host_reg, [_cpu, #(CPU_x0 + \guest_idx * 8)]
.endm

.macro store_guest_reg host_reg, guest_idx
    str \host_reg, [_cpu, #(CPU_x0 + \guest_idx * 8)]
.endm

.macro load_guest_sp host_reg
    ldr \host_reg, [_cpu, #CPU_sp]
.endm

.macro store_guest_sp host_reg
    str \host_reg, [_cpu, #CPU_sp]
.endm

.macro load_guest_pc host_reg
    ldr \host_reg, [_cpu, #CPU_pc]
.endm

.macro store_guest_pc host_reg
    str \host_reg, [_cpu, #CPU_pc]
.endm

// NZCV lives packed in cpu->arm64_nzcv (bits 31:28), matching AArch64
// PSTATE exactly (emu/cpu.h) — load/store it directly into the host NZCV
// flags register with mrs/msr, avoiding a decode/encode round-trip per
// flag-setting instruction.
//
// Uses x17 as scratch, not x9: add_imm/sub_imm (math.S) hold `rd` live in
// x9 across the call to store_flags (needed afterward to pick the
// store-result-or-SP path) — x9 would be a real, silent corruption bug
// here. x17 is dead at every call site in this port so far; if a future
// gadget needs x17 live across a flags macro, this needs revisiting.
.macro load_flags
    ldr w17, [_cpu, #CPU_nzcv]
    msr nzcv, x17
.endm

.macro store_flags
    mrs x17, nzcv
    str w17, [_cpu, #CPU_nzcv]
.endm

// ---- Memory access (TLB fast path) --------------------------------------
// Adapted from jit/gadgets-aarch64/gadgets.h's own read_prep/write_prep —
// NOT from OpenMinis' version, which assumes TLB_BITS=13 and a 32-byte
// tlb_entry stride (`lsl x9,x9,#5`). This codebase's actual layout is
// TLB_BITS=10 (jit/offsets.c's new MACRO(TLB_BITS) emits the real value)
// and a 24-byte tlb_entry (three 8-byte fields, MACRO(TLB_ENTRY_SIZE)) —
// i386's own gadgets.h already gets this exactly right (`ubfx x9,_xaddr,
// 12,10` / `eor x9,x9,_xaddr,lsr 22` / `mov w10,TLB_ENTRY_SIZE; madd x9,
// x9,w10,_tlb`), so this macro is a straight copy of that proven-correct
// pattern with register names adjusted for this file's _addr (x7, not
// i386's w3/x3) and _tlb already pointing at tlb->entries (same
// convention, set up once in entry.S, not per-gadget).
.irp type, read,write

.macro \type\()_prep size, id
    and w9, w7, #0xfff
    cmp x9, #(0x1000-(\size/8))
    // BOTH types branch to crosspage_load_\id -- this is i386's own design
    // (jit/gadgets-aarch64/gadgets.h line 48), deliberately: for a WRITE
    // that straddles a page boundary, arm64_crosspage_load fills
    // LOCAL_value with the current guest bytes, stashes the guest address
    // in LOCAL_value_addr, and redirects _addr to the LOCAL_value buffer;
    // the gadget's store instruction then writes into the buffer, and
    // write_done (below) detects _addr==LOCAL_value and calls
    // arm64_crosspage_store to flush the buffer back to guest memory
    // using the stashed address. A previous revision of this port
    // misdiagnosed this shared branch target as a label-collision bug and
    // "fixed" it by branching writes directly to crosspage_store_\id --
    // which flushes the still-uninitialized buffer and skips the actual
    // store entirely. Corrected by re-reading i386's flow end-to-end
    // (memory.S's crosspage_load/crosspage_store pair) rather than
    // reasoning about the prep macro in isolation.
    b.hi crosspage_load_\id
    .ifc \type,write
        ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
        ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
        ldr x11, [x10, #MMU_changes]
        cmp x9, x11
        b.ne slow_write_\id
        ldrb w10, [x10, #MMU_requires_write_revalidate]
        cbz w10, fast_write_\id
slow_write_\id :
        bl arm64_resolve_write_ptr
        b back_\id
fast_write_\id :
    .endif
    // 64-bit page math throughout: this macro was copied from the i386
    // gadget set, whose w-register page compare and unmasked `eor ..,
    // lsr #22` index are only correct for 32-bit guest addresses. arm64
    // guest addresses are 48-bit: the unmasked eor indexed OUTSIDE the
    // 1024-entry TLB array (ld crashed the whole app dereferencing a
    // garbage data_minus_addr), and the 32-bit page compare would treat
    // any two pages 4GiB apart as aliases — silent wrong-memory access.
    and x9, x7, #0xfffffffffffff000
    .ifc \type,write
        str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    .endif
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    .ifc \type,read
        ldr x11, [x10, #TLB_ENTRY_page]
    .else
        ldr x11, [x10, #TLB_ENTRY_page_if_writable]
    .endif
    cmp x9, x11
    b.ne handle_miss_\id
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7, uxtx
back_\id:
.endm

.macro \type\()_bullshit size, id
handle_miss_\id :
    bl arm64_handle_\type\()_miss
    b back_\id
crosspage_load_\id :
    mov x27, #(\size/8)
    bl arm64_crosspage_load
    b back_\id
.ifc \type,write
crosspage_store_\id :
    mov x27, #(\size/8)
    bl arm64_crosspage_store
    b back_write_done_\id
.endif
.endm

.endr

.macro write_done size, id
    mov x9, LOCAL_value
    add x9, _cpu, x9
    cmp x9, x7
    b.eq crosspage_store_\id
back_write_done_\id :
.endm

// ---- Single-register load/store shared prologue/epilogue ----------------
// Used by both the integer gadgets (memory.S) and the SIMD/FP gadgets
// (simd.S) -- lives here because assembler macros do not cross
// translation units. See memory.S's "Single-register load/store"
// section comment for the addressing-family/mode documentation.
// Shared prologue: parse params, resolve the register-offset form if
// mode 3, read the base (rn: 31 = SP), leave the effective address in
// x7 (= _addr) ready for a prep macro. Leaves rt in x20, rn in x22,
// mode in x23, base in x24, resolved offset in x19.
// Code stream (all single-register load/store gadgets, integer and SIMD):
// [rt | rn<<8 | mode<<16][offset word][orig_ip] — the trailing orig_ip is
// the fault-restart address consumed by arm64_segfault_read/write
// (memory.S) at a fixed [_ip, #-8].
.macro ldst_single_setup
    ldr x8, [_ip]
    ldr x19, [_ip, #8]
    add _ip, _ip, #24
    and x20, x8, #0x1f        // rt
    ubfx x22, x8, #8, #5      // rn
    ubfx x23, x8, #16, #2     // mode

    cmp x23, #3               // register-offset: resolve x19 = extend(Rm) << shift
    b.ne 10f
    and x10, x19, #0x1f       // rm
    ubfx x11, x19, #8, #3     // option: 010=UXTW 011=LSL/UXTX 110=SXTW 111=SXTX
    ubfx x12, x19, #16, #4    // shift amount (0 or log2(size))
    add x9, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 11f
    add x26, x9, x10, lsl #3
    ldr x13, [x26]
    b 12f
11: mov x13, #0               // Rm=31 is XZR here, never SP
12: tbnz x11, #0, 13f         // option bit0: 64-bit index, use Xm as-is
    tbnz x11, #2, 14f         // option bit2: signed 32-bit index
    mov w13, w13              // UXTW
    b 13f
14: sxtw x13, w13             // SXTW
13: lsl x19, x13, x12
10:
    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]            // base
    b 2f
1:  ldr x24, [_cpu, #CPU_sp]
2:
    cmp x23, #1               // post-index accesses at base; all others at base+offset
    b.eq 3f
    add x7, x24, x19
    b 4f
3:  mov x7, x24
4:
.endm

// Shared epilogue: writeback base+offset to rn for modes 1 (post) and 2
// (pre) only; modes 0 (no writeback) and 3 (register offset, no
// writeback) skip it.
.macro ldst_single_writeback
    cmp x23, #1
    b.lo 9f
    cmp x23, #2
    b.hi 9f
    add x25, x24, x19
    cmp x22, #31
    b.eq 8f
    add x9, _cpu, #CPU_x0
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 9f
8:  str x25, [_cpu, #CPU_sp]
9:  gret
.endm


.macro save_c
    stp x0, x1, [sp, -0xa0]!
    stp x2, x3, [sp, 0x10]
    stp x4, x5, [sp, 0x20]
    stp x6, x7, [sp, 0x30]
    stp x8, x9, [sp, 0x40]
    stp x10, x11, [sp, 0x50]
    stp x12, x13, [sp, 0x60]
    stp x14, x15, [sp, 0x70]
    stp x16, x17, [sp, 0x80]
    stp x18, lr, [sp, 0x90]
.endm

.macro restore_c
    ldp x18, lr, [sp, 0x90]
    ldp x16, x17, [sp, 0x80]
    ldp x14, x15, [sp, 0x70]
    ldp x12, x13, [sp, 0x60]
    ldp x10, x11, [sp, 0x50]
    ldp x8, x9, [sp, 0x40]
    ldp x6, x7, [sp, 0x30]
    ldp x4, x5, [sp, 0x20]
    ldp x2, x3, [sp, 0x10]
    ldp x0, x1, [sp], 0xa0
.endm

# vim: ft=gas
