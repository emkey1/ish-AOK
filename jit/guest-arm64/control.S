// AArch64 guest control-flow gadgets. `svc` adapted from OpenMinis/
// ish-arm64's asbestos/guest-arm64/gadgets-aarch64/control.S (GPLv3, see
// /CREDITS-aarch64.md; see below for its INT_SYSCALL->INT_ARM64_SVC fix).
//
// Everything else here (b/bl/br/blr/ret/cbz/cbnz/tbz/tbnz/b.cond) is
// independently written, NOT adapted from their equivalents (which use a
// differently-shaped inline_chain/fake_ip scheme).
//
// BLOCK CHAINING (the port's first deliberate perf change, added after
// the cross-arch benchmark showed branch-heavy code ~40% behind the x86
// guests on exit-to-C round trips): compile-time-known branch targets
// are emitted into the code stream TAGGED with bit 63 set
// (target | 1<<63), and gen_step_arm64 records their stream indices in
// state->jump_ip[]. When the C loop later executes destination blocks,
// it PATCHES those words in place with the destination block's host
// `code` pointer (bit 63 clear — a userspace host pointer). Branch
// gadgets load their chosen target word into _ip and jump to
// arm64_branch_dispatch: tag still set -> strip it, store CPU_pc, exit
// to C as before; tag clear -> the word IS the next block's gadget
// array, so after a poke check (signals/timers must not starve — the
// same protection amd64's jit_ret_chain has) dispatch straight into it
// with zero C round trips. The C loop only chains FORWARD edges
// (block->addr > last_block->addr), so every loop's backward edge still
// exits to C — the amd64-era "loop chaining starves jetsam" lesson,
// inherited rather than relearned. Dynamic-target branches (BR/BLR/RET)
// stay unchained.
//
// This is the amd64 frontend's exact tagged-word design
// (gadget_amd64_jmp / jit_ret_chain, jit/gadgets-aarch64/), re-expressed
// for this port's register convention and 64-bit guest PC.
//
// bcond_* gadgets use the host's own b.<cond> instruction after loading
// NZCV into the host flags register (load_flags, gadgets.h) -- one gadget
// per condition code, selected at gen_step_arm64 compile time, rather
// than decoding the condition and testing it generically in assembly.

#include "gadgets.h"

.text

.extern jit_ret
.extern jit_exit

/*
 * System call (SVC)
 * Code stream: [gadget addr][next_pc (64-bit)]
 * Sets guest PC to next instruction before exiting.
 *
 * Interrupt code changed from OpenMinis' INT_SYSCALL (0x80, i386's
 * syscall interrupt number) to INT_ARM64_SVC (emu/interrupt.h) -- this
 * codebase has a dedicated arm64 SVC interrupt code and dispatch path
 * (handle_arm64_syscall_interrupt, kernel/calls.c, patch 5) specifically
 * so arm64 doesn't get routed through i386's syscall ABI by accident.
 * fiber_exit -> jit_exit (this codebase's name for the same shared exit
 * path, jit/gadgets-aarch64/entry.S, reused unmodified).
 */
.gadget svc
    ldr x8, [_ip]           // next instruction address (guest PC after SVC)
    str x8, [_cpu, #CPU_pc]
    mov w0, #INT_ARM64_SVC
    b jit_exit

// Interrupt exit with a 64-BIT guest pc (INT_UNDEFINED and friends).
// arm64 rejects used to emit the shared i386 gadget_interrupt
// (jit/gadgets-aarch64/entry.S), which stores `eip` — not arm64_pc —
// and truncates segfault_addr to 32 bits: every arm64 SIGILL reported
// a stale block-start pc (current_fault_ip reads arm64_pc), the
// "illegal instruction" log dumped the WRONG instruction's bytes, and
// the guest signal frame got a truncated fault address. Same code
// stream layout as the shared gadget: [gadget][code][pc][addr].
.gadget interrupt
    ldr _tmp, [_ip]
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_pc]
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    b jit_exit

