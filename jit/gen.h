#ifndef EMU_GEN_H
#define EMU_GEN_H

#include <setjmp.h>
#include "jit/jit.h"
#include "emu/tlb.h"

// Which base an i386 segment override adds (gen_state.x86_seg).
enum x86_seg_base {
    X86_SEG_NONE,
    X86_SEG_GS,     // tls_ptr: GS's base, and FS's on the amd64 bring-up path
    X86_SEG_FS,     // i386_fs_base
};

struct gen_state {
    addr_t ip;
    addr_t orig_ip;
    guest_addr_t amd64_ip;
    guest_addr_t amd64_orig_ip;
    guest_addr_t arm64_ip;
    guest_addr_t arm64_orig_ip;
    guest_addr_t riscv64_ip;
    guest_addr_t riscv64_orig_ip;
    unsigned long orig_ip_extra;
    // Set by decode.h for a vector instruction whose 128-bit memory operand
    // is NOT alignment-checked (MOVUPS, MOVDQU, LDDQU, PCMPxSTRx) or is really
    // narrower (INSERTPS, PEXTRB/W, PINSRW); gen_vec consumes it. Reset per
    // i386 instruction.
    bool vec_noalign;
    // Set by decode.h for a packed float op (ADDPS, SQRTPD, ...) whose helper
    // size is the element width but whose memory operand is a whole m128:
    // it is alignment-checked like any other.
    bool vec_align128;
    bool amd64;
    bool arm64;
    bool riscv64;
    // ISH_JIT_PROFILE's record of this block (jit/jitprof.c); NULL when off.
    struct jitprof_block *jitprof;
    // True when the previous gen_step_arm64 emitted a gadget that ends
    // with HOST NZCV still equal to the guest flags it just computed
    // (the fast flag-setting ALU/CMP gadgets — nothing after their
    // adds/subs/ands touches host flags, and gret's dispatch sequence
    // doesn't either). A directly-following B.cond can then use the
    // bcond_nf_* family and skip the serializing `msr nzcv` reload.
    bool arm64_flags_live;
    bool amd64_fallback_to_interp;
    bool amd64_abort_block_to_interp;
    bool amd64_deferred_rip_valid;
    bool amd64_reg_cache_valid;
    bool amd64_reg_cache_dirty;
    // amd64 dead-flag elimination (gen.c, amd64_flags_note): the stream slot of
    // the last flag-producing gadget whose flags nothing has read yet, the
    // flags of it still live, and the gadget's full and no-flags forms.
    int amd64_flags_pending_slot;
    unsigned amd64_flags_pending_live;
    unsigned long amd64_flags_pending_full, amd64_flags_pending_nf;
    guest_addr_t amd64_deferred_rip;
    guest_addr_t amd64_fallback_ip;
    uint8_t amd64_fallback_opcode;
    uint8_t amd64_fallback_op2;
    uint8_t amd64_fallback_flags;
    // x86 cmp/test + jcc fusion (gen.c gen_try_fuse_jcc): stream size
    // right after the last 32-bit CMP/TEST finished emitting its flag-op
    // gadget, and which family it was. A position mismatch at the jcc
    // site (anything else emitted in between) invalidates automatically;
    // gen_start must zero these (uninitialized-flag bug class, see
    // gen_start's comment).
    unsigned x86_fuse_end;
    int x86_fuse_op; // 0 = none, 1 = sub (cmp), 2 = and (test)
    // Stream size when that CMP/TEST began, so the fusion can tell its own
    // load32_reg_<dst> from whatever came before (the reg,reg form).
    unsigned x86_fuse_start;
    // The segment override of the i386 instruction being decoded, whose base
    // gen_addr adds. Here rather than in the decoder's locals because an
    // operand-size prefix re-enters the decoder, and assemblers put it after
    // the segment override: `65 66 8b 15` is mov %gs:...,%dx, and was a flat
    // access while this lived in a local. gen_step clears it for each
    // instruction.
    enum x86_seg_base x86_seg;
    struct jit_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip; // for call/call_indir gadgets
    // OOM recovery: if oom_active, gen() longjmps instead of dying
    bool oom_active;
    jmp_buf oom_recovery;
    // Compiling a PTRACE_SINGLESTEP block (jit.c cpu_single_step_*): one
    // gen_step must consume exactly one guest instruction, so every lookahead
    // that would consume a second (fusions, constant folds, register-cached
    // runs) is refused -- gen_arm64_fits_block and gen_riscv64_peek say no.
    bool single_step;
    // riscv64 register-cached runs (gen_riscv64_try_rcache_run): the end of the
    // last window scanned, so instructions inside a rejected window are not
    // rescanned one gen_step at a time (quadratic translation cost).
    guest_addr_t rcache_scan_end;
    // The riscv64 vtype the next vector instruction is expected to run
    // under (UINT64_MAX: unknown): set by a vsetvli in the block, or guessed
    // from the one in force at compile time. The typed vector gadgets check
    // it at run time, so a wrong guess only costs the C path.
    uint64_t riscv64_vtype;
    // The process has V off (PR_RISCV_V_SET_CONTROL): every vector
    // instruction is an illegal one.
    bool riscv64_v_off;
};

bool gen_start(guest_addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_amd64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
void gen_start_x86_profile(guest_addr_t addr, struct gen_state *state);
bool gen_start_arm64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_riscv64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
void gen_exit(struct gen_state *state);
void gen_end(struct gen_state *state);

int gen_step(struct gen_state *state, struct tlb *tlb);
int gen_step_amd64(struct gen_state *state, struct tlb *tlb);
int gen_step_arm64(struct gen_state *state, struct tlb *tlb);
int gen_step_riscv64(struct gen_state *state, struct tlb *tlb);

// Append one raw code-stream word (external emitters, e.g. jit/hle.c).
void gen_raw(struct gen_state *state, unsigned long word);
// jit/hle.c: if the block starting at ip is a fingerprinted libc function
// and HLE is enabled, emit the whole block as one HLE gadget. The caller
// must skip per-instruction translation and go straight to gen_end.
bool hle_try_emit(struct gen_state *state, struct tlb *tlb, guest_addr_t ip,
        bool riscv64);

#endif
