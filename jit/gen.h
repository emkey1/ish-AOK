#ifndef EMU_GEN_H
#define EMU_GEN_H

#include <setjmp.h>
#include "jit/jit.h"
#include "emu/tlb.h"

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
    bool amd64;
    bool arm64;
    bool riscv64;
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
    struct jit_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip; // for call/call_indir gadgets
    // OOM recovery: if oom_active, gen() longjmps instead of dying
    bool oom_active;
    jmp_buf oom_recovery;
};

bool gen_start(guest_addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_amd64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_arm64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
bool gen_start_riscv64(guest_addr_t addr, struct gen_state *state); // returns false on OOM
void gen_exit(struct gen_state *state);
void gen_end(struct gen_state *state);

int gen_step(struct gen_state *state, struct tlb *tlb);
int gen_step_amd64(struct gen_state *state, struct tlb *tlb);
int gen_step_arm64(struct gen_state *state, struct tlb *tlb);
int gen_step_riscv64(struct gen_state *state, struct tlb *tlb);

#endif