// B, BL — unconditional branch (target is compile-time-known, computed by
// gen_step_arm64 since PC is known at compile time). Code stream for b:
// [gadget][target_pc]. For bl: [gadget][target_pc][return_addr] -- return
// address is written to X30 before branching.
// Common branch tail: _ip holds the loaded target word. Tagged (bit 63
// set) = unchained guest address; clear = patched host code pointer for
// direct block-to-block dispatch. See the file header.
.global arm64_branch_dispatch
arm64_branch_dispatch:
    tbz _ip, #63, arm64_chain
    and x8, _ip, #0x7fffffffffffffff
    str x8, [_cpu, #CPU_pc]
    b jit_ret
// Global, not file-local: the ret_cache_dispatch macro below enters here with a
// block->code pointer it has already validated, skipping the tagged-word test
// above (a cache entry is a host pointer, never a tagged guest address).
.global arm64_chain
arm64_chain:
    ldr x8, [_cpu, #CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, arm64_chain_poked
    // Chain budget: bounded time-in-JIT is what makes chaining BACKWARD
    // edges (loops) safe — see jit_frame.chain_budget. Expiry exits to C
    // exactly like a poke, at the destination block's start.
    ldr x8, [_cpu, #LOCAL_chain_budget]
    subs x8, x8, #1
    str x8, [_cpu, #LOCAL_chain_budget]
    b.le arm64_chain_poked
    sub x8, _ip, #JIT_BLOCK_code       // recover the block pointer for the
    str x8, [_cpu, #LOCAL_last_block]  // C loop's chaining bookkeeping
    gret
arm64_chain_poked:
    // Poked or out of budget mid-chain: resume at the DESTINATION block.
    ldur x8, [_ip, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, #CPU_pc]
    b jit_ret

.gadget b
    ldr _ip, [_ip]
    b arm64_branch_dispatch

.gadget bl
    ldr x9, [_ip, #8]
    add x10, _cpu, #CPU_x0
    str x9, [x10, #(30*8)]  // X30 = return address
    ldr _ip, [_ip]
    b arm64_branch_dispatch

// BR, BLR, RET — branch to a register value. Code stream: [gadget][rn].
// For blr, code stream also has [return_addr] after rn, same as bl.
.gadget br
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  str x10, [_cpu, #CPU_pc]
    b jit_ret

.gadget blr
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  ldr x11, [_ip, #8]      // return address
    str x11, [x9, #(30*8)]  // X30 = return address
    str x10, [_cpu, #CPU_pc]
    b jit_ret

// HLE "giant gadget": the whole block is one fingerprinted libc function
// (jit/hle.c). Bridges to hle_call(cpu, tlb, fn, entry_ip), which performs
// the function against guest memory, writes the return register, and sets
// cpu->pc to the guest return address. Result INT_NONE -> exit the block
// via jit_ret (dispatcher continues at the new pc); any other interrupt
// (INT_PF after a guest memory fault, pc rewound to entry) -> jit_exit.
// x27 carries the result across restore_c (x0-x18 + lr are reloaded).
// Code stream: [fn | abi<<8][entry_ip].
.gadget hle_call
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    save_c
    mov x0, x1                 // cpu   (x1 = _cpu; read before x1 is written)
    sub x1, x2, #TLB_entries   // tlb   (consumes x2 before x2 is written)
    mov x2, x8                 // fn word
    mov x3, x9                 // entry ip
    bl NAME(hle_call)
    mov x27, x0
    restore_c
    tbnz w27, #31, 1f          // INT_NONE (-1): success, pc updated
    mov w0, w27
    b jit_exit
1:  b jit_ret

.extern hle_call

// RET has identical semantics to BR (branch to register value) -- same
// gadget body, just invoked for a different guest mnemonic/encoding.
.gadget ret
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  str x10, [_cpu, #CPU_pc]
    b jit_ret

// ---- Return/indirect-target cache ----------------------------------------
//
// br/blr/ret each compute a target from a guest register and, above, each end
// in an unconditional exit to C. Those three are this engine's remaining JIT->C
// round trips: every other edge (b, bl, cbz/cbnz, tbz/tbnz, b.cond) is a
// compile-time target that goes through arm64_branch_dispatch and chains.
//
// Same design as the riscv64 engine's jalr cache (guest-riscv64/alu.S), which
// measured +15.1% on a dash shell loop and +70.5% on a function-pointer
// dispatch loop; the two guests both return through a link register and share
// LINKREG_RET_CACHE_HASH in jit/frame.h. The cache is filled by the FRONTEND at
// dispatch time (cpu_step_to_interrupt_arm64), not recorded at the call the way
// i386's `call` gadget records for its `ret`: since these three ARE the exits,
// the set of addresses the frontend dispatches at already IS the set of targets
// they compute. Keyed by target rather than by call site, so one lookup serves
// returns, indirect calls, PLT stubs and switch-table `br`s alike.
//
// Entries self-validate: one names block->code, so block->addr is a
// negative-offset load away and is compared against the computed target before
// use. The is_jetsam test is load-bearing, not defensive -- an invalidated block
// stays allocated until a write-locked cleanup, and the frontend's cleanup_seq
// purge (which is what clears this cache) only runs when the thread next returns
// to C, which is exactly what a hit avoids. A chained edge is covered there by
// jit_block_disconnect restoring its jump_ip; a cache entry has no such
// backlink. The pointer cannot dangle: freeing needs the jetsam WRITE lock and
// jit_enter runs under the read lock.
//
// \target is the register holding the computed guest address. The macro is
// placed AFTER the X30 write in blr_cached below, never before: blr x30 is this
// guest's rd/rs1 aliasing hazard (the twin of riscv64's `jalr ra, 0(ra)`), so
// the target must be read out of the register file before X30 is overwritten.
// Hash MUST match LINKREG_RET_CACHE_HASH bit for bit; a mismatch is a silent 0%
// hit rate, not a crash, and every correctness test still passes.
.macro ret_cache_dispatch target
    eor x12, \target, \target, lsr #12
    ubfx x12, x12, #1, #12
    mov x13, LOCAL_ret_cache
    add x13, _cpu, x13
    ldr x13, [x13, x12, lsl #3]
    cbz x13, 8f                        // empty slot
    ldur x14, [x13, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    cmp x14, \target
    b.ne 8f                            // some other address hashes here
    ldurb w14, [x13, #(JIT_BLOCK_is_jetsam - JIT_BLOCK_code)]
    cbnz w14, 8f                       // invalidated; let C compile the replacement
    mov _ip, x13
    b arm64_chain                      // poke check, chain budget, last_block
8:  str \target, [_cpu, #CPU_pc]
    b jit_ret
.endm

.gadget br_cached
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  ret_cache_dispatch x10

.gadget blr_cached
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  ldr x11, [_ip, #8]      // return address
    str x11, [x9, #(30*8)]  // X30 = return address, AFTER the target is read
    ret_cache_dispatch x10

.gadget ret_cached
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr x10, [x9, x8, lsl #3]
    b 2f
1:  mov x10, #0
2:  ret_cache_dispatch x10

// CBZ, CBNZ — code stream: [gadget][rt | sf<<8][taken_pc][fallthrough_pc]
.gadget cbz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #1     // sf
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  cbz w10, 3f
    cbz x12, taken_cbz
    b not_taken_cbz
3:  cbz w12, taken_cbz
    b not_taken_cbz
taken_cbz:
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_cbz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

.gadget cbnz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #1     // sf
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  cbz w10, 3f
    cbnz x12, taken_cbnz
    b not_taken_cbnz
3:  cbnz w12, taken_cbnz
    b not_taken_cbnz
taken_cbnz:
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_cbnz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

// TBZ, TBNZ — code stream: [gadget][rt | bit_pos<<8][taken_pc][fallthrough_pc]
.gadget tbz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #6     // bit position (0-63)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  lsr x12, x12, x10
    tbz x12, #0, taken_tbz
    b not_taken_tbz
taken_tbz:
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_tbz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

.gadget tbnz
    ldr x8, [_ip]
    and w9, w8, #0x1f        // rt
    ubfx w10, w8, #8, #6     // bit position (0-63)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    ldr x12, [x11, x9, lsl #3]
    b 2f
1:  mov x12, #0
2:  lsr x12, x12, x10
    tbnz x12, #0, taken_tbnz
    b not_taken_tbnz
taken_tbnz:
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
not_taken_tbnz:
    ldr _ip, [_ip, #16]
    b arm64_branch_dispatch

// B.cond family — code stream: [gadget][taken_pc][fallthrough_pc]. One
// gadget per condition code; gen_step_arm64 picks the right one at
// compile time based on the decoded cond field.
.macro bcond_gadget name, cc
.gadget \name
    load_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

// bcond_nf_* — B.cond WITHOUT the load_flags reload, for use directly
// after a fast flag-setting gadget (compare+branch fusion): the host
// NZCV is still live from that gadget's adds/subs/ands, because nothing
// in its tail (store_flags is just mrs+str) nor in gret's dispatch
// sequence (ldr/dmb/cbnz/br) writes flags. `msr nzcv` is serializing on
// Apple cores, so skipping it (plus the flags load) is the single
// biggest per-iteration saving in compare-branch loops. gen_step_arm64
// only emits these when the immediately preceding instruction in the
// SAME block set the flags via a fast gadget (state->arm64_flags_live);
// everything else keeps the reloading bcond_* family above.
.macro bcond_nf_gadget name, cc
.gadget \name
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

bcond_nf_gadget bcond_nf_eq, eq
bcond_nf_gadget bcond_nf_ne, ne
bcond_nf_gadget bcond_nf_cs, cs
bcond_nf_gadget bcond_nf_cc, cc
bcond_nf_gadget bcond_nf_mi, mi
bcond_nf_gadget bcond_nf_pl, pl
bcond_nf_gadget bcond_nf_vs, vs
bcond_nf_gadget bcond_nf_vc, vc
bcond_nf_gadget bcond_nf_hi, hi
bcond_nf_gadget bcond_nf_ls, ls
bcond_nf_gadget bcond_nf_ge, ge
bcond_nf_gadget bcond_nf_lt, lt
bcond_nf_gadget bcond_nf_gt, gt
bcond_nf_gadget bcond_nf_le, le

bcond_gadget bcond_eq, eq
bcond_gadget bcond_ne, ne
bcond_gadget bcond_cs, cs
bcond_gadget bcond_cc, cc
bcond_gadget bcond_mi, mi
bcond_gadget bcond_pl, pl
bcond_gadget bcond_vs, vs
bcond_gadget bcond_vc, vc
bcond_gadget bcond_hi, hi
bcond_gadget bcond_ls, ls
bcond_gadget bcond_ge, ge
bcond_gadget bcond_lt, lt
bcond_gadget bcond_gt, gt
bcond_gadget bcond_le, le

// ---- Single-gadget compare+branch fusion ---------------------------------
// One dispatch instead of two for the compare-and-branch pairs that
// dominate loop code. The compare executes natively, guest NZCV is still
// stored (later instructions may read the flags), and the branch tests
// the live host flags — no msr reload, no second gadget dispatch.
// gen_step_arm64 peeks at the next instruction when emitting a fast
// compare; on a B.cond it emits one of these instead of the pair.
//
// Per-condition gadget POINTER TABLES are exported from here (one .quad
// table per form) so gen.c indexes extern arrays instead of declaring
// 84 gadget symbols.
//
// Streams:
//   fused_cmpi:  [gadget][rn][imm][taken][fallthrough]
//   fused_cmpr:  [gadget][rn | rm<<8][taken][fallthrough]
//   fused_subsi: [gadget][rd | rn<<8][imm][taken][fallthrough]
//   fused_andsi: [gadget][rd | rn<<8][imm][taken][fallthrough]
//   fused_andsr: [gadget][rd | rn<<8 | rm<<16][taken][fallthrough]

.macro fused_cmpi name, cmpop, zr, r15, r11, cc
.gadget \name
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \cmpop \zr, \r15, \r11
    store_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.macro fused_cmpr name, cmpop, zr, r15, r13, cc
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x12, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    \cmpop \zr, \r15, \r13
    store_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.macro fused_subsi name, subop, r16, r15, r11, cc
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \subop \r16, \r15, \r11
    store_flags
    str x16, [x14, x9, lsl #3]
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

#define CC_LIST eq,ne,cs,cc,mi,pl,vs,vc,hi,ls,ge,lt,gt,le

.irp cc, CC_LIST
fused_cmpi  fused_cmpi64_\cc,  subs, xzr, x15, x11, \cc
fused_cmpi  fused_cmpi32_\cc,  subs, wzr, w15, w11, \cc
fused_cmpr  fused_cmpr64_\cc,  subs, xzr, x15, x13, \cc
fused_cmpr  fused_cmpr32_\cc,  subs, wzr, w15, w13, \cc
fused_subsi fused_subsi64_\cc, subs, x16, x15, x11, \cc
fused_subsi fused_subsi32_\cc, subs, w16, w15, w11, \cc
.endr

// Condition order in every table matches enum arm64_cond (decode.h):
// EQ,NE,CS,CC,MI,PL,VS,VC,HI,LS,GE,LT,GT,LE.
// Pointer tables live in .data — putting .quad symbol references in
// .text is an illegal text relocation on Mach-O.
.data
.macro fused_table name, prefix
    .global NAME(\name)
    .balign 8
NAME(\name) :
    .irp cc, CC_LIST
    .quad NAME(gadget_arm64_\prefix\()_\cc)
    .endr
.endm

fused_table arm64_fused_cmpi64_table,  fused_cmpi64
fused_table arm64_fused_cmpi32_table,  fused_cmpi32
fused_table arm64_fused_cmpr64_table,  fused_cmpr64
fused_table arm64_fused_cmpr32_table,  fused_cmpr32
fused_table arm64_fused_subsi64_table, fused_subsi64
fused_table arm64_fused_subsi32_table, fused_subsi32
.text

// ---- ANDS + branch fusion ------------------------------------------------
// ANDS+B.cond is the highest-impact missing fusion: 8-15% of branches.
// Same savings as fused_subsi (skip NZCV reload) plus eliminates the
// store_flags → load_flags pair that would otherwise bridge two gadgets.
//
// Stream:
//   fused_andsi: [gadget][rd | rn<<8][imm][taken][fallthrough]
//   fused_andsr: [gadget][rd | rn<<8 | rm<<16][taken][fallthrough]

.macro fused_andsi name, andop, r16, r15, r11, cc
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]  // load Rn
    \andop \r16, \r15, \r11      // ands Xd, Xn, #imm
    store_flags
    mrs x8, nzcv                 // save ANDS flags (x8 free after param load)
    cmp x9, #31
    b.eq 1f                       // TST: skip store
    str x16, [x14, x9, lsl #3]   // store result
1:  msr nzcv, x8                 // restore ANDS flags for branch
    b.\cc 2f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
2:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.macro fused_andsr name, andop, r16, r15, r13, cc
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x12, x8, #16, #5      // rm
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]  // load Rn
    ldr x13, [x14, x12, lsl #3]  // load Rm
    \andop \r16, \r15, \r13      // ands Xd, Xn, Xm
    store_flags
    mrs x8, nzcv                 // save ANDS flags (x8 free after param load)
    cmp x9, #31
    b.eq 1f                       // TST: skip store
    str x16, [x14, x9, lsl #3]   // store result
1:  msr nzcv, x8                 // restore ANDS flags for branch
    b.\cc 2f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
2:  ldr _ip, [_ip]
    b arm64_branch_dispatch
.endm

.irp cc, CC_LIST
fused_andsi fused_andsi64_\cc, ands, x16, x15, x11, \cc
fused_andsi fused_andsi32_\cc, ands, w16, w15, w11, \cc
fused_andsr fused_andsr64_\cc, ands, x16, x15, x13, \cc
fused_andsr fused_andsr32_\cc, ands, w16, w15, w13, \cc
.endr

// ---- Load + compare + branch fusion --------------------------------------
// The -O0 loop-exit idiom: `ldr Xt,[B,#o]; [movz/movk Xc, folded away by
// gen.c into a preceding mov_const]; cmp Rn, Rm; b.cond` — three or more
// dispatches collapsed to one (plus the mov_const). The load commits Xt to
// its slot first (fault-restart + guest visibility), then BOTH compare
// operands are re-read from their slots — post-commit, so Rn/Rm may be Xt
// or the just-materialized constant register, or anything else — and the
// branch tests live host flags exactly like fused_cmpr above.
// The load can fault: the stream carries orig_ip at the fixed [_ip, #-8]
// slot (same contract as memory.S's fast loads), and the taken/
// fallthrough words sit AFTER it at [_ip], [_ip, #8].
//
// Stream: [gadget][Xt_slot | B_slot<<16 | Rn_slot<<32 | Rm_slot<<48]
//         [offset][orig_ip][taken][fallthrough]
.macro fused_ldcmpr name, bits, ldreg, zr, rv1, rv2, cc
.gadget \name
    ldp x8, x19, [_ip]
    add _ip, _ip, #24         // past slots+offset+orig_ip: [-8] = orig_ip
    ubfx x9, x8, #16, #16
    ldr x10, [_cpu, x9]
    add x7, x10, x19
    read_prep \bits, \name
    ldr \ldreg, [_addr]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]       // commit Xt (w-load already zero-extended)
    ubfx x9, x8, #32, #16
    ldr x15, [_cpu, x9]       // cmp Rn, from its slot (post-commit)
    ubfx x9, x8, #48, #16
    ldr x13, [_cpu, x9]       // cmp Rm
    subs \zr, \rv1, \rv2
    store_flags
    b.\cc 1f
    ldr _ip, [_ip, #8]
    b arm64_branch_dispatch
1:  ldr _ip, [_ip]
    b arm64_branch_dispatch
    read_bullshit \bits, \name
.endm

.irp cc, CC_LIST
fused_ldcmpr fused_ldcmpr64_\cc, 64, x17, xzr, x15, x13, \cc
fused_ldcmpr fused_ldcmpr32_\cc, 32, w17, wzr, w15, w13, \cc
.endr

.data
fused_table arm64_fused_ldcmpr64_table, fused_ldcmpr64
fused_table arm64_fused_ldcmpr32_table, fused_ldcmpr32
fused_table arm64_fused_andsi64_table, fused_andsi64
fused_table arm64_fused_andsi32_table, fused_andsi32
fused_table arm64_fused_andsr64_table, fused_andsr64
fused_table arm64_fused_andsr32_table, fused_andsr32
.text
