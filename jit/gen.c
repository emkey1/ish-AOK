#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jit/gen.h"
#include "emu/modrm.h"
#include "emu/cpuid.h"
#include "emu/fpu.h"
#include "emu/avx.h"
#include "emu/vec.h"
#include "emu/interrupt.h"
#include "emu/arch/arm64/decode.h"

static int gen_step32(struct gen_state *state, struct tlb *tlb);
static int gen_step16(struct gen_state *state, struct tlb *tlb);
static int gen_step64(struct gen_state *state, struct tlb *tlb);

enum amd64_jit_rep_mode {
    amd64_jit_rep_none,
    amd64_jit_repz,
    amd64_jit_repnz,
};

struct amd64_jit_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_jit_insn {
    guest_addr_t start_ip;
    guest_addr_t end_ip;
    byte_t opcode;
    byte_t op2;
    byte_t modrm;
    bool two_byte_opcode;
    bool has_modrm;
    bool operand_size_prefix;
    bool address_size_prefix;
    bool fs_prefix;
    bool lock_prefix;
    enum amd64_jit_rep_mode rep_mode;
    struct amd64_jit_rex_prefix rex;
};

enum amd64_jit_mem_meta {
    AMD64_JIT_MEM_OPCODE_SHIFT = 0,
    AMD64_JIT_MEM_REG_SHIFT = 8,
    AMD64_JIT_MEM_SIZE_SHIFT = 12,
    AMD64_JIT_MEM_BASE_SHIFT = 20,
    AMD64_JIT_MEM_INDEX_SHIFT = 24,
    AMD64_JIT_MEM_SCALE_SHIFT = 28,
    AMD64_JIT_MEM_HAS_BASE = 1ul << 30,
    AMD64_JIT_MEM_HAS_INDEX = 1ul << 31,
    AMD64_JIT_MEM_RIP_REL = 1ul << 32,
    AMD64_JIT_MEM_FS = 1ul << 33,
    AMD64_JIT_MEM_REX_PRESENT = 1ul << 34,
};

static inline byte_t amd64_modrm_mod(byte_t modrm) {
    return (modrm >> 6) & 0x3;
}

static inline byte_t amd64_modrm_reg(byte_t modrm) {
    return (modrm >> 3) & 0x7;
}

static inline byte_t amd64_modrm_rm(byte_t modrm) {
    return modrm & 0x7;
}

static inline bool amd64_jit_ignored_segment_prefix(byte_t byte) {
    return byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e;
}

static bool amd64_opcode_needs_modrm(const struct amd64_jit_insn *insn) {
    if (insn->two_byte_opcode) {
        switch (insn->op2) {
        case 0x10:
        case 0x18:
        case 0x1f:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x2a:
        case 0x2c:
        case 0x2d:
        case 0x2e:
        case 0x2f:
        case 0x28:
        case 0x29:
        case 0x50:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5a:
        case 0x5c:
        case 0x5d:
        case 0x5e:
        case 0x5f:
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
        case 0x6a:
        case 0x6b:
        case 0x6c:
        case 0x6d:
        case 0x6e:
        case 0x6f:
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x7e:
        case 0x7f:
        case 0x40 ... 0x4f:
        case 0x90 ... 0x9f:
        case 0xa3:
        case 0xa4:
        case 0xa5:
        case 0xab:
        case 0xac:
        case 0xad:
        case 0xaf:
        case 0xb0:
        case 0xb1:
        case 0xb3:
        case 0xba:
        case 0xb6:
        case 0xb7:
        case 0xbe:
        case 0xbf:
        case 0xbc:
        case 0xbd:
        case 0xbb:
        case 0xc0:
        case 0xc1:
        case 0xc2:
        case 0xc4:
        case 0xc5:
        case 0xc6:
        case 0xd1:
        case 0xd2:
        case 0xd3:
        case 0xd4:
        case 0xd5:
        case 0xd6:
        case 0xd7:
        case 0xd8:
        case 0xd9:
        case 0xda:
        case 0xdb:
        case 0xdc:
        case 0xdd:
        case 0xde:
        case 0xdf:
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3:
        case 0xe4:
        case 0xe5:
        case 0xe7:
        case 0xe8:
        case 0xe9:
        case 0xea:
        case 0xeb:
        case 0xec:
        case 0xed:
        case 0xee:
        case 0xef:
        case 0xf1:
        case 0xf2:
        case 0xf3:
        case 0xf4:
        case 0xf6:
        case 0xf8:
        case 0xf9:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfd:
        case 0xfe:
            return true;
        default:
            return false;
        }
    }

    switch (insn->opcode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x38:
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x63:
    case 0x69:
    case 0x6b:
    case 0x80:
    case 0x81:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8d:
    case 0x8f:
    case 0xc0:
    case 0xc1:
    case 0xc6:
    case 0xc7:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xf6:
    case 0xf7:
    case 0xfe:
    case 0xff:
        return true;
    default:
        return false;
    }
}

static bool amd64_jit_debug_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    return enabled == 1;
}

static void amd64_jit_debug(const char *fmt, ...) {
    if (!amd64_jit_debug_enabled())
        return;

    va_list args;
    va_start(args, fmt);
    fputs("[amd64-jit] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int gen_step(struct gen_state *state, struct tlb *tlb) {
    if (state->arm64)
        return gen_step_arm64(state, tlb);
    if (state->riscv64)
        return gen_step_riscv64(state, tlb);
    state->orig_ip = state->ip;
    state->orig_ip_extra = 0;
    if (state->amd64)
        return gen_step64(state, tlb);
    return gen_step32(state, tlb);
}

static void gen(struct gen_state *state, unsigned long thing) {
    assert(state->size <= state->capacity);
    if (state->size >= state->capacity) {
        state->capacity *= 2;
        struct jit_block *bigger_block = realloc(state->block,
                sizeof(struct jit_block) + state->capacity * sizeof(unsigned long));
        if (bigger_block == NULL) {
            if (state->oom_active)
                // _longjmp to match the _setjmp in jit_block_compile_common:
                // no signal-mask restore, hence no sigprocmask syscall.
                _longjmp(state->oom_recovery, 1);
            die("out of memory while jitting");
        }
        state->block = bigger_block;
    }
    assert(state->size < state->capacity);
    state->block->code[state->size++] = thing;
}

void gen_raw(struct gen_state *state, unsigned long word) {
    gen(state, word);
}

static void gen_amd64_emit_rip(struct gen_state *state, guest_addr_t rip) {
    state->amd64_deferred_rip_valid = false;
#if defined(__aarch64__)
    extern void gadget_amd64_set_rip(void);
    gen(state, (unsigned long) gadget_amd64_set_rip);
    gen(state, (unsigned long) rip);
#else
    (void) rip;   // amd64 JIT gadgets are aarch64-only; x86_64 bridges to interp
#endif
}

static void gen_amd64_flush_reg_cache(struct gen_state *state) {
#if defined(__aarch64__)
    if (state->amd64_reg_cache_valid && state->amd64_reg_cache_dirty) {
        extern void gadget_amd64_store_low8_reg_cache(void);
        gen(state, (unsigned long) gadget_amd64_store_low8_reg_cache);
    }
#endif
    state->amd64_reg_cache_valid = false;
    state->amd64_reg_cache_dirty = false;
}

__attribute__((unused)) static void gen_amd64_ensure_reg_cache(struct gen_state *state) {
#if defined(__aarch64__)
    if (!state->amd64_reg_cache_valid) {
        extern void gadget_amd64_load_low8_reg_cache(void);
        gen(state, (unsigned long) gadget_amd64_load_low8_reg_cache);
        state->amd64_reg_cache_valid = true;
        state->amd64_reg_cache_dirty = false;
    }
#else
    (void) state;
#endif
}

__attribute__((unused)) static void gen_amd64_mark_reg_cache_dirty(struct gen_state *state) {
#if defined(__aarch64__)
    state->amd64_reg_cache_dirty = true;
#else
    (void) state;
#endif
}

static void gen_amd64_defer_rip(struct gen_state *state, guest_addr_t rip) {
    state->amd64_deferred_rip = rip;
    state->amd64_deferred_rip_valid = true;
}

static void gen_amd64_flush_rip(struct gen_state *state) {
    if (state->amd64_deferred_rip_valid)
        gen_amd64_emit_rip(state, state->amd64_deferred_rip);
}

static void gen_amd64_jmp_rel(struct gen_state *state, guest_addr_t target_ip) {
    gen_amd64_flush_reg_cache(state);
    state->amd64_deferred_rip_valid = false;
#if defined(__aarch64__)
    extern void gadget_amd64_jmp(void);
    gen(state, (unsigned long) gadget_amd64_jmp);
    gen(state, (unsigned long) (target_ip | (1ull << 63)));
    state->jump_ip[0] = state->size - 1;
#else
    (void) target_ip;
#endif
}

// amd64 cmp+jcc fusion: the cached cmp emitters note their stream position
// (x86_fuse_op 3 = reg_imm form, 4 = reg_reg form; same fields the i386
// fusion uses, reset by any other emission via the position check). When
// the jcc immediately follows, rewrite the cmp gadget in place to its
// fused twin (math.S amd64_fused_cmp_*), which deposits the identical
// eager flag state and then branches on the still-live cf/of/result
// registers -- one dispatch and no eflags reload. If the register cache
// is dirty, the flush gadget must run BEFORE the fused cmp+branch (the
// branch ends the block; a flush emitted after it never runs on the taken
// path) -- rewind the cmp words, emit the flush, re-emit. Runtime-safe:
// the store-back doesn't disturb the host cache registers the cmp reads.
static bool gen_amd64_try_fuse_jcc(struct gen_state *state, unsigned cc) {
#if defined(__aarch64__)
    extern void gadget_amd64_fused_cmp_ri_o(void), gadget_amd64_fused_cmp_ri_c(void),
            gadget_amd64_fused_cmp_ri_z(void), gadget_amd64_fused_cmp_ri_cz(void),
            gadget_amd64_fused_cmp_ri_s(void), gadget_amd64_fused_cmp_ri_sxo(void),
            gadget_amd64_fused_cmp_ri_sxoz(void);
    extern void gadget_amd64_fused_cmp_rr_o(void), gadget_amd64_fused_cmp_rr_c(void),
            gadget_amd64_fused_cmp_rr_z(void), gadget_amd64_fused_cmp_rr_cz(void),
            gadget_amd64_fused_cmp_rr_s(void), gadget_amd64_fused_cmp_rr_sxo(void),
            gadget_amd64_fused_cmp_rr_sxoz(void);
    static void (* const ri[8])(void) = {
        gadget_amd64_fused_cmp_ri_o, gadget_amd64_fused_cmp_ri_c,
        gadget_amd64_fused_cmp_ri_z, gadget_amd64_fused_cmp_ri_cz,
        gadget_amd64_fused_cmp_ri_s, NULL /* parity */,
        gadget_amd64_fused_cmp_ri_sxo, gadget_amd64_fused_cmp_ri_sxoz,
    };
    static void (* const rr[8])(void) = {
        gadget_amd64_fused_cmp_rr_o, gadget_amd64_fused_cmp_rr_c,
        gadget_amd64_fused_cmp_rr_z, gadget_amd64_fused_cmp_rr_cz,
        gadget_amd64_fused_cmp_rr_s, NULL /* parity */,
        gadget_amd64_fused_cmp_rr_sxo, gadget_amd64_fused_cmp_rr_sxoz,
    };
    if (state->x86_fuse_op < 3 || state->x86_fuse_end != state->size)
        return false;
    void (*fused)(void) = (state->x86_fuse_op == 3 ? ri : rr)[(cc >> 1) & 7];
    if (fused == NULL)
        return false;
    unsigned nops = state->x86_fuse_op == 3 ? 2 : 1; // operand words after the gadget
    unsigned slot = state->size - 1 - nops;
    unsigned long saved[2];
    for (unsigned i = 0; i < nops; i++)
        saved[i] = state->block->code[slot + 1 + i];
    state->size = slot;
    gen_amd64_flush_reg_cache(state);
    gen(state, (unsigned long) fused);
    for (unsigned i = 0; i < nops; i++)
        gen(state, saved[i]);
    return true;
#else
    (void) state; (void) cc;
    return false;
#endif
}

// Emit a native amd64 conditional jump. The 16 x86 condition codes map onto the
// 8 do_jump base conditions (base = cc>>1) plus a swap of the two targets for the
// negated (odd) codes. The gadget evaluates the condition from the eager eflags,
// sets CPU_amd64_rip to the taken-or-else target, and exits the block (the main
// loop resolves the dynamic target, exactly as the bridge did but without the
// gadget->C->gadget round-trip). flush + invalidate the deferred rip like
// gen_amd64_jmp_rel; the gadget writes rip itself, so no static link.
static void gen_amd64_jcc(struct gen_state *state, unsigned cc,
        guest_addr_t target_ip, guest_addr_t next_ip) {
#if defined(__aarch64__)
    extern void gadget_amd64_jcc_o(void), gadget_amd64_jcc_c(void),
            gadget_amd64_jcc_z(void), gadget_amd64_jcc_cz(void),
            gadget_amd64_jcc_s(void), gadget_amd64_jcc_p(void),
            gadget_amd64_jcc_sxo(void), gadget_amd64_jcc_sxoz(void);
    static void (* const gadgets[8])(void) = {
        gadget_amd64_jcc_o, gadget_amd64_jcc_c, gadget_amd64_jcc_z, gadget_amd64_jcc_cz,
        gadget_amd64_jcc_s, gadget_amd64_jcc_p, gadget_amd64_jcc_sxo, gadget_amd64_jcc_sxoz,
    };
    bool swap = cc & 1;
    if (gen_amd64_try_fuse_jcc(state, cc)) {
        state->amd64_deferred_rip_valid = false;
        // bit-63 tagged for amd64_branch_dispatch; the frontend patches a
        // matching word to the successor block's code (block chaining).
        gen(state, (unsigned long) ((swap ? next_ip : target_ip) | (1ull << 63)));
        state->jump_ip[0] = state->size - 1;
        gen(state, (unsigned long) ((swap ? target_ip : next_ip) | (1ull << 63)));
        state->jump_ip[1] = state->size - 1;
        return;
    }
    gen_amd64_flush_reg_cache(state);
    state->amd64_deferred_rip_valid = false;
    gen(state, (unsigned long) gadgets[(cc >> 1) & 7]);
    gen(state, (unsigned long) ((swap ? next_ip : target_ip) | (1ull << 63)));   // taken
    state->jump_ip[0] = state->size - 1;
    gen(state, (unsigned long) ((swap ? target_ip : next_ip) | (1ull << 63)));   // else
    state->jump_ip[1] = state->size - 1;
#else
    (void) state; (void) cc; (void) target_ip; (void) next_ip;
#endif
}

// LOOP (0xe2) / LOOPE (0xe1) / LOOPNE (0xe0): decrement RCX + conditional branch, like
// a jcc with an extra counter step. Native gadget (flush-style; the gadget exits the
// block via b jit_ret). operand 0 = taken target, operand 1 = else (fall-through).
static void gen_amd64_loop(struct gen_state *state, unsigned opcode,
        guest_addr_t target_ip, guest_addr_t next_ip) {
#if defined(__aarch64__)
    extern void gadget_amd64_loop(void), gadget_amd64_loope(void),
            gadget_amd64_loopne(void);
    gen_amd64_flush_reg_cache(state);
    state->amd64_deferred_rip_valid = false;
    gen(state, (unsigned long) (opcode == 0xe2 ? gadget_amd64_loop
                : opcode == 0xe1 ? gadget_amd64_loope : gadget_amd64_loopne));
    gen(state, (unsigned long) target_ip);   // operand 0: taken
    gen(state, (unsigned long) next_ip);      // operand 1: else (fall-through)
#else
    (void) state; (void) opcode; (void) target_ip; (void) next_ip;
#endif
}

__attribute__((unused)) static bool amd64_jit_low8_reg(unsigned reg) {
    return reg < 8;
}

bool gen_start(guest_addr_t addr, struct gen_state *state) {
    // Real bug (branch-wide i386/amd64 SIGILL regression, caught by the
    // cross-arch benchmark run, bisected to Phase A): gen_start_arm64 sets
    // state->arm64 = true but plain gen_start never set it false, so
    // i386/amd64 block compiles read UNINITIALIZED stack garbage here and,
    // whenever it happened to be nonzero, gen_step() dispatched x86 bytes
    // into the arm64 decoder — instant INT_UNDEFINED/SIGILL for every
    // x86-guest binary.
    state->arm64 = false;
    state->arm64_flags_live = false;
    state->arm64_ip = addr;
    state->arm64_orig_ip = addr;
    state->riscv64 = false; // same uninitialized-flag bug class as arm64 above
    state->riscv64_ip = addr;
    state->riscv64_orig_ip = addr;
    state->amd64 = false;
    state->amd64_fallback_to_interp = false;
    state->amd64_abort_block_to_interp = false;
    state->amd64_deferred_rip_valid = false;
    state->amd64_reg_cache_valid = false;
    state->amd64_reg_cache_dirty = false;
    state->amd64_deferred_rip = addr;
    state->amd64_fallback_ip = addr;
    state->amd64_fallback_opcode = 0;
    state->amd64_fallback_op2 = 0;
    state->amd64_fallback_flags = 0;
    state->x86_fuse_end = 0; // same uninitialized-flag bug class as arm64 above
    state->x86_fuse_op = 0;
    state->capacity = JIT_BLOCK_INITIAL_CAPACITY;
    state->size = 0;
    state->ip = addr;
    state->amd64_ip = addr;
    state->amd64_orig_ip = addr;
    state->oom_active = false;
    for (int i = 0; i <= 1; i++) {
        state->jump_ip[i] = 0;
    }
    state->block_patch_ip = 0;

    struct jit_block *block = malloc(sizeof(struct jit_block) + state->capacity * sizeof(unsigned long));
    if (block == NULL) {
        state->block = NULL;
        return false;
    }
    state->block = block;
    block->addr = addr;
    return true;
}

bool gen_start_amd64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->amd64 = true;
    return true;
}

bool gen_start_arm64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->arm64 = true;
    state->arm64_ip = addr;
    state->arm64_orig_ip = addr;
    return true;
}

// ---- AArch64 guest code generator ---------------------------------------
// Everything from here through gen_step_arm64 references the arm64-GUEST
// gadget symbols (jit/guest-arm64/*.S), which are AArch64 assembly and only
// assembled when the host gadget set is aarch64 (meson.build defines
// ISH_JIT_ARM64_GUEST there). On other hosts (e.g. the x86_64 CLI build)
// this whole section is compiled out and gen_step_arm64 becomes the clean
// unsupported stub at the #else below — otherwise the static gadget-address
// tables in these functions fail to link.
#ifdef ISH_JIT_ARM64_GUEST

// (size, opc) -> single-register load/store gadget (jit/guest-arm64/
// memory.S), shared by all four addressing families that lower to that
// gadget set. opc semantics per the ARM ARM: size<3: 0=store, 1=load,
// 2=LDRS-to-64, 3=LDRS-to-32; size=3: 0=store, 1=load (2=PRFM is a hint
// the callers lower to nothing before getting here; 3=unallocated).
// Returns NULL for unallocated combinations (size=2/3 with opc=3, size=3
// with opc=2).
static void *gen_arm64_ldst_single_gadget(unsigned size, unsigned opc) {
    extern void gadget_arm64_load8(void), gadget_arm64_load16(void);
    extern void gadget_arm64_load32(void), gadget_arm64_load64(void);
    extern void gadget_arm64_loads8_32(void), gadget_arm64_loads8_64(void);
    extern void gadget_arm64_loads16_32(void), gadget_arm64_loads16_64(void);
    extern void gadget_arm64_loads32_64(void);
    extern void gadget_arm64_store8(void), gadget_arm64_store16(void);
    extern void gadget_arm64_store32(void), gadget_arm64_store64(void);
    static void *const table[4][4] = {
        // size=0 (byte)
        { (void *) gadget_arm64_store8, (void *) gadget_arm64_load8,
          (void *) gadget_arm64_loads8_64, (void *) gadget_arm64_loads8_32 },
        // size=1 (halfword)
        { (void *) gadget_arm64_store16, (void *) gadget_arm64_load16,
          (void *) gadget_arm64_loads16_64, (void *) gadget_arm64_loads16_32 },
        // size=2 (word)
        { (void *) gadget_arm64_store32, (void *) gadget_arm64_load32,
          (void *) gadget_arm64_loads32_64, NULL },
        // size=3 (doubleword)
        { (void *) gadget_arm64_store64, (void *) gadget_arm64_load64,
          NULL, NULL },
    };
    return table[size & 3][opc & 3];
}

// Fast-path variants of the single-register set (jit/guest-arm64/
// memory.S's *_fast family): offset addressing only (no writeback),
// rt != 31, with the cpu_state slot offsets precomputed here instead of
// decoded at runtime. Same (size, opc) shape as the generic table; the
// callers only consult this after the generic lookup verified the
// combination is allocated.
static void *gen_arm64_ldst_single_fast_gadget(unsigned size, unsigned opc) {
    extern void gadget_arm64_load8_fast(void), gadget_arm64_load16_fast(void);
    extern void gadget_arm64_load32_fast(void), gadget_arm64_load64_fast(void);
    extern void gadget_arm64_loads8_32_fast(void), gadget_arm64_loads8_64_fast(void);
    extern void gadget_arm64_loads16_32_fast(void), gadget_arm64_loads16_64_fast(void);
    extern void gadget_arm64_loads32_64_fast(void);
    extern void gadget_arm64_store8_fast(void), gadget_arm64_store16_fast(void);
    extern void gadget_arm64_store32_fast(void), gadget_arm64_store64_fast(void);
    static void *const table[4][4] = {
        { (void *) gadget_arm64_store8_fast, (void *) gadget_arm64_load8_fast,
          (void *) gadget_arm64_loads8_64_fast, (void *) gadget_arm64_loads8_32_fast },
        { (void *) gadget_arm64_store16_fast, (void *) gadget_arm64_load16_fast,
          (void *) gadget_arm64_loads16_64_fast, (void *) gadget_arm64_loads16_32_fast },
        { (void *) gadget_arm64_store32_fast, (void *) gadget_arm64_load32_fast,
          (void *) gadget_arm64_loads32_64_fast, NULL },
        { (void *) gadget_arm64_store64_fast, (void *) gadget_arm64_load64_fast,
          NULL, NULL },
    };
    return table[size & 3][opc & 3];
}

// cpu_state byte offset of a guest register slot for the fast load/store
// param word. r=31 is SP in load/store base position (never XZR there).
// Both offsets must fit the param word's 16-bit fields.
static uint64_t gen_arm64_reg_slot(unsigned r) {
    _Static_assert(offsetof(struct cpu_state, arm64_sp) < 0x10000 &&
                   offsetof(struct cpu_state, arm64_regs) + 31 * 8 < 0x10000,
                   "arm64 register file must sit in cpu_state's first 64k");
    if (r == 31)
        return offsetof(struct cpu_state, arm64_sp);
    return offsetof(struct cpu_state, arm64_regs) + r * 8;
}

// cond -> condition-evaluation gadget (jit/guest-arm64/dpextra.S's
// cond_* family). AL and NV are both architecturally "always" for the
// consuming instructions here (CSEL/CCMP never invert them the way
// B.cond's pseudo-encoding table might suggest).
static void *gen_arm64_cond_gadget(unsigned cond) {
    extern void gadget_arm64_cond_eq(void), gadget_arm64_cond_ne(void);
    extern void gadget_arm64_cond_cs(void), gadget_arm64_cond_cc(void);
    extern void gadget_arm64_cond_mi(void), gadget_arm64_cond_pl(void);
    extern void gadget_arm64_cond_vs(void), gadget_arm64_cond_vc(void);
    extern void gadget_arm64_cond_hi(void), gadget_arm64_cond_ls(void);
    extern void gadget_arm64_cond_ge(void), gadget_arm64_cond_lt(void);
    extern void gadget_arm64_cond_gt(void), gadget_arm64_cond_le(void);
    extern void gadget_arm64_cond_al(void);
    static void *const table[16] = {
        (void *) gadget_arm64_cond_eq, (void *) gadget_arm64_cond_ne,
        (void *) gadget_arm64_cond_cs, (void *) gadget_arm64_cond_cc,
        (void *) gadget_arm64_cond_mi, (void *) gadget_arm64_cond_pl,
        (void *) gadget_arm64_cond_vs, (void *) gadget_arm64_cond_vc,
        (void *) gadget_arm64_cond_hi, (void *) gadget_arm64_cond_ls,
        (void *) gadget_arm64_cond_ge, (void *) gadget_arm64_cond_lt,
        (void *) gadget_arm64_cond_gt, (void *) gadget_arm64_cond_le,
        (void *) gadget_arm64_cond_al, (void *) gadget_arm64_cond_al,
    };
    return table[cond & 0xf];
}

// SIMD/FP single-register load/store gadget by access size (log2 bytes,
// 0..4 = B/H/S/D/Q). Same parameter format as the integer set — the
// gadgets share ldst_single_setup/-writeback (gadgets.h).
static void *gen_arm64_vldst_single_gadget(unsigned size_log2, bool is_load) {
    extern void gadget_arm64_vload8(void), gadget_arm64_vload16(void);
    extern void gadget_arm64_vload32(void), gadget_arm64_vload64(void);
    extern void gadget_arm64_vload128(void);
    extern void gadget_arm64_vstore8(void), gadget_arm64_vstore16(void);
    extern void gadget_arm64_vstore32(void), gadget_arm64_vstore64(void);
    extern void gadget_arm64_vstore128(void);
    static void *const loads[5] = {
        (void *) gadget_arm64_vload8, (void *) gadget_arm64_vload16,
        (void *) gadget_arm64_vload32, (void *) gadget_arm64_vload64,
        (void *) gadget_arm64_vload128,
    };
    static void *const stores[5] = {
        (void *) gadget_arm64_vstore8, (void *) gadget_arm64_vstore16,
        (void *) gadget_arm64_vstore32, (void *) gadget_arm64_vstore64,
        (void *) gadget_arm64_vstore128,
    };
    if (size_log2 > 4)
        return NULL;
    return is_load ? loads[size_log2] : stores[size_log2];
}

// SIMD/FP load/store size decode: the access size is size:opc<1>
// (0..4 = B/H/S/D/Q); opc<0> is the load bit. Returns size_log2 or -1
// for the unallocated size=Q-with-size!=00 combinations.
static int gen_arm64_vldst_size(unsigned size, unsigned opc, bool *is_load) {
    *is_load = opc & 1;
    if (opc & 2) {
        // Q access: only size=00 is allocated
        if (size != 0)
            return -1;
        return 4;
    }
    return (int) size;
}

// AdvSIMDExpandImm (ARM ARM): expand the MOVI/MVNI/FMOV-immediate
// abc:defgh byte + cmode + op into the 64-bit element pattern. Direct
// transcription of the pseudocode's cmode<3:1> switch. Returns false
// for the one unallocated combination (op=1, cmode=1111, handled by
// the caller as Q-only FMOV .2d, which this covers too).
static bool gen_arm64_expand_imm(unsigned op, unsigned cmode, uint64_t imm8, uint64_t *out) {
    uint64_t imm32;
    switch (cmode >> 1) {
    case 0: imm32 = imm8; break;
    case 1: imm32 = imm8 << 8; break;
    case 2: imm32 = imm8 << 16; break;
    case 3: imm32 = imm8 << 24; break;
    case 4: // 16-bit elements
        *out = imm8 | (imm8 << 16) | (imm8 << 32) | (imm8 << 48);
        return true;
    case 5:
        *out = (imm8 << 8) | (imm8 << 24) | (imm8 << 40) | (imm8 << 56);
        return true;
    case 6: // MSL: ones shifted in from the bottom
        imm32 = (cmode & 1) ? ((imm8 << 16) | 0xffff) : ((imm8 << 8) | 0xff);
        break;
    default: { // cmode 111x
        uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
        if (cmode == 14 && op == 0) { // bytes
            imm32 = imm8 | (imm8 << 8) | (imm8 << 16) | (imm8 << 24);
            break;
        }
        if (cmode == 14 && op == 1) { // each imm8 bit -> one 0x00/0xff byte
            uint64_t v = 0;
            for (int i = 0; i < 8; i++)
                if (imm8 & (1u << i))
                    v |= 0xffULL << (i * 8);
            *out = v;
            return true;
        }
        if (cmode == 15 && op == 0) { // FMOV single: a:~b:bbbbb:cdefgh:0*19, per 32 bits
            imm32 = (a << 31) | ((b ? 0x3e0ULL : 0x400ULL) << 20) | ((imm8 & 0x3f) << 19);
            break;
        }
        if (cmode == 15 && op == 1) { // FMOV double: a:~b:bbbbbbbb:cdefgh:0*48
            *out = (a << 63) | ((b ? 0x3fcULL : 0x400ULL) << 52) | ((imm8 & 0x3f) << 48);
            return true;
        }
        return false;
    }
    }
    *out = imm32 | (imm32 << 32);
    return true;
}

// Compare+branch fusion: peek at the instruction after a fast compare;
// if it's a B.cond, return the gadget from the given per-condition table
// and compute its targets. The caller then emits ONE fused gadget for
// the pair (one dispatch instead of two, and the branch tests live host
// flags — no msr nzcv reload). Returns NULL when the next instruction
// isn't a fusable B.cond (or can't be fetched — page-crossing decode
// just declines to fuse).
// Lookahead page budget. The compile loop (jit.c's
// jit_block_compile_common) only checks its "no block spans more than
// 2 pages" cap BETWEEN gen_steps, so any lookahead that consumes extra
// instructions within one step must bound itself: a step may not push
// arm64_ip past block start + PAGE_SIZE (the invariant jit.c asserts
// after gen_end, and what jit_insert's two-page registration relies on
// for invalidation — a 3-page block's middle page would be invisible to
// jit_invalidate_page). This is the arm64 equivalent of the x86 loop's
// 15-byte instruction-length slack. Callers stop folding/fusing when the
// consumed end would exceed the budget and fall back to the plain
// unfused path.
static bool gen_arm64_fits_block(struct gen_state *state, uint64_t end_ip) {
    return end_ip - state->block->addr <= PAGE_SIZE;
}

// Bisection escape hatch AND measurement switch: ISH_ARM64_NO_FUSE=1 disables all
// three arm64 lookahead fusion passes (compare+branch, load+store RMW,
// load+compare) so a deterministic miscompilation can be pinned to a fusion vs.
// the base gadgets, and /proc/ish/arm64_jit_fuse turns each pass on and off
// INDIVIDUALLY at runtime so one can be sized without rebuilding.
//
// The `static int cached` this replaced was a correctness hazard for the proc
// node, not just a missed feature: it would have frozen the answer at the first
// translated block and silently ignored every later write. Read live instead --
// one relaxed atomic load per fusion attempt, at translation time.
static bool arm64_fuse_pass_enabled(unsigned bit) {
    return (arm64_jit_fuse_mask() & bit) != 0;
}

static void *gen_arm64_peek_bcond(struct gen_state *state, struct tlb *tlb,
        void *const table[14], uint64_t *taken_out, uint64_t *fallthrough_out) {
    if (!arm64_fuse_pass_enabled(JIT_FUSE_A64_BCOND))
        return NULL;
    uint32_t next;
    if (!tlb_read(tlb, state->arm64_ip, &next, sizeof(next)))
        return NULL;
    if ((next & 0xff000010) != 0x54000000)
        return NULL;
    unsigned cond = next & 0xf;
    if (cond >= 14)
        return NULL; // AL/NV: plain unconditional, no fusion needed
    *taken_out = state->arm64_ip + (uint64_t) arm64_branch_imm19(next);
    *fallthrough_out = state->arm64_ip + 4;
    state->arm64_ip += 4; // consume the B.cond too
    return table[cond];
}

// Host optional-ISA probe for gadget selection. The crypto/CRC gadgets
// execute the corresponding host instruction natively, but those are
// optional extensions Apple only added over time: FEAT_CRC32 with A10,
// FEAT_SHA512/SHA3 with A13 — and we support devices back to A7-class
// hardware (iOS 14 baseline is A8/A9). On hosts that lack them, gen
// emits the soft-fallback gadgets (crc32_soft, sha512_soft) instead, so
// the guest-visible feature set (AT_HWCAP, ID_AA64ISAR0) stays identical
// on every device. The SHA3 data ops need no gating: their gadgets are
// synthesized from baseline NEON (crypto.S).
#if defined(__APPLE__)
#include <sys/sysctl.h>
static bool arm64_host_flag(const char *feat, const char *legacy) {
    uint32_t val = 0;
    size_t size = sizeof(val);
    if (sysctlbyname(feat, &val, &size, NULL, 0) == 0 && val)
        return true;
    val = 0; size = sizeof(val);
    return sysctlbyname(legacy, &val, &size, NULL, 0) == 0 && val;
}
#elif defined(__linux__)
#include <sys/auxv.h>
#endif
static bool arm64_host_has_sha512;
static bool arm64_host_has_crc32;
__attribute__((constructor)) static void arm64_probe_host_caps(void) {
#if defined(__APPLE__)
    // New-style names first (iOS 15+/macOS 12+), then the legacy spellings.
    arm64_host_has_sha512 = arm64_host_flag("hw.optional.arm.FEAT_SHA512",
                                            "hw.optional.armv8_2_sha512");
    arm64_host_has_crc32 = arm64_host_flag("hw.optional.arm.FEAT_CRC32",
                                           "hw.optional.armv8_crc32");
#elif defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    arm64_host_has_sha512 = hwcap & (1ul << 21); // HWCAP_SHA512
    arm64_host_has_crc32 = hwcap & (1ul << 7);   // HWCAP_CRC32
#endif
    // Anything unprobed stays false: the soft paths are always correct.
    // Test escape hatch: exercise the pre-A13/pre-A10 soft paths on a
    // host that has the native instructions.
    const char *force = getenv("ISH_ARM64_FORCE_SOFT_CRYPTO");
    if (force != NULL && *force == '1')
        arm64_host_has_sha512 = arm64_host_has_crc32 = false;
}

// -O0 idiom fusion (see memory.S's "Fused -O0 idioms" section): starting
// from a plain fast-eligible LDR (unsigned-imm form, 32/64-bit, rt != 31),
// peek at the following instructions and fuse gcc's stack-slot patterns
// into one gadget:
//
//   ldr Xt,[B,#o]; add/sub Xt,Xt,#imm (no flags); str Xt,[B,#o]
//     -> rmw_{add,sub}i_fast{32,64}
//   ldr Xa,[B1,#o1]; ldr Xb,[B2,#o2]; OP Xd,Xa,Xb; str Xd,[B3,#o3]
//     -> ldld{op}_st_fast{32,64}   (OP: plain add/sub/and/orr/eor, shift 0)
//
// Aliasing guards keep the fault-restart contract sound (a replayed
// sub-instruction must see exactly the register state the unfused
// sequence would have had — see memory.S): Xa != Xb, B2 != Xa, the store
// base is none of {Xa, Xb, Xd}, and the RMW form's base is not its data
// register. SUB fuses only in Xa - Xb operand order; the commutative ops
// accept either order. Same lookahead-consume precedent as
// gen_arm64_peek_bcond. Returns true if a fused gadget was emitted (the
// extra instructions are consumed); false leaves state untouched.
static bool gen_arm64_try_ldst_fusion(struct gen_state *state, struct tlb *tlb,
        unsigned size, unsigned rt, unsigned rn, uint64_t off) {
    if (!arm64_fuse_pass_enabled(JIT_FUSE_A64_LDST))
        return false;
    extern void gadget_arm64_rmw_addi_fast64(void), gadget_arm64_rmw_subi_fast64(void);
    extern void gadget_arm64_rmw_addi_fast32(void), gadget_arm64_rmw_subi_fast32(void);
    extern void gadget_arm64_ldldadd_st_fast64(void), gadget_arm64_ldldsub_st_fast64(void);
    extern void gadget_arm64_ldldand_st_fast64(void), gadget_arm64_ldldorr_st_fast64(void);
    extern void gadget_arm64_ldldeor_st_fast64(void);
    extern void gadget_arm64_ldldadd_st_fast32(void), gadget_arm64_ldldsub_st_fast32(void);
    extern void gadget_arm64_ldldand_st_fast32(void), gadget_arm64_ldldorr_st_fast32(void);
    extern void gadget_arm64_ldldeor_st_fast32(void);
    unsigned sf = size == 3;
    uint32_t str_match = ((uint32_t) size << 30) | 0x39000000; // same-width STR (unsigned imm)
    uint32_t i2;
    if (!tlb_read(tlb, state->arm64_ip, &i2, sizeof(i2)))
        return false;

    // ldr; ldr; op; str
    if ((i2 & 0xffc00000) == (str_match | 0x00400000)) { // same-width LDR (opc=1)
        unsigned rb = i2 & 0x1f, rn2 = (i2 >> 5) & 0x1f;
        uint64_t off2 = (uint64_t) ((i2 >> 10) & 0xfff) << size;
        uint32_t i3, i4;
        if (rb == 31 || rb == rt || rn2 == rt ||
                !tlb_read(tlb, state->arm64_ip + 4, &i3, sizeof(i3)) ||
                !tlb_read(tlb, state->arm64_ip + 8, &i4, sizeof(i4)))
            goto try_rmw;
        // plain shifted-register ALU op, shift amount 0, no flags, right sf
        static void *const ldld_table[5][2] = {
            {(void *) gadget_arm64_ldldadd_st_fast32, (void *) gadget_arm64_ldldadd_st_fast64},
            {(void *) gadget_arm64_ldldsub_st_fast32, (void *) gadget_arm64_ldldsub_st_fast64},
            {(void *) gadget_arm64_ldldand_st_fast32, (void *) gadget_arm64_ldldand_st_fast64},
            {(void *) gadget_arm64_ldldorr_st_fast32, (void *) gadget_arm64_ldldorr_st_fast64},
            {(void *) gadget_arm64_ldldeor_st_fast32, (void *) gadget_arm64_ldldeor_st_fast64},
        };
        int op;
        switch (i3 & 0x7fe0fc00) {
        case 0x0b000000: op = 0; break; // ADD (shifted reg), shift 0
        case 0x4b000000: op = 1; break; // SUB
        case 0x0a000000: op = 2; break; // AND
        case 0x2a000000: op = 3; break; // ORR
        case 0x4a000000: op = 4; break; // EOR
        default: goto try_rmw;
        }
        if (((i3 >> 31) & 1) != sf)
            goto try_rmw;
        unsigned rn3 = (i3 >> 5) & 0x1f, rm3 = (i3 >> 16) & 0x1f, rd3 = i3 & 0x1f;
        bool in_order = rn3 == rt && rm3 == rb;
        bool swapped = rn3 == rb && rm3 == rt;
        if (rd3 == 31 || !(in_order || (swapped && op != 1)))
            goto try_rmw;
        if ((i4 & 0xffc00000) != str_match)
            goto try_rmw;
        unsigned rt4 = i4 & 0x1f, rn4 = (i4 >> 5) & 0x1f;
        if (rt4 != rd3 || rn4 == rt || rn4 == rb || rn4 == rd3)
            goto try_rmw;
        uint64_t off3 = (uint64_t) ((i4 >> 10) & 0xfff) << size;
        gen(state, (unsigned long) ldld_table[op][sf]);
        gen(state, gen_arm64_reg_slot(rt) | (gen_arm64_reg_slot(rn) << 16));
        gen(state, off);
        gen(state, state->arm64_orig_ip);
        gen(state, gen_arm64_reg_slot(rb) | (gen_arm64_reg_slot(rn2) << 16));
        gen(state, off2);
        gen(state, state->arm64_orig_ip + 4);
        gen(state, gen_arm64_reg_slot(rd3) | (gen_arm64_reg_slot(rn4) << 16));
        gen(state, off3);
        gen(state, state->arm64_orig_ip + 12);
        state->arm64_ip += 12; // consume ldr2 + op + str
        return true;
    }

try_rmw:
    // ldr; add/sub-imm in place; str back to the same slot
    {
        // ADD/SUB (immediate), no flags: sf|op|0|100010|sh|imm12|rn|rd
        // (mask leaves op free — bit 30 picks add vs sub below — and
        // pins S=0: flag-setting forms keep the generic path.)
        if ((i2 & 0x3f800000) != 0x11000000 || ((i2 >> 31) & 1) != sf)
            return false;
        unsigned rd2 = i2 & 0x1f, rn2 = (i2 >> 5) & 0x1f;
        if (rd2 != rt || rn2 != rt || rn == rt)
            return false;
        uint64_t imm = (i2 >> 10) & 0xfff;
        if ((i2 >> 22) & 1)
            imm <<= 12;
        uint32_t i3;
        if (!tlb_read(tlb, state->arm64_ip + 4, &i3, sizeof(i3)))
            return false;
        if ((i3 & 0xffc00000) != str_match)
            return false;
        unsigned rt3 = i3 & 0x1f, rn3 = (i3 >> 5) & 0x1f;
        if (rt3 != rt || rn3 != rn || (uint64_t) (((i3 >> 10) & 0xfff)) << size != off)
            return false;
        bool is_sub = (i2 >> 30) & 1;
        static void *const rmw_table[2][2] = {
            {(void *) gadget_arm64_rmw_addi_fast32, (void *) gadget_arm64_rmw_addi_fast64},
            {(void *) gadget_arm64_rmw_subi_fast32, (void *) gadget_arm64_rmw_subi_fast64},
        };
        gen(state, (unsigned long) rmw_table[is_sub][sf]);
        gen(state, gen_arm64_reg_slot(rt) | (gen_arm64_reg_slot(rn) << 16));
        gen(state, off);
        gen(state, state->arm64_orig_ip);
        gen(state, imm);
        gen(state, state->arm64_orig_ip + 8);
        state->arm64_ip += 8; // consume the ALU op + str
        return true;
    }
}

// Load + compare + branch fusion (control.S's fused_ldcmpr family): the
// -O0 loop-exit idiom
//
//   ldr Xt,[B,#o]; [movz/movk chain -> Xc]; cmp Rn, Rm; b.cond
//
// becomes [mov_const Xc] + ONE gadget. The optional constant chain between
// the load and the compare is folded and emitted BEFORE the load —
// reorder-safe because Xc is guarded away from the load's base and target
// (and a load-fault replay recomputes the identical constant), and
// guest-invisible otherwise since no fault point separates them in the
// fused block. Returns true with the block ended (caller returns 0).
static bool gen_arm64_try_ld_cmp_fusion(struct gen_state *state, struct tlb *tlb,
        unsigned size, unsigned rt, unsigned rn, uint64_t off) {
    if (!arm64_fuse_pass_enabled(JIT_FUSE_A64_LDCMP))
        return false;
    extern void gadget_arm64_mov_const(void);
    extern void *const arm64_fused_ldcmpr64_table[14];
    extern void *const arm64_fused_ldcmpr32_table[14];
    unsigned sf = size == 3;
    uint64_t scan = state->arm64_ip;
    uint32_t next;
    if (!tlb_read(tlb, scan, &next, sizeof(next)))
        return false;
    // Optional MOVZ/MOVN(+MOVK chain) materializing a compare operand.
    bool have_const = false;
    unsigned rc = 0;
    uint64_t cval = 0;
    if ((next & 0x1f800000) == 0x12800000) {
        unsigned copc = (next >> 29) & 3, chw = (next >> 21) & 3;
        unsigned csf = (next >> 31) & 1;
        rc = next & 0x1f;
        if ((copc != 0b00 && copc != 0b10) || rc == 31 || rc == rt || rc == rn ||
                (!csf && chw >= 2))
            return false;
        cval = ((uint64_t) ((next >> 5) & 0xffff)) << (chw * 16);
        if (copc == 0b00)
            cval = ~cval;
        if (!csf)
            cval &= 0xffffffff;
        scan += 4;
        // gen_arm64_fits_block: bound the scan so the fused consumption
        // (this chain + the cmp + b.cond below) can't exceed the page
        // budget; a budget-stopped chain fails the cmp match below and
        // declines to fuse, leaving everything unconsumed.
        while (gen_arm64_fits_block(state, scan + 4) &&
                tlb_read(tlb, scan, &next, sizeof(next)) &&
                (next & 0x7f800000) == 0x72800000 && // MOVK
                ((next >> 31) & 1) == csf && (next & 0x1f) == rc &&
                (csf || ((next >> 21) & 0x3) < 2)) {
            unsigned hw2 = (next >> 21) & 0x3;
            uint64_t imm2 = (next >> 5) & 0xffff;
            cval = (cval & ~(0xffffULL << (hw2 * 16))) | (imm2 << (hw2 * 16));
            scan += 4;
        }
        have_const = true;
        if (!tlb_read(tlb, scan, &next, sizeof(next)))
            return false;
    }
    // CMP = SUBS to ZR, shifted register, shift 0, width matching the load.
    // ZR compare operands keep the generic path (no slot to read them from).
    if ((next & 0x7fe0fc00) != 0x6b000000 || (next & 0x1f) != 31 ||
            ((next >> 31) & 1) != sf)
        return false;
    unsigned cmp_rn = (next >> 5) & 0x1f, cmp_rm = (next >> 16) & 0x1f;
    if (cmp_rn == 31 || cmp_rm == 31)
        return false;
    scan += 4;
    if (!tlb_read(tlb, scan, &next, sizeof(next)) ||
            (next & 0xff000010) != 0x54000000 || (next & 0xf) >= 14)
        return false;
    uint64_t taken = scan + (uint64_t) arm64_branch_imm19(next);
    uint64_t fallthrough = scan + 4;
    scan += 4;
    // Final page-budget gate on the whole fused consumption (the loop
    // bound above keeps the scan short, but the trailing cmp + b.cond
    // still add 8 bytes). Declining here consumes nothing: the caller
    // emits the plain unfused load and later steps handle the rest.
    if (!gen_arm64_fits_block(state, scan))
        return false;

    if (have_const) {
        gen(state, (unsigned long) gadget_arm64_mov_const);
        gen(state, gen_arm64_reg_slot(rc));
        gen(state, cval);
    }
    void *const *table = sf ? arm64_fused_ldcmpr64_table : arm64_fused_ldcmpr32_table;
    gen(state, (unsigned long) table[next & 0xf]);
    gen(state, gen_arm64_reg_slot(rt) | (gen_arm64_reg_slot(rn) << 16) |
               (gen_arm64_reg_slot(cmp_rn) << 32) | (gen_arm64_reg_slot(cmp_rm) << 48));
    gen(state, off);
    gen(state, state->arm64_orig_ip);
    gen(state, taken | 0x8000000000000000ULL);
    state->jump_ip[0] = state->size - 1;
    gen(state, fallthrough | 0x8000000000000000ULL);
    state->jump_ip[1] = state->size - 1;
    state->arm64_ip = scan;
    return true;
}

// AArch64 guest code generator — Phase A of the JIT gadget port
// (aarch64_guest_plan.md). Emits gadget-array entries for jit/guest-arm64/'s
// ported gadgets instead of computing results directly, the same relationship
// emu/arm64_interp.c's arm64_execute() has to real semantics but one level
// removed (that file computes results now; this one emits an instruction
// for the host CPU to compute them later, in the compiled block). Field
// extraction here deliberately mirrors arm64_execute()'s masks exactly —
// same bit layout, same instruction subset (movz/movk/movn, adr/adrp, svc
// in this first slice) — so a reader comparing the two files sees the same
// decode logic, just packing gadget parameters instead of registers.
//
// Undefined/unhandled instructions emit the existing shared gadget_interrupt
// (jit/gadgets-aarch64/entry.S's `.gadget interrupt`, already used by the
// i386 JIT for the same purpose — reused directly, not re-implemented) with
// INT_UNDEFINED, ending the block there. No interpreter fallback, matching
// i386's own precedent (aarch64_guest_plan.md's direction-change rationale).
// Emit an INT_UNDEFINED interrupt for an unallocated (or unported) arm64
// encoding and end the block, so the guest gets SIGILL at the right PC.
// Logs the encoding: a matched-family-but-rejected-variant SIGILL is
// otherwise invisible (the "no gadget" warning only covers the final
// decoder fallthrough).
static int gen_arm64_undefined_at(struct gen_state *state, uint32_t insn) {
    extern void gadget_arm64_interrupt(void);
    if (insn != 0)
        printk("WARNING: arm64 JIT: rejected encoding %#010x at pc %#llx\n",
               insn, (unsigned long long) state->arm64_orig_ip);
    gen(state, (unsigned long) gadget_arm64_interrupt);
    gen(state, INT_UNDEFINED);
    gen(state, state->arm64_orig_ip);
    gen(state, state->arm64_orig_ip);
    return 0;
}
#define gen_arm64_undefined(state) gen_arm64_undefined_at(state, insn)

int gen_step_arm64(struct gen_state *state, struct tlb *tlb) {
    extern void gadget_interrupt(void);
    extern void gadget_arm64_movz(void);
    extern void gadget_arm64_movk(void);
    extern void gadget_arm64_movn(void);
    extern void gadget_arm64_adr(void);
    extern void gadget_arm64_svc(void);
    extern void gadget_arm64_b(void);
    extern void gadget_arm64_bl(void);
    extern void gadget_arm64_br(void);
    extern void gadget_arm64_blr(void);
    extern void gadget_arm64_ret(void);
    extern void gadget_arm64_cbz(void);
    extern void gadget_arm64_cbnz(void);
    extern void gadget_arm64_tbz(void);
    extern void gadget_arm64_tbnz(void);
    extern void gadget_arm64_bcond_eq(void), gadget_arm64_bcond_ne(void);
    extern void gadget_arm64_bcond_cs(void), gadget_arm64_bcond_cc(void);
    extern void gadget_arm64_bcond_mi(void), gadget_arm64_bcond_pl(void);
    extern void gadget_arm64_bcond_vs(void), gadget_arm64_bcond_vc(void);
    extern void gadget_arm64_bcond_hi(void), gadget_arm64_bcond_ls(void);
    extern void gadget_arm64_bcond_ge(void), gadget_arm64_bcond_lt(void);
    extern void gadget_arm64_bcond_gt(void), gadget_arm64_bcond_le(void);
    extern void gadget_arm64_add_imm(void);
    extern void gadget_arm64_sub_imm(void);
    extern void gadget_arm64_ldp64(void), gadget_arm64_ldp32(void);
    extern void gadget_arm64_stp64(void), gadget_arm64_stp32(void);

    state->arm64_orig_ip = state->arm64_ip;
    state->orig_ip_extra = 0;
    // ISH_ARM64_TRACE_IP debug probe: run arm64_trace_probe (emu/tlb.c)
    // before the instruction at the traced guest pc.
    if (unlikely(arm64_trace_ip_target() != 0) &&
            state->arm64_ip == arm64_trace_ip_target()) {
        extern void gadget_arm64_trace_probe(void);
        gen(state, (unsigned long) gadget_arm64_trace_probe);
        gen(state, state->arm64_orig_ip);
    }
    // Compare+branch fusion bookkeeping: consume the previous
    // instruction's flags-live claim, and default to NOT live for this
    // one (only the fast flag-setting paths below re-assert it).
    bool arm64_flags_were_live = state->arm64_flags_live;
    state->arm64_flags_live = false;
    (void) arm64_flags_were_live;

    uint32_t insn;
    if (!tlb_read(tlb, state->arm64_ip, &insn, sizeof(insn))) {
        // Instruction fetch from an unmapped page: deliver SIGSEGV with
        // the fault address, not SIGILL — Linux (and the i386/amd64
        // paths, via the SEGFAULT macro's INT_GPF) reports SEGV_MAPERR
        // here. The arm64 interrupt gadget (control.S) stores the code
        // stream's [pc][addr] words into arm64_pc/segfault_addr and
        // clears segfault_was_write; the kernel's GPF handler forwards
        // an unmapped read fault address to handle_page_fault_interrupt,
        // which resolves it or delivers SIGSEGV at that address. arm64
        // instructions are 4-byte aligned, so the fetch never straddles
        // a page and the fetch address IS the faulting address.
        extern void gadget_arm64_interrupt(void);
        gen(state, (unsigned long) gadget_arm64_interrupt);
        gen(state, INT_GPF);
        gen(state, state->arm64_orig_ip);
        gen(state, state->arm64_orig_ip); // segfault_addr = fetch address
        return 0;
    }
    state->arm64_ip += sizeof(insn);

    // Move wide (immediate): MOVN/MOVZ/MOVK — same mask as
    // emu/arm64_interp.c's arm64_execute() (bits[28:23]=100101).
    if ((insn & 0x1f800000) == 0x12800000) {
        bool sf = (insn >> 31) & 1;
        unsigned opc = (insn >> 29) & 0x3;
        unsigned hw = (insn >> 21) & 0x3;
        uint64_t imm16 = (insn >> 5) & 0xffff;
        unsigned rd = insn & 0x1f;
        if (!sf && hw >= 2) {
            return gen_arm64_undefined(state);
        }
        if (opc == 0b01) {
            return gen_arm64_undefined(state);
        }
        if (rd != 31 && opc != 0b11) {
            // MOVZ/MOVN: the result is fully compile-time known. Fold any
            // immediately-following MOVKs to the same rd/sf into the
            // constant (gcc's standard big-immediate/address build), same
            // lookahead-consume precedent as gen_arm64_peek_bcond. An
            // sf=0 MOVK with hw>=2 is unallocated — decline to fold it so
            // it reaches its own gen_step and takes the undefined path.
            extern void gadget_arm64_mov_const(void);
            uint64_t value = imm16 << (hw * 16);
            if (opc == 0b00)
                value = ~value;
            if (!sf)
                value &= 0xffffffff;
            uint32_t next;
            // gen_arm64_fits_block: stop folding at the page budget — the
            // unconsumed MOVK simply decodes on its own (movk_fast below)
            // in a later step/block, an identical result.
            while (gen_arm64_fits_block(state, state->arm64_ip + 4) &&
                    tlb_read(tlb, state->arm64_ip, &next, sizeof(next)) &&
                    (next & 0x7f800000) == 0x72800000 && // MOVK
                    ((next >> 31) & 1) == (unsigned) sf &&
                    (next & 0x1f) == rd &&
                    (sf || ((next >> 21) & 0x3) < 2)) {
                unsigned hw2 = (next >> 21) & 0x3;
                uint64_t imm2 = (next >> 5) & 0xffff;
                value = (value & ~(0xffffULL << (hw2 * 16))) | (imm2 << (hw2 * 16));
                state->arm64_ip += 4; // consume the MOVK too
            }
            gen(state, (unsigned long) gadget_arm64_mov_const);
            gen(state, offsetof(struct cpu_state, arm64_regs) + rd * 8);
            gen(state, value);
            return 1;
        }
        if (rd != 31 && opc == 0b11) {
            // MOVK with a runtime-live old value: compile-time-shifted
            // insert under a keep-mask (math.S's movk_fast). The sf=0
            // upper-32 clear rides in the mask.
            extern void gadget_arm64_movk_fast(void);
            uint64_t mask = ~(0xffffULL << (hw * 16));
            if (!sf)
                mask &= 0xffffffff;
            gen(state, (unsigned long) gadget_arm64_movk_fast);
            gen(state, offsetof(struct cpu_state, arm64_regs) + rd * 8);
            gen(state, imm16 << (hw * 16));
            gen(state, mask);
            return 1;
        }
        // rd == 31: the write is discarded — keep the generic gadgets'
        // proven handling of that corner rather than special-casing it.
        void *gadget;
        switch (opc) {
        case 0b00: gadget = (void *) gadget_arm64_movn; break;
        case 0b10: gadget = (void *) gadget_arm64_movz; break;
        default:   gadget = (void *) gadget_arm64_movk; break;
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) hw << 8) | (imm16 << 16) | ((uint64_t) sf << 32));
        return 1;
    }

    // ADR/ADRP — mask matches arm64_execute()'s (bits[28:24]=10000). Target
    // is computed here at compile time (PC is compile-time known) rather
    // than by the gadget at runtime — see math.S's adr gadget comment.
    if ((insn & 0x1f000000) == 0x10000000) {
        unsigned rd = insn & 0x1f;
        bool is_adrp = (insn >> 31) & 1;
        int64_t imm = arm64_adr_imm(insn); // shared with emu/arm64_interp.c's decoder
        uint64_t target = is_adrp
            ? (state->arm64_orig_ip & ~(uint64_t) 0xfff) + ((uint64_t) imm << 12)
            : state->arm64_orig_ip + (uint64_t) imm;
        // adrp+add page-address formation: `adrp rd, X; add rd, rd, #lo`
        // completes the target at compile time, so fold the pair into the
        // one adr gadget (measured 1.8-3.8% of static instructions on
        // Alpine aarch64 busybox/musl). Same consumption rules as the
        // other fusions here: page budget checked, and a jump landing on
        // the consumed add just compiles it as its own block. rd==31 is
        // excluded: ADR's 31 is XZR but ADD-immediate's is SP, and the
        // fold's same-register requirement would conflate them.
        uint32_t next;
        if (rd != 31 && gen_arm64_fits_block(state, state->arm64_ip + 4) &&
                tlb_read(tlb, state->arm64_ip, &next, sizeof(next)) &&
                (next & 0xffc00000) == 0x91000000 && // ADD Xd, Xn, #imm12 (sf=1, no S, shift 0)
                (next & 0x1f) == rd && ((next >> 5) & 0x1f) == rd) {
            target += (next >> 10) & 0xfff;
            state->arm64_ip += 4;
        }
        gen(state, (unsigned long) gadget_arm64_adr);
        gen(state, (rd & 0x1f) | ((target & 0xffffffffffffULL) << 8));
        return 1;
    }

    // Add/subtract (immediate) — mask matches arm64_execute()'s
    // (bits[28:24]=10001, flattened form). Params packed exactly as
    // jit/guest-arm64/math.S's add_imm/sub_imm gadgets expect
    // (adapted from OpenMinis' math.S): rd | rn<<8 | imm12<<16 | sf<<28 |
    // S<<29 | sh<<30.
    if ((insn & 0x1f000000) == 0x11000000) {
        unsigned rd = insn & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned imm12 = (insn >> 10) & 0xfff;
        bool sh = (insn >> 22) & 1;
        bool S = (insn >> 29) & 1;
        bool op_sub = (insn >> 30) & 1;
        bool sf = (insn >> 31) & 1;
        // Fast path (the specialization pass — see math.S's fast-gadget
        // header): op/width/flags baked at compile time, immediate
        // pre-shifted. SP-involving forms (rd/rn = 31) keep the generic
        // gadget, except the very hot CMP/CMN alias (S=1, rd=31).
        if (rn != 31 && (rd != 31 || S)) {
            extern void gadget_arm64_addi_fast64(void), gadget_arm64_addi_fast32(void);
            extern void gadget_arm64_subi_fast64(void), gadget_arm64_subi_fast32(void);
            extern void gadget_arm64_addsi_fast64(void), gadget_arm64_addsi_fast32(void);
            extern void gadget_arm64_subsi_fast64(void), gadget_arm64_subsi_fast32(void);
            extern void gadget_arm64_cmpi_fast64(void), gadget_arm64_cmpi_fast32(void);
            extern void gadget_arm64_cmni_fast64(void), gadget_arm64_cmni_fast32(void);
            uint64_t imm = (uint64_t) imm12 << (sh ? 12 : 0);
            if (S && rd == 31) {
                if (op_sub) { // CMP: try single-gadget fusion with a following B.cond
                    extern void *const arm64_fused_cmpi64_table[14];
                    extern void *const arm64_fused_cmpi32_table[14];
                    uint64_t taken, fallthrough;
                    void *fused = gen_arm64_peek_bcond(state, tlb,
                            sf ? arm64_fused_cmpi64_table : arm64_fused_cmpi32_table,
                            &taken, &fallthrough);
                    if (fused != NULL) {
                        gen(state, (unsigned long) fused);
                        gen(state, rn);
                        gen(state, imm);
                        gen(state, taken | 0x8000000000000000ULL);
                        state->jump_ip[0] = state->size - 1;
                        gen(state, fallthrough | 0x8000000000000000ULL);
                        state->jump_ip[1] = state->size - 1;
                        return 0; // the fused pair ends the block
                    }
                }
                static void *const cmp_t[2][2] = { // [op_sub][sf]
                    {(void *) gadget_arm64_cmni_fast32, (void *) gadget_arm64_cmni_fast64},
                    {(void *) gadget_arm64_cmpi_fast32, (void *) gadget_arm64_cmpi_fast64}};
                gen(state, (unsigned long) cmp_t[op_sub][sf]);
                gen(state, rn);
                gen(state, imm);
                state->arm64_flags_live = true;
                return 1;
            }
            if (S && op_sub) { // SUBS with result (loop counters): try fusion
                extern void *const arm64_fused_subsi64_table[14];
                extern void *const arm64_fused_subsi32_table[14];
                uint64_t taken, fallthrough;
                void *fused = gen_arm64_peek_bcond(state, tlb,
                        sf ? arm64_fused_subsi64_table : arm64_fused_subsi32_table,
                        &taken, &fallthrough);
                if (fused != NULL) {
                    gen(state, (unsigned long) fused);
                    gen(state, rd | ((uint64_t) rn << 8));
                    gen(state, imm);
                    gen(state, taken | 0x8000000000000000ULL);
                    state->jump_ip[0] = state->size - 1;
                    gen(state, fallthrough | 0x8000000000000000ULL);
                    state->jump_ip[1] = state->size - 1;
                    return 0;
                }
            }
            static void *const t[2][2][2] = { // [op_sub][S][sf]
                {{(void *) gadget_arm64_addi_fast32, (void *) gadget_arm64_addi_fast64},
                 {(void *) gadget_arm64_addsi_fast32, (void *) gadget_arm64_addsi_fast64}},
                {{(void *) gadget_arm64_subi_fast32, (void *) gadget_arm64_subi_fast64},
                 {(void *) gadget_arm64_subsi_fast32, (void *) gadget_arm64_subsi_fast64}}};
            gen(state, (unsigned long) t[op_sub][S][sf]);
            gen(state, rd | ((uint64_t) rn << 8));
            gen(state, imm);
            state->arm64_flags_live = S;
            return 1;
        }
        uint64_t params = rd | ((uint64_t) rn << 8) | ((uint64_t) imm12 << 16)
            | ((uint64_t) sf << 28) | ((uint64_t) S << 29) | ((uint64_t) sh << 30);
        gen(state, (unsigned long) (op_sub ? gadget_arm64_sub_imm : gadget_arm64_add_imm));
        gen(state, params);
        return 1;
    }

    // Logical (immediate): AND/ORR/EOR/ANDS — mask matches
    // arm64_execute()'s (bits[28:23]=100100). The bitmask immediate itself
    // (immN:imms:immr) is decoded once here at compile time via
    // arm64_decode_bitmask_imm (emu/arch/arm64/decode.h, shared with the
    // interpreter -- same "one implementation" discipline as the branch
    // sign-extension fix) and packed into the code stream as a plain
    // 64-bit value; logical.S's gadget never needs to reimplement
    // DecodeBitMasks in assembly. Params: rd | rn<<8 | opc<<16 | sf<<24,
    // then the decoded imm as a second 64-bit word.
    if ((insn & 0x1f800000) == 0x12000000) {
        extern void gadget_arm64_logical_imm(void);
        bool sf = (insn >> 31) & 1;
        unsigned opc = (insn >> 29) & 0x3;
        unsigned N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3f;
        unsigned imms = (insn >> 10) & 0x3f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        uint64_t imm;
        if ((!sf && N != 0) || !arm64_decode_bitmask_imm(N, imms, immr, sf, &imm)) {
            return gen_arm64_undefined(state);
        }
        if (opc == 3) { // ANDS: try single-gadget fusion with following B.cond
            extern void *const arm64_fused_andsi64_table[14];
            extern void *const arm64_fused_andsi32_table[14];
            uint64_t taken, fallthrough;
            void *fused = gen_arm64_peek_bcond(state, tlb,
                    sf ? arm64_fused_andsi64_table : arm64_fused_andsi32_table,
                    &taken, &fallthrough);
            if (fused != NULL) {
                gen(state, (unsigned long) fused);
                gen(state, rd | ((uint64_t) rn << 8));
                gen(state, imm);
                gen(state, taken | 0x8000000000000000ULL);
                state->jump_ip[0] = state->size - 1;
                gen(state, fallthrough | 0x8000000000000000ULL);
                state->jump_ip[1] = state->size - 1;
                return 0; // the fused pair ends the block
            }
        }
        gen(state, (unsigned long) gadget_arm64_logical_imm);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) opc << 16) | ((uint64_t) sf << 24));
        gen(state, imm);
        return 1;
    }

    // Logical (shifted register): AND/ORR/EOR/ANDS/BIC/ORN/EON/BICS,
    // including the "MOV (register)" alias (ORR, Rn=XZR, shift_amount=0).
    // Mask matches bits[28:24]=01010. Params packed to match
    // jit/guest-arm64/dpreg.S's logical_reg gadget (independently
    // written, see that file's header): rd | rn<<5 | rm<<10 |
    // shift_type<<15 | shift_amount<<17 | sf<<23 | opc<<24 | N<<26.
    if ((insn & 0x1f000000) == 0x0a000000) {
        extern void gadget_arm64_logical_reg(void);
        unsigned rd = insn & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned imm6 = (insn >> 10) & 0x3f;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned N = (insn >> 21) & 1;
        unsigned shift_type = (insn >> 22) & 0x3;
        unsigned opc = (insn >> 29) & 0x3;
        bool sf = (insn >> 31) & 1;
        if (!sf && (imm6 & 0x20)) {
            // 32-bit form only allows a 5-bit shift amount (bit5 of imm6
            // must be 0) -- unallocated otherwise.
            return gen_arm64_undefined(state);
        }
        // Fast paths: the MOV-register alias (ORR, Rn=ZR, shift 0 — the
        // hottest register-form instruction there is) and plain unshifted
        // AND/ORR/EOR/ANDS. Inverted (N=1) and shifted forms fall back.
        if (imm6 == 0 && N == 0 && rd != 31) {
            extern void gadget_arm64_movr_fast64(void), gadget_arm64_movr_fast32(void);
            extern void gadget_arm64_andr_fast64(void), gadget_arm64_andr_fast32(void);
            extern void gadget_arm64_orrr_fast64(void), gadget_arm64_orrr_fast32(void);
            extern void gadget_arm64_eorr_fast64(void), gadget_arm64_eorr_fast32(void);
            extern void gadget_arm64_andsr_fast64(void), gadget_arm64_andsr_fast32(void);
            if (opc == 1 && rn == 31 && rm != 31) { // MOV xd, xm
                gen(state, (unsigned long) (sf ? gadget_arm64_movr_fast64
                                               : gadget_arm64_movr_fast32));
                gen(state, rd | ((uint64_t) rm << 8));
                return 1;
            }
            if (rn != 31 && rm != 31) {
                if (opc == 3) { // ANDS: try single-gadget fusion with following B.cond
                    extern void *const arm64_fused_andsr64_table[14];
                    extern void *const arm64_fused_andsr32_table[14];
                    uint64_t taken, fallthrough;
                    void *fused = gen_arm64_peek_bcond(state, tlb,
                            sf ? arm64_fused_andsr64_table : arm64_fused_andsr32_table,
                            &taken, &fallthrough);
                    if (fused != NULL) {
                        gen(state, (unsigned long) fused);
                        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
                        gen(state, taken | 0x8000000000000000ULL);
                        state->jump_ip[0] = state->size - 1;
                        gen(state, fallthrough | 0x8000000000000000ULL);
                        state->jump_ip[1] = state->size - 1;
                        return 0; // the fused pair ends the block
                    }
                }
                static void *const t[4][2] = { // [opc][sf]
                    {(void *) gadget_arm64_andr_fast32, (void *) gadget_arm64_andr_fast64},
                    {(void *) gadget_arm64_orrr_fast32, (void *) gadget_arm64_orrr_fast64},
                    {(void *) gadget_arm64_eorr_fast32, (void *) gadget_arm64_eorr_fast64},
                    {(void *) gadget_arm64_andsr_fast32, (void *) gadget_arm64_andsr_fast64}};
                gen(state, (unsigned long) t[opc][sf]);
                gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
                state->arm64_flags_live = opc == 3; // ANDS
                return 1;
            }
        }
        // TST (ANDS XZR, Xn, Xm) register form: same fused_andsr gadgets
        // as the rd!=31 path above — they already handle rd=31 by skipping
        // the result store. TST+B.cond is common (bitmask/pointer-tag
        // checks) and currently falls through to the two-dispatch generic.
        if (imm6 == 0 && N == 0 && rd == 31 && opc == 3
                && rn != 31 && rm != 31) {
            extern void *const arm64_fused_andsr64_table[14];
            extern void *const arm64_fused_andsr32_table[14];
            uint64_t taken, fallthrough;
            void *fused = gen_arm64_peek_bcond(state, tlb,
                    sf ? arm64_fused_andsr64_table : arm64_fused_andsr32_table,
                    &taken, &fallthrough);
            if (fused != NULL) {
                gen(state, (unsigned long) fused);
                gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
                gen(state, taken | 0x8000000000000000ULL);
                state->jump_ip[0] = state->size - 1;
                gen(state, fallthrough | 0x8000000000000000ULL);
                state->jump_ip[1] = state->size - 1;
                return 0;
            }
        }
        uint64_t params = rd | ((uint64_t) rn << 5) | ((uint64_t) rm << 10)
            | ((uint64_t) shift_type << 15) | ((uint64_t) imm6 << 17)
            | ((uint64_t) sf << 23) | ((uint64_t) opc << 24) | ((uint64_t) N << 26);
        gen(state, (unsigned long) gadget_arm64_logical_reg);
        gen(state, params);
        return 1;
    }

    // Add/subtract (shifted register): ADD/SUB/ADDS/SUBS. Mask matches
    // bits[28:24]=01011 with bit21=0 (the extended-register variant has
    // bit21=1 and a different imm6-field meaning, not handled here).
    // Params packed to match dpreg.S's addsub_reg gadget: rd | rn<<5 |
    // rm<<10 | shift_type<<15 | shift_amount<<17 | sf<<23 | op<<24 |
    // S<<26.
    if ((insn & 0x1f200000) == 0x0b000000) {
        extern void gadget_arm64_addsub_reg(void);
        unsigned rd = insn & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned imm6 = (insn >> 10) & 0x3f;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned shift_type = (insn >> 22) & 0x3;
        bool S = (insn >> 29) & 1;
        bool op_sub = (insn >> 30) & 1;
        bool sf = (insn >> 31) & 1;
        if (shift_type == 3 || (!sf && (imm6 & 0x20))) {
            // shift_type=3 (ROR) is unallocated for add/subtract (only
            // logical allows ROR); 32-bit form only allows a 5-bit shift.
            return gen_arm64_undefined(state);
        }
        // Fast path: unshifted plain-register forms (the vast majority),
        // op/width/flags baked at compile time. rn/rm = 31 (ZR here)
        // falls back to generic; the CMP/CMN alias gets its own gadget.
        if (imm6 == 0 && rn != 31 && rm != 31 && (rd != 31 || S)) {
            extern void gadget_arm64_addr_fast64(void), gadget_arm64_addr_fast32(void);
            extern void gadget_arm64_subr_fast64(void), gadget_arm64_subr_fast32(void);
            extern void gadget_arm64_addsr_fast64(void), gadget_arm64_addsr_fast32(void);
            extern void gadget_arm64_subsr_fast64(void), gadget_arm64_subsr_fast32(void);
            extern void gadget_arm64_cmpr_fast64(void), gadget_arm64_cmpr_fast32(void);
            extern void gadget_arm64_cmnr_fast64(void), gadget_arm64_cmnr_fast32(void);
            if (S && rd == 31) {
                if (op_sub) { // CMP (register): try fusion
                    extern void *const arm64_fused_cmpr64_table[14];
                    extern void *const arm64_fused_cmpr32_table[14];
                    uint64_t taken, fallthrough;
                    void *fused = gen_arm64_peek_bcond(state, tlb,
                            sf ? arm64_fused_cmpr64_table : arm64_fused_cmpr32_table,
                            &taken, &fallthrough);
                    if (fused != NULL) {
                        gen(state, (unsigned long) fused);
                        gen(state, rn | ((uint64_t) rm << 8));
                        gen(state, taken | 0x8000000000000000ULL);
                        state->jump_ip[0] = state->size - 1;
                        gen(state, fallthrough | 0x8000000000000000ULL);
                        state->jump_ip[1] = state->size - 1;
                        return 0;
                    }
                }
                static void *const cmp_t[2][2] = { // [op_sub][sf]
                    {(void *) gadget_arm64_cmnr_fast32, (void *) gadget_arm64_cmnr_fast64},
                    {(void *) gadget_arm64_cmpr_fast32, (void *) gadget_arm64_cmpr_fast64}};
                gen(state, (unsigned long) cmp_t[op_sub][sf]);
                gen(state, rn | ((uint64_t) rm << 8));
                state->arm64_flags_live = true;
                return 1;
            }
            static void *const t[2][2][2] = { // [op_sub][S][sf]
                {{(void *) gadget_arm64_addr_fast32, (void *) gadget_arm64_addr_fast64},
                 {(void *) gadget_arm64_addsr_fast32, (void *) gadget_arm64_addsr_fast64}},
                {{(void *) gadget_arm64_subr_fast32, (void *) gadget_arm64_subr_fast64},
                 {(void *) gadget_arm64_subsr_fast32, (void *) gadget_arm64_subsr_fast64}}};
            gen(state, (unsigned long) t[op_sub][S][sf]);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
            state->arm64_flags_live = S;
            return 1;
        }
        uint64_t params = rd | ((uint64_t) rn << 5) | ((uint64_t) rm << 10)
            | ((uint64_t) shift_type << 15) | ((uint64_t) imm6 << 17)
            | ((uint64_t) sf << 23) | ((uint64_t) op_sub << 24) | ((uint64_t) S << 26);
        gen(state, (unsigned long) gadget_arm64_addsub_reg);
        gen(state, params);
        return 1;
    }

    // Add/subtract with carry: ADC/ADCS/SBC/SBCS (and the NGC/NGCS
    // aliases). Mask matches bits[28:21]=11010000 with bits[15:10]=0.
    if ((insn & 0x1fe0fc00) == 0x1a000000) {
        extern void gadget_arm64_adcsbc(void);
        unsigned rd = insn & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rm = (insn >> 16) & 0x1f;
        bool S = (insn >> 29) & 1;
        bool op_sbc = (insn >> 30) & 1;
        bool sf = (insn >> 31) & 1;
        gen(state, (unsigned long) gadget_arm64_adcsbc);
        gen(state, rd | ((uint64_t) rn << 5) | ((uint64_t) rm << 10)
            | ((uint64_t) sf << 15) | ((uint64_t) op_sbc << 16) | ((uint64_t) S << 17));
        return 1;
    }

    // Add/subtract (extended register): ADD/SUB/ADDS/SUBS with an
    // extended Rm operand — the SP-capable register form (bit21=1, which
    // the shifted-register decode above deliberately excludes). Mask
    // matches bits[28:24]=01011 with bit21=1; the `opt` field (bits
    // 23:22) must be 00 (anything else is unallocated).
    if ((insn & 0x1f200000) == 0x0b200000) {
        extern void gadget_arm64_addsub_ext(void);
        unsigned rd = insn & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned imm3 = (insn >> 10) & 0x7;
        unsigned option = (insn >> 13) & 0x7;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opt = (insn >> 22) & 0x3;
        bool S = (insn >> 29) & 1;
        bool op_sub = (insn >> 30) & 1;
        bool sf = (insn >> 31) & 1;
        if (opt != 0 || imm3 > 4) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_addsub_ext);
        gen(state, rd | ((uint64_t) rn << 5) | ((uint64_t) rm << 10)
            | ((uint64_t) option << 15) | ((uint64_t) imm3 << 18)
            | ((uint64_t) sf << 21) | ((uint64_t) op_sub << 22) | ((uint64_t) S << 23));
        return 1;
    }

    // Load/store pair (LDP/STP), GPR only (V=0) — mask matches
    // arm64_execute()'s (bits[29:25]=10100, V=bit26=0). Params packed to
    // match jit/guest-arm64/memory.S's ldp64/ldp32/stp64/stp32 gadgets
    // (independently written, see that file's header comment): rt |
    // rt2<<8 | rn<<16 | mode<<24, then the signed scaled offset as a
    // second 64-bit word.
    if ((insn & 0x3a000000) == 0x28000000 && ((insn >> 26) & 1) == 0) {
        unsigned opc = (insn >> 30) & 0x3;
        unsigned mode = (insn >> 23) & 0x7; // 0=non-temporal, 1=post, 2=offset, 3=pre
        bool is_load = (insn >> 22) & 1;
        int32_t imm7 = (insn >> 15) & 0x7f;
        if (imm7 & 0x40)
            imm7 |= ~0x7f; // sign-extend 7 bits
        unsigned rt2 = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        // opc: 00=32-bit, 10=64-bit, 01=LDPSW (load-only; su/busybox login
        // uses it). opc=01 with a store is unallocated.
        bool is_ldpsw = opc == 0b01;
        // mode 0 is LDNP/STNP (non-temporal pair): architecturally LDP/STP
        // in the offset form (no writeback) plus a cache hint we can't
        // model — lower it to plain offset-mode LDP/STP (optimized memcpy
        // uses it; it used to SIGILL here). No LDPSW non-temporal form
        // exists, so opc=01 stays unallocated there.
        if (mode == 0) {
            if (is_ldpsw) {
                return gen_arm64_undefined(state);
            }
            mode = 2; // offset addressing, no writeback
        }
        if ((opc == 0b11) || (is_ldpsw && !is_load)) {
            return gen_arm64_undefined(state);
        }
        bool sf = opc == 0b10;
        // LDPSW accesses 4-byte elements; the scaled offset uses 4 too.
        int64_t offset = (int64_t) imm7 * (sf ? 8 : 4);
        void *gadget;
        if (is_ldpsw) {
            extern void gadget_arm64_ldpsw(void);
            gadget = (void *) gadget_arm64_ldpsw;
        } else {
            gadget = sf
                ? (is_load ? (void *) gadget_arm64_ldp64 : (void *) gadget_arm64_stp64)
                : (is_load ? (void *) gadget_arm64_ldp32 : (void *) gadget_arm64_stp32);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rt | ((uint64_t) rt2 << 8) | ((uint64_t) rn << 16) | ((uint64_t) mode << 24));
        gen(state, (uint64_t) offset);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // ---- Single-register load/store families -----------------------------
    // Four guest encodings lower to the same generic gadget set in
    // jit/guest-arm64/memory.S (load8..load64 / loads8_32..loads32_64 /
    // store8..store64) via the shared parameter format documented there:
    // [rt | rn<<8 | mode<<16][offset word]. All the per-encoding decode
    // (scaled vs. unscaled immediates, addressing mode, sign-extension
    // width, register-offset extend/shift) happens here, once, at compile
    // time. gadget selection is a (size, opc) table lookup; opc semantics
    // per the ARM ARM: size<3: 0=store, 1=load, 2=LDRS-to-64, 3=LDRS-to-32;
    // size=3: 0=store, 1=load, 2=PRFM (unsigned-imm family only; a hint,
    // lowered to nothing), 3=unallocated.

    // Load/store register (unsigned immediate), GPR only. Mask covers ALL
    // of bits[29:24]=111001, including V=bit26=0 — deliberately stricter
    // than arm64_execute()'s (insn & 0x3b000000) mask, which leaves V
    // unmasked and therefore also matches SIMD loads/stores (a latent
    // interpreter misdecode, moot there since the interpreter is frozen,
    // but not one to replicate here: a SIMD LDR reaching a GPR gadget
    // would silently corrupt a general register).
    if ((insn & 0x3f000000) == 0x39000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        uint64_t imm12 = (insn >> 10) & 0xfff;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (size == 3 && opc == 2)
            return 1; // PRFM: a prefetch hint, architecturally allowed to do nothing
        void *gadget = gen_arm64_ldst_single_gadget(size, opc);
        if (gadget == NULL) {
            return gen_arm64_undefined(state);
        }
        if (opc == 1 && size >= 2 && rt != 31) {
            if (gen_arm64_try_ldst_fusion(state, tlb, size, rt, rn, imm12 << size))
                return 1; // fused -O0 idiom (see memory.S)
            if (gen_arm64_try_ld_cmp_fusion(state, tlb, size, rt, rn, imm12 << size))
                return 0; // fused load+cmp+b.cond ends the block
        }
        if (rt != 31) { // fast path: offset mode, precomputed slot offsets
            gen(state, (unsigned long) gen_arm64_ldst_single_fast_gadget(size, opc));
            gen(state, gen_arm64_reg_slot(rt) | (gen_arm64_reg_slot(rn) << 16));
        } else {
            gen(state, (unsigned long) gadget);
            gen(state, rt | ((uint64_t) rn << 8) | (0ULL << 16)); // mode 0: no writeback
        }
        gen(state, imm12 << size);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // Load/store register (imm9): LDUR/STUR (unscaled, bits[11:10]=00),
    // post-index (01), LDTR/STTR (10 — unprivileged; identical semantics
    // under emulation, so lowered to a plain mode-0 access), pre-index
    // (11). GPR only (bit26=0 is part of the mask).
    if ((insn & 0x3f200000) == 0x38000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        int64_t imm9 = arm64_sign_extend((insn >> 12) & 0x1ff, 9);
        unsigned insn_mode = (insn >> 10) & 0x3;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (size == 3 && opc == 2)
            return 1; // PRFUM: prefetch hint, lowered to nothing
        // instruction mode bits -> gadget mode: 00/10 -> 0 (no writeback),
        // 01 -> 1 (post-index), 11 -> 2 (pre-index)
        unsigned mode = insn_mode == 1 ? 1 : insn_mode == 3 ? 2 : 0;
        void *gadget = gen_arm64_ldst_single_gadget(size, opc);
        if (gadget == NULL) {
            return gen_arm64_undefined(state);
        }
        if (mode == 0 && rt != 31) { // LDUR/STUR/LDTR/STTR: same fast form
            gen(state, (unsigned long) gen_arm64_ldst_single_fast_gadget(size, opc));
            gen(state, gen_arm64_reg_slot(rt) | (gen_arm64_reg_slot(rn) << 16));
        } else {
            gen(state, (unsigned long) gadget);
            gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) mode << 16));
        }
        gen(state, (uint64_t) imm9);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // Load/store register (register offset), GPR only. mode 3: the gadget
    // resolves extend(Rm) << shift at runtime; option/shift are decoded
    // here. Valid options (ARM ARM): 010=UXTW, 011=LSL/UXTX, 110=SXTW,
    // 111=SXTX; anything else is unallocated.
    if ((insn & 0x3f200c00) == 0x38200800) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned option = (insn >> 13) & 0x7;
        unsigned S = (insn >> 12) & 1;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (size == 3 && opc == 2)
            return 1; // PRFM (register): prefetch hint, lowered to nothing
        void *gadget = gen_arm64_ldst_single_gadget(size, opc);
        if (gadget == NULL || (option & 0x2) == 0) {
            return gen_arm64_undefined(state);
        }
        unsigned shift = S ? size : 0;
        gen(state, (unsigned long) gadget);
        gen(state, rt | ((uint64_t) rn << 8) | (3ULL << 16));
        gen(state, rm | ((uint64_t) option << 8) | ((uint64_t) shift << 16));
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // LDR (literal): PC-relative, address fully known at compile time.
    // GPR only (V=bit26=0 in the mask). opc: 00=LDR Wt, 01=LDR Xt,
    // 10=LDRSW, 11=PRFM (hint, lowered to nothing).
    if ((insn & 0x3f000000) == 0x18000000) {
        extern void gadget_arm64_load_lit32(void), gadget_arm64_load_lit64(void);
        extern void gadget_arm64_load_lit_sw(void);
        unsigned opc = (insn >> 30) & 0x3;
        unsigned rt = insn & 0x1f;
        if (opc == 3)
            return 1; // PRFM (literal)
        int64_t offset = arm64_branch_imm19(insn); // same imm19*4 field as branches
        uint64_t addr = state->arm64_orig_ip + (uint64_t) offset;
        void *gadget = opc == 0 ? (void *) gadget_arm64_load_lit32
                     : opc == 1 ? (void *) gadget_arm64_load_lit64
                                : (void *) gadget_arm64_load_lit_sw;
        gen(state, (unsigned long) gadget);
        gen(state, rt);
        gen(state, addr);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // LDR (literal, SIMD&FP): PC-relative load into a V register, the V=1
    // counterpart of the GPR form above (same imm19*4 addressing). opc:
    // 00=LDR St (32-bit), 01=LDR Dt (64-bit), 10=LDR Qt (128-bit),
    // 11=unallocated (unlike the GPR family, there's no PRFM literal here).
    if ((insn & 0x3f000000) == 0x1c000000) {
        extern void gadget_arm64_vload_lit_s(void), gadget_arm64_vload_lit_d(void);
        extern void gadget_arm64_vload_lit_q(void);
        unsigned opc = (insn >> 30) & 0x3;
        unsigned rt = insn & 0x1f;
        if (opc == 3)
            return gen_arm64_undefined(state);
        int64_t offset = arm64_branch_imm19(insn); // same imm19*4 field as branches
        uint64_t addr = state->arm64_orig_ip + (uint64_t) offset;
        void *gadget = opc == 0 ? (void *) gadget_arm64_vload_lit_s
                     : opc == 1 ? (void *) gadget_arm64_vload_lit_d
                                : (void *) gadget_arm64_vload_lit_q;
        gen(state, (unsigned long) gadget);
        gen(state, rt);
        gen(state, addr);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // ---- SIMD/FP loads/stores and register constants (Phase D) -----------
    // The blocker-driven minimal subset for musl memset/memcpy and
    // compiler-generated struct zeroing — see jit/guest-arm64/simd.S.

    // SIMD/FP load/store (unsigned immediate): V=1 counterpart of the
    // integer family above. Access size is size:opc<1> (B/H/S/D/Q).
    if ((insn & 0x3f000000) == 0x3d000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        uint64_t imm12 = (insn >> 10) & 0xfff;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        bool is_load;
        int size_log2 = gen_arm64_vldst_size(size, opc, &is_load);
        void *gadget = size_log2 < 0 ? NULL
            : gen_arm64_vldst_single_gadget((unsigned) size_log2, is_load);
        if (gadget == NULL) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rt | ((uint64_t) rn << 8) | (0ULL << 16));
        gen(state, imm12 << size_log2); // scaled by the ACCESS size, incl. Q's 16
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // SIMD/FP load/store (imm9): unscaled/post-index/pre-index, V=1.
    if ((insn & 0x3f200000) == 0x3c000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        int64_t imm9 = arm64_sign_extend((insn >> 12) & 0x1ff, 9);
        unsigned insn_mode = (insn >> 10) & 0x3;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        unsigned mode = insn_mode == 1 ? 1 : insn_mode == 3 ? 2 : 0;
        bool is_load;
        int size_log2 = gen_arm64_vldst_size(size, opc, &is_load);
        void *gadget = size_log2 < 0 ? NULL
            : gen_arm64_vldst_single_gadget((unsigned) size_log2, is_load);
        if (gadget == NULL || insn_mode == 2 /* no LDTR/STTR in SIMD space */) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) mode << 16));
        gen(state, (uint64_t) imm9);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // SIMD/FP load/store (register offset), V=1.
    if ((insn & 0x3f200c00) == 0x3c200800) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned option = (insn >> 13) & 0x7;
        unsigned S = (insn >> 12) & 1;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        bool is_load;
        int size_log2 = gen_arm64_vldst_size(size, opc, &is_load);
        void *gadget = size_log2 < 0 ? NULL
            : gen_arm64_vldst_single_gadget((unsigned) size_log2, is_load);
        if (gadget == NULL || (option & 0x2) == 0) {
            return gen_arm64_undefined(state);
        }
        unsigned shift = S ? (unsigned) size_log2 : 0;
        gen(state, (unsigned long) gadget);
        gen(state, rt | ((uint64_t) rn << 8) | (3ULL << 16));
        gen(state, rm | ((uint64_t) option << 8) | ((uint64_t) shift << 16));
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // SIMD/FP load/store pair (LDP/STP, V=1): opc 00=S, 01=D, 10=Q.
    if ((insn & 0x3a000000) == 0x28000000 && ((insn >> 26) & 1) == 1) {
        extern void gadget_arm64_vldp32(void), gadget_arm64_vldp64(void);
        extern void gadget_arm64_vldp128(void);
        extern void gadget_arm64_vstp32(void), gadget_arm64_vstp64(void);
        extern void gadget_arm64_vstp128(void);
        unsigned opc = (insn >> 30) & 0x3;
        unsigned mode = (insn >> 23) & 0x7; // 0=non-temporal, 1=post, 2=offset, 3=pre
        bool is_load = (insn >> 22) & 1;
        int32_t imm7 = (insn >> 15) & 0x7f;
        if (imm7 & 0x40)
            imm7 |= ~0x7f;
        unsigned rt2 = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (opc > 2) {
            return gen_arm64_undefined(state);
        }
        // mode 0 is LDNP/STNP (non-temporal pair, S/D/Q forms): LDP/STP
        // offset form plus an unmodelable cache hint — same lowering as
        // the GPR branch above.
        if (mode == 0)
            mode = 2; // offset addressing, no writeback
        unsigned esize = 4u << opc; // bytes per element: 4/8/16
        int64_t offset = (int64_t) imm7 * esize;
        static void *const vldp_gadgets[3] = {
            (void *) gadget_arm64_vldp32, (void *) gadget_arm64_vldp64,
            (void *) gadget_arm64_vldp128,
        };
        static void *const vstp_gadgets[3] = {
            (void *) gadget_arm64_vstp32, (void *) gadget_arm64_vstp64,
            (void *) gadget_arm64_vstp128,
        };
        gen(state, (unsigned long) (is_load ? vldp_gadgets[opc] : vstp_gadgets[opc]));
        gen(state, rt | ((uint64_t) rt2 << 8) | ((uint64_t) rn << 16) | ((uint64_t) mode << 24));
        gen(state, (uint64_t) offset);
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // DUP (general register to all vector lanes) — musl memset's opener.
    if ((insn & 0xbfe0fc00) == 0x0e000c00) {
        extern void gadget_arm64_dup_gen(void);
        unsigned q = (insn >> 30) & 1;
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        // lowest set bit of imm5 selects the element size
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0 || (size == 3 && !q)) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_dup_gen);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) size << 16) | ((uint64_t) q << 19));
        return 1;
    }

    // UMOV/SMOV (vector element -> general register). All lowered to an
    // integer load from the V slot at a compile-time byte offset — see
    // simd.S's vext_to_gpr.
    if ((insn & 0xbfe0fc00) == 0x0e003c00 || (insn & 0xbfe0fc00) == 0x0e002c00) {
        extern void gadget_arm64_vext_to_gpr(void);
        bool is_smov = ((insn >> 11) & 0x7) == 0x5; // bits13:11: 111=UMOV, 101=SMOV
        unsigned q = (insn >> 30) & 1;
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned vn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        // UMOV: Q=0 allows B/H/S, Q=1 allows D only (the S-with-Q form is
        // MOV's alias space but unallocated as a distinct transfer).
        // SMOV: Q selects the GPR width; B/H valid either way, S needs Q=1.
        bool valid = size >= 0 &&
            (is_smov ? (size <= 1 || (size == 2 && q))
                     : (q ? size == 3 : size <= 2));
        if (!valid) {
            return gen_arm64_undefined(state);
        }
        unsigned byteoff = (imm5 >> (size + 1)) << size;
        unsigned sf = is_smov ? q : (size == 3);
        gen(state, (unsigned long) gadget_arm64_vext_to_gpr);
        gen(state, rd | ((uint64_t) vn << 8) | ((uint64_t) byteoff << 16)
            | ((uint64_t) size << 24) | ((uint64_t) is_smov << 26) | ((uint64_t) sf << 27));
        return 1;
    }

    // INS (general register -> vector element, preserving other lanes).
    if ((insn & 0xffe0fc00) == 0x4e001c00) {
        extern void gadget_arm64_vins_gpr(void);
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned vd = insn & 0x1f;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0) {
            return gen_arm64_undefined(state);
        }
        unsigned byteoff = (imm5 >> (size + 1)) << size;
        gen(state, (unsigned long) gadget_arm64_vins_gpr);
        gen(state, vd | ((uint64_t) rn << 8) | ((uint64_t) byteoff << 16)
            | ((uint64_t) size << 24));
        return 1;
    }

    // INS (element -> element, other lanes preserved) — OpenMinis L4494.
    if ((insn & 0xffe08400) == 0x6e000400) {
        extern void gadget_arm64_ins_elem(void);
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned imm4 = (insn >> 11) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0)
            return gen_arm64_undefined(state);
        unsigned i1 = imm5 >> (size + 1);
        unsigned i2 = imm4 >> size;
        gen(state, (unsigned long) gadget_arm64_ins_elem);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) size << 16)
            | ((uint64_t) i1 << 18) | ((uint64_t) i2 << 22));
        return 1;
    }

    // DUP (element -> all vector lanes) — OpenMinis L4420.
    if ((insn & 0xbfe0fc00) == 0x0e000400) {
        extern void gadget_arm64_dup_elem(void);
        unsigned q = (insn >> 30) & 1;
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0 || (size == 3 && !q))
            return gen_arm64_undefined(state);
        unsigned lane = imm5 >> (size + 1);
        gen(state, (unsigned long) gadget_arm64_dup_elem);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) size << 16)
            | ((uint64_t) lane << 18) | ((uint64_t) q << 22));
        return 1;
    }

    // DUP (element -> scalar, aka MOV Bd/Hd/Sd/Dd, Vn.T[i]) — L4457.
    if ((insn & 0xffe0fc00) == 0x5e000400) {
        extern void gadget_arm64_dup_elem_scalar(void);
        unsigned imm5 = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0)
            return gen_arm64_undefined(state);
        unsigned lane = imm5 >> (size + 1);
        gen(state, (unsigned long) gadget_arm64_dup_elem_scalar);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) size << 16)
            | ((uint64_t) lane << 18));
        return 1;
    }

    // EXT (byte-wise extract from a register pair) — OpenMinis L5920.
    if ((insn & 0xbfe08400) == 0x2e000000) {
        extern void gadget_arm64_ext(void);
        unsigned q = (insn >> 30) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned imm4 = (insn >> 11) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (!q && (imm4 & 8)) // 8B form: index must be < 8
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget_arm64_ext);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) imm4 << 24) | ((uint64_t) q << 28));
        return 1;
    }

    // TBL/TBX (table lookup, 1-4 consecutive table registers) — L5902.
    if ((insn & 0xbfe08c00) == 0x0e000000) {
        extern void gadget_arm64_vtbl(void), gadget_arm64_vtbx(void);
        unsigned q = (insn >> 30) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned len = (insn >> 13) & 3;
        bool is_tbx = (insn >> 12) & 1;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        gen(state, (unsigned long) (is_tbx ? gadget_arm64_vtbx : gadget_arm64_vtbl));
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) len << 24) | ((uint64_t) q << 26));
        return 1;
    }

    // FMOV, scalar general<->FP forms: same slot-integer-move lowering.
    // To-FP writes zero the rest of the V register (vmov_from_gpr);
    // to-general is a plain extract. The v.d[1] forms move the top half.
    if ((insn & 0xfffffc00) == 0x1e260000    // FMOV Wd, Sn
            || (insn & 0xfffffc00) == 0x9e660000   // FMOV Xd, Dn
            || (insn & 0xfffffc00) == 0x9eae0000) { // FMOV Xd, Vn.D[1]
        extern void gadget_arm64_vext_to_gpr(void);
        unsigned vn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        bool is_64 = (insn >> 31) & 1;
        unsigned byteoff = (insn & 0xfffffc00) == 0x9eae0000 ? 8 : 0;
        gen(state, (unsigned long) gadget_arm64_vext_to_gpr);
        gen(state, rd | ((uint64_t) vn << 8) | ((uint64_t) byteoff << 16)
            | ((uint64_t) (is_64 ? 3 : 2) << 24));
        return 1;
    }
    if ((insn & 0xfffffc00) == 0x1e270000    // FMOV Sd, Wn
            || (insn & 0xfffffc00) == 0x9e670000) { // FMOV Dd, Xn
        extern void gadget_arm64_vmov_from_gpr(void);
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned vd = insn & 0x1f;
        bool is_64 = (insn >> 31) & 1;
        gen(state, (unsigned long) gadget_arm64_vmov_from_gpr);
        gen(state, vd | ((uint64_t) rn << 8) | ((uint64_t) (is_64 ? 3 : 2) << 16));
        return 1;
    }
    if ((insn & 0xfffffc00) == 0x9eaf0000) { // FMOV Vd.D[1], Xn (preserves low half)
        extern void gadget_arm64_vins_gpr(void);
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned vd = insn & 0x1f;
        gen(state, (unsigned long) gadget_arm64_vins_gpr);
        gen(state, vd | ((uint64_t) rn << 8) | (8ULL << 16) | (3ULL << 24));
        return 1;
    }

    // ---- Scalar floating point (jit/guest-arm64/fp.S) ---------------------
    // S and D precision only; half (type=11) rejects. The exact FMOV
    // general<->FP matches above run first, so the FP<->integer family here
    // only sees the rounding conversions and SCVTF/UCVTF.

    // FP<->fixed-point conversions (bit21=0): SCVTF/UCVTF Fd, Rn, #fbits and
    // FCVTZS/FCVTZU Rd, Fn, #fbits (gcc emits these for integer<->scaled-
    // fixed arithmetic; tmux's server SIGILL'd on `scvtf d0, w0, #2`).
    // Lowered as the register-form conversion plus an fmul by 2^±fbits — the
    // constant is built here in the operand precision and rides the code
    // stream; see fp.S's cvtf_fix_gadget comment for the bit-exactness
    // argument. sf|0|0|11110|type|0|rmode|opcode|scale|rn|rd
    if ((insn & 0x5f200000) == 0x1e000000) {
        extern void gadget_arm64_scvtf_fix_ws(void), gadget_arm64_scvtf_fix_xs(void);
        extern void gadget_arm64_scvtf_fix_wd(void), gadget_arm64_scvtf_fix_xd(void);
        extern void gadget_arm64_ucvtf_fix_ws(void), gadget_arm64_ucvtf_fix_xs(void);
        extern void gadget_arm64_ucvtf_fix_wd(void), gadget_arm64_ucvtf_fix_xd(void);
        extern void gadget_arm64_fcvtzs_fix_sw(void), gadget_arm64_fcvtzs_fix_sx(void);
        extern void gadget_arm64_fcvtzs_fix_dw(void), gadget_arm64_fcvtzs_fix_dx(void);
        extern void gadget_arm64_fcvtzu_fix_sw(void), gadget_arm64_fcvtzu_fix_sx(void);
        extern void gadget_arm64_fcvtzu_fix_dw(void), gadget_arm64_fcvtzu_fix_dx(void);
        bool sf = (insn >> 31) & 1;
        unsigned type = (insn >> 22) & 0x3;
        unsigned rmode = (insn >> 19) & 0x3;
        unsigned opcode = (insn >> 16) & 0x7;
        unsigned fbits = 64 - ((insn >> 10) & 0x3f);
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        // op: 0=SCVTF 1=UCVTF (int->FP), 2=FCVTZS 3=FCVTZU (FP->int)
        int op = rmode == 0 && opcode == 2 ? 0
               : rmode == 0 && opcode == 3 ? 1
               : rmode == 3 && opcode == 0 ? 2
               : rmode == 3 && opcode == 1 ? 3 : -1;
        if (op < 0 || type > 1 || fbits > (sf ? 64u : 32u))
            return gen_arm64_undefined(state);
        bool is_d = type == 1;
        int exp = op <= 1 ? -(int) fbits : (int) fbits; // 2^-fbits in, 2^+fbits out
        uint64_t scale_bits = is_d ? (uint64_t) (1023 + exp) << 52
                                   : (uint64_t) (127 + exp) << 23;
        static void *const t[4][2][2] = { // [op][is_d][sf]
            {{(void *) gadget_arm64_scvtf_fix_ws, (void *) gadget_arm64_scvtf_fix_xs},
             {(void *) gadget_arm64_scvtf_fix_wd, (void *) gadget_arm64_scvtf_fix_xd}},
            {{(void *) gadget_arm64_ucvtf_fix_ws, (void *) gadget_arm64_ucvtf_fix_xs},
             {(void *) gadget_arm64_ucvtf_fix_wd, (void *) gadget_arm64_ucvtf_fix_xd}},
            {{(void *) gadget_arm64_fcvtzs_fix_sw, (void *) gadget_arm64_fcvtzs_fix_sx},
             {(void *) gadget_arm64_fcvtzs_fix_dw, (void *) gadget_arm64_fcvtzs_fix_dx}},
            {{(void *) gadget_arm64_fcvtzu_fix_sw, (void *) gadget_arm64_fcvtzu_fix_sx},
             {(void *) gadget_arm64_fcvtzu_fix_dw, (void *) gadget_arm64_fcvtzu_fix_dx}}};
        gen(state, (unsigned long) t[op][is_d][sf]);
        gen(state, rd | ((uint64_t) rn << 8));
        gen(state, scale_bits);
        return 1;
    }

    // FP<->integer conversions: sf|0|0|11110|type|1|rmode|opcode|000000|rn|rd
    if ((insn & 0x5f20fc00) == 0x1e200000) {
        extern void gadget_arm64_scvtf_ws(void), gadget_arm64_scvtf_xs(void);
        extern void gadget_arm64_scvtf_wd(void), gadget_arm64_scvtf_xd(void);
        extern void gadget_arm64_ucvtf_ws(void), gadget_arm64_ucvtf_xs(void);
        extern void gadget_arm64_ucvtf_wd(void), gadget_arm64_ucvtf_xd(void);
        extern void gadget_arm64_fcvtzs_sw(void), gadget_arm64_fcvtzs_sx(void);
        extern void gadget_arm64_fcvtzs_dw(void), gadget_arm64_fcvtzs_dx(void);
        extern void gadget_arm64_fcvtzu_sw(void), gadget_arm64_fcvtzu_sx(void);
        extern void gadget_arm64_fcvtzu_dw(void), gadget_arm64_fcvtzu_dx(void);
        extern void gadget_arm64_fcvtns_sw(void), gadget_arm64_fcvtns_sx(void);
        extern void gadget_arm64_fcvtns_dw(void), gadget_arm64_fcvtns_dx(void);
        extern void gadget_arm64_fcvtms_sw(void), gadget_arm64_fcvtms_sx(void);
        extern void gadget_arm64_fcvtms_dw(void), gadget_arm64_fcvtms_dx(void);
        extern void gadget_arm64_fcvtps_sw(void), gadget_arm64_fcvtps_sx(void);
        extern void gadget_arm64_fcvtps_dw(void), gadget_arm64_fcvtps_dx(void);
        extern void gadget_arm64_fcvtas_sw(void), gadget_arm64_fcvtas_sx(void);
        extern void gadget_arm64_fcvtas_dw(void), gadget_arm64_fcvtas_dx(void);
        extern void gadget_arm64_fcvtnu_sw(void), gadget_arm64_fcvtnu_sx(void);
        extern void gadget_arm64_fcvtnu_dw(void), gadget_arm64_fcvtnu_dx(void);
        extern void gadget_arm64_fcvtpu_sw(void), gadget_arm64_fcvtpu_sx(void);
        extern void gadget_arm64_fcvtpu_dw(void), gadget_arm64_fcvtpu_dx(void);
        extern void gadget_arm64_fcvtmu_sw(void), gadget_arm64_fcvtmu_sx(void);
        extern void gadget_arm64_fcvtmu_dw(void), gadget_arm64_fcvtmu_dx(void);
        extern void gadget_arm64_fcvtau_sw(void), gadget_arm64_fcvtau_sx(void);
        extern void gadget_arm64_fcvtau_dw(void), gadget_arm64_fcvtau_dx(void);
        bool sf = (insn >> 31) & 1;
        unsigned type = (insn >> 22) & 0x3;
        unsigned rmode = (insn >> 19) & 0x3;
        unsigned opcode = (insn >> 16) & 0x7;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        if (type <= 1) {
            bool is_d = type == 1;
            // idx: [is_d][sf] for the 4-way variants
            if (rmode == 0 && opcode == 2) { // SCVTF
                static void *const t[2][2] = {
                    {(void *) gadget_arm64_scvtf_ws, (void *) gadget_arm64_scvtf_xs},
                    {(void *) gadget_arm64_scvtf_wd, (void *) gadget_arm64_scvtf_xd}};
                gadget = t[is_d][sf];
            } else if (rmode == 0 && opcode == 3) { // UCVTF
                static void *const t[2][2] = {
                    {(void *) gadget_arm64_ucvtf_ws, (void *) gadget_arm64_ucvtf_xs},
                    {(void *) gadget_arm64_ucvtf_wd, (void *) gadget_arm64_ucvtf_xd}};
                gadget = t[is_d][sf];
            } else if (opcode == 0) { // FCVT{N,P,M,Z}S by rmode
                static void *const t[4][2][2] = {
                    {{(void *) gadget_arm64_fcvtns_sw, (void *) gadget_arm64_fcvtns_sx},
                     {(void *) gadget_arm64_fcvtns_dw, (void *) gadget_arm64_fcvtns_dx}},
                    {{(void *) gadget_arm64_fcvtps_sw, (void *) gadget_arm64_fcvtps_sx},
                     {(void *) gadget_arm64_fcvtps_dw, (void *) gadget_arm64_fcvtps_dx}},
                    {{(void *) gadget_arm64_fcvtms_sw, (void *) gadget_arm64_fcvtms_sx},
                     {(void *) gadget_arm64_fcvtms_dw, (void *) gadget_arm64_fcvtms_dx}},
                    {{(void *) gadget_arm64_fcvtzs_sw, (void *) gadget_arm64_fcvtzs_sx},
                     {(void *) gadget_arm64_fcvtzs_dw, (void *) gadget_arm64_fcvtzs_dx}}};
                gadget = t[rmode][is_d][sf];
            } else if (opcode == 1) { // FCVT{N,P,M,Z}U by rmode (rust: f64 as u64)
                static void *const t[4][2][2] = {
                    {{(void *) gadget_arm64_fcvtnu_sw, (void *) gadget_arm64_fcvtnu_sx},
                     {(void *) gadget_arm64_fcvtnu_dw, (void *) gadget_arm64_fcvtnu_dx}},
                    {{(void *) gadget_arm64_fcvtpu_sw, (void *) gadget_arm64_fcvtpu_sx},
                     {(void *) gadget_arm64_fcvtpu_dw, (void *) gadget_arm64_fcvtpu_dx}},
                    {{(void *) gadget_arm64_fcvtmu_sw, (void *) gadget_arm64_fcvtmu_sx},
                     {(void *) gadget_arm64_fcvtmu_dw, (void *) gadget_arm64_fcvtmu_dx}},
                    {{(void *) gadget_arm64_fcvtzu_sw, (void *) gadget_arm64_fcvtzu_sx},
                     {(void *) gadget_arm64_fcvtzu_dw, (void *) gadget_arm64_fcvtzu_dx}}};
                gadget = t[rmode][is_d][sf];
            } else if (rmode == 0 && opcode == 5) { // FCVTAU
                static void *const t[2][2] = {
                    {(void *) gadget_arm64_fcvtau_sw, (void *) gadget_arm64_fcvtau_sx},
                    {(void *) gadget_arm64_fcvtau_dw, (void *) gadget_arm64_fcvtau_dx}};
                gadget = t[is_d][sf];
            } else if (rmode == 0 && opcode == 4) { // FCVTAS
                static void *const t[2][2] = {
                    {(void *) gadget_arm64_fcvtas_sw, (void *) gadget_arm64_fcvtas_sx},
                    {(void *) gadget_arm64_fcvtas_dw, (void *) gadget_arm64_fcvtas_dx}};
                gadget = t[is_d][sf];
            }
        }
        if (gadget == NULL) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8));
        return 1;
    }

    // FP data-processing (1 source): FMOV/FABS/FNEG/FSQRT, FCVT S<->D,
    // FRINT*. opcode bits20:15.
    if ((insn & 0xff207c00) == 0x1e204000) {
        extern void gadget_arm64_fmov_s(void), gadget_arm64_fmov_d(void);
        extern void gadget_arm64_fabs_s(void), gadget_arm64_fabs_d(void);
        extern void gadget_arm64_fneg_s(void), gadget_arm64_fneg_d(void);
        extern void gadget_arm64_fsqrt_s(void), gadget_arm64_fsqrt_d(void);
        extern void gadget_arm64_fcvt_sd(void), gadget_arm64_fcvt_ds(void);
        extern void gadget_arm64_fcvt_sh(void), gadget_arm64_fcvt_hs(void);
        extern void gadget_arm64_fcvt_dh(void), gadget_arm64_fcvt_hd(void);
        extern void gadget_arm64_frintn_s(void), gadget_arm64_frintn_d(void);
        extern void gadget_arm64_frintp_s(void), gadget_arm64_frintp_d(void);
        extern void gadget_arm64_frintm_s(void), gadget_arm64_frintm_d(void);
        extern void gadget_arm64_frintz_s(void), gadget_arm64_frintz_d(void);
        extern void gadget_arm64_frinta_s(void), gadget_arm64_frinta_d(void);
        extern void gadget_arm64_frintx_s(void), gadget_arm64_frintx_d(void);
        extern void gadget_arm64_frinti_s(void), gadget_arm64_frinti_d(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned opcode = (insn >> 15) & 0x3f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        if (type <= 1) {
            bool is_d = type == 1;
            switch (opcode) {
            case 0x00: gadget = is_d ? (void *) gadget_arm64_fmov_d : (void *) gadget_arm64_fmov_s; break;
            case 0x01: gadget = is_d ? (void *) gadget_arm64_fabs_d : (void *) gadget_arm64_fabs_s; break;
            case 0x02: gadget = is_d ? (void *) gadget_arm64_fneg_d : (void *) gadget_arm64_fneg_s; break;
            case 0x03: gadget = is_d ? (void *) gadget_arm64_fsqrt_d : (void *) gadget_arm64_fsqrt_s; break;
            case 0x04: gadget = is_d ? (void *) gadget_arm64_fcvt_ds : NULL; break; // FCVT Sd, Dn
            case 0x05: gadget = is_d ? NULL : (void *) gadget_arm64_fcvt_sd; break; // FCVT Dd, Sn
            case 0x07: gadget = is_d ? (void *) gadget_arm64_fcvt_dh : (void *) gadget_arm64_fcvt_sh; break; // FCVT Hd, {D,S}n
            case 0x08: gadget = is_d ? (void *) gadget_arm64_frintn_d : (void *) gadget_arm64_frintn_s; break;
            case 0x09: gadget = is_d ? (void *) gadget_arm64_frintp_d : (void *) gadget_arm64_frintp_s; break;
            case 0x0a: gadget = is_d ? (void *) gadget_arm64_frintm_d : (void *) gadget_arm64_frintm_s; break;
            case 0x0b: gadget = is_d ? (void *) gadget_arm64_frintz_d : (void *) gadget_arm64_frintz_s; break;
            case 0x0c: gadget = is_d ? (void *) gadget_arm64_frinta_d : (void *) gadget_arm64_frinta_s; break;
            case 0x0e: gadget = is_d ? (void *) gadget_arm64_frintx_d : (void *) gadget_arm64_frintx_s; break;
            case 0x0f: gadget = is_d ? (void *) gadget_arm64_frinti_d : (void *) gadget_arm64_frinti_s; break;
            }
        } else if (type == 3) { // half-precision source: FCVT Sd/Dd, Hn
            switch (opcode) {
            case 0x04: gadget = (void *) gadget_arm64_fcvt_hs; break; // FCVT Sd, Hn
            case 0x05: gadget = (void *) gadget_arm64_fcvt_hd; break; // FCVT Dd, Hn
            }
        }
        if (gadget == NULL) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8));
        return 1;
    }

    // FP data-processing (2 source). opcode bits15:12.
    if ((insn & 0xff200c00) == 0x1e200800) {
        extern void gadget_arm64_fmul_s(void), gadget_arm64_fmul_d(void);
        extern void gadget_arm64_fdiv_s(void), gadget_arm64_fdiv_d(void);
        extern void gadget_arm64_fadd_s(void), gadget_arm64_fadd_d(void);
        extern void gadget_arm64_fsub_s(void), gadget_arm64_fsub_d(void);
        extern void gadget_arm64_fmax_s(void), gadget_arm64_fmax_d(void);
        extern void gadget_arm64_fmin_s(void), gadget_arm64_fmin_d(void);
        extern void gadget_arm64_fmaxnm_s(void), gadget_arm64_fmaxnm_d(void);
        extern void gadget_arm64_fminnm_s(void), gadget_arm64_fminnm_d(void);
        extern void gadget_arm64_fnmul_s(void), gadget_arm64_fnmul_d(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned opcode = (insn >> 12) & 0xf;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        static void *const t[9][2] = {
            {(void *) gadget_arm64_fmul_s, (void *) gadget_arm64_fmul_d},
            {(void *) gadget_arm64_fdiv_s, (void *) gadget_arm64_fdiv_d},
            {(void *) gadget_arm64_fadd_s, (void *) gadget_arm64_fadd_d},
            {(void *) gadget_arm64_fsub_s, (void *) gadget_arm64_fsub_d},
            {(void *) gadget_arm64_fmax_s, (void *) gadget_arm64_fmax_d},
            {(void *) gadget_arm64_fmin_s, (void *) gadget_arm64_fmin_d},
            {(void *) gadget_arm64_fmaxnm_s, (void *) gadget_arm64_fmaxnm_d},
            {(void *) gadget_arm64_fminnm_s, (void *) gadget_arm64_fminnm_d},
            {(void *) gadget_arm64_fnmul_s, (void *) gadget_arm64_fnmul_d},
        };
        if (type > 1 || opcode > 8) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) t[opcode][type]);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }

    // FCMP/FCMPE (register and #0.0 forms). FCMPE lowers to the quiet
    // compare — identical NZCV, no FP exception modeling (see fp.S).
    if ((insn & 0xff203c00) == 0x1e202000) {
        extern void gadget_arm64_fcmp_s(void), gadget_arm64_fcmp_d(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned opcode2 = insn & 0x1f;
        bool cmp0 = (opcode2 & 0x8) != 0;
        if (type > 1 || (opcode2 & 0x7) != 0) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) (type ? gadget_arm64_fcmp_d : gadget_arm64_fcmp_s));
        gen(state, rn | ((uint64_t) rm << 8) | ((uint64_t) cmp0 << 16));
        return 1;
    }

    // FCCMP/FCCMPE — same [cond gadget][consumer] pattern as CCMP.
    if ((insn & 0xff200c00) == 0x1e200400) {
        extern void gadget_arm64_fccmp(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned cond = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned nzcv = insn & 0xf;
        if (type > 1) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gen_arm64_cond_gadget(cond));
        gen(state, (unsigned long) gadget_arm64_fccmp);
        gen(state, rn | ((uint64_t) rm << 8) | ((uint64_t) type << 16) | ((uint64_t) nzcv << 17));
        return 1;
    }

    // FCSEL — same [cond gadget][consumer] pattern as CSEL.
    if ((insn & 0xff200c00) == 0x1e200c00) {
        extern void gadget_arm64_fcsel(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned cond = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (type > 1) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gen_arm64_cond_gadget(cond));
        gen(state, (unsigned long) gadget_arm64_fcsel);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }

    // FMOV (scalar immediate): VFPExpandImm == the AdvSIMD cmode=1111
    // expansion already implemented in gen_arm64_expand_imm; write the
    // constant into the low lane via the vconst gadget.
    if ((insn & 0xff201fe0) == 0x1e201000) {
        extern void gadget_arm64_vconst(void);
        unsigned type = (insn >> 22) & 0x3;
        uint64_t imm8 = (insn >> 13) & 0xff;
        unsigned rd = insn & 0x1f;
        uint64_t imm64;
        if (type > 1 || !gen_arm64_expand_imm(type == 1 ? 1 : 0, 15, imm8, &imm64)) {
            return gen_arm64_undefined(state);
        }
        if (type == 0)
            imm64 &= 0xffffffffu; // single: 32-bit constant in the low word
        gen(state, (unsigned long) gadget_arm64_vconst);
        gen(state, rd);
        gen(state, imm64);
        gen(state, 0);
        return 1;
    }

    // FP data-processing (3 source): FMADD/FMSUB/FNMADD/FNMSUB.
    if ((insn & 0xff000000) == 0x1f000000) {
        extern void gadget_arm64_fmadd_s(void), gadget_arm64_fmadd_d(void);
        extern void gadget_arm64_fmsub_s(void), gadget_arm64_fmsub_d(void);
        extern void gadget_arm64_fnmadd_s(void), gadget_arm64_fnmadd_d(void);
        extern void gadget_arm64_fnmsub_s(void), gadget_arm64_fnmsub_d(void);
        unsigned type = (insn >> 22) & 0x3;
        unsigned o1 = (insn >> 21) & 1;
        unsigned o0 = (insn >> 15) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned ra = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        static void *const t[2][2][2] = { // [o1][o0][type]
            {{(void *) gadget_arm64_fmadd_s, (void *) gadget_arm64_fmadd_d},
             {(void *) gadget_arm64_fmsub_s, (void *) gadget_arm64_fmsub_d}},
            {{(void *) gadget_arm64_fnmadd_s, (void *) gadget_arm64_fnmadd_d},
             {(void *) gadget_arm64_fnmsub_s, (void *) gadget_arm64_fnmsub_d}},
        };
        if (type > 1) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) t[o1][o0][type]);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) | ((uint64_t) ra << 24));
        return 1;
    }

    // AdvSIMD load/store multiple structures (contiguous LD1/ST1 only —
    // musl's getcwd/string ops use them, e.g. `ld1 {v30.16b,v31.16b},[x2]`
    // crashed `cd /AOK` on-device). The interleaving LD2/3/4 opcodes are
    // NOT handled (different semantics) and fall through to SIGILL.
    // No-offset form: bits[31,29:23]=0,0011000. Post-index: bit23=1.
    if ((insn & 0xbfbf0000) == 0x0c000000 || (insn & 0xbfa00000) == 0x0c800000) {
        extern void gadget_arm64_ld1st1_multi(void);
        bool post = (insn >> 23) & 1;
        unsigned q = (insn >> 30) & 1;
        bool is_load = (insn >> 22) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        // opcode -> register count for the contiguous LD1/ST1 forms.
        unsigned count = opcode == 0x7 ? 1 : opcode == 0xa ? 2
                       : opcode == 0x6 ? 3 : opcode == 0x2 ? 4 : 0;
        // mode: 0=none, 1=post-index by total (Rm==31), 2=post-index by Xrm
        unsigned mode = !post ? 0 : (rm == 31 ? 1 : 2);
        if (count != 0) {
            gen(state, (unsigned long) gadget_arm64_ld1st1_multi);
            gen(state, rt | ((uint64_t) rn << 5) | ((uint64_t) count << 10)
                | ((uint64_t) q << 13) | ((uint64_t) is_load << 14)
                | ((uint64_t) mode << 15) | ((uint64_t) rm << 17));
            gen(state, state->arm64_orig_ip); // fault-restart address
            return 1;
        }
        // Interleaved LD2/ST2 (opcode 8), LD3/ST3 (4), LD4/ST4 (0):
        // element-interleaving via the arm64_vldst_struct helper (ported
        // functionality from OpenMinis' interleaved branch, buffered
        // instead of per-element micro-gadgets).
        extern void gadget_arm64_ldst_struct(void);
        unsigned size = (insn >> 10) & 3;
        count = opcode == 0x8 ? 2 : opcode == 0x4 ? 3 : opcode == 0x0 ? 4 : 0;
        if (count == 0 || (size == 3 && q == 0)) // .1d interleaved reserved
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget_arm64_ldst_struct);
        gen(state, rt | ((uint64_t) rn << 5) | ((uint64_t) count << 10)
            | ((uint64_t) q << 13) | ((uint64_t) is_load << 14)
            | ((uint64_t) mode << 15) | ((uint64_t) rm << 17)
            | (1ULL << 22) | ((uint64_t) size << 24));
        gen(state, state->arm64_orig_ip);
        return 1;
    }

    // AdvSIMD load/store single structure (LD1-4/ST1-4 lane, LD1R-LD4R
    // replicate), no-offset and post-indexed. Field derivation (esize,
    // lane, register count) ported from OpenMinis' 0x0d000000 branch.
    if ((insn & 0xbf000000) == 0x0d000000) {
        extern void gadget_arm64_ldst_struct(void);
        unsigned q = (insn >> 30) & 1;
        bool post = (insn >> 23) & 1;
        bool is_load = (insn >> 22) & 1;
        unsigned r = (insn >> 21) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 13) & 7;
        unsigned s = (insn >> 12) & 1;
        unsigned size = (insn >> 10) & 3;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        unsigned count = ((opcode & 1) << 1 | r) + 1;
        unsigned mode = !post ? 0 : (rm == 31 ? 1 : 2);
        unsigned esize_log2, lane, kind;
        if (opcode >= 6) {
            // LD1R..LD4R: replicate (must be a load, S must be 0)
            if (!is_load || s)
                return gen_arm64_undefined(state);
            count = ((opcode & 1) << 1 | r) + 1;
            esize_log2 = size;
            lane = 0;
            kind = 3;
        } else {
            unsigned scale = opcode >> 1;
            if (scale == 0) {          // B
                esize_log2 = 0;
                lane = (q << 3) | (s << 2) | size;
            } else if (scale == 1) {   // H
                if (size & 1)
                    return gen_arm64_undefined(state);
                esize_log2 = 1;
                lane = (q << 2) | (s << 1) | (size >> 1);
            } else if (size == 0) {    // S
                esize_log2 = 2;
                lane = (q << 1) | s;
            } else if (size == 1 && !s) { // D
                esize_log2 = 3;
                lane = q;
            } else {
                return gen_arm64_undefined(state);
            }
            kind = 2;
        }
        gen(state, (unsigned long) gadget_arm64_ldst_struct);
        gen(state, rt | ((uint64_t) rn << 5) | ((uint64_t) count << 10)
            | ((uint64_t) q << 13) | ((uint64_t) is_load << 14)
            | ((uint64_t) mode << 15) | ((uint64_t) rm << 17)
            | ((uint64_t) kind << 22) | ((uint64_t) esize_log2 << 24)
            | ((uint64_t) lane << 26));
        gen(state, state->arm64_orig_ip);
        return 1;
    }

    // AdvSIMD vector shift by immediate — the FULL opcode space (ported
    // against OpenMinis' per-op branches, unified): SSHR/USHR, SSRA/USRA,
    // SRSHR/URSHR, SRSRA/URSRA, SRI/SLI, SQSHL/UQSHL #imm, SHL,
    // SHRN/RSHRN + the saturating narrows, SSHLL/USHLL, and the
    // fixed-point converts. Host shift-immediates can't take runtime
    // amounts, so everything lowers to broadcast register-form shifts
    // (negative = right); see simd_shift.S. SQSHLU (U=1, opcode 0x0c) has
    // no register form and stays UNDEFINED (OpenMinis lacks it too).
    // Modified-immediate (MOVI) shares this space with immh=0 and is
    // matched by its own earlier check.
    if ((insn & 0x9f800400) == 0x0f000400 && ((insn >> 19) & 0xf) != 0) {
        extern void gadget_arm64_vshl_s_8b(void), gadget_arm64_vshl_s_16b(void);
        extern void gadget_arm64_vshl_s_4h(void), gadget_arm64_vshl_s_8h(void);
        extern void gadget_arm64_vshl_s_2s(void), gadget_arm64_vshl_s_4s(void);
        extern void gadget_arm64_vshl_s_2d(void);
        extern void gadget_arm64_vshl_u_8b(void), gadget_arm64_vshl_u_16b(void);
        extern void gadget_arm64_vshl_u_4h(void), gadget_arm64_vshl_u_8h(void);
        extern void gadget_arm64_vshl_u_2s(void), gadget_arm64_vshl_u_4s(void);
        extern void gadget_arm64_vshl_u_2d(void);
        extern void gadget_arm64_vrshl_s_8b(void), gadget_arm64_vrshl_s_16b(void);
        extern void gadget_arm64_vrshl_s_4h(void), gadget_arm64_vrshl_s_8h(void);
        extern void gadget_arm64_vrshl_s_2s(void), gadget_arm64_vrshl_s_4s(void);
        extern void gadget_arm64_vrshl_s_2d(void);
        extern void gadget_arm64_vrshl_u_8b(void), gadget_arm64_vrshl_u_16b(void);
        extern void gadget_arm64_vrshl_u_4h(void), gadget_arm64_vrshl_u_8h(void);
        extern void gadget_arm64_vrshl_u_2s(void), gadget_arm64_vrshl_u_4s(void);
        extern void gadget_arm64_vrshl_u_2d(void);
        extern void gadget_arm64_vsra_s_8b(void), gadget_arm64_vsra_s_16b(void);
        extern void gadget_arm64_vsra_s_4h(void), gadget_arm64_vsra_s_8h(void);
        extern void gadget_arm64_vsra_s_2s(void), gadget_arm64_vsra_s_4s(void);
        extern void gadget_arm64_vsra_s_2d(void);
        extern void gadget_arm64_vsra_u_8b(void), gadget_arm64_vsra_u_16b(void);
        extern void gadget_arm64_vsra_u_4h(void), gadget_arm64_vsra_u_8h(void);
        extern void gadget_arm64_vsra_u_2s(void), gadget_arm64_vsra_u_4s(void);
        extern void gadget_arm64_vsra_u_2d(void);
        extern void gadget_arm64_vrsra_s_8b(void), gadget_arm64_vrsra_s_16b(void);
        extern void gadget_arm64_vrsra_s_4h(void), gadget_arm64_vrsra_s_8h(void);
        extern void gadget_arm64_vrsra_s_2s(void), gadget_arm64_vrsra_s_4s(void);
        extern void gadget_arm64_vrsra_s_2d(void);
        extern void gadget_arm64_vrsra_u_8b(void), gadget_arm64_vrsra_u_16b(void);
        extern void gadget_arm64_vrsra_u_4h(void), gadget_arm64_vrsra_u_8h(void);
        extern void gadget_arm64_vrsra_u_2s(void), gadget_arm64_vrsra_u_4s(void);
        extern void gadget_arm64_vrsra_u_2d(void);
        extern void gadget_arm64_vqshli_s_8b(void), gadget_arm64_vqshli_s_16b(void);
        extern void gadget_arm64_vqshli_s_4h(void), gadget_arm64_vqshli_s_8h(void);
        extern void gadget_arm64_vqshli_s_2s(void), gadget_arm64_vqshli_s_4s(void);
        extern void gadget_arm64_vqshli_s_2d(void);
        extern void gadget_arm64_vqshli_u_8b(void), gadget_arm64_vqshli_u_16b(void);
        extern void gadget_arm64_vqshli_u_4h(void), gadget_arm64_vqshli_u_8h(void);
        extern void gadget_arm64_vqshli_u_2s(void), gadget_arm64_vqshli_u_4s(void);
        extern void gadget_arm64_vqshli_u_2d(void);
        extern void gadget_arm64_vsli_8b(void), gadget_arm64_vsli_16b(void);
        extern void gadget_arm64_vsli_4h(void), gadget_arm64_vsli_8h(void);
        extern void gadget_arm64_vsli_2s(void), gadget_arm64_vsli_4s(void);
        extern void gadget_arm64_vsli_2d(void);
        extern void gadget_arm64_vsri_8b(void), gadget_arm64_vsri_16b(void);
        extern void gadget_arm64_vsri_4h(void), gadget_arm64_vsri_8h(void);
        extern void gadget_arm64_vsri_2s(void), gadget_arm64_vsri_4s(void);
        extern void gadget_arm64_vsri_2d(void);
        extern void gadget_arm64_vshll_s_8b(void), gadget_arm64_vshll_s2_16b(void);
        extern void gadget_arm64_vshll_s_4h(void), gadget_arm64_vshll_s2_8h(void);
        extern void gadget_arm64_vshll_s_2s(void), gadget_arm64_vshll_s2_4s(void);
        extern void gadget_arm64_vshll_u_8b(void), gadget_arm64_vshll_u2_16b(void);
        extern void gadget_arm64_vshll_u_4h(void), gadget_arm64_vshll_u2_8h(void);
        extern void gadget_arm64_vshll_u_2s(void), gadget_arm64_vshll_u2_4s(void);
        extern void gadget_arm64_vshrn(void), gadget_arm64_vrshrn(void);
        extern void gadget_arm64_vsqshrn(void), gadget_arm64_vsqrshrn(void);
        extern void gadget_arm64_vuqshrn(void), gadget_arm64_vuqrshrn(void);
        extern void gadget_arm64_vsqshrun(void), gadget_arm64_vsqrshrun(void);
        extern void gadget_arm64_vscvtf_fix(void), gadget_arm64_vucvtf_fix(void);
        extern void gadget_arm64_vfcvtzs_fix(void), gadget_arm64_vfcvtzu_fix(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned immhb = (insn >> 16) & 0x7f;
        unsigned immh = immhb >> 3;
        unsigned opcode = (insn >> 11) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int esize_log2 = immh & 8 ? 3 : immh & 4 ? 2 : immh & 2 ? 1 : 0;
        unsigned esize = 8u << esize_log2;
        unsigned rshift = 2 * esize - immhb; // right-shift amount
        unsigned lshift = immhb - esize;     // left-shift amount
        // [esize_log2][q] gadget tables
        static void *const t_shl_s[4][2] = {
            {gadget_arm64_vshl_s_8b, gadget_arm64_vshl_s_16b},
            {gadget_arm64_vshl_s_4h, gadget_arm64_vshl_s_8h},
            {gadget_arm64_vshl_s_2s, gadget_arm64_vshl_s_4s},
            {NULL, gadget_arm64_vshl_s_2d}};
        static void *const t_shl_u[4][2] = {
            {gadget_arm64_vshl_u_8b, gadget_arm64_vshl_u_16b},
            {gadget_arm64_vshl_u_4h, gadget_arm64_vshl_u_8h},
            {gadget_arm64_vshl_u_2s, gadget_arm64_vshl_u_4s},
            {NULL, gadget_arm64_vshl_u_2d}};
        static void *const t_rshl_s[4][2] = {
            {gadget_arm64_vrshl_s_8b, gadget_arm64_vrshl_s_16b},
            {gadget_arm64_vrshl_s_4h, gadget_arm64_vrshl_s_8h},
            {gadget_arm64_vrshl_s_2s, gadget_arm64_vrshl_s_4s},
            {NULL, gadget_arm64_vrshl_s_2d}};
        static void *const t_rshl_u[4][2] = {
            {gadget_arm64_vrshl_u_8b, gadget_arm64_vrshl_u_16b},
            {gadget_arm64_vrshl_u_4h, gadget_arm64_vrshl_u_8h},
            {gadget_arm64_vrshl_u_2s, gadget_arm64_vrshl_u_4s},
            {NULL, gadget_arm64_vrshl_u_2d}};
        static void *const t_sra_s[4][2] = {
            {gadget_arm64_vsra_s_8b, gadget_arm64_vsra_s_16b},
            {gadget_arm64_vsra_s_4h, gadget_arm64_vsra_s_8h},
            {gadget_arm64_vsra_s_2s, gadget_arm64_vsra_s_4s},
            {NULL, gadget_arm64_vsra_s_2d}};
        static void *const t_sra_u[4][2] = {
            {gadget_arm64_vsra_u_8b, gadget_arm64_vsra_u_16b},
            {gadget_arm64_vsra_u_4h, gadget_arm64_vsra_u_8h},
            {gadget_arm64_vsra_u_2s, gadget_arm64_vsra_u_4s},
            {NULL, gadget_arm64_vsra_u_2d}};
        static void *const t_rsra_s[4][2] = {
            {gadget_arm64_vrsra_s_8b, gadget_arm64_vrsra_s_16b},
            {gadget_arm64_vrsra_s_4h, gadget_arm64_vrsra_s_8h},
            {gadget_arm64_vrsra_s_2s, gadget_arm64_vrsra_s_4s},
            {NULL, gadget_arm64_vrsra_s_2d}};
        static void *const t_rsra_u[4][2] = {
            {gadget_arm64_vrsra_u_8b, gadget_arm64_vrsra_u_16b},
            {gadget_arm64_vrsra_u_4h, gadget_arm64_vrsra_u_8h},
            {gadget_arm64_vrsra_u_2s, gadget_arm64_vrsra_u_4s},
            {NULL, gadget_arm64_vrsra_u_2d}};
        static void *const t_qshl_s[4][2] = {
            {gadget_arm64_vqshli_s_8b, gadget_arm64_vqshli_s_16b},
            {gadget_arm64_vqshli_s_4h, gadget_arm64_vqshli_s_8h},
            {gadget_arm64_vqshli_s_2s, gadget_arm64_vqshli_s_4s},
            {NULL, gadget_arm64_vqshli_s_2d}};
        static void *const t_qshl_u[4][2] = {
            {gadget_arm64_vqshli_u_8b, gadget_arm64_vqshli_u_16b},
            {gadget_arm64_vqshli_u_4h, gadget_arm64_vqshli_u_8h},
            {gadget_arm64_vqshli_u_2s, gadget_arm64_vqshli_u_4s},
            {NULL, gadget_arm64_vqshli_u_2d}};
        static void *const t_sli[4][2] = {
            {gadget_arm64_vsli_8b, gadget_arm64_vsli_16b},
            {gadget_arm64_vsli_4h, gadget_arm64_vsli_8h},
            {gadget_arm64_vsli_2s, gadget_arm64_vsli_4s},
            {NULL, gadget_arm64_vsli_2d}};
        static void *const t_sri[4][2] = {
            {gadget_arm64_vsri_8b, gadget_arm64_vsri_16b},
            {gadget_arm64_vsri_4h, gadget_arm64_vsri_8h},
            {gadget_arm64_vsri_2s, gadget_arm64_vsri_4s},
            {NULL, gadget_arm64_vsri_2d}};
        static void *const t_shll_s[3][2] = {
            {gadget_arm64_vshll_s_8b, gadget_arm64_vshll_s2_16b},
            {gadget_arm64_vshll_s_4h, gadget_arm64_vshll_s2_8h},
            {gadget_arm64_vshll_s_2s, gadget_arm64_vshll_s2_4s}};
        static void *const t_shll_u[3][2] = {
            {gadget_arm64_vshll_u_8b, gadget_arm64_vshll_u2_16b},
            {gadget_arm64_vshll_u_4h, gadget_arm64_vshll_u2_8h},
            {gadget_arm64_vshll_u_2s, gadget_arm64_vshll_u2_4s}};
        static void *const t_shrn[2][4] = { // [u][opcode-0x10]
            {gadget_arm64_vshrn, gadget_arm64_vrshrn,
             gadget_arm64_vsqshrn, gadget_arm64_vsqrshrn},
            {gadget_arm64_vsqshrun, gadget_arm64_vsqrshrun,
             gadget_arm64_vuqshrn, gadget_arm64_vuqrshrn}};
        void *gadget = NULL;
        int64_t amount = 0;
        uint64_t mask = 0;
        int extra = 0; // 1: emit mask word too (SLI/SRI)
        switch (opcode) {
        case 0x00: // SSHR/USHR
            gadget = (u ? t_shl_u : t_shl_s)[esize_log2][q];
            amount = -(int64_t) rshift;
            break;
        case 0x02: // SSRA/USRA
            gadget = (u ? t_sra_u : t_sra_s)[esize_log2][q];
            amount = -(int64_t) rshift;
            break;
        case 0x04: // SRSHR/URSHR
            gadget = (u ? t_rshl_u : t_rshl_s)[esize_log2][q];
            amount = -(int64_t) rshift;
            break;
        case 0x06: // SRSRA/URSRA
            gadget = (u ? t_rsra_u : t_rsra_s)[esize_log2][q];
            amount = -(int64_t) rshift;
            break;
        case 0x08: // SRI (U=1 only)
            if (u) {
                gadget = t_sri[esize_log2][q];
                amount = -(int64_t) rshift;
                // per-lane bits that take the shifted source, repeated to 64
                uint64_t lane = rshift >= esize ? 0
                    : (esize == 64 ? ~0ull >> rshift
                                   : ((1ull << (esize - rshift)) - 1));
                for (unsigned p = 0; p < 64; p += esize)
                    mask |= lane << p;
                extra = 1;
            }
            break;
        case 0x0a: // SHL (U=0) / SLI (U=1)
            if (!u) {
                gadget = t_shl_u[esize_log2][q]; // same for s/u
                amount = (int64_t) lshift;
            } else {
                gadget = t_sli[esize_log2][q];
                amount = (int64_t) lshift;
                uint64_t elem_mask = esize == 64 ? ~0ull : (1ull << esize) - 1;
                uint64_t lane = (elem_mask << lshift) & elem_mask;
                for (unsigned p = 0; p < 64; p += esize)
                    mask |= lane << p;
                extra = 1;
            }
            break;
        case 0x0e: // SQSHL/UQSHL #imm
            gadget = (u ? t_qshl_u : t_qshl_s)[esize_log2][q];
            amount = (int64_t) lshift;
            break;
        case 0x10: case 0x11: case 0x12: case 0x13: { // narrowing shifts
            if (esize_log2 >= 3)
                return gen_arm64_undefined(state);
            gadget = t_shrn[u][opcode - 0x10];
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) |
                       ((uint64_t) esize_log2 << 16) | ((uint64_t) q << 18));
            gen(state, (uint64_t) -(int64_t) rshift);
            return 1;
        }
        case 0x14: // SSHLL/USHLL
            if (esize_log2 < 3) {
                gadget = (u ? t_shll_u : t_shll_s)[esize_log2][q];
                amount = (int64_t) lshift;
            }
            break;
        case 0x1c: case 0x1f: { // fixed-point converts
            extern void gadget_arm64_vscvtf_fix(void), gadget_arm64_vucvtf_fix(void);
            if (esize_log2 < 2) // FP16 forms unsupported
                return gen_arm64_undefined(state);
            unsigned sz = esize_log2 == 3;
            if (sz && !q)
                return gen_arm64_undefined(state);
            unsigned fbits = rshift;
            uint64_t scale;
            if (opcode == 0x1c) { // SCVTF/UCVTF: multiply by 2^-fbits after
                gadget = u ? (void *) gadget_arm64_vucvtf_fix
                           : (void *) gadget_arm64_vscvtf_fix;
                scale = sz ? (uint64_t) (1023 - fbits) << 52
                           : (uint64_t) ((127 - fbits) << 23);
            } else {              // FCVTZS/FCVTZU: multiply by 2^+fbits first
                gadget = u ? (void *) gadget_arm64_vfcvtzu_fix
                           : (void *) gadget_arm64_vfcvtzs_fix;
                scale = sz ? (uint64_t) (1023 + fbits) << 52
                           : (uint64_t) ((127 + fbits) << 23);
            }
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) |
                       ((uint64_t) sz << 16) | ((uint64_t) q << 18));
            gen(state, scale);
            return 1;
        }
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8));
        gen(state, (uint64_t) amount);
        if (extra)
            gen(state, mask);
        return 1;
    }

    // AdvSIMD by-element (indexed) multiplies, vector: ported from
    // OpenMinis' 0x0f000000 branch (elem_setup/elem_op_store). The element
    // is read from the V file at a compile-time byte offset, dup'd, and
    // run through the plain three-same op (simd_shift.S).
    if ((insn & 0x9f000400) == 0x0f000000) {
        extern void gadget_arm64_velem_mul(void), gadget_arm64_velem_mla(void);
        extern void gadget_arm64_velem_mls(void);
        extern void gadget_arm64_velem_sqdmulh(void), gadget_arm64_velem_sqrdmulh(void);
        extern void gadget_arm64_velem_smull(void), gadget_arm64_velem_umull(void);
        extern void gadget_arm64_velem_smlal(void), gadget_arm64_velem_umlal(void);
        extern void gadget_arm64_velem_smlsl(void), gadget_arm64_velem_umlsl(void);
        extern void gadget_arm64_velem_sqdmull(void), gadget_arm64_velem_sqdmlal(void);
        extern void gadget_arm64_velem_sqdmlsl(void);
        extern void gadget_arm64_velem_fmul(void), gadget_arm64_velem_fmulx(void);
        extern void gadget_arm64_velem_fmla(void), gadget_arm64_velem_fmls(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned l = (insn >> 21) & 1;
        unsigned m = (insn >> 20) & 1;
        unsigned rm4 = (insn >> 16) & 0xf;
        unsigned opcode = (insn >> 12) & 0xf;
        unsigned h = (insn >> 11) & 1;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        unsigned rm, index;
        if (size == 1) {        // H
            index = (h << 2) | (l << 1) | m;
            rm = rm4;
        } else if (size == 2) { // S
            index = (h << 1) | l;
            rm = (m << 4) | rm4;
        } else if (size == 3) { // D (FP only)
            index = h;
            rm = (m << 4) | rm4;
        } else {
            return gen_arm64_undefined(state);
        }
        unsigned byteoff = index << (size == 1 ? 1 : size == 2 ? 2 : 3);
        void *gadget = NULL;
        bool is_fp = false;
        switch ((u << 4) | opcode) {
        case 0x08: gadget = gadget_arm64_velem_mul; break;
        case 0x10: gadget = gadget_arm64_velem_mla; break;
        case 0x14: gadget = gadget_arm64_velem_mls; break;
        case 0x0c: gadget = gadget_arm64_velem_sqdmulh; break;
        case 0x0d: gadget = gadget_arm64_velem_sqrdmulh; break;
        case 0x0a: gadget = gadget_arm64_velem_smull; break;
        case 0x1a: gadget = gadget_arm64_velem_umull; break;
        case 0x02: gadget = gadget_arm64_velem_smlal; break;
        case 0x12: gadget = gadget_arm64_velem_umlal; break;
        case 0x06: gadget = gadget_arm64_velem_smlsl; break;
        case 0x16: gadget = gadget_arm64_velem_umlsl; break;
        case 0x0b: gadget = gadget_arm64_velem_sqdmull; break;
        case 0x03: gadget = gadget_arm64_velem_sqdmlal; break;
        case 0x07: gadget = gadget_arm64_velem_sqdmlsl; break;
        case 0x09: gadget = gadget_arm64_velem_fmul; is_fp = true; break;
        case 0x19: gadget = gadget_arm64_velem_fmulx; is_fp = true; break;
        case 0x01: gadget = gadget_arm64_velem_fmla; is_fp = true; break;
        case 0x05: gadget = gadget_arm64_velem_fmls; is_fp = true; break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        if (is_fp) {
            if (size < 2 || (size == 3 && !q))
                return gen_arm64_undefined(state);
        } else {
            if (size != 1 && size != 2)
                return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) size << 24) | ((uint64_t) q << 26) |
                   ((uint64_t) byteoff << 27));
        return 1;
    }

    // AdvSIMD by-element, scalar: FMUL/FMULX/FMLA/FMLS Sd/Dd, Vm.T[i].
    if ((insn & 0xdf000400) == 0x5f000000) {
        extern void gadget_arm64_selem_fmul(void), gadget_arm64_selem_fmulx(void);
        extern void gadget_arm64_selem_fmla(void), gadget_arm64_selem_fmls(void);
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned l = (insn >> 21) & 1;
        unsigned m = (insn >> 20) & 1;
        unsigned rm4 = (insn >> 16) & 0xf;
        unsigned opcode = (insn >> 12) & 0xf;
        unsigned h = (insn >> 11) & 1;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (size < 2)
            return gen_arm64_undefined(state);
        unsigned rm = (m << 4) | rm4;
        unsigned index = size == 2 ? ((h << 1) | l) : h;
        unsigned byteoff = index << (size == 2 ? 2 : 3);
        void *gadget = NULL;
        switch ((u << 4) | opcode) {
        case 0x09: gadget = gadget_arm64_selem_fmul; break;
        case 0x19: gadget = gadget_arm64_selem_fmulx; break;
        case 0x01: gadget = gadget_arm64_selem_fmla; break;
        case 0x05: gadget = gadget_arm64_selem_fmls; break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) size << 24) | ((uint64_t) byteoff << 27));
        return 1;
    }

    // AdvSIMD scalar shift by immediate, D size: SHL/USHR/SSHR (musl
    // strtod's exponent construction). immh<3>=1 selects the 64-bit
    // element; smaller scalar element sizes are unallocated here.
    if ((insn & 0xff80fc00) == 0x5f005400          // SHL (U=0, opcode 01010)
            || (insn & 0xff80fc00) == 0x7f000400   // USHR (U=1, opcode 00000)
            || (insn & 0xff80fc00) == 0x5f000400) { // SSHR (U=0, opcode 00000)
        extern void gadget_arm64_vshift_d(void);
        unsigned immhb = (insn >> 16) & 0x7f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        bool is_shl = ((insn >> 11) & 0x1f) == 0x0a;
        bool is_ushr = !is_shl && ((insn >> 29) & 1);
        if ((immhb & 0x40) == 0) { // immh<3> must be 1 for the D form
            return gen_arm64_undefined(state);
        }
        unsigned shift = is_shl ? immhb - 64 : 128 - immhb;
        unsigned op = is_shl ? 0 : is_ushr ? 1 : 2;
        gen(state, (unsigned long) gadget_arm64_vshift_d);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) shift << 16) | ((uint64_t) op << 24));
        return 1;
    }

    // AdvSIMD scalar shift by immediate, remaining opcodes: SSRA/USRA,
    // SRSHR/URSHR, SRSRA/URSRA (D), SQSHL/UQSHL #imm (B/H/S/D), the
    // saturating narrows SQSHRN/UQSHRN/SQRSHRN/UQRSHRN/SQSHRUN/SQRSHRUN
    // (B/H/S results), and the fixed-point converts SCVTF/UCVTF/FCVTZS/
    // FCVTZU #fbits (D only — rtorrent's MSE Diffie-Hellman/SHA-1 path hit
    // `ucvtf d2, d2, #0x14` = encoding 0x7f6ce442, opcode 0x1c, missing
    // from this switch). Same register-form lowering as the vector space.
    if ((insn & 0xdf800400) == 0x5f000400 && ((insn >> 19) & 0xf) != 0) {
        extern void gadget_arm64_ssrshr_d(void), gadget_arm64_surshr_d(void);
        extern void gadget_arm64_sssra_d(void), gadget_arm64_susra_d(void);
        extern void gadget_arm64_ssrsra_d(void), gadget_arm64_sursra_d(void);
        extern void gadget_arm64_ssqshli(void), gadget_arm64_suqshli(void);
        extern void gadget_arm64_ssqshrn(void), gadget_arm64_ssqrshrn(void);
        extern void gadget_arm64_suqshrn(void), gadget_arm64_suqrshrn(void);
        extern void gadget_arm64_ssqshrun(void), gadget_arm64_ssqrshrun(void);
        unsigned u = (insn >> 29) & 1;
        unsigned immhb = (insn >> 16) & 0x7f;
        unsigned immh = immhb >> 3;
        unsigned opcode = (insn >> 11) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        int esize_log2 = immh & 8 ? 3 : immh & 4 ? 2 : immh & 2 ? 1 : 0;
        unsigned esize = 8u << esize_log2;
        unsigned rshift = 2 * esize - immhb;
        unsigned lshift = immhb - esize;
        void *gadget = NULL;
        switch (opcode) {
        case 0x02: case 0x04: case 0x06: // D-only accumulate/rounding
            if (esize_log2 != 3)
                return gen_arm64_undefined(state);
            gadget = opcode == 0x02 ? (u ? (void *) gadget_arm64_susra_d : (void *) gadget_arm64_sssra_d)
                   : opcode == 0x04 ? (u ? (void *) gadget_arm64_surshr_d : (void *) gadget_arm64_ssrshr_d)
                   : (u ? (void *) gadget_arm64_sursra_d : (void *) gadget_arm64_ssrsra_d);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8));
            gen(state, (uint64_t) -(int64_t) rshift);
            return 1;
        case 0x0e: // SQSHL/UQSHL #imm, any element size
            gadget = u ? (void *) gadget_arm64_suqshli : (void *) gadget_arm64_ssqshli;
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) esize_log2 << 16));
            gen(state, (uint64_t) lshift);
            return 1;
        case 0x1c: case 0x1f: { // SCVTF/UCVTF, FCVTZS/FCVTZU #fbits (scalar; S or D)
            extern void gadget_arm64_sscvtf_fix(void), gadget_arm64_sucvtf_fix(void);
            extern void gadget_arm64_sfcvtzs_fix(void), gadget_arm64_sfcvtzu_fix(void);
            if (esize_log2 < 2) // B/H forms unallocated for this opcode
                return gen_arm64_undefined(state);
            unsigned sz = esize_log2 == 3;
            unsigned fbits = rshift;
            uint64_t scale;
            if (opcode == 0x1c) { // SCVTF/UCVTF: multiply by 2^-fbits after
                gadget = u ? (void *) gadget_arm64_sucvtf_fix : (void *) gadget_arm64_sscvtf_fix;
                scale = sz ? (uint64_t) (1023 - fbits) << 52 : (uint64_t) ((127 - fbits) << 23);
            } else { // FCVTZS/FCVTZU: multiply by 2^+fbits first
                gadget = u ? (void *) gadget_arm64_sfcvtzu_fix : (void *) gadget_arm64_sfcvtzs_fix;
                scale = sz ? (uint64_t) (1023 + fbits) << 52 : (uint64_t) ((127 + fbits) << 23);
            }
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) sz << 16));
            gen(state, scale);
            return 1;
        }
        case 0x10: case 0x11: case 0x12: case 0x13: { // saturating narrows
            if (esize_log2 >= 3)
                return gen_arm64_undefined(state);
            static void *const t[2][4] = {
                {NULL, NULL, gadget_arm64_ssqshrn, gadget_arm64_ssqrshrn},
                {gadget_arm64_ssqshrun, gadget_arm64_ssqrshrun,
                 gadget_arm64_suqshrn, gadget_arm64_suqrshrn}};
            gadget = t[u][opcode - 0x10];
            if (gadget == NULL) // scalar SHRN/RSHRN don't exist (U=0 10/11)
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) esize_log2 << 16));
            gen(state, (uint64_t) -(int64_t) rshift);
            return 1;
        }
        }
        return gen_arm64_undefined(state);
    }

    // AdvSIMD three-same bitwise: AND/BIC/ORR/ORN (U=0) and EOR/BSL/BIT/
    // BIF (U=1), selected by the size field. Includes the vector MOV
    // alias (ORR, Rn==Rm) — how compilers copy V registers.
    // (Mask must leave U (bit 29) free: with 0xbf20fc00 the whole U=1 half
    // of the table below was unreachable and `eor v.8b` SIGILLed ld.)
    if ((insn & 0x9f20fc00) == 0x0e201c00) {
        extern void gadget_arm64_vand_16b(void), gadget_arm64_vand_8b(void);
        extern void gadget_arm64_vbic_16b(void), gadget_arm64_vbic_8b(void);
        extern void gadget_arm64_vorr_16b(void), gadget_arm64_vorr_8b(void);
        extern void gadget_arm64_vorn_16b(void), gadget_arm64_vorn_8b(void);
        extern void gadget_arm64_veor_16b(void), gadget_arm64_veor_8b(void);
        extern void gadget_arm64_vbsl_16b(void), gadget_arm64_vbsl_8b(void);
        extern void gadget_arm64_vbit_16b(void), gadget_arm64_vbit_8b(void);
        extern void gadget_arm64_vbif_16b(void), gadget_arm64_vbif_8b(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 0x3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        static void *const t[2][4][2] = { // [u][size][q]
            {{(void *) gadget_arm64_vand_8b, (void *) gadget_arm64_vand_16b},
             {(void *) gadget_arm64_vbic_8b, (void *) gadget_arm64_vbic_16b},
             {(void *) gadget_arm64_vorr_8b, (void *) gadget_arm64_vorr_16b},
             {(void *) gadget_arm64_vorn_8b, (void *) gadget_arm64_vorn_16b}},
            {{(void *) gadget_arm64_veor_8b, (void *) gadget_arm64_veor_16b},
             {(void *) gadget_arm64_vbsl_8b, (void *) gadget_arm64_vbsl_16b},
             {(void *) gadget_arm64_vbit_8b, (void *) gadget_arm64_vbit_16b},
             {(void *) gadget_arm64_vbif_8b, (void *) gadget_arm64_vbif_16b}},
        };
        gen(state, (unsigned long) t[u][size][q]);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }

    // AdvSIMD three-same, vector (integer + FP): ported from OpenMinis'
    // gen.c three-same decode (mask 0x9f200400) and its U/opcode table,
    // with fixes: per-op element-size validity is enforced here (theirs
    // only rejects size=3 for ADD/SUB/SHSUB/UHSUB and lets e.g. `smax .2d`
    // silently zero Vd), and the FMAXNMP/FMINNMP rows (U=1, opcode 0x18)
    // absent from their table are filled in. The bitwise opcode-0x03 group
    // never reaches here — the dedicated branch above matches it first.
    if ((insn & 0x9f200400) == 0x0e200400) {
        extern void gadget_arm64_vshadd(void), gadget_arm64_vuhadd(void);
        extern void gadget_arm64_vsrhadd(void), gadget_arm64_vurhadd(void);
        extern void gadget_arm64_vshsub(void), gadget_arm64_vuhsub(void);
        extern void gadget_arm64_vsqadd(void), gadget_arm64_vuqadd(void);
        extern void gadget_arm64_vsqsub(void), gadget_arm64_vuqsub(void);
        extern void gadget_arm64_vcmgt(void), gadget_arm64_vcmge(void);
        extern void gadget_arm64_vcmhi(void), gadget_arm64_vcmhs(void);
        extern void gadget_arm64_vcmeq(void), gadget_arm64_vcmtst(void);
        extern void gadget_arm64_vsshl(void), gadget_arm64_vushl(void);
        extern void gadget_arm64_vsrshl(void), gadget_arm64_vurshl(void);
        extern void gadget_arm64_vsqshl(void), gadget_arm64_vuqshl(void);
        extern void gadget_arm64_vsqrshl(void), gadget_arm64_vuqrshl(void);
        extern void gadget_arm64_vsmax(void), gadget_arm64_vsmin(void);
        extern void gadget_arm64_vumax(void), gadget_arm64_vumin(void);
        extern void gadget_arm64_vsabd(void), gadget_arm64_vuabd(void);
        extern void gadget_arm64_vsaba(void), gadget_arm64_vuaba(void);
        extern void gadget_arm64_vadd(void), gadget_arm64_vsub(void);
        extern void gadget_arm64_vmla(void), gadget_arm64_vmls(void);
        extern void gadget_arm64_vmul(void), gadget_arm64_vpmul(void);
        extern void gadget_arm64_vsmaxp(void), gadget_arm64_vsminp(void);
        extern void gadget_arm64_vumaxp(void), gadget_arm64_vuminp(void);
        extern void gadget_arm64_vsqdmulh(void), gadget_arm64_vsqrdmulh(void);
        extern void gadget_arm64_vaddp(void);
        extern void gadget_arm64_vfmaxnm(void), gadget_arm64_vfminnm(void);
        extern void gadget_arm64_vfmaxnmp(void), gadget_arm64_vfminnmp(void);
        extern void gadget_arm64_vfmla(void), gadget_arm64_vfmls(void);
        extern void gadget_arm64_vfadd(void), gadget_arm64_vfsub(void);
        extern void gadget_arm64_vfabd(void), gadget_arm64_vfaddp(void);
        extern void gadget_arm64_vfmulx(void), gadget_arm64_vfmul(void);
        extern void gadget_arm64_vfcmeq(void), gadget_arm64_vfcmge(void);
        extern void gadget_arm64_vfcmgt(void);
        extern void gadget_arm64_vfacge(void), gadget_arm64_vfacgt(void);
        extern void gadget_arm64_vfmax(void), gadget_arm64_vfmin(void);
        extern void gadget_arm64_vfmaxp(void), gadget_arm64_vfminp(void);
        extern void gadget_arm64_vfrecps(void), gadget_arm64_vfrsqrts(void);
        extern void gadget_arm64_vfdiv(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 11) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        if (opcode < 0x18) {
            // Integer rows. Validity classes: D = all element sizes but
            // .1d needs Q; NOD = no 64-bit elements at all; HS = 16/32-bit
            // only (SQDMULH/SQRDMULH); B = bytes only (PMUL).
            enum { V_D, V_NOD, V_HS, V_B } valid = V_D;
            switch ((u << 5) | opcode) {
            case 0x00: gadget = gadget_arm64_vshadd; valid = V_NOD; break;
            case 0x20: gadget = gadget_arm64_vuhadd; valid = V_NOD; break;
            case 0x01: gadget = gadget_arm64_vsqadd; break;
            case 0x21: gadget = gadget_arm64_vuqadd; break;
            case 0x02: gadget = gadget_arm64_vsrhadd; valid = V_NOD; break;
            case 0x22: gadget = gadget_arm64_vurhadd; valid = V_NOD; break;
            case 0x04: gadget = gadget_arm64_vshsub; valid = V_NOD; break;
            case 0x24: gadget = gadget_arm64_vuhsub; valid = V_NOD; break;
            case 0x05: gadget = gadget_arm64_vsqsub; break;
            case 0x25: gadget = gadget_arm64_vuqsub; break;
            case 0x06: gadget = gadget_arm64_vcmgt; break;
            case 0x26: gadget = gadget_arm64_vcmhi; break;
            case 0x07: gadget = gadget_arm64_vcmge; break;
            case 0x27: gadget = gadget_arm64_vcmhs; break;
            case 0x08: gadget = gadget_arm64_vsshl; break;
            case 0x28: gadget = gadget_arm64_vushl; break;
            case 0x09: gadget = gadget_arm64_vsqshl; break;
            case 0x29: gadget = gadget_arm64_vuqshl; break;
            case 0x0a: gadget = gadget_arm64_vsrshl; break;
            case 0x2a: gadget = gadget_arm64_vurshl; break;
            case 0x0b: gadget = gadget_arm64_vsqrshl; break;
            case 0x2b: gadget = gadget_arm64_vuqrshl; break;
            case 0x0c: gadget = gadget_arm64_vsmax; valid = V_NOD; break;
            case 0x2c: gadget = gadget_arm64_vumax; valid = V_NOD; break;
            case 0x0d: gadget = gadget_arm64_vsmin; valid = V_NOD; break;
            case 0x2d: gadget = gadget_arm64_vumin; valid = V_NOD; break;
            case 0x0e: gadget = gadget_arm64_vsabd; valid = V_NOD; break;
            case 0x2e: gadget = gadget_arm64_vuabd; valid = V_NOD; break;
            case 0x0f: gadget = gadget_arm64_vsaba; valid = V_NOD; break;
            case 0x2f: gadget = gadget_arm64_vuaba; valid = V_NOD; break;
            case 0x10: gadget = gadget_arm64_vadd; break;
            case 0x30: gadget = gadget_arm64_vsub; break;
            case 0x11: gadget = gadget_arm64_vcmtst; break;
            case 0x31: gadget = gadget_arm64_vcmeq; break;
            case 0x12: gadget = gadget_arm64_vmla; valid = V_NOD; break;
            case 0x32: gadget = gadget_arm64_vmls; valid = V_NOD; break;
            case 0x13: gadget = gadget_arm64_vmul; valid = V_NOD; break;
            case 0x33: gadget = gadget_arm64_vpmul; valid = V_B; break;
            case 0x14: gadget = gadget_arm64_vsmaxp; valid = V_NOD; break;
            case 0x34: gadget = gadget_arm64_vumaxp; valid = V_NOD; break;
            case 0x15: gadget = gadget_arm64_vsminp; valid = V_NOD; break;
            case 0x35: gadget = gadget_arm64_vuminp; valid = V_NOD; break;
            case 0x16: gadget = gadget_arm64_vsqdmulh; valid = V_HS; break;
            case 0x36: gadget = gadget_arm64_vsqrdmulh; valid = V_HS; break;
            case 0x17: gadget = gadget_arm64_vaddp; break;
            }
            if (gadget == NULL)
                return gen_arm64_undefined(state);
            if ((valid == V_D && size == 3 && q == 0) ||
                    (valid == V_NOD && size == 3) ||
                    (valid == V_HS && (size == 0 || size == 3)) ||
                    (valid == V_B && size != 0))
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                       ((uint64_t) size << 24) | ((uint64_t) q << 26));
            return 1;
        }
        // FP rows (opcodes 0x18-0x1f): size<1> is the operation-variant
        // bit 'a', size<0> is the precision (0=.2s/.4s, 1=.2d).
        unsigned sz = size & 1, a = size >> 1;
        switch (opcode) {
        case 0x18:
            if (!u) gadget = a ? gadget_arm64_vfminnm : gadget_arm64_vfmaxnm;
            else    gadget = a ? gadget_arm64_vfminnmp : gadget_arm64_vfmaxnmp;
            break;
        case 0x19:
            if (!u) gadget = a ? gadget_arm64_vfmls : gadget_arm64_vfmla;
            break;
        case 0x1a:
            if (!u) gadget = a ? gadget_arm64_vfsub : gadget_arm64_vfadd;
            else    gadget = a ? gadget_arm64_vfabd : gadget_arm64_vfaddp;
            break;
        case 0x1b:
            if (!a) gadget = u ? gadget_arm64_vfmul : gadget_arm64_vfmulx;
            break;
        case 0x1c:
            if (!u && !a) gadget = gadget_arm64_vfcmeq;
            else if (u)   gadget = a ? gadget_arm64_vfcmgt : gadget_arm64_vfcmge;
            break;
        case 0x1d:
            if (u) gadget = a ? gadget_arm64_vfacgt : gadget_arm64_vfacge;
            break;
        case 0x1e:
            if (!u) gadget = a ? gadget_arm64_vfmin : gadget_arm64_vfmax;
            else    gadget = a ? gadget_arm64_vfminp : gadget_arm64_vfmaxp;
            break;
        case 0x1f:
            if (!u)     gadget = a ? gadget_arm64_vfrsqrts : gadget_arm64_vfrecps;
            else if (!a) gadget = gadget_arm64_vfdiv;
            break;
        }
        if (gadget == NULL || (sz && !q)) // no .1d arrangement
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) sz << 24) | ((uint64_t) q << 26));
        return 1;
    }

    // AdvSIMD three-same, scalar: ported from OpenMinis' scalar decode
    // (mask 0xdf200400), which only covers the D-size integer ops and
    // FABD and silently falls through otherwise; extended here with the
    // saturating b/h/s/d ops, SQDMULH/SQRDMULH, CMTST, the rounding
    // shifts, and the FP register compares/reciprocal steps.
    if ((insn & 0xdf200400) == 0x5e200400) {
        extern void gadget_arm64_sadd(void), gadget_arm64_ssub(void);
        extern void gadget_arm64_scmeq(void), gadget_arm64_scmtst(void);
        extern void gadget_arm64_scmgt(void), gadget_arm64_scmge(void);
        extern void gadget_arm64_scmhi(void), gadget_arm64_scmhs(void);
        extern void gadget_arm64_ssshl(void), gadget_arm64_sushl(void);
        extern void gadget_arm64_ssrshl(void), gadget_arm64_surshl(void);
        extern void gadget_arm64_ssqadd(void), gadget_arm64_suqadd(void);
        extern void gadget_arm64_ssqsub(void), gadget_arm64_suqsub(void);
        extern void gadget_arm64_ssqshl(void), gadget_arm64_suqshl(void);
        extern void gadget_arm64_ssqrshl(void), gadget_arm64_suqrshl(void);
        extern void gadget_arm64_ssqdmulh(void), gadget_arm64_ssqrdmulh(void);
        extern void gadget_arm64_sfabd(void), gadget_arm64_sfmulx(void);
        extern void gadget_arm64_sfrecps(void), gadget_arm64_sfrsqrts(void);
        extern void gadget_arm64_sfcmeq(void), gadget_arm64_sfcmge(void);
        extern void gadget_arm64_sfcmgt(void);
        extern void gadget_arm64_sfacge(void), gadget_arm64_sfacgt(void);
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 11) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        if (opcode < 0x18) {
            bool any_size = false, hs_only = false;
            switch ((u << 5) | opcode) {
            // D-size only (size must be 3)
            case 0x10: gadget = gadget_arm64_sadd; break;
            case 0x30: gadget = gadget_arm64_ssub; break;
            case 0x06: gadget = gadget_arm64_scmgt; break;
            case 0x26: gadget = gadget_arm64_scmhi; break;
            case 0x07: gadget = gadget_arm64_scmge; break;
            case 0x27: gadget = gadget_arm64_scmhs; break;
            case 0x08: gadget = gadget_arm64_ssshl; break;
            case 0x28: gadget = gadget_arm64_sushl; break;
            case 0x0a: gadget = gadget_arm64_ssrshl; break;
            case 0x2a: gadget = gadget_arm64_surshl; break;
            case 0x11: gadget = gadget_arm64_scmtst; break;
            case 0x31: gadget = gadget_arm64_scmeq; break;
            // saturating: b/h/s/d, size in the code stream
            case 0x01: gadget = gadget_arm64_ssqadd; any_size = true; break;
            case 0x21: gadget = gadget_arm64_suqadd; any_size = true; break;
            case 0x05: gadget = gadget_arm64_ssqsub; any_size = true; break;
            case 0x25: gadget = gadget_arm64_suqsub; any_size = true; break;
            case 0x09: gadget = gadget_arm64_ssqshl; any_size = true; break;
            case 0x29: gadget = gadget_arm64_suqshl; any_size = true; break;
            case 0x0b: gadget = gadget_arm64_ssqrshl; any_size = true; break;
            case 0x2b: gadget = gadget_arm64_suqrshl; any_size = true; break;
            case 0x16: gadget = gadget_arm64_ssqdmulh; hs_only = true; break;
            case 0x36: gadget = gadget_arm64_ssqrdmulh; hs_only = true; break;
            }
            if (gadget == NULL)
                return gen_arm64_undefined(state);
            if (hs_only ? (size == 0 || size == 3) : (!any_size && size != 3))
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                       ((uint64_t) size << 24));
            return 1;
        }
        // FP scalar rows: size<1> is 'a', size<0> is precision (S/D).
        unsigned sz = size & 1, a = size >> 1;
        switch (opcode) {
        case 0x1a: if (u && a) gadget = gadget_arm64_sfabd; break;
        case 0x1b: if (!u && !a) gadget = gadget_arm64_sfmulx; break;
        case 0x1c:
            if (!u && !a) gadget = gadget_arm64_sfcmeq;
            else if (u)   gadget = a ? gadget_arm64_sfcmgt : gadget_arm64_sfcmge;
            break;
        case 0x1d:
            if (u) gadget = a ? gadget_arm64_sfacgt : gadget_arm64_sfacge;
            break;
        case 0x1f:
            if (!u) gadget = a ? gadget_arm64_sfrsqrts : gadget_arm64_sfrecps;
            break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) sz << 24));
        return 1;
    }

    // AdvSIMD three-different (widening/narrowing two-source): ported from
    // OpenMinis' table (mask 0x9f200c00), extended with the SQDMLAL/
    // SQDMLSL/SQDMULL rows (U=0 opcodes 9/b/d) their switch omits. PMULL
    // (opcode 0xe) is decoded here too instead of falling through.
    if ((insn & 0x9f200c00) == 0x0e200000) {
        extern void gadget_arm64_vsaddl(void), gadget_arm64_vuaddl(void);
        extern void gadget_arm64_vsaddw(void), gadget_arm64_vuaddw(void);
        extern void gadget_arm64_vssubl(void), gadget_arm64_vusubl(void);
        extern void gadget_arm64_vssubw(void), gadget_arm64_vusubw(void);
        extern void gadget_arm64_vaddhn(void), gadget_arm64_vraddhn(void);
        extern void gadget_arm64_vsubhn(void), gadget_arm64_vrsubhn(void);
        extern void gadget_arm64_vsabal(void), gadget_arm64_vuabal(void);
        extern void gadget_arm64_vsabdl(void), gadget_arm64_vuabdl(void);
        extern void gadget_arm64_vsmlal(void), gadget_arm64_vumlal(void);
        extern void gadget_arm64_vsmlsl(void), gadget_arm64_vumlsl(void);
        extern void gadget_arm64_vsmull_d(void), gadget_arm64_vumull_d(void);
        extern void gadget_arm64_vsqdmlal(void), gadget_arm64_vsqdmlsl(void);
        extern void gadget_arm64_vsqdmull(void), gadget_arm64_vpmull(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        bool hs_only = false;
        if (opcode == 0xe) {
            // PMULL/PMULL2: U=0, size 0 (8H) or 3 (1Q, FEAT_PMULL).
            if (u || (size != 0 && size != 3))
                return gen_arm64_undefined(state);
            gadget = gadget_arm64_vpmull;
        } else if (size == 3) {
            return gen_arm64_undefined(state);
        } else if (!u) {
            switch (opcode) {
            case 0x0: gadget = gadget_arm64_vsaddl; break;
            case 0x1: gadget = gadget_arm64_vsaddw; break;
            case 0x2: gadget = gadget_arm64_vssubl; break;
            case 0x3: gadget = gadget_arm64_vssubw; break;
            case 0x4: gadget = gadget_arm64_vaddhn; break;
            case 0x5: gadget = gadget_arm64_vsabal; break;
            case 0x6: gadget = gadget_arm64_vsubhn; break;
            case 0x7: gadget = gadget_arm64_vsabdl; break;
            case 0x8: gadget = gadget_arm64_vsmlal; break;
            case 0x9: gadget = gadget_arm64_vsqdmlal; hs_only = true; break;
            case 0xa: gadget = gadget_arm64_vsmlsl; break;
            case 0xb: gadget = gadget_arm64_vsqdmlsl; hs_only = true; break;
            case 0xc: gadget = gadget_arm64_vsmull_d; break;
            case 0xd: gadget = gadget_arm64_vsqdmull; hs_only = true; break;
            }
        } else {
            switch (opcode) {
            case 0x0: gadget = gadget_arm64_vuaddl; break;
            case 0x1: gadget = gadget_arm64_vuaddw; break;
            case 0x2: gadget = gadget_arm64_vusubl; break;
            case 0x3: gadget = gadget_arm64_vusubw; break;
            case 0x4: gadget = gadget_arm64_vraddhn; break;
            case 0x5: gadget = gadget_arm64_vuabal; break;
            case 0x6: gadget = gadget_arm64_vrsubhn; break;
            case 0x7: gadget = gadget_arm64_vuabdl; break;
            case 0x8: gadget = gadget_arm64_vumlal; break;
            case 0xa: gadget = gadget_arm64_vumlsl; break;
            case 0xc: gadget = gadget_arm64_vumull_d; break;
            }
        }
        if (gadget == NULL || (hs_only && size == 0))
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) size << 24) | ((uint64_t) q << 26));
        return 1;
    }

    // AdvSIMD two-register misc (integer + FP unary/convert/round, compare
    // with zero, narrows, pairwise-long): unified port of OpenMinis' dozen
    // separate per-op decode branches. Gaps in theirs filled: REV16, CNT
    // (they route CNT via a different path), SQABS/SQNEG, SUQADD/USQADD,
    // FCVTN/FCVTL/FCVTXN, URECPE/URSQRTE, SHLL here rather than ad-hoc.
    if ((insn & 0x9f3e0c00) == 0x0e200800) {
        extern void gadget_arm64_vrev64(void), gadget_arm64_vrev32(void);
        extern void gadget_arm64_vrev16(void), gadget_arm64_vcnt(void);
        extern void gadget_arm64_vsaddlp(void), gadget_arm64_vuaddlp(void);
        extern void gadget_arm64_vsadalp(void), gadget_arm64_vuadalp(void);
        extern void gadget_arm64_vsuqadd(void), gadget_arm64_vusqadd(void);
        extern void gadget_arm64_vcls(void), gadget_arm64_vclz(void);
        extern void gadget_arm64_vsqabs(void), gadget_arm64_vsqneg(void);
        extern void gadget_arm64_vcmgt0(void), gadget_arm64_vcmeq0(void);
        extern void gadget_arm64_vcmlt0(void), gadget_arm64_vcmge0(void);
        extern void gadget_arm64_vcmle0(void);
        extern void gadget_arm64_vabs(void), gadget_arm64_vneg(void);
        extern void gadget_arm64_vnot(void), gadget_arm64_vrbit(void);
        extern void gadget_arm64_vxtn(void), gadget_arm64_vsqxtn(void);
        extern void gadget_arm64_vuqxtn(void), gadget_arm64_vsqxtun(void);
        extern void gadget_arm64_vshll(void);
        extern void gadget_arm64_vfcvtn(void), gadget_arm64_vfcvtl(void);
        extern void gadget_arm64_vfcvtxn(void);
        extern void gadget_arm64_vfrintn(void), gadget_arm64_vfrintm(void);
        extern void gadget_arm64_vfrintp(void), gadget_arm64_vfrintz(void);
        extern void gadget_arm64_vfrinta(void), gadget_arm64_vfrintx(void);
        extern void gadget_arm64_vfrinti(void);
        extern void gadget_arm64_vfcvtns(void), gadget_arm64_vfcvtnu(void);
        extern void gadget_arm64_vfcvtms(void), gadget_arm64_vfcvtmu(void);
        extern void gadget_arm64_vfcvtas(void), gadget_arm64_vfcvtau(void);
        extern void gadget_arm64_vfcvtps(void), gadget_arm64_vfcvtpu(void);
        extern void gadget_arm64_vfcvtzs(void), gadget_arm64_vfcvtzu(void);
        extern void gadget_arm64_vscvtf(void), gadget_arm64_vucvtf(void);
        extern void gadget_arm64_vfcmgt0(void), gadget_arm64_vfcmeq0(void);
        extern void gadget_arm64_vfcmlt0(void), gadget_arm64_vfcmge0(void);
        extern void gadget_arm64_vfcmle0(void);
        extern void gadget_arm64_vfabs(void), gadget_arm64_vfneg(void);
        extern void gadget_arm64_vfsqrt(void);
        extern void gadget_arm64_vfrecpe(void), gadget_arm64_vfrsqrte(void);
        extern void gadget_arm64_vurecpe(void), gadget_arm64_vursqrte(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned opcode = (insn >> 12) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        unsigned sz = size & 1;
        void *gadget = NULL;
        // Integer rows: validity mirrors the vector three-same classes.
        enum { V_D, V_NOD, V_HB, V_B, V_FP, V_FPN } valid = V_D;
        switch ((u << 5) | opcode) {
        case 0x00: gadget = gadget_arm64_vrev64; valid = V_NOD; break;
        case 0x20: gadget = gadget_arm64_vrev32; valid = V_HB; break;
        case 0x01: gadget = gadget_arm64_vrev16; valid = V_B; break;
        case 0x02: gadget = gadget_arm64_vsaddlp; valid = V_NOD; break;
        case 0x22: gadget = gadget_arm64_vuaddlp; valid = V_NOD; break;
        case 0x03: gadget = gadget_arm64_vsuqadd; break;
        case 0x23: gadget = gadget_arm64_vusqadd; break;
        case 0x04: gadget = gadget_arm64_vcls; valid = V_NOD; break;
        case 0x24: gadget = gadget_arm64_vclz; valid = V_NOD; break;
        case 0x05: gadget = gadget_arm64_vcnt; valid = V_B; break;
        case 0x25:
            // NOT (size 0) / RBIT (size 1) share U=1 opcode 0x05.
            if (size == 0) gadget = gadget_arm64_vnot;
            else if (size == 1) gadget = gadget_arm64_vrbit;
            else return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) q << 18));
            return 1;
        case 0x06: gadget = gadget_arm64_vsadalp; valid = V_NOD; break;
        case 0x26: gadget = gadget_arm64_vuadalp; valid = V_NOD; break;
        case 0x07: gadget = gadget_arm64_vsqabs; break;
        case 0x27: gadget = gadget_arm64_vsqneg; break;
        case 0x08: gadget = gadget_arm64_vcmgt0; break;
        case 0x28: gadget = gadget_arm64_vcmge0; break;
        case 0x09: gadget = gadget_arm64_vcmeq0; break;
        case 0x29: gadget = gadget_arm64_vcmle0; break;
        case 0x0a: gadget = gadget_arm64_vcmlt0; break;
        case 0x0b: gadget = gadget_arm64_vabs; break;
        case 0x2b: gadget = gadget_arm64_vneg; break;
        case 0x12: gadget = gadget_arm64_vxtn; valid = V_NOD; break;
        case 0x32: gadget = gadget_arm64_vsqxtun; valid = V_NOD; break;
        case 0x33: gadget = gadget_arm64_vshll; valid = V_NOD; break;
        case 0x14: gadget = gadget_arm64_vsqxtn; valid = V_NOD; break;
        case 0x34: gadget = gadget_arm64_vuqxtn; valid = V_NOD; break;
        // FP rows, size<1>=0 group
        case 0x16: gadget = gadget_arm64_vfcvtn; valid = V_FPN; break;
        case 0x36: gadget = gadget_arm64_vfcvtxn; valid = V_FPN; break;
        case 0x17: gadget = gadget_arm64_vfcvtl; valid = V_FPN; break;
        case 0x18:
            gadget = size & 2 ? gadget_arm64_vfrintp : gadget_arm64_vfrintn;
            valid = V_FP; break;
        case 0x38:
            if (!(size & 2)) { gadget = gadget_arm64_vfrinta; valid = V_FP; }
            break;
        case 0x19:
            gadget = size & 2 ? gadget_arm64_vfrintz : gadget_arm64_vfrintm;
            valid = V_FP; break;
        case 0x39:
            gadget = size & 2 ? gadget_arm64_vfrinti : gadget_arm64_vfrintx;
            valid = V_FP; break;
        case 0x1a:
            gadget = size & 2 ? gadget_arm64_vfcvtps : gadget_arm64_vfcvtns;
            valid = V_FP; break;
        case 0x3a:
            gadget = size & 2 ? gadget_arm64_vfcvtpu : gadget_arm64_vfcvtnu;
            valid = V_FP; break;
        case 0x1b:
            gadget = size & 2 ? gadget_arm64_vfcvtzs : gadget_arm64_vfcvtms;
            valid = V_FP; break;
        case 0x3b:
            gadget = size & 2 ? gadget_arm64_vfcvtzu : gadget_arm64_vfcvtmu;
            valid = V_FP; break;
        case 0x1c:
            gadget = size & 2 ? gadget_arm64_vurecpe : gadget_arm64_vfcvtas;
            valid = size & 2 ? V_FPN : V_FP;
            if ((size & 2) && sz) return gen_arm64_undefined(state);
            if (size & 2) { // URECPE: .2s/.4s only, q in the usual slot
                gen(state, (unsigned long) gadget);
                gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) q << 18));
                return 1;
            }
            break;
        case 0x3c:
            gadget = size & 2 ? gadget_arm64_vursqrte : gadget_arm64_vfcvtau;
            if ((size & 2) && sz) return gen_arm64_undefined(state);
            if (size & 2) {
                gen(state, (unsigned long) gadget);
                gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) q << 18));
                return 1;
            }
            valid = V_FP; break;
        case 0x1d:
            gadget = size & 2 ? gadget_arm64_vfrecpe : gadget_arm64_vscvtf;
            valid = V_FP; break;
        case 0x3d:
            gadget = size & 2 ? gadget_arm64_vfrsqrte : gadget_arm64_vucvtf;
            valid = V_FP; break;
        // FP rows, size<1>=1 group (compare with zero, abs/neg/sqrt)
        case 0x0c:
            if (size & 2) { gadget = gadget_arm64_vfcmgt0; valid = V_FP; }
            break;
        case 0x2c:
            if (size & 2) { gadget = gadget_arm64_vfcmge0; valid = V_FP; }
            break;
        case 0x0d:
            if (size & 2) { gadget = gadget_arm64_vfcmeq0; valid = V_FP; }
            break;
        case 0x2d:
            if (size & 2) { gadget = gadget_arm64_vfcmle0; valid = V_FP; }
            break;
        case 0x0e:
            if (size & 2) { gadget = gadget_arm64_vfcmlt0; valid = V_FP; }
            break;
        case 0x0f:
            if (size & 2) { gadget = gadget_arm64_vfabs; valid = V_FP; }
            break;
        case 0x2f:
            if (size & 2) { gadget = gadget_arm64_vfneg; valid = V_FP; }
            break;
        case 0x3f:
            if (size & 2) { gadget = gadget_arm64_vfsqrt; valid = V_FP; }
            break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        switch (valid) {
        case V_D:   if (size == 3 && q == 0) return gen_arm64_undefined(state); break;
        case V_NOD: if (size == 3) return gen_arm64_undefined(state); break;
        case V_HB:  if (size >= 2) return gen_arm64_undefined(state); break;
        case V_B:   if (size != 0) return gen_arm64_undefined(state); break;
        case V_FP:  if (sz && !q) return gen_arm64_undefined(state); break;
        case V_FPN: // FCVTN/FCVTL/FCVTXN: sz=0 is the FP16 form (unsupported)
            if (!sz) return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) q << 18));
            return 1;
        }
        bool is_fp = valid == V_FP;
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) |
                   ((uint64_t) (is_fp ? sz : size) << 16) | ((uint64_t) q << 18));
        return 1;
    }

    // AdvSIMD across-lanes reductions: ADDV, S/UADDLV, S/UMAXV, S/UMINV,
    // FMAXV/FMINV/FMAXNMV/FMINNMV. Ported from OpenMinis' four branches;
    // the reserved .2s arrangement (size=2, Q=0) is rejected here where
    // they emulate it via a pairwise op.
    if ((insn & 0x9f3e0c00) == 0x0e300800) {
        extern void gadget_arm64_vaddv(void);
        extern void gadget_arm64_vsaddlv(void), gadget_arm64_vuaddlv(void);
        extern void gadget_arm64_vsmaxv(void), gadget_arm64_vsminv(void);
        extern void gadget_arm64_vumaxv(void), gadget_arm64_vuminv(void);
        extern void gadget_arm64_vfmaxv(void), gadget_arm64_vfminv(void);
        extern void gadget_arm64_vfmaxnmv(void), gadget_arm64_vfminnmv(void);
        unsigned q = (insn >> 30) & 1;
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned opcode = (insn >> 12) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        switch ((u << 5) | opcode) {
        case 0x03: gadget = gadget_arm64_vsaddlv; break;
        case 0x23: gadget = gadget_arm64_vuaddlv; break;
        case 0x0a: gadget = gadget_arm64_vsmaxv; break;
        case 0x2a: gadget = gadget_arm64_vumaxv; break;
        case 0x1a: gadget = gadget_arm64_vsminv; break;
        case 0x3a: gadget = gadget_arm64_vuminv; break;
        case 0x1b: gadget = gadget_arm64_vaddv; break;
        // FP reductions: U=1, only the .4s arrangement exists.
        case 0x2c:
            gadget = size & 2 ? gadget_arm64_vfminnmv : gadget_arm64_vfmaxnmv;
            break;
        case 0x2f:
            gadget = size & 2 ? gadget_arm64_vfminv : gadget_arm64_vfmaxv;
            break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        if (opcode == 0x0c || opcode == 0x0f) {
            if ((size & 1) || !q) // .4s only
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8));
            return 1;
        }
        // Integer: size 3 reserved everywhere; size 2 requires Q=1 (.4s).
        if (size == 3 || (size == 2 && q == 0))
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) size << 16) |
                   ((uint64_t) q << 18));
        return 1;
    }

    // AdvSIMD permute: UZP1/UZP2, TRN1/TRN2, ZIP1/ZIP2 (OpenMinis'
    // op2 table, mask 0xbf208c00).
    if ((insn & 0xbf208c00) == 0x0e000800) {
        extern void gadget_arm64_vuzp1(void), gadget_arm64_vuzp2(void);
        extern void gadget_arm64_vtrn1(void), gadget_arm64_vtrn2(void);
        extern void gadget_arm64_vzip1(void), gadget_arm64_vzip2(void);
        unsigned q = (insn >> 30) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned op2 = (insn >> 12) & 7;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        void *gadget = NULL;
        switch (op2) {
        case 1: gadget = gadget_arm64_vuzp1; break;
        case 5: gadget = gadget_arm64_vuzp2; break;
        case 2: gadget = gadget_arm64_vtrn1; break;
        case 6: gadget = gadget_arm64_vtrn2; break;
        case 3: gadget = gadget_arm64_vzip1; break;
        case 7: gadget = gadget_arm64_vzip2; break;
        }
        if (gadget == NULL || (size == 3 && q == 0))
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) size << 24) | ((uint64_t) q << 26));
        return 1;
    }

    // AdvSIMD scalar two-register misc (the cc1 startup blockers FRECPE
    // and CMGE-zero live here). OpenMinis covers only fragments of this
    // family in scattered branches; full table.
    if ((insn & 0xdf3e0c00) == 0x5e200800) {
        extern void gadget_arm64_ssuqadd(void), gadget_arm64_susqadd(void);
        extern void gadget_arm64_ssqabs(void), gadget_arm64_ssqneg(void);
        extern void gadget_arm64_scmgt0(void), gadget_arm64_scmeq0(void);
        extern void gadget_arm64_scmlt0(void), gadget_arm64_scmge0(void);
        extern void gadget_arm64_scmle0(void);
        extern void gadget_arm64_sabs_d(void), gadget_arm64_sneg_d(void);
        extern void gadget_arm64_ssqxtn(void), gadget_arm64_suqxtn(void);
        extern void gadget_arm64_ssqxtun(void), gadget_arm64_sfcvtxn(void);
        extern void gadget_arm64_sfcvtns(void), gadget_arm64_sfcvtnu(void);
        extern void gadget_arm64_sfcvtms(void), gadget_arm64_sfcvtmu(void);
        extern void gadget_arm64_sfcvtas(void), gadget_arm64_sfcvtau(void);
        extern void gadget_arm64_sfcvtps(void), gadget_arm64_sfcvtpu(void);
        extern void gadget_arm64_sfcvtzs(void), gadget_arm64_sfcvtzu(void);
        extern void gadget_arm64_sscvtf(void), gadget_arm64_sucvtf(void);
        extern void gadget_arm64_sfcmgt0(void), gadget_arm64_sfcmeq0(void);
        extern void gadget_arm64_sfcmlt0(void), gadget_arm64_sfcmge0(void);
        extern void gadget_arm64_sfcmle0(void);
        extern void gadget_arm64_sfrecpe(void), gadget_arm64_sfrsqrte(void);
        extern void gadget_arm64_sfrecpx(void);
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned opcode = (insn >> 12) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        unsigned sz = size & 1;
        void *gadget = NULL;
        enum { S_ANY, S_D, S_NARROW, S_FP } valid = S_ANY;
        switch ((u << 5) | opcode) {
        case 0x03: gadget = gadget_arm64_ssuqadd; break;
        case 0x23: gadget = gadget_arm64_susqadd; break;
        case 0x07: gadget = gadget_arm64_ssqabs; break;
        case 0x27: gadget = gadget_arm64_ssqneg; break;
        case 0x08: gadget = gadget_arm64_scmgt0; valid = S_D; break;
        case 0x28: gadget = gadget_arm64_scmge0; valid = S_D; break;
        case 0x09: gadget = gadget_arm64_scmeq0; valid = S_D; break;
        case 0x29: gadget = gadget_arm64_scmle0; valid = S_D; break;
        case 0x0a: gadget = gadget_arm64_scmlt0; valid = S_D; break;
        case 0x0b: gadget = gadget_arm64_sabs_d; valid = S_D; break;
        case 0x2b: gadget = gadget_arm64_sneg_d; valid = S_D; break;
        case 0x14: gadget = gadget_arm64_ssqxtn; valid = S_NARROW; break;
        case 0x34: gadget = gadget_arm64_suqxtn; valid = S_NARROW; break;
        case 0x32: gadget = gadget_arm64_ssqxtun; valid = S_NARROW; break;
        case 0x36: // FCVTXN scalar: sz must be 1 (D -> S)
            if (!sz) return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget_arm64_sfcvtxn);
            gen(state, rd | ((uint64_t) rn << 8));
            return 1;
        case 0x1a:
            gadget = size & 2 ? gadget_arm64_sfcvtps : gadget_arm64_sfcvtns;
            valid = S_FP; break;
        case 0x3a:
            gadget = size & 2 ? gadget_arm64_sfcvtpu : gadget_arm64_sfcvtnu;
            valid = S_FP; break;
        case 0x1b:
            gadget = size & 2 ? gadget_arm64_sfcvtzs : gadget_arm64_sfcvtms;
            valid = S_FP; break;
        case 0x3b:
            gadget = size & 2 ? gadget_arm64_sfcvtzu : gadget_arm64_sfcvtmu;
            valid = S_FP; break;
        case 0x1c:
            if (!(size & 2)) { gadget = gadget_arm64_sfcvtas; valid = S_FP; }
            break;
        case 0x3c:
            if (!(size & 2)) { gadget = gadget_arm64_sfcvtau; valid = S_FP; }
            break;
        case 0x1d:
            gadget = size & 2 ? gadget_arm64_sfrecpe : gadget_arm64_sscvtf;
            valid = S_FP; break;
        case 0x3d:
            gadget = size & 2 ? gadget_arm64_sfrsqrte : gadget_arm64_sucvtf;
            valid = S_FP; break;
        case 0x1f:
            if (size & 2) { gadget = gadget_arm64_sfrecpx; valid = S_FP; }
            break;
        case 0x0c:
            if (size & 2) { gadget = gadget_arm64_sfcmgt0; valid = S_FP; }
            break;
        case 0x2c:
            if (size & 2) { gadget = gadget_arm64_sfcmge0; valid = S_FP; }
            break;
        case 0x0d:
            if (size & 2) { gadget = gadget_arm64_sfcmeq0; valid = S_FP; }
            break;
        case 0x2d:
            if (size & 2) { gadget = gadget_arm64_sfcmle0; valid = S_FP; }
            break;
        case 0x0e:
            if (size & 2) { gadget = gadget_arm64_sfcmlt0; valid = S_FP; }
            break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        switch (valid) {
        case S_D:      if (size != 3) return gen_arm64_undefined(state); break;
        case S_NARROW: if (size == 3) return gen_arm64_undefined(state); break;
        default: break;
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) |
                   ((uint64_t) (valid == S_FP ? sz : size) << 16));
        return 1;
    }

    // AdvSIMD scalar pairwise: ADDP (integer, D), FADDP/FMAXP/FMINP/
    // FMAXNMP/FMINNMP (scalar from a two-lane vector).
    if ((insn & 0xdf3e0c00) == 0x5e300800) {
        extern void gadget_arm64_saddp_d(void);
        extern void gadget_arm64_sfaddp(void), gadget_arm64_sfmaxp(void);
        extern void gadget_arm64_sfminp(void), gadget_arm64_sfmaxnmp(void);
        extern void gadget_arm64_sfminnmp(void);
        unsigned u = (insn >> 29) & 1;
        unsigned size = (insn >> 22) & 3;
        unsigned opcode = (insn >> 12) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        unsigned sz = size & 1;
        void *gadget = NULL;
        if (!u) {
            if (opcode == 0x1b && size == 3)
                gadget = gadget_arm64_saddp_d;
            if (gadget == NULL)
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) gadget);
            gen(state, rd | ((uint64_t) rn << 8));
            return 1;
        }
        switch (opcode) {
        case 0x0c:
            gadget = size & 2 ? gadget_arm64_sfminnmp : gadget_arm64_sfmaxnmp;
            break;
        case 0x0d:
            if (!(size & 2)) gadget = gadget_arm64_sfaddp;
            break;
        case 0x0f:
            gadget = size & 2 ? gadget_arm64_sfminp : gadget_arm64_sfmaxp;
            break;
        }
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) sz << 16));
        return 1;
    }

    // AArch64 crypto extension: AES, SHA1, SHA256, SHA512, SHA3
    // (ported from OpenMinis' crypto decode branches). SM3/SM4/SVE stay
    // unimplemented (no host coverage on Apple silicon).
    if ((insn & 0xff3f0c00) == 0x4e280800) { // AES
        extern void gadget_arm64_aese(void), gadget_arm64_aesd(void);
        extern void gadget_arm64_aesmc(void), gadget_arm64_aesimc(void);
        unsigned opcode = (insn >> 12) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        void *gadget = opcode == 4 ? (void *) gadget_arm64_aese
                     : opcode == 5 ? (void *) gadget_arm64_aesd
                     : opcode == 6 ? (void *) gadget_arm64_aesmc
                     : opcode == 7 ? (void *) gadget_arm64_aesimc : NULL;
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8));
        return 1;
    }
    if ((insn & 0xffffcc00) == 0x5e280800) { // SHA1H / SHA1SU1 / SHA256SU0
        extern void gadget_arm64_sha1h(void), gadget_arm64_sha1su1(void);
        extern void gadget_arm64_sha256su0(void);
        unsigned opcode = (insn >> 12) & 3;
        unsigned rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        void *gadget = opcode == 0 ? (void *) gadget_arm64_sha1h
                     : opcode == 1 ? (void *) gadget_arm64_sha1su1
                     : opcode == 2 ? (void *) gadget_arm64_sha256su0 : NULL;
        if (gadget == NULL)
            return gen_arm64_undefined(state);
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8));
        return 1;
    }
    if ((insn & 0xffe04c00) == 0x5e000000 ||   // SHA1 three-reg (bit14=0)
            (insn & 0xffe04c00) == 0x5e004000) { // SHA256 three-reg (bit14=1)
        extern void gadget_arm64_sha1c(void), gadget_arm64_sha1p(void);
        extern void gadget_arm64_sha1m(void), gadget_arm64_sha1su0(void);
        extern void gadget_arm64_sha256h(void), gadget_arm64_sha256h2(void);
        extern void gadget_arm64_sha256su1(void);
        unsigned sha256 = (insn >> 14) & 1;
        unsigned opcode = (insn >> 12) & 3;
        unsigned rm = (insn >> 16) & 0x1f, rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        void *gadget;
        if (!sha256)
            gadget = opcode == 0 ? (void *) gadget_arm64_sha1c
                   : opcode == 1 ? (void *) gadget_arm64_sha1p
                   : opcode == 2 ? (void *) gadget_arm64_sha1m
                   : (void *) gadget_arm64_sha1su0;
        else {
            gadget = opcode == 0 ? (void *) gadget_arm64_sha256h
                   : opcode == 1 ? (void *) gadget_arm64_sha256h2
                   : opcode == 2 ? (void *) gadget_arm64_sha256su1 : NULL;
            if (gadget == NULL)
                return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }
    if ((insn & 0xfffffc00) == 0xcec08000) { // SHA512SU0 (two-reg)
        extern void gadget_arm64_sha512su0(void), gadget_arm64_sha512_soft(void);
        unsigned rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        if (!arm64_host_has_sha512) { // pre-A13 host: run the op in C
            gen(state, (unsigned long) gadget_arm64_sha512_soft);
            gen(state, rd | ((uint64_t) rn << 8) | (3ULL << 24)); // op 3 = SU0
            return 1;
        }
        gen(state, (unsigned long) gadget_arm64_sha512su0);
        gen(state, rd | ((uint64_t) rn << 8));
        return 1;
    }
    if ((insn & 0xffe0fc00) == 0xce608000 ||   // SHA512H
            (insn & 0xffe0fc00) == 0xce608400 || // SHA512H2
            (insn & 0xffe0fc00) == 0xce608800) { // SHA512SU1
        extern void gadget_arm64_sha512h(void), gadget_arm64_sha512h2(void);
        extern void gadget_arm64_sha512su1(void), gadget_arm64_sha512_soft(void);
        unsigned sel = (insn >> 10) & 3;
        unsigned rm = (insn >> 16) & 0x1f, rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        if (!arm64_host_has_sha512) { // pre-A13 host: run the op in C
            gen(state, (unsigned long) gadget_arm64_sha512_soft);
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
                | ((uint64_t) sel << 24)); // op: 0=H 1=H2 2=SU1
            return 1;
        }
        void *gadget = sel == 0 ? (void *) gadget_arm64_sha512h
                     : sel == 1 ? (void *) gadget_arm64_sha512h2
                     : (void *) gadget_arm64_sha512su1;
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }
    if ((insn & 0xffe08c00) == 0xce608c00) { // RAX1 (three-reg, SHA3)
        extern void gadget_arm64_rax1(void);
        unsigned rm = (insn >> 16) & 0x1f, rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        gen(state, (unsigned long) gadget_arm64_rax1);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        return 1;
    }
    if ((insn & 0xffe08000) == 0xce000000 ||   // EOR3 (four-reg, SHA3)
            (insn & 0xffe08000) == 0xce200000) { // BCAX
        extern void gadget_arm64_eor3(void), gadget_arm64_bcax(void);
        unsigned rm = (insn >> 16) & 0x1f, ra = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        void *gadget = (insn & 0x00200000) ? (void *) gadget_arm64_bcax
                                           : (void *) gadget_arm64_eor3;
        gen(state, (unsigned long) gadget);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16) |
                   ((uint64_t) ra << 24));
        return 1;
    }
    if ((insn & 0xffe00000) == 0xce800000) { // XAR (SHA3, imm6 rotate)
        extern void gadget_arm64_xar(void);
        unsigned imm6 = (insn >> 10) & 0x3f;
        unsigned rm = (insn >> 16) & 0x1f, rn = (insn >> 5) & 0x1f, rd = insn & 0x1f;
        gen(state, (unsigned long) gadget_arm64_xar);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16));
        gen(state, (uint64_t) -(int64_t) imm6);         // right shift
        gen(state, (uint64_t) ((64 - imm6) & 63));      // complementary left
        return 1;
    }

    // AdvSIMD modified immediate: MOVI/MVNI/FMOV-imm — compiler struct
    // zeroing is `movi v0.2d, #0; stp q0, q0` everywhere — plus the
    // ORR/BIC register-modifying cmode variants (op picks ORR vs BIC;
    // cargo's core-arch code hit `orr v0.2s, #0x10, lsl #16`), lowered
    // to an RMW gadget with the same expanded-constant scheme.
    if ((insn & 0x9ff80c00) == 0x0f000400) {
        extern void gadget_arm64_vconst(void);
        unsigned q = (insn >> 30) & 1;
        unsigned op = (insn >> 29) & 1;
        unsigned cmode = (insn >> 12) & 0xf;
        uint64_t imm8 = (((insn >> 16) & 0x7) << 5) | ((insn >> 5) & 0x1f);
        unsigned rd = insn & 0x1f;
        uint64_t imm64;
        bool is_orr_bic = (cmode & 1) && cmode < 12;
        if (!gen_arm64_expand_imm(is_orr_bic ? 0 : op, cmode, imm8, &imm64)
                || (op == 1 && cmode == 15 && !q) /* FMOV .2d needs Q */) {
            return gen_arm64_undefined(state);
        }
        if (is_orr_bic) {
            // ORR (op=0) / BIC (op=1) fold the expanded pattern into the
            // live Vd value; the third stream word is the high-half keep
            // mask (the 64-bit form's write architecturally zeroes it).
            extern void gadget_arm64_vorr_imm(void), gadget_arm64_vbic_imm(void);
            gen(state, (unsigned long) (op ? gadget_arm64_vbic_imm
                                           : gadget_arm64_vorr_imm));
            gen(state, rd);
            gen(state, imm64);
            gen(state, q ? ~0ULL : 0);
            return 1;
        }
        if (op == 1 && cmode <= 13)
            imm64 = ~imm64; // MVNI
        gen(state, (unsigned long) gadget_arm64_vconst);
        gen(state, rd);
        gen(state, imm64);
        gen(state, q ? imm64 : 0);
        return 1;
    }

    // LSE atomic memory operations: LDADD/LDCLR/LDEOR/LDSET/LDSMAX/
    // LDSMIN/LDUMAX/LDUMIN (opc 0-7) and SWP (opc 8). A/R
    // acquire/release bits accepted and ignored (single-stepped-CPU
    // model). Ported from OpenMinis' 0x38200000 branch; unlike theirs
    // (multi-gadget with separate barriers) this is one C-helper gadget.
    // The ...AL/...L "st" store-only aliases share this encoding with
    // Rt==31 (result discarded) and decode identically.
    if ((insn & 0x3f200c00) == 0x38200000) {
        extern void gadget_arm64_lse_rmw(void);
        unsigned size = (insn >> 30) & 3;
        unsigned rs = (insn >> 16) & 0x1f;
        unsigned opc = (insn >> 12) & 7;
        unsigned ar = (insn >> 22) & 3; // bit23=A, bit22=R (accepted, ignored)
        unsigned o3 = (insn >> 15) & 1; // 0 = LD<op>/ST<op>, 1 = SWP when opc=0
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        (void) ar;
        // SWP is opc=0 with bit15(o3)=1; the eight LD<op> forms have o3=0.
        unsigned op;
        if (o3) {
            if (opc != 0)
                return gen_arm64_undefined(state);
            op = 8; // SWP
        } else {
            op = opc;
        }
        gen(state, (unsigned long) gadget_arm64_lse_rmw);
        gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rs << 16) |
                   ((uint64_t) size << 24) | ((uint64_t) op << 26));
        gen(state, state->arm64_orig_ip);
        return 1;
    }

    // CAS (LSE compare-and-swap, single register) — field layout matches
    // arm64_execute()'s, INCLUDING the ordering constraint documented
    // there: this check must come before the broader exclusive-family
    // check below, whose mask also matches CAS encodings. The mask pins
    // bit23=1 (size:001000:1:L:1:Rs:o0:11111:Rn:Rt): bit23=0 in this
    // slot is CASP (compare-and-swap PAIR), handled separately below —
    // an earlier mask that left bit23 free silently compiled CASP down
    // to the byte/halfword single-register CAS gadgets, corrupting guest
    // memory. A/R (acquire/release) bits accepted and ignored, same as
    // the interpreter. Params: [rt | rn<<8 | rs<<16].
    if ((insn & 0x3fa00c00) == 0x08a00c00) {
        extern void gadget_arm64_cas8(void), gadget_arm64_cas16(void);
        extern void gadget_arm64_cas32(void), gadget_arm64_cas64(void);
        unsigned size = (insn >> 30) & 0x3;
        unsigned rs = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        static void *const cas_gadgets[4] = {
            (void *) gadget_arm64_cas8, (void *) gadget_arm64_cas16,
            (void *) gadget_arm64_cas32, (void *) gadget_arm64_cas64,
        };
        gen(state, (unsigned long) cas_gadgets[size]);
        gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rs << 16));
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // CASP/CASPA/CASPL/CASPAL (LSE compare-and-swap pair):
    // 0:sz:001000:0:L:1:Rs:o0:11111:Rn:Rt. bit30 (sz) selects a pair of
    // 32-bit or 64-bit registers; bit23=0 distinguishes it from the
    // single-register CAS above; bit31=0 distinguishes it from LDXP/STXP
    // (handled in the exclusive family below). Like CAS,
    // this must precede the exclusive-family check, whose 0x3f000000 mask
    // also matches CASP encodings. L (acquire, bit22) and o0 (release,
    // bit15) are accepted and ignored, same as the CAS path. Rs and Rt
    // must be even and Rt2 must be 11111, else UNDEFINED (ARM ARM).
    // The guest advertises HWCAP_ATOMICS (kernel/exec.c), so gcc/LLVM
    // outline-atomics emit CASP for 16-byte atomics — real software hits
    // this. Params: [rt | rn<<8 | rs<<16]; Rs+1/Rt+1 are the pair highs.
    if ((insn & 0xbfa00000) == 0x08200000) {
        extern void gadget_arm64_casp32(void), gadget_arm64_casp64(void);
        unsigned sz64 = (insn >> 30) & 1;
        unsigned rs = (insn >> 16) & 0x1f;
        unsigned rt2 = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (rt2 != 0x1f || (rs & 1) || (rt & 1)) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) (sz64 ? gadget_arm64_casp64
                                         : gadget_arm64_casp32));
        gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rs << 16));
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // Load/store exclusive + load-acquire/store-release family. Decode
    // mirrors arm64_execute()'s field layout (size:001000:o2:L:o1:Rs:o0:
    // Rt2:Rn:Rt), with one widening: o2=1,o1=0 (non-exclusive LDAR/STLR/
    // LDLAR/STLLR) lowers to the plain single-register load/store gadgets
    // (mode 0, offset 0) — their exact semantics minus the unmodeled
    // barrier — where the interpreter still rejects them. o0 (LDAXR/
    // STLXR's acquire/release bit) is accepted and ignored for the
    // exclusive forms, same as the interpreter. Pair exclusives (o1=1,
    // now necessarily LDXP/STXP — CASP encodings, which this mask also
    // matches, are consumed by the dedicated check above) lower to the
    // ldxp/stxp gadgets.
    if ((insn & 0x3f000000) == 0x08000000) {
        extern void gadget_arm64_ldxr8(void), gadget_arm64_ldxr16(void);
        extern void gadget_arm64_ldxr32(void), gadget_arm64_ldxr64(void);
        extern void gadget_arm64_stxr8(void), gadget_arm64_stxr16(void);
        extern void gadget_arm64_stxr32(void), gadget_arm64_stxr64(void);
        unsigned size = (insn >> 30) & 0x3;
        unsigned o2 = (insn >> 23) & 1;
        unsigned L = (insn >> 22) & 1;
        unsigned o1 = (insn >> 21) & 1;
        unsigned rs = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rt = insn & 0x1f;
        if (o1 != 0) {
            // LDXP/STXP/LDAXP/STLXP (load/store exclusive PAIR):
            // 1:sz:001000:0:L:1:Rs:o0:Rt2:Rn:Rt. bit31 must be 1 — the
            // bit31=0 half of this slot is CASP, consumed by the check
            // above — and o2 must be 0 (o2=1,o1=1 is the CAS space; any
            // encoding there that escaped the dedicated CAS check is
            // genuinely undefined). bit30 (sz) picks a pair of 32-bit or
            // 64-bit registers. o0 (acquire/release) is accepted and
            // ignored like everywhere else in this family. Compilers
            // only emit these when NOT emitting LSE (-mno-outline-atomics
            // on v8.0 baseline, or older toolchains): a 16-byte atomic is
            // an LDXP/STXP loop instead of CASP — real software hits
            // this. Unlike CASP there is no evenness constraint: Rt, Rt2
            // and Rs are independent registers (31 = XZR / discard).
            // LDXP params: [rt | rn<<8 | rt2<<16];
            // STXP params: [rt | rn<<8 | rs<<16 | rt2<<24].
            extern void gadget_arm64_ldxp32(void), gadget_arm64_ldxp64(void);
            extern void gadget_arm64_stxp32(void), gadget_arm64_stxp64(void);
            unsigned rt2 = (insn >> 10) & 0x1f;
            if (!(insn >> 31) || o2 != 0)
                return gen_arm64_undefined(state);
            unsigned sz64 = size & 1;
            if (L) {
                gen(state, (unsigned long) (sz64 ? gadget_arm64_ldxp64
                                                 : gadget_arm64_ldxp32));
                gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rt2 << 16));
            } else {
                gen(state, (unsigned long) (sz64 ? gadget_arm64_stxp64
                                                 : gadget_arm64_stxp32));
                gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rs << 16) |
                           ((uint64_t) rt2 << 24));
            }
            gen(state, state->arm64_orig_ip); // fault-restart address
            return 1;
        }
        if (o2 == 1) {
            // LDAR/STLR: an ordered but non-exclusive plain access. The
            // plain gadget performs the access; the ORDERING must be
            // preserved with a real host barrier, because guest threads
            // run on concurrent host threads and V8 publishes objects
            // cross-thread with exactly this pair (release-store a
            // pointer/flag, acquire-load it elsewhere, then read the
            // data it guards). Lowering these to plain accesses let the
            // consumer thread observe the publication before the
            // published data — Sparkplug/GC threads compiled from
            // half-published SharedFunctionInfo/BytecodeArray state,
            // which surfaced as npm/node crashing ~50% of runs with a
            // LoadIC probing a feedback slot that belongs to a different
            // IC kind (Smi(kMaxInt) deref -> SIGSEGV / V8 CHECK
            // failures). A full barrier BEFORE a store-release and AFTER
            // a load-acquire is conservative but correct; barriers are
            // idempotent so fault-restart re-executing them is fine.
            extern void gadget_arm64_barrier(void);
            if (!L)
                gen(state, (unsigned long) gadget_arm64_barrier);
            void *gadget = gen_arm64_ldst_single_gadget(size, L ? 1 : 0);
            gen(state, (unsigned long) gadget);
            gen(state, rt | ((uint64_t) rn << 8) | (0ULL << 16));
            gen(state, 0);
            gen(state, state->arm64_orig_ip); // fault-restart address
            if (L)
                gen(state, (unsigned long) gadget_arm64_barrier);
            return 1;
        }
        static void *const ldxr_gadgets[4] = {
            (void *) gadget_arm64_ldxr8, (void *) gadget_arm64_ldxr16,
            (void *) gadget_arm64_ldxr32, (void *) gadget_arm64_ldxr64,
        };
        static void *const stxr_gadgets[4] = {
            (void *) gadget_arm64_stxr8, (void *) gadget_arm64_stxr16,
            (void *) gadget_arm64_stxr32, (void *) gadget_arm64_stxr64,
        };
        gen(state, (unsigned long) (L ? ldxr_gadgets[size] : stxr_gadgets[size]));
        gen(state, rt | ((uint64_t) rn << 8) | ((uint64_t) rs << 16));
        gen(state, state->arm64_orig_ip); // fault-restart address (see memory.S's segfault paths)
        return 1;
    }

    // B, BL — mask matches arm64_execute()'s (bits[30:26]=00101). Target
    // computed at compile time (PC-relative, compile-time-known). Branches
    // always end the block — see control.S's header comment on why these
    // gadgets exit to C via jit_ret rather than inline-chaining.
    if ((insn & 0x7c000000) == 0x14000000) {
        bool is_bl = (insn >> 31) & 1;
        // Real bug caught by testing: an earlier version reimplemented this
        // sign extension inline as `(raw << 2) | (~0u << 27)` cast to
        // int64_t — since the intermediate OR is computed in uint32_t, the
        // cast to int64_t zero-extends (source is unsigned) instead of
        // sign-extending, corrupting every negative (backward) branch
        // target by +2^32. Fixed by calling the already-correct
        // arm64_branch_imm26 (emu/arch/arm64/decode.h), used successfully
        // by the interpreter — one implementation, not two chances to get
        // the sign extension wrong.
        int64_t offset = arm64_branch_imm26(insn);
        uint64_t target = state->arm64_orig_ip + (uint64_t) offset;
        // Targets are emitted TAGGED (bit 63 set) and their stream indices
        // recorded in jump_ip[] so the C loop can patch them into direct
        // block-to-block links — see control.S's chaining header.
        if (is_bl) {
            gen(state, (unsigned long) gadget_arm64_bl);
            gen(state, target | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
            gen(state, state->arm64_ip); // return address
        } else {
            gen(state, (unsigned long) gadget_arm64_b);
            gen(state, target | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
        }
        return 0;
    }

    // B.cond — mask matches arm64_execute()'s (bits[31:25]=0101010, bit4=0).
    if ((insn & 0xff000010) == 0x54000000) {
        uint32_t cond = insn & 0xf;
        int64_t offset = arm64_branch_imm19(insn);
        uint64_t taken = state->arm64_orig_ip + (uint64_t) offset;
        uint64_t fallthrough = state->arm64_ip;
        static void *const bcond_gadgets[16] = {
            [0] = (void *) gadget_arm64_bcond_eq, [1] = (void *) gadget_arm64_bcond_ne,
            [2] = (void *) gadget_arm64_bcond_cs, [3] = (void *) gadget_arm64_bcond_cc,
            [4] = (void *) gadget_arm64_bcond_mi, [5] = (void *) gadget_arm64_bcond_pl,
            [6] = (void *) gadget_arm64_bcond_vs, [7] = (void *) gadget_arm64_bcond_vc,
            [8] = (void *) gadget_arm64_bcond_hi, [9] = (void *) gadget_arm64_bcond_ls,
            [10] = (void *) gadget_arm64_bcond_ge, [11] = (void *) gadget_arm64_bcond_lt,
            [12] = (void *) gadget_arm64_bcond_gt, [13] = (void *) gadget_arm64_bcond_le,
            [14] = (void *) gadget_arm64_b, [15] = (void *) gadget_arm64_b, // AL/NV: always taken
        };
        extern void gadget_arm64_bcond_nf_eq(void), gadget_arm64_bcond_nf_ne(void);
        extern void gadget_arm64_bcond_nf_cs(void), gadget_arm64_bcond_nf_cc(void);
        extern void gadget_arm64_bcond_nf_mi(void), gadget_arm64_bcond_nf_pl(void);
        extern void gadget_arm64_bcond_nf_vs(void), gadget_arm64_bcond_nf_vc(void);
        extern void gadget_arm64_bcond_nf_hi(void), gadget_arm64_bcond_nf_ls(void);
        extern void gadget_arm64_bcond_nf_ge(void), gadget_arm64_bcond_nf_lt(void);
        extern void gadget_arm64_bcond_nf_gt(void), gadget_arm64_bcond_nf_le(void);
        static void *const bcond_nf_gadgets[14] = {
            (void *) gadget_arm64_bcond_nf_eq, (void *) gadget_arm64_bcond_nf_ne,
            (void *) gadget_arm64_bcond_nf_cs, (void *) gadget_arm64_bcond_nf_cc,
            (void *) gadget_arm64_bcond_nf_mi, (void *) gadget_arm64_bcond_nf_pl,
            (void *) gadget_arm64_bcond_nf_vs, (void *) gadget_arm64_bcond_nf_vc,
            (void *) gadget_arm64_bcond_nf_hi, (void *) gadget_arm64_bcond_nf_ls,
            (void *) gadget_arm64_bcond_nf_ge, (void *) gadget_arm64_bcond_nf_lt,
            (void *) gadget_arm64_bcond_nf_gt, (void *) gadget_arm64_bcond_nf_le,
        };
        if (cond >= 14) {
            // Always-taken: just an unconditional branch to the target.
            gen(state, (unsigned long) gadget_arm64_b);
            gen(state, taken | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
        } else if (arm64_flags_were_live) {
            // Compare+branch fusion: host NZCV is still live from the
            // immediately preceding fast flag-setting gadget — branch on
            // it directly, skipping load_flags' serializing msr nzcv.
            gen(state, (unsigned long) bcond_nf_gadgets[cond]);
            gen(state, taken | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
            gen(state, fallthrough | 0x8000000000000000ULL);
            state->jump_ip[1] = state->size - 1;
            return 0;
        } else {
            gen(state, (unsigned long) bcond_gadgets[cond]);
            gen(state, taken | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
            gen(state, fallthrough | 0x8000000000000000ULL);
            state->jump_ip[1] = state->size - 1;
        }
        return 0;
    }

    // CBZ, CBNZ — mask matches arm64_execute()'s (bits[30:25]=011010).
    if ((insn & 0x7e000000) == 0x34000000) {
        bool sf = (insn >> 31) & 1;
        bool is_cbnz = (insn >> 24) & 1;
        unsigned rt = insn & 0x1f;
        int64_t offset = arm64_branch_imm19(insn);
        uint64_t taken = state->arm64_orig_ip + (uint64_t) offset;
        uint64_t fallthrough = state->arm64_ip;
        gen(state, (unsigned long) (is_cbnz ? gadget_arm64_cbnz : gadget_arm64_cbz));
        gen(state, rt | ((uint64_t) sf << 8));
        gen(state, taken | 0x8000000000000000ULL);
        state->jump_ip[0] = state->size - 1;
        gen(state, fallthrough | 0x8000000000000000ULL);
        state->jump_ip[1] = state->size - 1;
        return 0;
    }

    // TBZ, TBNZ — mask matches arm64_execute()'s (bits[30:25]=011011).
    if ((insn & 0x7e000000) == 0x36000000) {
        bool is_tbnz = (insn >> 24) & 1;
        unsigned b5 = (insn >> 31) & 1;
        unsigned b40 = (insn >> 19) & 0x1f;
        unsigned bit_pos = (b5 << 5) | b40;
        unsigned rt = insn & 0x1f;
        int64_t offset = arm64_branch_imm14(insn);
        uint64_t taken = state->arm64_orig_ip + (uint64_t) offset;
        uint64_t fallthrough = state->arm64_ip;
        gen(state, (unsigned long) (is_tbnz ? gadget_arm64_tbnz : gadget_arm64_tbz));
        gen(state, rt | ((uint64_t) bit_pos << 8));
        gen(state, taken | 0x8000000000000000ULL);
        state->jump_ip[0] = state->size - 1;
        gen(state, fallthrough | 0x8000000000000000ULL);
        state->jump_ip[1] = state->size - 1;
        return 0;
    }

    // BR, BLR, RET — mask matches arm64_execute()'s (bits[31:25]=1101011).
    if ((insn & 0xfe000000) == 0xd6000000) {
        unsigned opc = (insn >> 21) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        // These three are the whole of this engine's remaining exits to C --
        // every compile-time-target edge already chains. The _cached variants
        // add a jit_frame.ret_cache lookup on the computed target and dispatch
        // straight into an already-translated block; see ret_cache_dispatch in
        // jit/guest-arm64/control.S. Gated so both arms live in one binary
        // (/proc/ish/arm64_jit_fuse retcache), with the OFF arm byte-for-byte
        // the gadget that shipped before. All THREE get it, not just ret: br is
        // PLT and switch-table dispatch, blr is every indirect call, and the
        // cache is keyed by target rather than by call site, so they are the
        // identical lookup. On riscv64 the indirect half was worth 3.4x on its
        // own, i.e. far more than the returns.
        extern void gadget_arm64_br_cached(void);
        extern void gadget_arm64_blr_cached(void);
        extern void gadget_arm64_ret_cached(void);
        bool retcache = (arm64_jit_fuse_mask() & JIT_FUSE_A64_RETCACHE) != 0;
        switch (opc) {
        case 0: // BR
            gen(state, (unsigned long) (retcache ? gadget_arm64_br_cached
                                                 : gadget_arm64_br));
            gen(state, rn);
            break;
        case 1: // BLR
            gen(state, (unsigned long) (retcache ? gadget_arm64_blr_cached
                                                 : gadget_arm64_blr));
            gen(state, rn);
            gen(state, state->arm64_ip); // return address
            break;
        case 2: // RET
            gen(state, (unsigned long) (retcache ? gadget_arm64_ret_cached
                                                 : gadget_arm64_ret));
            gen(state, rn);
            break;
        default:
            return gen_arm64_undefined(state);
        }
        return 0;
    }

    // SVC — fixed encoding, matches arm64_execute()'s mask.
    if ((insn & 0xffe0001f) == 0xd4000001) {
        gen(state, (unsigned long) gadget_arm64_svc);
        gen(state, state->arm64_ip); // next instruction, already advanced above
        return 0; // block ends: SVC always exits to the main loop
    }

    // ---- Integer data-processing batch 2 (jit/guest-arm64/dpextra.S) -----

    // Bitfield: SBFM/BFM/UBFM (and all their aliases: LSL/LSR/ASR
    // immediate, UBFX/SBFX/BFI/BFXIL, SXTB/SXTH/SXTW/UXTB/UXTH). L and S
    // are the compile-time double-shift parameters — see dpextra.S's
    // header for the lowering and its verification against the ARM ARM.
    if ((insn & 0x1f800000) == 0x13000000) {
        extern void gadget_arm64_sbfm(void), gadget_arm64_ubfm(void);
        extern void gadget_arm64_bfm(void);
        bool sf = (insn >> 31) & 1;
        unsigned opc = (insn >> 29) & 0x3;
        unsigned N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3f;
        unsigned imms = (insn >> 10) & 0x3f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (opc == 3 || N != sf || (!sf && ((immr | imms) & 0x20))) {
            return gen_arm64_undefined(state);
        }
        unsigned R = sf ? 64 : 32;
        unsigned L = R - 1 - imms;
        unsigned S = (L + immr) % R;
        uint64_t params = rd | ((uint64_t) rn << 8) | ((uint64_t) sf << 16)
            | ((uint64_t) L << 24) | ((uint64_t) S << 32);
        if (opc == 1) { // BFM: also needs the insert mask
            unsigned width = imms >= immr ? imms - immr + 1 : imms + 1;
            unsigned lsbpos = imms >= immr ? 0 : R - immr;
            uint64_t M = (width >= 64 ? ~0ULL : (1ULL << width) - 1) << lsbpos;
            gen(state, (unsigned long) gadget_arm64_bfm);
            gen(state, params);
            gen(state, M);
        } else {
            gen(state, (unsigned long) (opc == 0 ? gadget_arm64_sbfm : gadget_arm64_ubfm));
            gen(state, params);
        }
        return 1;
    }

    // EXTR (and the ROR-immediate alias, Rn==Rm).
    if ((insn & 0x1f800000) == 0x13800000) {
        extern void gadget_arm64_extr(void);
        bool sf = (insn >> 31) & 1;
        unsigned op21 = (insn >> 29) & 0x3;
        unsigned N = (insn >> 22) & 1;
        unsigned o0 = (insn >> 21) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned imms = (insn >> 10) & 0x3f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (op21 != 0 || N != sf || o0 != 0 || (!sf && (imms & 0x20))) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_extr);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) sf << 24) | ((uint64_t) imms << 32));
        return 1;
    }

    // Conditional select: CSEL/CSINC/CSINV/CSNEG (and CSET/CSETM/CINC/
    // CINV/CNEG aliases, which are just these with Rn=Rm=ZR and/or the
    // condition inverted — handled naturally by the decode). Emitted as a
    // [cond_<cc> gadget][csel gadget] pair — see dpextra.S's header for
    // the w10 handoff convention.
    if ((insn & 0x3fe00800) == 0x1a800000) {
        extern void gadget_arm64_csel(void);
        bool sf = (insn >> 31) & 1;
        unsigned op = (((insn >> 30) & 1) << 1) | ((insn >> 10) & 1);
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned cond = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        gen(state, (unsigned long) gen_arm64_cond_gadget(cond));
        gen(state, (unsigned long) gadget_arm64_csel);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) sf << 24) | ((uint64_t) op << 25));
        return 1;
    }

    // Conditional compare: CCMP/CCMN, immediate and register forms. Same
    // [cond gadget][consumer] pair as CSEL.
    if ((insn & 0x3fe00410) == 0x3a400000) {
        extern void gadget_arm64_ccmp(void);
        bool sf = (insn >> 31) & 1;
        bool is_cmn = !((insn >> 30) & 1); // op=0: CCMN (adds), op=1: CCMP (subs)
        bool is_imm = (insn >> 11) & 1;
        unsigned val = (insn >> 16) & 0x1f; // imm5 or Rm
        unsigned cond = (insn >> 12) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned nzcv = insn & 0xf;
        gen(state, (unsigned long) gen_arm64_cond_gadget(cond));
        gen(state, (unsigned long) gadget_arm64_ccmp);
        gen(state, rn | ((uint64_t) val << 8) | ((uint64_t) is_imm << 16)
            | ((uint64_t) is_cmn << 17) | ((uint64_t) sf << 18) | ((uint64_t) nzcv << 19));
        return 1;
    }

    // Data-processing (2 source): UDIV/SDIV/LSLV/LSRV/ASRV/RORV, plus the
    // CRC32{B,H,W,X}/CRC32C{B,H,W,X} opcodes (0x10-0x17; ID_AA64ISAR0/
    // HWCAP advertise them, computed natively when the host has FEAT_CRC32
    // — A10+ — and in C otherwise). PACGA stays unimplemented.
    if ((insn & 0x7fe00000) == 0x1ac00000) {
        extern void gadget_arm64_dp2src(void);
        extern void gadget_arm64_crc32(void);
        bool sf = (insn >> 31) & 1;
        unsigned opcode = (insn >> 10) & 0x3f;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        if (opcode >= 0x10 && opcode <= 0x17) {
            // CRC32: sz = opcode<1:0> (b/h/w/x), C variant = opcode<2>.
            // The X form requires sf=1, the others sf=0.
            extern void gadget_arm64_crc32_soft(void);
            unsigned crc_sz = opcode & 3;
            bool crc_c = opcode & 4;
            if ((crc_sz == 3) != sf)
                return gen_arm64_undefined(state);
            gen(state, (unsigned long) (arm64_host_has_crc32
                    ? gadget_arm64_crc32 : gadget_arm64_crc32_soft));
            gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
                | ((uint64_t) crc_sz << 24) | ((uint64_t) crc_c << 26));
            return 1;
        }
        // opcode -> gadget op: 000010 UDIV, 000011 SDIV, 001000 LSLV,
        // 001001 LSRV, 001010 ASRV, 001011 RORV
        int op = opcode == 0x02 ? 0 : opcode == 0x03 ? 1
               : opcode == 0x08 ? 2 : opcode == 0x09 ? 3
               : opcode == 0x0a ? 4 : opcode == 0x0b ? 5 : -1;
        if (op < 0) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_dp2src);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) sf << 24) | ((uint64_t) op << 25));
        return 1;
    }

    // Data-processing (1 source): RBIT/REV16/REV32/REV/CLZ/CLS.
    if ((insn & 0x7fe00000) == 0x5ac00000) {
        extern void gadget_arm64_dp1src(void);
        bool sf = (insn >> 31) & 1;
        unsigned opcode2 = (insn >> 16) & 0x1f;
        unsigned opcode = (insn >> 10) & 0x3f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        // opcode -> gadget op (same numbering): 0 RBIT, 1 REV16,
        // 2 REV32(x)/REV(w), 3 REV(x, 64-bit only), 4 CLZ, 5 CLS
        if (opcode2 != 0 || opcode > 5 || (opcode == 3 && !sf)) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_dp1src);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) sf << 16)
            | ((uint64_t) opcode << 17));
        return 1;
    }

    // Data-processing (3 source): MADD/MSUB (and the MUL/MNEG aliases),
    // SMADDL/SMSUBL/UMADDL/UMSUBL (and SMULL/UMULL), SMULH/UMULH.
    if ((insn & 0x7f000000) == 0x1b000000) {
        extern void gadget_arm64_dp3src(void);
        bool sf = (insn >> 31) & 1;
        unsigned op31 = (insn >> 21) & 0x7;
        unsigned o0 = (insn >> 15) & 1;
        unsigned rm = (insn >> 16) & 0x1f;
        unsigned ra = (insn >> 10) & 0x1f;
        unsigned rn = (insn >> 5) & 0x1f;
        unsigned rd = insn & 0x1f;
        // (op31, o0) -> gadget op: see dpextra.S. Widening/high forms
        // require sf=1 (the sf=0 encodings are unallocated).
        int op = -1;
        if (op31 == 0)
            op = o0 ? 1 : 0; // MSUB : MADD
        else if (op31 == 1 && sf)
            op = o0 ? 3 : 2; // SMSUBL : SMADDL
        else if (op31 == 2 && !o0 && sf)
            op = 4; // SMULH
        else if (op31 == 5 && sf)
            op = o0 ? 6 : 5; // UMSUBL : UMADDL
        else if (op31 == 6 && !o0 && sf)
            op = 7; // UMULH
        if (op < 0) {
            return gen_arm64_undefined(state);
        }
        gen(state, (unsigned long) gadget_arm64_dp3src);
        gen(state, rd | ((uint64_t) rn << 8) | ((uint64_t) rm << 16)
            | ((uint64_t) ra << 24) | ((uint64_t) sf << 32) | ((uint64_t) op << 33));
        return 1;
    }

    // Hint space (NOP/YIELD/WFE/WFI/SEV/SEVL/BTI/PACIASP...): all
    // architecturally allowed to behave as NOP for userspace emulation.
    if ((insn & 0xfffff01f) == 0xd503201f)
        return 1;

    // CLREX: clears the exclusive monitor. Must be matched BEFORE the
    // barrier space below — 0xd503305f fits the barrier mask (CRm is a
    // don't-care there) and used to decode as a plain DMB, leaving the
    // reservation armed across the CLREX (spurious STXR success).
    if ((insn & 0xfffff0ff) == 0xd503305f) {
        extern void gadget_arm64_clrex(void);
        gen(state, (unsigned long) gadget_arm64_clrex);
        return 1;
    }

    // Barrier space (DSB/DMB/ISB/SB...): one full-strength host barrier
    // covers all of them — see dpextra.S.
    if ((insn & 0xfffff01f) == 0xd503301f) {
        extern void gadget_arm64_barrier(void);
        gen(state, (unsigned long) gadget_arm64_barrier);
        return 1;
    }

    // EL0 cache maintenance (SYS op1=3, CRn=C7): IC IVAU and the DC
    // clean/invalidate-by-VA family, which Linux exposes to userspace via
    // SCTLR_EL1.{UCI} and __clear_cache/__aarch64_sync_cache_range emits
    // after runtime code generation (first seen: syslog-ng). Translated-
    // block invalidation here is driven by the guest's WRITES to code
    // pages, which have already happened by the time the cache ops run,
    // so these are architecturally safe no-ops — same self-modifying-code
    // model as the x86 guests, no gadget emitted (hint-space precedent
    // above). Whitelisted CRm values only: DC ZVA (CRm=4, same op1/CRn/
    // op2 space) must keep faulting because DCZID_EL0 advertises DZP=1
    // (see the MRS below), and unallocated encodings stay UNDEFINED.
    // EL1-only maintenance ops have op1!=3 and never match this mask.
    if ((insn & 0xfffff0e0) == 0xd50b7020) {
        unsigned crm = (insn >> 8) & 0xf;
        if (crm == 0x5) {
            // IC IVAU must NOT be a no-op: write-driven block invalidation
            // only fires on the TLB write-MISS path, so a code rewrite
            // through a still-cached writable TLB entry is invisible and
            // this instruction is the guest's only invalidation signal.
            // Leaving it out let V8's recycled code addresses keep running
            // their stale old translation (npm install died ~50% of runs);
            // distilled A/B: tests/manual/arm64/smc_stale_block.c. Xt=31
            // is XZR (VA 0): architecturally valid but nothing compiles
            // from page 0, keep it a no-op.
            unsigned rt = insn & 0x1f;
            if (rt != 31) {
                extern void gadget_arm64_ic_ivau(void);
                gen(state, (unsigned long) gadget_arm64_ic_ivau);
                gen(state, rt);
            }
            return 1;
        }
        if (crm == 0xa || crm == 0xb ||  // DC CVAC, DC CVAU
            crm == 0xc || crm == 0xd ||  // DC CVAP, DC CVADP
            crm == 0xe)                  // DC CIVAC
            return 1;
        // fall through: DC ZVA / EL1-only ops reject below
    }

    // MRS/MSR TPIDR_EL0 — the TLS base register (see cpu_state.arm64_tpidr).
    if ((insn & 0xffffffe0) == 0xd53bd040) {
        extern void gadget_arm64_mrs_tpidr(void);
        gen(state, (unsigned long) gadget_arm64_mrs_tpidr);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd51bd040) {
        extern void gadget_arm64_msr_tpidr(void);
        gen(state, (unsigned long) gadget_arm64_msr_tpidr);
        gen(state, insn & 0x1f);
        return 1;
    }

    // MRS/MSR FPCR and FPSR — stored in cpu_state, not installed on the
    // host (see fp.S's rounding-mode note).
    if ((insn & 0xffffffe0) == 0xd53b4400) {
        extern void gadget_arm64_mrs_fpcr(void);
        gen(state, (unsigned long) gadget_arm64_mrs_fpcr);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd51b4400) {
        extern void gadget_arm64_msr_fpcr(void);
        gen(state, (unsigned long) gadget_arm64_msr_fpcr);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd53b4420) {
        extern void gadget_arm64_mrs_fpsr(void);
        gen(state, (unsigned long) gadget_arm64_mrs_fpsr);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd51b4420) {
        extern void gadget_arm64_msr_fpsr(void);
        gen(state, (unsigned long) gadget_arm64_msr_fpsr);
        gen(state, insn & 0x1f);
        return 1;
    }

    // MRS of constant system registers, via the generic mrs_const gadget:
    // CTR_EL0 (cache-line geometry: 64-byte I/D lines, PIPT, the standard
    // QEMU-user value) and DCZID_EL0 with DZP=1 (DC ZVA prohibited, so
    // libc memset never tries the zeroing instruction and no DC ZVA
    // emulation is needed).
    if ((insn & 0xffffffe0) == 0xd53b0020 || (insn & 0xffffffe0) == 0xd53b00e0) {
        extern void gadget_arm64_mrs_const(void);
        bool is_ctr = (insn & 0xffffffe0) == 0xd53b0020;
        gen(state, (unsigned long) gadget_arm64_mrs_const);
        gen(state, insn & 0x1f);
        gen(state, is_ctr ? 0x8444c004ULL : 0x10ULL);
        return 1;
    }

    // MRS Xt, NZCV / MSR NZCV, Xt (compiler-generated flag save/restore;
    // ported from OpenMinis d53b4200/d51b4200).
    if ((insn & 0xffffffe0) == 0xd53b4200) {
        extern void gadget_arm64_mrs_nzcv(void);
        gen(state, (unsigned long) gadget_arm64_mrs_nzcv);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd51b4200) {
        extern void gadget_arm64_msr_nzcv(void);
        gen(state, (unsigned long) gadget_arm64_msr_nzcv);
        gen(state, insn & 0x1f);
        return 1;
    }

    // MSR DIT, Xt (d51b4220): data-independent timing hint the Go runtime
    // toggles around async preemption — a no-op under emulation
    // (OpenMinis treats it the same way).
    if ((insn & 0xffffffe0) == 0xd51b4220)
        return 1; // emit nothing; fall through to the next instruction

    // MRS Xt, CNTVCT_EL0 (virtual counter) / CNTFRQ_EL0 (frequency):
    // runtime monotonic nanoseconds + constant 1 GHz.
    if ((insn & 0xffffffe0) == 0xd53be040) {
        extern void gadget_arm64_mrs_cntvct(void);
        gen(state, (unsigned long) gadget_arm64_mrs_cntvct);
        gen(state, insn & 0x1f);
        return 1;
    }
    if ((insn & 0xffffffe0) == 0xd53be000) {
        extern void gadget_arm64_mrs_const(void);
        gen(state, (unsigned long) gadget_arm64_mrs_const);
        gen(state, insn & 0x1f);
        gen(state, 1000000000ULL);
        return 1;
    }

    // MRS of the CPU-feature ID registers (Linux traps and emulates these
    // for EL0, so real binaries do read them — musl/glibc feature probes).
    // Values advertise exactly what this JIT implements: base FP+AdvSIMD
    // (PFR0), AES+PMULL / SHA1 / SHA2+SHA512 / SHA3 / CRC32 / LSE atomics
    // (ISAR0). SHA512/SHA3/CRC32 hold on every host: gadgets fall back to
    // soft implementations where the host instruction is missing. The
    // rest read as zero. Ported from OpenMinis' d5300000 sysreg fallback,
    // with the values matched to OUR feature set rather than theirs.
    // Kept in sync with AT_HWCAP (kernel/exec.c).
    if ((insn & 0xfff00000) == 0xd5300000) {
        unsigned op1 = (insn >> 16) & 7, crn = (insn >> 12) & 0xf;
        unsigned crm = (insn >> 8) & 0xf, op2 = (insn >> 5) & 7;
        uint64_t value;
        bool known = true;
        if (op1 == 0 && crn == 0 && crm == 4 && op2 == 0)
            value = 0x11; // ID_AA64PFR0_EL1: EL0/EL1 AArch64, FP=0, AdvSIMD=0 (present)
        else if (op1 == 0 && crn == 0 && crm == 4 && (op2 == 1 || op2 == 4))
            value = 0;    // ID_AA64PFR1_EL1 / ID_AA64ZFR0_EL1
        else if (op1 == 0 && crn == 0 && crm == 6 && op2 == 0)
            // ID_AA64ISAR0_EL1: AES=2(+PMULL) SHA1=1 SHA2=2(+SHA512)
            // CRC32=1 Atomics=2(LSE) SHA3=1
            value = 0x212120 | (1ULL << 32);
        else if (op1 == 0 && crn == 0 && crm == 6 && op2 == 1)
            value = 0;    // ID_AA64ISAR1_EL1
        else if (op1 == 0 && crn == 0 && crm == 7 && (op2 == 0 || op2 == 1 || op2 == 2))
            value = 0;    // ID_AA64MMFR0/1/2_EL1
        else if (op1 == 0 && crn == 0 && crm == 5 && (op2 == 0 || op2 == 1))
            value = 0;    // ID_AA64DFR0/1_EL1
        else
            known = false;
        if (known) {
            extern void gadget_arm64_mrs_const(void);
            gen(state, (unsigned long) gadget_arm64_mrs_const);
            gen(state, insn & 0x1f);
            gen(state, value);
            return 1;
        }
        // unknown system register: fall through to SIGILL below
    }

    // BRK #imm16: debug breakpoint -> SIGTRAP (__builtin_trap, abort
    // fast-paths, debuggers). Ported from OpenMinis' d4200000 branch.
    if ((insn & 0xffe0001f) == 0xd4200000) {
        extern void gadget_arm64_interrupt(void);
        gen(state, (unsigned long) gadget_arm64_interrupt);
        gen(state, INT_BREAKPOINT);
        gen(state, state->arm64_orig_ip);
        gen(state, state->arm64_orig_ip);
        return 0;
    }

    // Not yet ported to a gadget — raise INT_UNDEFINED and end the block
    // here (everything decoded so far in this block still runs; this
    // instruction, when reached, cleanly signals SIGILL instead of being
    // silently misexecuted). The printk names the encoding so the next
    // bring-up gap is identifiable from a plain failure log — this is how
    // each successive real-rootfs blocker in the port has been found.
    printk("WARNING: arm64 JIT: no gadget for insn %#010x at pc %#llx\n",
           insn, (unsigned long long) state->arm64_orig_ip);
    return gen_arm64_undefined(state);
}

#else // !ISH_JIT_ARM64_GUEST

// No aarch64 host gadget set (see the #ifdef header above): there is no
// execution engine for an arm64 guest on this host. Fail loudly at the
// first attempt to compile arm64 guest code — a clear die() beats an
// undefined-symbol link failure or a crash into a missing gadget.
int gen_step_arm64(struct gen_state *state, struct tlb *tlb) {
    (void) tlb;
    die("arm64 guest binaries are not supported on this host "
        "(no aarch64 guest JIT; pc=%#llx)",
        (unsigned long long) state->arm64_ip);
}

#endif // ISH_JIT_ARM64_GUEST

// ---- RISC-V guest code generator -----------------------------------------
// Same host restriction and stub arrangement as the arm64 section above:
// the riscv64 guest gadgets (jit/guest-riscv64/*.S) are AArch64 assembly,
// gated by ISH_JIT_RISCV64_GUEST from meson.
#ifdef ISH_JIT_RISCV64_GUEST

bool gen_start_riscv64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->riscv64 = true;
    state->riscv64_ip = addr;
    state->riscv64_orig_ip = addr;
    return true;
}

#include "emu/arch/riscv64/decode.h"

// Byte offsets into cpu_state for gadget operands. rd==x0 writes are
// redirected to the zero sink; rs==x0 reads load the never-written
// riscv64_regs[0] slot (always 0). See emu/cpu.h's riscv64 block.
static unsigned long riscv64_rd_off(unsigned rd) {
    if (rd == 0)
        return offsetof(struct cpu_state, riscv64_zero_sink);
    return offsetof(struct cpu_state, riscv64_regs) + rd * sizeof(qword_t);
}
static unsigned long riscv64_rs_off(unsigned rs) {
    return offsetof(struct cpu_state, riscv64_regs) + rs * sizeof(qword_t);
}

static int gen_riscv64_interrupt_at(struct gen_state *state, int code,
        guest_addr_t pc, guest_addr_t addr) {
    extern void gadget_riscv64_interrupt(void);
    gen(state, (unsigned long) gadget_riscv64_interrupt);
    gen(state, code);
    gen(state, pc);
    gen(state, addr);
    return 0;
}

static int gen_riscv64_undefined(struct gen_state *state, uint32_t insn) {
    // The port's bring-up tool, same as arm64's: run a real binary, read
    // the encoding this logs, implement that instruction family next.
    printk("WARNING: riscv64 JIT: no gadget for insn %#010x at pc %#llx\n",
           insn, (unsigned long long) state->riscv64_orig_ip);
    return gen_riscv64_interrupt_at(state, INT_UNDEFINED,
            state->riscv64_orig_ip, state->riscv64_orig_ip);
}

static void gen_riscv64_mov_const(struct gen_state *state, unsigned rd, uint64_t value) {
    extern void gadget_riscv64_mov_const(void);
    gen(state, (unsigned long) gadget_riscv64_mov_const);
    gen(state, riscv64_rd_off(rd));
    gen(state, value);
}

// Fetch (but do not consume) the instruction at state->riscv64_ip.
// Returns its length in bytes (2 or 4) with the RVC-expanded encoding in
// *insn_out, or 0 if it can't be fetched, expanded, or isn't a standard
// 2/4-byte encoding. Mirrors the main fetch's split halfword reads so a
// peek never touches the page after a trailing compressed instruction.
static unsigned gen_riscv64_peek(struct gen_state *state, struct tlb *tlb,
        uint32_t *insn_out) {
    uint16_t low16;
    if (!tlb_read(tlb, state->riscv64_ip, &low16, sizeof(low16)))
        return 0;
    unsigned length = riscv64_insn_length(low16);
    if (length == 2) {
        uint32_t expanded = riscv64_expand_rvc(low16);
        if (expanded == 0)
            return 0;
        *insn_out = expanded;
        return 2;
    }
    if (length != 4)
        return 0;
    uint16_t high16;
    if (!tlb_read(tlb, state->riscv64_ip + 2, &high16, sizeof(high16)))
        return 0;
    *insn_out = (uint32_t) low16 | ((uint32_t) high16 << 16);
    return 4;
}

// lui/auipc + {addi, addiw, load} same-register pairs fold into a single
// gadget: the first instruction's result is compile-time known (gen knows
// the guest pc), so the pair costs one dispatch instead of two, and the
// load form (auipc+ld = GOT loads and other PC-relative accesses) also
// skips the runtime add by loading from an absolute address through the
// always-zero x0 slot. Measured on Alpine riscv64 busybox + musl + apk:
// auipc+addi covers 5.4% of static instructions, auipc+load 3.6%,
// lui+addi 0.7%.
//
// Consuming the second instruction is safe under the same rules as the
// arm64 guest fusions: a jump landing on the consumed instruction just
// compiles a fresh block that decodes it standalone, and the fused
// result/pc equal exactly what the unfused pair leaves. For the load
// form the fault-restart pc is the PAIR START: the folded constant has
// no runtime state, so replaying from the first instruction after a
// kernel-resolved fault recomputes the identical address.
static bool gen_riscv64_fold_const(struct gen_state *state, struct tlb *tlb,
        unsigned rd, uint64_t value) {
    uint32_t next;
    unsigned len = gen_riscv64_peek(state, tlb, &next);
    if (len == 0)
        return false;
    // Same page budget as gen_arm64_fits_block: consumed instructions
    // must not push the decoded range past one page from block start
    // (jit.c only enforces its cap between gen_step calls).
    if (state->riscv64_ip + len - state->block->addr > PAGE_SIZE)
        return false;
    if (riscv64_rd(next) != rd || riscv64_rs1(next) != rd)
        return false;
    unsigned opcode = riscv64_opcode(next);
    unsigned funct3 = riscv64_funct3(next);
    if (opcode == RISCV64_OP_OP_IMM && funct3 == 0) { // addi
        gen_riscv64_mov_const(state, rd,
                value + (uint64_t) riscv64_imm_i(next));
        state->riscv64_ip += len;
        return true;
    }
    if (opcode == RISCV64_OP_OP_IMM_32 && funct3 == 0) { // addiw (sext.w)
        gen_riscv64_mov_const(state, rd, (uint64_t) (int64_t) (int32_t)
                (value + (uint64_t) riscv64_imm_i(next)));
        state->riscv64_ip += len;
        return true;
    }
    if (opcode == RISCV64_OP_LOAD) {
        extern void gadget_riscv64_lb(void);
        extern void gadget_riscv64_lh(void);
        extern void gadget_riscv64_lw(void);
        extern void gadget_riscv64_ld(void);
        extern void gadget_riscv64_lbu(void);
        extern void gadget_riscv64_lhu(void);
        extern void gadget_riscv64_lwu(void);
        static void (*const load_gadgets[8])(void) = {
            gadget_riscv64_lb, gadget_riscv64_lh, gadget_riscv64_lw,
            gadget_riscv64_ld, gadget_riscv64_lbu, gadget_riscv64_lhu,
            gadget_riscv64_lwu, NULL,
        };
        void (*gadget)(void) = load_gadgets[funct3];
        if (gadget == NULL)
            return false;
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd));
        gen(state, riscv64_rs_off(0)); // always-zero slot: absolute address
        gen(state, value + (uint64_t) riscv64_imm_i(next));
        gen(state, state->riscv64_orig_ip); // pair start; ALWAYS last
        state->riscv64_ip += len;
        return true;
    }
    return false;
}

// Unconditional compile-time branch: ends the block. Target is tagged with
// bit 63 (unchained) for riscv64_branch_dispatch; gen_end turns the stream
// slot recorded in jump_ip[0] into a chainable word.
static int gen_riscv64_branch_to(struct gen_state *state, guest_addr_t target) {
    extern void gadget_riscv64_b(void);
    gen(state, (unsigned long) gadget_riscv64_b);
    gen(state, target | 0x8000000000000000ULL);
    state->jump_ip[0] = state->size - 1;
    return 0;
}


// CSR access helper, called through gadget_riscv64_call_helper (fp.S).
// Only the FP CSRs exist in this port: fflags (0x001) = fcsr[4:0],
// frm (0x002) = fcsr[7:5], fcsr (0x003) = fcsr[7:0]. Exception flags are
// whatever the guest last wrote — host FP status is not synced back
// (deviation; musl/printf only ever set the rounding mode).
// Also handles the read-only Zicntr counters (cycle/time/instret, 0xc00-2):
// there's no real cycle or instruction count to report, so all three alias
// a host monotonic nanosecond counter -- Go's runtime.nanotime() (compiled
// to `csrrs rd, time, x0`) only needs *a* monotonically increasing value.
// Write attempts to these never reach here (rejected as illegal
// instructions at decode time, see gen_step_riscv64's RISCV64_OP_SYSTEM).
void riscv64_csr_helper(struct cpu_state *cpu, unsigned long arg) {
    unsigned rd = arg & 31, rs1 = (arg >> 5) & 31;
    unsigned funct3 = (arg >> 10) & 7, csr = (unsigned) (arg >> 13);
    if (csr == 0xc00 || csr == 0xc01 || csr == 0xc02) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (rd != 0)
            cpu->riscv64_regs[rd] = (uint64_t) now.tv_sec * 1000000000ull + (uint64_t) now.tv_nsec;
        return;
    }
    dword_t fcsr = cpu->riscv64_fcsr;
    qword_t old = csr == 1 ? (fcsr & 0x1f)
                : csr == 2 ? ((fcsr >> 5) & 7)
                : (fcsr & 0xff);
    qword_t src = (funct3 & 4) ? rs1 : cpu->riscv64_regs[rs1];
    // csrrw/csrrwi always write; csrrs/c (and i forms) skip the write
    // side effect when the rs1/uimm field is 0 (both encode it there).
    bool write = (funct3 & 3) == 1 || rs1 != 0;
    if (write) {
        qword_t nv = (funct3 & 3) == 1 ? src
                   : (funct3 & 3) == 2 ? (old | src)
                   : (old & ~src);
        if (csr == 1)
            fcsr = (fcsr & ~0x1fu) | (nv & 0x1f);
        else if (csr == 2)
            fcsr = (fcsr & ~0xe0u) | ((nv & 7) << 5);
        else
            fcsr = nv & 0xff;
        cpu->riscv64_fcsr = fcsr;
    }
    if (rd != 0)
        cpu->riscv64_regs[rd] = old;
}


// fclass.{s,d}: classify into the 10 RISC-V class bits. Rare enough that a
// C helper through call_helper beats eight branches of assembly.
// arg: rd | rs1<<5 | is_d<<10
void riscv64_fclass_helper(struct cpu_state *cpu, unsigned long arg) {
    unsigned rd = arg & 31, rs1 = (arg >> 5) & 31;
    bool is_d = arg & (1 << 10);
    qword_t bits = cpu->riscv64_f[rs1];
    bool sign, is_inf, is_nan, is_sub, is_zero, is_quiet;
    if (is_d) {
        sign = bits >> 63;
        unsigned exp = (bits >> 52) & 0x7ff;
        qword_t frac = bits & 0xfffffffffffffULL;
        is_inf = exp == 0x7ff && frac == 0;
        is_nan = exp == 0x7ff && frac != 0;
        is_sub = exp == 0 && frac != 0;
        is_zero = exp == 0 && frac == 0;
        is_quiet = frac >> 51;
    } else {
        dword_t b = (dword_t) bits;
        sign = b >> 31;
        unsigned exp = (b >> 23) & 0xff;
        dword_t frac = b & 0x7fffff;
        is_inf = exp == 0xff && frac == 0;
        is_nan = exp == 0xff && frac != 0;
        is_sub = exp == 0 && frac != 0;
        is_zero = exp == 0 && frac == 0;
        is_quiet = frac >> 22;
    }
    unsigned cls;
    if (is_nan)
        cls = is_quiet ? 9 : 8;
    else if (is_inf)
        cls = sign ? 0 : 7;
    else if (is_zero)
        cls = sign ? 3 : 4;
    else if (is_sub)
        cls = sign ? 2 : 5;
    else
        cls = sign ? 1 : 6;
    if (rd != 0)
        cpu->riscv64_regs[rd] = 1u << cls;
}

int gen_step_riscv64(struct gen_state *state, struct tlb *tlb) {
    extern void gadget_riscv64_addi(void);
    extern void gadget_riscv64_add_rr(void);
    extern void gadget_riscv64_add_ri(void);
    extern void gadget_riscv64_and_rr(void);
    extern void gadget_riscv64_and_ri(void);
    extern void gadget_riscv64_or_rr(void);
    extern void gadget_riscv64_or_ri(void);
    extern void gadget_riscv64_xor_rr(void);
    extern void gadget_riscv64_xor_ri(void);
    extern void gadget_riscv64_sll_rr(void);
    extern void gadget_riscv64_sll_ri(void);
    extern void gadget_riscv64_srl_rr(void);
    extern void gadget_riscv64_srl_ri(void);
    extern void gadget_riscv64_sra_rr(void);
    extern void gadget_riscv64_sra_ri(void);
    extern void gadget_riscv64_slt_rr(void);
    extern void gadget_riscv64_slt_ri(void);
    extern void gadget_riscv64_sltu_rr(void);
    extern void gadget_riscv64_sltu_ri(void);
    extern void gadget_riscv64_addw_rr(void);
    extern void gadget_riscv64_addw_ri(void);
    extern void gadget_riscv64_sllw_rr(void);
    extern void gadget_riscv64_sllw_ri(void);
    extern void gadget_riscv64_srlw_rr(void);
    extern void gadget_riscv64_srlw_ri(void);
    extern void gadget_riscv64_sraw_rr(void);
    extern void gadget_riscv64_sraw_ri(void);
    extern void gadget_riscv64_sub_rr(void);
    extern void gadget_riscv64_subw_rr(void);
    extern void gadget_riscv64_mul_rr(void);
    extern void gadget_riscv64_mulw_rr(void);
    extern void gadget_riscv64_mulh_rr(void);
    extern void gadget_riscv64_mulhu_rr(void);
    extern void gadget_riscv64_mulhsu_rr(void);
    extern void gadget_riscv64_div_rr(void);
    extern void gadget_riscv64_divu_rr(void);
    extern void gadget_riscv64_rem_rr(void);
    extern void gadget_riscv64_remu_rr(void);
    extern void gadget_riscv64_divw_rr(void);
    extern void gadget_riscv64_divuw_rr(void);
    extern void gadget_riscv64_remw_rr(void);
    extern void gadget_riscv64_remuw_rr(void);
    extern void gadget_riscv64_beq(void);
    extern void gadget_riscv64_fmadd_d(void);
    extern void gadget_riscv64_fmsub_d(void);
    extern void gadget_riscv64_fnmsub_d(void);
    extern void gadget_riscv64_fnmadd_d(void);
    extern void gadget_riscv64_fmadd_s(void);
    extern void gadget_riscv64_fmsub_s(void);
    extern void gadget_riscv64_fnmsub_s(void);
    extern void gadget_riscv64_fnmadd_s(void);
    extern void gadget_riscv64_fadd_d(void);
    extern void gadget_riscv64_fadd_s(void);
    extern void gadget_riscv64_fsub_d(void);
    extern void gadget_riscv64_fsub_s(void);
    extern void gadget_riscv64_fmul_d(void);
    extern void gadget_riscv64_fmul_s(void);
    extern void gadget_riscv64_fdiv_d(void);
    extern void gadget_riscv64_fdiv_s(void);
    extern void gadget_riscv64_fmin_d(void);
    extern void gadget_riscv64_fmin_s(void);
    extern void gadget_riscv64_fmax_d(void);
    extern void gadget_riscv64_fmax_s(void);
    extern void gadget_riscv64_fsqrt_d(void);
    extern void gadget_riscv64_fsqrt_s(void);
    extern void gadget_riscv64_fsgnj_d(void);
    extern void gadget_riscv64_fsgnj_s(void);
    extern void gadget_riscv64_fsgnjn_d(void);
    extern void gadget_riscv64_fsgnjn_s(void);
    extern void gadget_riscv64_fsgnjx_d(void);
    extern void gadget_riscv64_fsgnjx_s(void);
    extern void gadget_riscv64_feq_d(void);
    extern void gadget_riscv64_feq_s(void);
    extern void gadget_riscv64_flt_d(void);
    extern void gadget_riscv64_flt_s(void);
    extern void gadget_riscv64_fle_d(void);
    extern void gadget_riscv64_fle_s(void);
    extern void gadget_riscv64_fcvt_d_w(void);
    extern void gadget_riscv64_fcvt_s_w(void);
    extern void gadget_riscv64_fcvtn_w_d(void);
    extern void gadget_riscv64_fcvtn_w_s(void);
    extern void gadget_riscv64_fcvtz_w_d(void);
    extern void gadget_riscv64_fcvtz_w_s(void);
    extern void gadget_riscv64_fcvt_d_wu(void);
    extern void gadget_riscv64_fcvt_s_wu(void);
    extern void gadget_riscv64_fcvtn_wu_d(void);
    extern void gadget_riscv64_fcvtn_wu_s(void);
    extern void gadget_riscv64_fcvtz_wu_d(void);
    extern void gadget_riscv64_fcvtz_wu_s(void);
    extern void gadget_riscv64_fcvt_d_l(void);
    extern void gadget_riscv64_fcvt_s_l(void);
    extern void gadget_riscv64_fcvtn_l_d(void);
    extern void gadget_riscv64_fcvtn_l_s(void);
    extern void gadget_riscv64_fcvtz_l_d(void);
    extern void gadget_riscv64_fcvtz_l_s(void);
    extern void gadget_riscv64_fcvt_d_lu(void);
    extern void gadget_riscv64_fcvt_s_lu(void);
    extern void gadget_riscv64_fcvtn_lu_d(void);
    extern void gadget_riscv64_fcvtn_lu_s(void);
    extern void gadget_riscv64_fcvtz_lu_d(void);
    extern void gadget_riscv64_fcvtz_lu_s(void);
    extern void gadget_riscv64_fcvt_d_s(void);
    extern void gadget_riscv64_fcvt_s_d(void);
    extern void gadget_riscv64_fmv_x_w(void);
    extern void gadget_riscv64_fmv_w_x(void);
    extern void gadget_riscv64_flw(void);
    extern void gadget_riscv64_lr_w(void);
    extern void gadget_riscv64_lr_d(void);
    extern void gadget_riscv64_sc_w(void);
    extern void gadget_riscv64_sc_d(void);
    extern void gadget_riscv64_amoswap_w(void);
    extern void gadget_riscv64_amoswap_d(void);
    extern void gadget_riscv64_amoadd_w(void);
    extern void gadget_riscv64_amoadd_d(void);
    extern void gadget_riscv64_amoxor_w(void);
    extern void gadget_riscv64_amoxor_d(void);
    extern void gadget_riscv64_amoand_w(void);
    extern void gadget_riscv64_amoand_d(void);
    extern void gadget_riscv64_amoor_w(void);
    extern void gadget_riscv64_amoor_d(void);
    extern void gadget_riscv64_amomin_w(void);
    extern void gadget_riscv64_amomin_d(void);
    extern void gadget_riscv64_amomax_w(void);
    extern void gadget_riscv64_amomax_d(void);
    extern void gadget_riscv64_amominu_w(void);
    extern void gadget_riscv64_amominu_d(void);
    extern void gadget_riscv64_amomaxu_w(void);
    extern void gadget_riscv64_amomaxu_d(void);
    extern void gadget_riscv64_lb(void);
    extern void gadget_riscv64_lbu(void);
    extern void gadget_riscv64_lh(void);
    extern void gadget_riscv64_lhu(void);
    extern void gadget_riscv64_lw(void);
    extern void gadget_riscv64_lwu(void);
    extern void gadget_riscv64_ld(void);
    extern void gadget_riscv64_sb(void);
    extern void gadget_riscv64_sh(void);
    extern void gadget_riscv64_sw(void);
    extern void gadget_riscv64_sd(void);
    extern void gadget_riscv64_bne(void);
    extern void gadget_riscv64_blt(void);
    extern void gadget_riscv64_bge(void);
    extern void gadget_riscv64_bltu(void);
    extern void gadget_riscv64_bgeu(void);
    extern void gadget_riscv64_jalr(void);
    state->riscv64_orig_ip = state->riscv64_ip;
    state->orig_ip_extra = 0;

    // Fetch 2 bytes first, then the upper half only for a 4-byte encoding:
    // a compressed instruction may be the last 2 bytes of a mapped page,
    // and reading 4 unconditionally would take a spurious fetch fault on
    // the following unmapped page.
    uint16_t low16;
    if (!tlb_read(tlb, state->riscv64_ip, &low16, sizeof(low16)))
        return gen_riscv64_interrupt_at(state, INT_GPF,
                state->riscv64_orig_ip, state->riscv64_orig_ip);
    uint32_t insn;
    unsigned length = riscv64_insn_length(low16);
    if (length == 2) {
        insn = riscv64_expand_rvc(low16);
        if (insn == 0) {
            // reserved/illegal compressed encoding
            uint32_t raw = low16;
            return gen_riscv64_undefined(state, raw);
        }
    } else {
        uint16_t high16;
        if (!tlb_read(tlb, state->riscv64_ip + 2, &high16, sizeof(high16)))
            return gen_riscv64_interrupt_at(state, INT_GPF,
                    state->riscv64_orig_ip, state->riscv64_orig_ip + 2);
        insn = (uint32_t) low16 | ((uint32_t) high16 << 16);
    }
    state->riscv64_ip += length;

    unsigned rd = riscv64_rd(insn);
    unsigned rs1 = riscv64_rs1(insn);
    unsigned funct3 = riscv64_funct3(insn);

    switch (riscv64_opcode(insn)) {
    case RISCV64_OP_LUI:
    case RISCV64_OP_AUIPC: {
        uint64_t value = (uint64_t) riscv64_imm_u(insn);
        if (riscv64_opcode(insn) == RISCV64_OP_AUIPC)
            value += state->riscv64_orig_ip;
        // rd == x0: the value is discarded, so a same-rd fold can't match
        // (rd_off is the zero sink but rs_off(0) is the real zero slot).
        if (rd != 0 && (riscv64_jit_fuse_mask() & JIT_FUSE_RV_FOLD) &&
                gen_riscv64_fold_const(state, tlb, rd, value))
            return 1;
        gen_riscv64_mov_const(state, rd, value);
        return 1;
    }

    case RISCV64_OP_OP_IMM: {
        int64_t imm = riscv64_imm_i(insn);
        if (funct3 == 0) { // addi (li/mv/nop forms included)
            if (rs1 == 0) {
                gen_riscv64_mov_const(state, rd, (uint64_t) imm);
            } else {
                extern void gadget_riscv64_addi(void);
                gen(state, (unsigned long) gadget_riscv64_addi);
                gen(state, riscv64_rd_off(rd));
                gen(state, riscv64_rs_off(rs1));
                gen(state, (uint64_t) imm);
            }
            return 1;
        }
        static void (*const op_imm_gadgets[8])(void) = {
            NULL, gadget_riscv64_sll_ri, gadget_riscv64_slt_ri,
            gadget_riscv64_sltu_ri, gadget_riscv64_xor_ri,
            NULL /* srli/srai below */, gadget_riscv64_or_ri,
            gadget_riscv64_and_ri,
        };
        void (*gadget)(void) = op_imm_gadgets[funct3];
        if (funct3 == 1) { // slli: shamt[5:0], upper imm bits must be 0
            if ((imm & ~0x3f) != 0)
                return gen_riscv64_undefined(state, insn);
            imm &= 0x3f;
        } else if (funct3 == 5) { // srli/srai by imm bit 10
            gadget = (imm & 0x400) ? gadget_riscv64_sra_ri : gadget_riscv64_srl_ri;
            if ((imm & ~0x43f) != 0)
                return gen_riscv64_undefined(state, insn);
            imm &= 0x3f;
        }
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd));
        gen(state, riscv64_rs_off(rs1));
        gen(state, (uint64_t) imm);
        return 1;
    }

    case RISCV64_OP_OP_IMM_32: {
        int64_t imm = riscv64_imm_i(insn);
        void (*gadget)(void);
        switch (funct3) {
        case 0: gadget = gadget_riscv64_addw_ri; break;
        case 1:
            if ((imm & ~0x1f) != 0)
                return gen_riscv64_undefined(state, insn);
            gadget = gadget_riscv64_sllw_ri; break;
        case 5:
            gadget = (imm & 0x400) ? gadget_riscv64_sraw_ri : gadget_riscv64_srlw_ri;
            if ((imm & ~0x41f) != 0)
                return gen_riscv64_undefined(state, insn);
            imm &= 0x1f;
            break;
        default:
            return gen_riscv64_undefined(state, insn);
        }
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd));
        gen(state, riscv64_rs_off(rs1));
        gen(state, (uint64_t) imm);
        return 1;
    }

    case RISCV64_OP_OP: case RISCV64_OP_OP_32: {
        bool is_w = riscv64_opcode(insn) == RISCV64_OP_OP_32;
        unsigned funct7 = riscv64_funct7(insn);
        void (*gadget)(void) = NULL;
        if (funct7 == 0x00) {
            static void (*const base[8])(void) = {
                gadget_riscv64_add_rr, gadget_riscv64_sll_rr,
                gadget_riscv64_slt_rr, gadget_riscv64_sltu_rr,
                gadget_riscv64_xor_rr, gadget_riscv64_srl_rr,
                gadget_riscv64_or_rr, gadget_riscv64_and_rr,
            };
            static void (*const base_w[8])(void) = {
                gadget_riscv64_addw_rr, gadget_riscv64_sllw_rr,
                NULL, NULL, NULL, gadget_riscv64_srlw_rr, NULL, NULL,
            };
            gadget = is_w ? base_w[funct3] : base[funct3];
        } else if (funct7 == 0x20) {
            if (funct3 == 0)
                gadget = is_w ? gadget_riscv64_subw_rr : gadget_riscv64_sub_rr;
            else if (funct3 == 5)
                gadget = is_w ? gadget_riscv64_sraw_rr : gadget_riscv64_sra_rr;
        } else if (funct7 == 0x01) { // M extension
            static void (*const m[8])(void) = {
                gadget_riscv64_mul_rr, gadget_riscv64_mulh_rr,
                gadget_riscv64_mulhsu_rr, gadget_riscv64_mulhu_rr,
                gadget_riscv64_div_rr, gadget_riscv64_divu_rr,
                gadget_riscv64_rem_rr, gadget_riscv64_remu_rr,
            };
            static void (*const m_w[8])(void) = {
                gadget_riscv64_mulw_rr, NULL, NULL, NULL,
                gadget_riscv64_divw_rr, gadget_riscv64_divuw_rr,
                gadget_riscv64_remw_rr, gadget_riscv64_remuw_rr,
            };
            gadget = is_w ? m_w[funct3] : m[funct3];
        }
        if (gadget == NULL)
            return gen_riscv64_undefined(state, insn);
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd));
        gen(state, riscv64_rs_off(rs1));
        gen(state, riscv64_rs_off(riscv64_rs2(insn)));
        return 1;
    }

    case RISCV64_OP_BRANCH: {
        static void (*const branch_gadgets[8])(void) = {
            gadget_riscv64_beq, gadget_riscv64_bne, NULL, NULL,
            gadget_riscv64_blt, gadget_riscv64_bge,
            gadget_riscv64_bltu, gadget_riscv64_bgeu,
        };
        void (*gadget)(void) = branch_gadgets[funct3];
        if (gadget == NULL)
            return gen_riscv64_undefined(state, insn);
        guest_addr_t taken = state->riscv64_orig_ip + riscv64_imm_b(insn);
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rs_off(rs1));
        gen(state, riscv64_rs_off(riscv64_rs2(insn)));
        gen(state, taken | 0x8000000000000000ULL);
        state->jump_ip[0] = state->size - 1;
        gen(state, state->riscv64_ip | 0x8000000000000000ULL);
        state->jump_ip[1] = state->size - 1;
        return 0; // block ends at a conditional branch
    }

    case RISCV64_OP_LOAD: {
        static void (*const load_gadgets[8])(void) = {
            gadget_riscv64_lb, gadget_riscv64_lh, gadget_riscv64_lw,
            gadget_riscv64_ld, gadget_riscv64_lbu, gadget_riscv64_lhu,
            gadget_riscv64_lwu, NULL,
        };
        void (*gadget)(void) = load_gadgets[funct3];
        if (gadget == NULL)
            return gen_riscv64_undefined(state, insn);
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd)); // x0 target still faults; sink absorbs it
        gen(state, riscv64_rs_off(rs1));
        gen(state, (uint64_t) riscv64_imm_i(insn));
        gen(state, state->riscv64_orig_ip); // fault-restart pc, ALWAYS last
        return 1;
    }

    case RISCV64_OP_STORE: {
        static void (*const store_gadgets[8])(void) = {
            gadget_riscv64_sb, gadget_riscv64_sh, gadget_riscv64_sw,
            gadget_riscv64_sd, NULL, NULL, NULL, NULL,
        };
        void (*gadget)(void) = store_gadgets[funct3];
        if (gadget == NULL)
            return gen_riscv64_undefined(state, insn);
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rs_off(riscv64_rs2(insn)));
        gen(state, riscv64_rs_off(rs1));
        gen(state, (uint64_t) riscv64_imm_s(insn));
        gen(state, state->riscv64_orig_ip); // fault-restart pc, ALWAYS last
        return 1;
    }

    case RISCV64_OP_AMO: {
        if (funct3 != 2 && funct3 != 3) // W and D only
            return gen_riscv64_undefined(state, insn);
        bool is_d = funct3 == 3;
        unsigned funct5 = riscv64_funct7(insn) >> 2; // aq/rl in bits 1:0, always honored as aq+rl
        unsigned rs2 = riscv64_rs2(insn);
        void (*gadget)(void) = NULL;
        switch (funct5) {
        case 0x02: // lr
            if (rs2 != 0)
                return gen_riscv64_undefined(state, insn);
            gadget = is_d ? gadget_riscv64_lr_d : gadget_riscv64_lr_w;
            break;
        case 0x03: gadget = is_d ? gadget_riscv64_sc_d : gadget_riscv64_sc_w; break;
        case 0x01: gadget = is_d ? gadget_riscv64_amoswap_d : gadget_riscv64_amoswap_w; break;
        case 0x00: gadget = is_d ? gadget_riscv64_amoadd_d : gadget_riscv64_amoadd_w; break;
        case 0x04: gadget = is_d ? gadget_riscv64_amoxor_d : gadget_riscv64_amoxor_w; break;
        case 0x0c: gadget = is_d ? gadget_riscv64_amoand_d : gadget_riscv64_amoand_w; break;
        case 0x08: gadget = is_d ? gadget_riscv64_amoor_d : gadget_riscv64_amoor_w; break;
        case 0x10: gadget = is_d ? gadget_riscv64_amomin_d : gadget_riscv64_amomin_w; break;
        case 0x14: gadget = is_d ? gadget_riscv64_amomax_d : gadget_riscv64_amomax_w; break;
        case 0x18: gadget = is_d ? gadget_riscv64_amominu_d : gadget_riscv64_amominu_w; break;
        case 0x1c: gadget = is_d ? gadget_riscv64_amomaxu_d : gadget_riscv64_amomaxu_w; break;
        default:
            return gen_riscv64_undefined(state, insn);
        }
        gen(state, (unsigned long) gadget);
        gen(state, riscv64_rd_off(rd));
        gen(state, riscv64_rs_off(rs1));
        gen(state, riscv64_rs_off(rs2));
        gen(state, state->riscv64_orig_ip); // fault-restart pc, ALWAYS last
        return 1;
    }

    case RISCV64_OP_LOAD_FP: case RISCV64_OP_STORE_FP: {
        // F/D loads/stores. fld/fsd/fsw reuse the integer gadgets with the
        // f-register slot offset (same byte semantics); flw NaN-boxes.
        bool is_load = riscv64_opcode(insn) == RISCV64_OP_LOAD_FP;
        if (funct3 != 2 && funct3 != 3)
            return gen_riscv64_undefined(state, insn);
        unsigned long freg_off = offsetof(struct cpu_state, riscv64_f)
            + (is_load ? rd : riscv64_rs2(insn)) * sizeof(qword_t);
        void (*gadget)(void);
        if (is_load)
            gadget = funct3 == 3 ? gadget_riscv64_ld : gadget_riscv64_flw;
        else
            gadget = funct3 == 3 ? gadget_riscv64_sd : gadget_riscv64_sw;
        gen(state, (unsigned long) gadget);
        gen(state, freg_off);
        gen(state, riscv64_rs_off(rs1));
        gen(state, (uint64_t) (is_load ? riscv64_imm_i(insn) : riscv64_imm_s(insn)));
        gen(state, state->riscv64_orig_ip); // fault-restart pc, ALWAYS last
        return 1;
    }

    case RISCV64_OP_MISC_MEM:
        // FENCE (funct3=0): guest threads are concurrent host threads, so
        // emit a real host barrier. FENCE.I (funct3=1): must invalidate
        // translated blocks — write-driven invalidation only fires on the
        // TLB write-MISS path, so a code rewrite through a still-cached
        // writable TLB entry is invisible and FENCE.I is the guest's only
        // signal (same bug class as arm64's IC IVAU above).
        if (funct3 == 0) {
            extern void gadget_riscv64_fence(void);
            gen(state, (unsigned long) gadget_riscv64_fence);
        } else if (funct3 == 1) {
            extern void gadget_riscv64_fence_i(void);
            gen(state, (unsigned long) gadget_riscv64_fence_i);
        }
        return 1;

    case RISCV64_OP_JALR:
        if (funct3 != 0)
            return gen_riscv64_undefined(state, insn);
        {
            // Single gadget: target computed from rs1 BEFORE the link
            // write, so rd==rs1 (jalr ra, 0(ra)) follows the spec.
            //
            // The _cached variant adds a jit_frame.ret_cache lookup on the
            // computed target and dispatches straight into an already-
            // translated block instead of exiting to C; see jalr_cached in
            // jit/guest-riscv64/alu.S. This is the whole lever: an instrumented
            // count put this engine's JIT->C exits and its jalr count one apart,
            // so essentially every round trip through the frontend is here.
            //
            // Emitted for EVERY jalr, not only the rd==0 return shape. The
            // cache is keyed by the TARGET, not by a recorded call site, so an
            // indirect call (rd!=0) and a `jr`/`jalr x0` tail call are the exact
            // same lookup and want it for the same reason -- a PLT stub or a
            // vtable dispatch lands on a function entry the frontend has
            // dispatched before. Nothing in an entry says "return", so there is
            // nothing to restrict, and restricting it would only forgo hits.
            extern void gadget_riscv64_jalr(void);
            extern void gadget_riscv64_jalr_cached(void);
            gen(state, (unsigned long)
                    ((riscv64_jit_fuse_mask() & JIT_FUSE_RV_RETCACHE)
                     ? gadget_riscv64_jalr_cached : gadget_riscv64_jalr));
            gen(state, riscv64_rd_off(rd));
            gen(state, riscv64_rs_off(rs1));
            gen(state, (uint64_t) riscv64_imm_i(insn));
            gen(state, state->riscv64_ip); // link = pc + length
        }
        return 0; // block ends

    case RISCV64_OP_JAL: {
        int64_t offset = riscv64_imm_j(insn);
        guest_addr_t target = state->riscv64_orig_ip + offset;
        if (rd != 0 && !(riscv64_jit_fuse_mask() & JIT_FUSE_RV_JAL)) {
            // Fusion switched off: emit the link write separately, as this used
            // to. This branch is NOT dead code -- it is the control arm of every
            // A/B -- and omitting it is a miscompile, not a slowdown: `jal ra`
            // that never writes ra sends the callee's `ret` to garbage. Caught
            // exactly that way, by testing the OFF arm.
            gen_riscv64_mov_const(state, rd, state->riscv64_ip);
        } else if (rd != 0) {
            // A call. One fused gadget instead of mov_const + b; see jal_link in
            // jit/guest-riscv64/alu.S. Emits the same tagged target word in the
            // same chainable position as gen_riscv64_branch_to, and records
            // jump_ip[0] the same way, so the frontend's edge patching is
            // unaffected by the fusion.
            extern void gadget_riscv64_jal_link(void);
            gen(state, (unsigned long) gadget_riscv64_jal_link);
            gen(state, riscv64_rd_off(rd));
            gen(state, state->riscv64_ip);  // link = pc + length
            gen(state, target | 0x8000000000000000ULL);
            state->jump_ip[0] = state->size - 1;
            return 0;
        }
        return gen_riscv64_branch_to(state, target);
    }

    case RISCV64_OP_MADD: case RISCV64_OP_MSUB:
    case RISCV64_OP_NMSUB: case RISCV64_OP_NMADD: {
        unsigned fmt = (insn >> 25) & 3;
        if (fmt > 1)
            return gen_riscv64_undefined(state, insn);
        unsigned rs3 = insn >> 27;
        static void (*const fma[4][2])(void) = {
            { gadget_riscv64_fmadd_s, gadget_riscv64_fmadd_d },
            { gadget_riscv64_fmsub_s, gadget_riscv64_fmsub_d },
            { gadget_riscv64_fnmsub_s, gadget_riscv64_fnmsub_d },
            { gadget_riscv64_fnmadd_s, gadget_riscv64_fnmadd_d },
        };
        unsigned op = (riscv64_opcode(insn) - RISCV64_OP_MADD) >> 2;
        gen(state, (unsigned long) fma[op][fmt]);
        gen(state, offsetof(struct cpu_state, riscv64_f) + rd * sizeof(qword_t));
        gen(state, offsetof(struct cpu_state, riscv64_f) + rs1 * sizeof(qword_t));
        gen(state, offsetof(struct cpu_state, riscv64_f) + riscv64_rs2(insn) * sizeof(qword_t));
        gen(state, offsetof(struct cpu_state, riscv64_f) + rs3 * sizeof(qword_t));
        return 1;
    }

    case RISCV64_OP_OP_FP: {
        unsigned funct7 = riscv64_funct7(insn);
        unsigned rs2 = riscv64_rs2(insn);
        bool is_d = funct7 & 1;
        unsigned long fd = offsetof(struct cpu_state, riscv64_f) + rd * sizeof(qword_t);
        unsigned long f1 = offsetof(struct cpu_state, riscv64_f) + rs1 * sizeof(qword_t);
        unsigned long f2 = offsetof(struct cpu_state, riscv64_f) + rs2 * sizeof(qword_t);
        void (*gadget)(void) = NULL;
        unsigned long a = fd, b = f1, c = f2; // default: all-FP operands
        switch (funct7 & ~1u) {
        case 0x00: gadget = is_d ? gadget_riscv64_fadd_d : gadget_riscv64_fadd_s; break;
        case 0x04: gadget = is_d ? gadget_riscv64_fsub_d : gadget_riscv64_fsub_s; break;
        case 0x08: gadget = is_d ? gadget_riscv64_fmul_d : gadget_riscv64_fmul_s; break;
        case 0x0c: gadget = is_d ? gadget_riscv64_fdiv_d : gadget_riscv64_fdiv_s; break;
        case 0x2c: gadget = is_d ? gadget_riscv64_fsqrt_d : gadget_riscv64_fsqrt_s; break;
        case 0x10:
            switch (funct3) {
            case 0: gadget = is_d ? gadget_riscv64_fsgnj_d : gadget_riscv64_fsgnj_s; break;
            case 1: gadget = is_d ? gadget_riscv64_fsgnjn_d : gadget_riscv64_fsgnjn_s; break;
            case 2: gadget = is_d ? gadget_riscv64_fsgnjx_d : gadget_riscv64_fsgnjx_s; break;
            }
            break;
        case 0x14:
            if (funct3 == 0) gadget = is_d ? gadget_riscv64_fmin_d : gadget_riscv64_fmin_s;
            else if (funct3 == 1) gadget = is_d ? gadget_riscv64_fmax_d : gadget_riscv64_fmax_s;
            break;
        case 0x50: // fle/flt/feq -> integer rd
            a = riscv64_rd_off(rd);
            switch (funct3) {
            case 0: gadget = is_d ? gadget_riscv64_fle_d : gadget_riscv64_fle_s; break;
            case 1: gadget = is_d ? gadget_riscv64_flt_d : gadget_riscv64_flt_s; break;
            case 2: gadget = is_d ? gadget_riscv64_feq_d : gadget_riscv64_feq_s; break;
            }
            break;
        case 0x60: { // fcvt.{w,wu,l,lu}.{s,d}; rm=1 (RTZ, C casts) vs RNE
            a = riscv64_rd_off(rd);
            bool rtz = funct3 == 1;
            static void (*const f2i[2][2][4])(void) = {
                { { gadget_riscv64_fcvtn_w_s, gadget_riscv64_fcvtn_wu_s,
                    gadget_riscv64_fcvtn_l_s, gadget_riscv64_fcvtn_lu_s },
                  { gadget_riscv64_fcvtz_w_s, gadget_riscv64_fcvtz_wu_s,
                    gadget_riscv64_fcvtz_l_s, gadget_riscv64_fcvtz_lu_s } },
                { { gadget_riscv64_fcvtn_w_d, gadget_riscv64_fcvtn_wu_d,
                    gadget_riscv64_fcvtn_l_d, gadget_riscv64_fcvtn_lu_d },
                  { gadget_riscv64_fcvtz_w_d, gadget_riscv64_fcvtz_wu_d,
                    gadget_riscv64_fcvtz_l_d, gadget_riscv64_fcvtz_lu_d } },
            };
            if (rs2 < 4)
                gadget = f2i[is_d][rtz][rs2];
            break;
        }
        case 0x68: { // fcvt.{s,d}.{w,wu,l,lu}
            b = riscv64_rs_off(rs1);
            static void (*const i2f[2][4])(void) = {
                { gadget_riscv64_fcvt_s_w, gadget_riscv64_fcvt_s_wu,
                  gadget_riscv64_fcvt_s_l, gadget_riscv64_fcvt_s_lu },
                { gadget_riscv64_fcvt_d_w, gadget_riscv64_fcvt_d_wu,
                  gadget_riscv64_fcvt_d_l, gadget_riscv64_fcvt_d_lu },
            };
            if (rs2 < 4)
                gadget = i2f[is_d][rs2];
            break;
        }
        case 0x20: // fcvt.s.d (0x20, rs2=1) / fcvt.d.s (0x21, rs2=0)
            if (!is_d && rs2 == 1) gadget = gadget_riscv64_fcvt_s_d;
            else if (is_d && rs2 == 0) gadget = gadget_riscv64_fcvt_d_s;
            break;
        case 0x70: // fmv.x.w/.d (rm=0) or fclass (rm=1)
            if (funct3 == 1) {
                extern void gadget_riscv64_call_helper(void);
                extern void riscv64_fclass_helper(struct cpu_state *cpu, unsigned long arg);
                gen(state, (unsigned long) gadget_riscv64_call_helper);
                gen(state, (unsigned long) riscv64_fclass_helper);
                gen(state, rd | (rs1 << 5) | ((unsigned long) is_d << 10));
                return 1;
            }
            if (funct3 == 0) {
                if (!is_d) {
                    a = riscv64_rd_off(rd);
                    gadget = gadget_riscv64_fmv_x_w;
                } else {
                    // pure 64-bit bit copy: reuse the integer addi gadget
                    gen(state, (unsigned long) gadget_riscv64_addi);
                    gen(state, riscv64_rd_off(rd));
                    gen(state, f1);
                    gen(state, 0);
                    return 1;
                }
            }
            break;
        case 0x78: // fmv.w.x (S) / fmv.d.x (D)
            if (funct3 == 0) {
                if (!is_d) {
                    b = riscv64_rs_off(rs1);
                    gadget = gadget_riscv64_fmv_w_x;
                } else {
                    gen(state, (unsigned long) gadget_riscv64_addi);
                    gen(state, fd);
                    gen(state, riscv64_rs_off(rs1));
                    gen(state, 0);
                    return 1;
                }
            }
            break;
        }
        if (gadget == NULL)
            return gen_riscv64_undefined(state, insn);
        gen(state, (unsigned long) gadget);
        gen(state, a);
        gen(state, b);
        gen(state, c);
        return 1;
    }

    case RISCV64_OP_CUSTOM0: case RISCV64_OP_CUSTOM1:
    case RISCV64_OP_CUSTOM2: case RISCV64_OP_CUSTOM3: {
        // Vendor/user extension hook (riscv64_guest_plan.md patch 5b,
        // jit/riscv64_vendor_ext.c). These four opcodes are permanently
        // reserved by the ISA for non-standard use, so consulting a
        // registry here can never shadow a real instruction — see that
        // file's header for the full design and /AOK/docs for the
        // walkthrough. Off by default; falls through to the normal
        // undefined-instruction path (SIGILL) on no match or when disabled,
        // exactly like any other unimplemented encoding.
        extern bool riscv64_vendor_ext_enabled(void);
        extern const char *riscv64_vendor_ext_lookup(uint32_t insn);
        extern void riscv64_vendor_ext_dispatch(struct cpu_state *cpu, unsigned long arg);
        if (riscv64_vendor_ext_enabled() && riscv64_vendor_ext_lookup(insn) != NULL) {
            extern void gadget_riscv64_call_helper(void);
            gen(state, (unsigned long) gadget_riscv64_call_helper);
            gen(state, (unsigned long) riscv64_vendor_ext_dispatch);
            gen(state, (unsigned long) insn);
            return 1;
        }
        return gen_riscv64_undefined(state, insn);
    }

    case RISCV64_OP_SYSTEM:
        if (insn == 0x00000073) { // ecall
            extern void gadget_riscv64_ecall(void);
            gen(state, (unsigned long) gadget_riscv64_ecall);
            gen(state, state->riscv64_ip); // pc after the ecall
            return 0; // block ends: exits to the main loop
        }
        if (insn == 0x00100073) // ebreak
            return gen_riscv64_interrupt_at(state, INT_BREAKPOINT,
                    state->riscv64_orig_ip, state->riscv64_orig_ip);
        if (funct3 >= 1 && funct3 <= 7 && funct3 != 4) { // csrrw/s/c[i]
            unsigned csr = insn >> 20;
            bool is_counter = csr == 0xc00 || csr == 0xc01 || csr == 0xc02; // cycle/time/instret
            // csrrs/c(i) with rs1/uimm == 0 is a pure read (this is what the
            // `rdtime`/`csrr` pseudo-ops assemble to); anything else would
            // write, which is illegal for these read-only counters and
            // falls through to gen_riscv64_undefined below.
            bool counter_read_only = is_counter && (funct3 & 3) != 1 && rs1 == 0;
            if ((csr >= 1 && csr <= 3) || counter_read_only) { // fflags/frm/fcsr, or cycle/time/instret
                extern void gadget_riscv64_call_helper(void);
                extern void riscv64_csr_helper(struct cpu_state *cpu, unsigned long arg);
                gen(state, (unsigned long) gadget_riscv64_call_helper);
                gen(state, (unsigned long) riscv64_csr_helper);
                gen(state, rd | (rs1 << 5) | (funct3 << 10)
                        | ((unsigned long) csr << 13));
                return 1;
            }
        }
        return gen_riscv64_undefined(state, insn);

    default:
        return gen_riscv64_undefined(state, insn);
    }
}

#else // !ISH_JIT_RISCV64_GUEST

bool gen_start_riscv64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->riscv64 = true;
    state->riscv64_ip = addr;
    state->riscv64_orig_ip = addr;
    return true;
}

int gen_step_riscv64(struct gen_state *state, struct tlb *tlb) {
    (void) tlb;
    die("riscv64 guest binaries are not supported on this host "
        "(no riscv64 guest JIT; pc=%#llx)",
        (unsigned long long) state->riscv64_ip);
}

#endif // ISH_JIT_RISCV64_GUEST

static bool gen_fetch_amd64(struct gen_state *state, struct tlb *tlb, void *out, size_t size) {
    if (!tlb_read(tlb, state->amd64_ip, out, size))
        return false;
    state->amd64_ip += size;
    return true;
}

static bool gen_decode_amd64(struct gen_state *state, struct tlb *tlb,
        struct amd64_jit_insn *insn) {
    byte_t byte;

    insn->start_ip = state->amd64_orig_ip;
    insn->end_ip = state->amd64_orig_ip;
    insn->opcode = 0;
    insn->op2 = 0;
    insn->modrm = 0;
    insn->two_byte_opcode = false;
    insn->has_modrm = false;
    insn->operand_size_prefix = false;
    insn->address_size_prefix = false;
    insn->fs_prefix = false;
    insn->lock_prefix = false;
    insn->rep_mode = amd64_jit_rep_none;
    insn->rex = (struct amd64_jit_rex_prefix) {0};

    for (;;) {
        if (!gen_fetch_amd64(state, tlb, &byte, sizeof(byte)))
            return false;
        if (byte == 0x66) {
            insn->operand_size_prefix = true;
            continue;
        }
        if (byte == 0x67) {
            insn->address_size_prefix = true;
            continue;
        }
        if (amd64_jit_ignored_segment_prefix(byte)) {
            continue;
        }
        if (byte == 0x64) {
            insn->fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            insn->lock_prefix = true;
            continue;
        }
        if (byte == 0xf2) {
            insn->rep_mode = amd64_jit_repnz;
            continue;
        }
        if (byte == 0xf3) {
            insn->rep_mode = amd64_jit_repz;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            insn->rex.present = true;
            insn->rex.w = (byte & 8) != 0;
            insn->rex.r = (byte & 4) != 0;
            insn->rex.x = (byte & 2) != 0;
            insn->rex.b = (byte & 1) != 0;
            continue;
        }
        insn->opcode = byte;
        break;
    }

    if (insn->opcode == 0x0f) {
        if (!gen_fetch_amd64(state, tlb, &insn->op2, sizeof(insn->op2)))
            return false;
        insn->two_byte_opcode = true;
    }
    if (amd64_opcode_needs_modrm(insn)) {
        if (!tlb_read(tlb, state->amd64_ip, &insn->modrm, sizeof(insn->modrm)))
            return false;
        insn->has_modrm = true;
    }
    insn->end_ip = state->amd64_ip;
    return true;
}

static bool amd64_jit_plain_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_plain_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode &&
        !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_branch_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_branch_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode && amd64_jit_branch_prefixes(insn);
}

static bool amd64_jit_one_byte_rel_call_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode &&
        !insn->operand_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&
        insn->rep_mode == amd64_jit_rep_none;
}

static bool amd64_jit_one_byte_ret_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->two_byte_opcode;
}

static bool amd64_jit_push_pop_prefixes_ok(const struct amd64_jit_insn *insn) {
    if (!amd64_jit_one_byte_plain_prefixes(insn))
        return false;
    if (!insn->rex.present)
        return true;
    return !insn->rex.w && !insn->rex.r && !insn->rex.x;
}

static bool gen_amd64_decode_mem_meta(struct gen_state *state, struct tlb *tlb,
        const struct amd64_jit_insn *insn, unsigned size,
        unsigned long *meta_out, unsigned long *disp_out,
        guest_addr_t *next_ip_out) {
    guest_addr_t ip = state->amd64_ip + 1;
    byte_t modrm = insn->modrm;
    unsigned mod = amd64_modrm_mod(modrm);
    unsigned rm_low = amd64_modrm_rm(modrm);
    unsigned reg_id = amd64_modrm_reg(modrm) | (insn->rex.r ? 8 : 0);
    unsigned base = 0;
    unsigned index = 0;
    unsigned scale = 0;
    bool has_base = false;
    bool has_index = false;
    bool rip_relative = false;
    int32_t disp = 0;

    if (mod == 3 || insn->address_size_prefix)
        return false;

    if (rm_low == 4) {
        byte_t sib;
        if (!tlb_read(tlb, ip, &sib, sizeof(sib)))
            return false;
        ip += sizeof(sib);
        unsigned base_low = amd64_modrm_rm(sib);
        unsigned index_low = amd64_modrm_reg(sib);
        scale = (sib >> 6) & 0x3;
        if (index_low != 4 || insn->rex.x) {
            has_index = true;
            index = index_low | (insn->rex.x ? 8 : 0);
        }
        // mod=00 with base=101 means no base + disp32, regardless of REX.B
        // (r13 as a base requires mod=01/10).
        if (mod == 0 && base_low == 5) {
            has_base = false;
        } else {
            has_base = true;
            base = base_low | (insn->rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        rip_relative = true;
    } else {
        has_base = true;
        base = rm_low | (insn->rex.b ? 8 : 0);
    }

    if (mod == 1) {
        int8_t disp8;
        if (!tlb_read(tlb, ip, &disp8, sizeof(disp8)))
            return false;
        disp = disp8;
        ip += sizeof(disp8);
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 ||
            (rm_low == 4 && !has_base)))) {
        if (!tlb_read(tlb, ip, &disp, sizeof(disp)))
            return false;
        ip += sizeof(disp);
    }

    *meta_out = ((unsigned long) insn->opcode << AMD64_JIT_MEM_OPCODE_SHIFT) |
        ((unsigned long) reg_id << AMD64_JIT_MEM_REG_SHIFT) |
        ((unsigned long) size << AMD64_JIT_MEM_SIZE_SHIFT) |
        ((unsigned long) base << AMD64_JIT_MEM_BASE_SHIFT) |
        ((unsigned long) index << AMD64_JIT_MEM_INDEX_SHIFT) |
        ((unsigned long) scale << AMD64_JIT_MEM_SCALE_SHIFT);
    if (has_base)
        *meta_out |= AMD64_JIT_MEM_HAS_BASE;
    if (has_index)
        *meta_out |= AMD64_JIT_MEM_HAS_INDEX;
    if (rip_relative)
        *meta_out |= AMD64_JIT_MEM_RIP_REL;
    if (insn->fs_prefix)
        *meta_out |= AMD64_JIT_MEM_FS;
    if (insn->rex.present)
        *meta_out |= AMD64_JIT_MEM_REX_PRESENT;
    *disp_out = (unsigned long) (qword_t) (sqword_t) disp;
    *next_ip_out = ip;
    return true;
}

static bool gen_amd64_decode_rm_extent(struct gen_state *state, struct tlb *tlb,
        const struct amd64_jit_insn *insn, guest_addr_t *next_ip_out) {
    guest_addr_t ip = state->amd64_ip + 1;
    byte_t modrm = insn->modrm;
    unsigned mod = amd64_modrm_mod(modrm);
    unsigned rm_low = amd64_modrm_rm(modrm);
    bool has_base = true;

    if (insn->address_size_prefix)
        return false;
    if (mod == 3) {
        *next_ip_out = ip;
        return true;
    }
    if (rm_low == 4) {
        byte_t sib;
        if (!tlb_read(tlb, ip, &sib, sizeof(sib)))
            return false;
        ip += sizeof(sib);
        unsigned base_low = amd64_modrm_rm(sib);
        has_base = !(mod == 0 && base_low == 5);
    } else if (mod == 0 && rm_low == 5) {
        has_base = false;
    }

    if (mod == 1) {
        ip += sizeof(int8_t);
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 ||
            (rm_low == 4 && !has_base)))) {
        ip += sizeof(int32_t);
    }
    *next_ip_out = ip;
    return true;
}

static void gen_amd64_helper_tlb_0_retint(struct gen_state *state, void *helper) {
    extern void gadget_helper_tlb_0_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_0_retint);
    gen(state, (unsigned long) helper);
}

static void gen_amd64_helper_tlb_1_retint(struct gen_state *state, void *helper,
        unsigned long arg0) {
    extern void gadget_helper_tlb_1_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_1_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
}

static void gen_amd64_helper_tlb_2_retint(struct gen_state *state, void *helper,
        unsigned long arg0, unsigned long arg1) {
    extern void gadget_helper_tlb_2_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_2_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
    gen(state, arg1);
}

static void gen_amd64_helper_tlb_3_retint(struct gen_state *state, void *helper,
        unsigned long arg0, unsigned long arg1, unsigned long arg2) {
    extern void gadget_helper_tlb_3_retint(void);
    gen_amd64_flush_reg_cache(state);
    gen_amd64_flush_rip(state);
    gen(state, (unsigned long) gadget_helper_tlb_3_retint);
    gen(state, (unsigned long) helper);
    gen(state, arg0);
    gen(state, arg1);
    gen(state, arg2);
}

int gen_step_amd64(struct gen_state *state, struct tlb *tlb) {
    return gen_step64(state, tlb);
}

#if defined(__aarch64__)
static int gen_step64(struct gen_state *state, struct tlb *tlb) {
    struct amd64_jit_insn insn;
    int8_t rel8;
    int32_t rel32;
    guest_addr_t next_ip;
    guest_addr_t target_ip;
    unsigned long reg;

    state->amd64_orig_ip = state->amd64_ip;
    state->orig_ip_extra = 0;
    state->amd64_fallback_to_interp = false;
    state->amd64_fallback_ip = state->amd64_orig_ip;

    if (!gen_decode_amd64(state, tlb, &insn)) {
        amd64_jit_debug("decode fail ip=%llx",
                (unsigned long long) state->amd64_orig_ip);
        state->amd64_ip = state->amd64_orig_ip;
        state->amd64_fallback_to_interp = true;
        state->amd64_fallback_opcode = 0xff;
        state->amd64_fallback_op2 = 0;
        state->amd64_fallback_flags = 0x80;
        return false;
    }

    state->amd64_fallback_opcode = insn.opcode;
    state->amd64_fallback_op2 = insn.op2;
    state->amd64_fallback_flags =
        (insn.two_byte_opcode ? 0x01 : 0) |
        (insn.rex.present ? 0x02 : 0) |
        (insn.rex.w ? 0x04 : 0) |
        (insn.operand_size_prefix ? 0x08 : 0) |
        (insn.address_size_prefix ? 0x10 : 0) |
        (insn.fs_prefix ? 0x20 : 0) |
        (insn.rep_mode != amd64_jit_rep_none ? 0x40 : 0);

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc3) {
        amd64_jit_debug("ret ip=%llx",
                (unsigned long long) insn.start_ip);
        // Native ret: pop the return address into rip and exit the block. The
        // gadget branches to jit_ret itself (indirect target -> no static link),
        // so no gen_exit. rip flushed so a #PF on the stack read re-executes.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_ret(void);
        gen(state, (unsigned long) gadget_amd64_ret);
        return false;
    }

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc2) {
        uint16_t imm16;
        if (!tlb_read(tlb, state->amd64_ip, &imm16, sizeof(imm16))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(imm16);
        state->amd64_ip = next_ip;
        amd64_jit_debug("ret-imm-helper ip=%llx imm=%u",
                (unsigned long long) insn.start_ip,
                (unsigned) imm16);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_ret_imm,
                (unsigned long) imm16);
        gen_exit(state);
        return false;
    }

    if (amd64_jit_one_byte_ret_prefixes(&insn) && insn.opcode == 0xc9) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        if (insn.operand_size_prefix) {
            // 16-bit leave is rare; keep bridging it (ends the block).
            amd64_jit_debug("leave16-helper ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_helper_tlb_2_retint(state, amd64_jit_leave,
                    16, (unsigned long) next_ip);
            gen_exit(state);
            return false;
        }
        amd64_jit_debug("leave ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        // Native 64-bit leave continues in-block like push/pop.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_leave(void);
        gen(state, (unsigned long) gadget_amd64_leave);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) && insn.opcode == 0xe9) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jmp-rel32-direct ip=%llx target=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_rel_call_prefixes(&insn) && insn.opcode == 0xe8) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("call-rel32 ip=%llx target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        // Native call: push the 64-bit return address (reusing the push-imm
        // gadget, which stores its operand verbatim), then statically link to
        // the target block exactly as jmp rel32 does. A page fault during the
        // push re-executes this instruction (rip flushed to the call's addr).
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_imm(void);
        gen(state, (unsigned long) gadget_amd64_push_imm);
        gen(state, (unsigned long) next_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) && insn.opcode == 0xeb) {
        if (!tlb_read(tlb, state->amd64_ip, &rel8, sizeof(rel8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel8);
        target_ip = next_ip + rel8;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jmp-rel8-direct ip=%llx target=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) target_ip);
        gen_amd64_jmp_rel(state, target_ip);
        return false;
    }

    if (amd64_jit_one_byte_branch_prefixes(&insn) &&
            insn.opcode >= 0x70 && insn.opcode <= 0x7f) {
        if (!tlb_read(tlb, state->amd64_ip, &rel8, sizeof(rel8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel8);
        target_ip = next_ip + rel8;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jcc-rel8 ip=%llx cc=%u target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned) (insn.opcode & 0xf),
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        gen_amd64_jcc(state, insn.opcode & 0xf, target_ip, next_ip);
        return false;
    }

    // Native LOOP/LOOPE/LOOPNE (0xe0-0xe2), rel8, no address-size prefix (RCX counter --
    // the 0x67/ECX form, like operand-size, falls through to the interp, which now
    // implements all of them). Were unimplemented in BOTH engines -> SIGILL; the interp
    // cases were added alongside this. Mirrors the jcc rel8 path.
    if (amd64_jit_one_byte_branch_prefixes(&insn) &&
            (insn.opcode == 0xe0 || insn.opcode == 0xe1 || insn.opcode == 0xe2)) {
        if (!tlb_read(tlb, state->amd64_ip, &rel8, sizeof(rel8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel8);
        target_ip = next_ip + rel8;
        state->amd64_ip = next_ip;
        amd64_jit_debug("loop ip=%llx op=%02x target=%llx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode,
                (unsigned long long) target_ip, (unsigned long long) next_ip);
        gen_amd64_loop(state, insn.opcode, target_ip, next_ip);
        return false;
    }

    if (amd64_jit_branch_prefixes(&insn) && insn.two_byte_opcode &&
            insn.op2 >= 0x80 && insn.op2 <= 0x8f) {
        if (!tlb_read(tlb, state->amd64_ip, &rel32, sizeof(rel32))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(rel32);
        target_ip = next_ip + rel32;
        state->amd64_ip = next_ip;
        amd64_jit_debug("jcc-rel32 ip=%llx cc=%u target=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned) (insn.op2 & 0xf),
                (unsigned long long) target_ip,
                (unsigned long long) next_ip);
        gen_amd64_jcc(state, insn.op2 & 0xf, target_ip, next_ip);
        return false;
    }

    // IN/OUT (e4-e7 imm8-port, ec-ef dx-port). Ring-0 only from user mode, so
    // every form raises #GP -- SIGSEGV, not the SIGILL the unhandled-opcode
    // fallback was producing. lscpu's VMware backdoor probe catches SIGSEGV
    // and siglongjmps out of it; SIGILL killed the process instead. Operand
    // size makes no difference to the fault, so a 66 prefix is fine here; a
    // LOCK prefix is genuinely #UD on hardware, so leave that to the fallback.
    if (!insn.two_byte_opcode && !insn.lock_prefix &&
            ((insn.opcode >= 0xe4 && insn.opcode <= 0xe7) ||
             (insn.opcode >= 0xec && insn.opcode <= 0xef))) {
        amd64_jit_debug("port-io-gpf ip=%llx opcode=%02x",
                (unsigned long long) insn.start_ip,
                (unsigned) insn.opcode);
        // The imm8 of the e4-e7 forms is deliberately not consumed: the fault
        // reports the instruction's own address, so nothing after the opcode
        // is ever needed.
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_port_io,
                (unsigned long) insn.start_ip);
        gen_exit(state);
        return false;
    }

    if (amd64_jit_plain_prefixes(&insn) && insn.two_byte_opcode &&
            insn.op2 == 0x05) {
        next_ip = insn.end_ip;
        amd64_jit_debug("syscall-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_syscall,
                (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (amd64_jit_plain_prefixes(&insn) && insn.two_byte_opcode &&
            (insn.op2 == 0x31 || insn.op2 == 0xa2)) {
        next_ip = insn.end_ip;
        amd64_jit_debug("%s-helper ip=%llx next=%llx",
                insn.op2 == 0x31 ? "rdtsc" : "cpuid",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state,
                insn.op2 == 0x31 ? amd64_jit_rdtsc : amd64_jit_cpuid,
                (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    // XGETBV (0f 01 d0). 0f 01's other register forms are ring-0, so only this
    // exact ModRM is accepted and everything else falls through to the normal
    // undefined-opcode path. Handled here rather than in the interpreter
    // because the interpreter is being retired: implementing it there would
    // make the instruction work today only via the interpreter fallback and
    // then disappear with it.
    if (amd64_jit_plain_prefixes(&insn) && insn.two_byte_opcode && insn.op2 == 0x01) {
        byte_t modrm;
        if (!tlb_read(tlb, state->amd64_ip, &modrm, sizeof(modrm))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (modrm == 0xd0) {
            next_ip = state->amd64_ip + sizeof(modrm);
            state->amd64_ip = next_ip;
            amd64_jit_debug("xgetbv ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_helper_tlb_1_retint(state, amd64_jit_xgetbv,
                    (unsigned long) next_ip);
            gen_exit(state);
            return false;
        }
    }

    if (!insn.address_size_prefix && !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_repz && insn.two_byte_opcode &&
            insn.op2 == 0x1e) {
        byte_t op3;
        if (!tlb_read(tlb, state->amd64_ip, &op3, sizeof(op3))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (op3 == 0xfa || op3 == 0xfb) {
            next_ip = state->amd64_ip + sizeof(op3);
            state->amd64_ip = next_ip;
            amd64_jit_debug("endbr-nop ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        // RDSSPD/RDSSPQ (F3 0F 1E /1, mod==11): read the CET shadow stack
        // pointer into a register. iSH implements no shadow stack, so the
        // architecturally-correct behavior when CET is disabled is a NOP that
        // leaves the destination unchanged. glibc's exception/unwind and
        // setjmp paths pre-zero the register and then test it, so NOP-ing this
        // makes their "shadow stack disabled" branch be taken. Without this
        // every C++ throw (and gdb, longjmp probes, ...) hit INT_UNDEFINED ->
        // SIGILL. Register form only, so op3 is the whole ModRM (no SIB/disp).
        if (((op3 >> 3) & 7) == 1 && (op3 >> 6) == 3) {
            next_ip = state->amd64_ip + sizeof(op3);
            state->amd64_ip = next_ip;
            amd64_jit_debug("rdssp-nop ip=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    (unsigned long long) next_ip);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
    }

    if (!insn.address_size_prefix && !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.op2 >= 0xc8 && insn.op2 <= 0xcf) {
        unsigned size = insn.rex.w ? 64 : 32;
        reg = (unsigned long) (insn.op2 - 0xc8);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("bswap-direct ip=%llx reg=%lu size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_bswap_reg(void);
        gen(state, (unsigned long) gadget_amd64_bswap_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.opcode == 0x98 || insn.opcode == 0x99)) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("sign-extend-direct ip=%llx opcode=%02x size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_sign_extend(void);
        gen(state, (unsigned long) gadget_amd64_sign_extend);
        gen(state, (unsigned long) insn.opcode | ((unsigned long) size << 8));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x69 || insn.opcode == 0x6b)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        guest_addr_t imul_imm_ip = next_ip;
        next_ip += insn.opcode == 0x69
            ? (insn.operand_size_prefix ? sizeof(int16_t) : sizeof(int32_t))
            : sizeof(int8_t);
#if defined(__aarch64__)
        // imul reg, rm, imm (reg form): native multiply with overflow flags.
        if (!insn.fs_prefix && !insn.lock_prefix &&
                amd64_modrm_mod(insn.modrm) == 3) {
            unsigned size = insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32);
            unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if ((size == 32 || size == 64) &&
                    amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                unsigned long imm_val;
                if (insn.opcode == 0x6b) {
                    int8_t i;
                    if (!tlb_read(tlb, imul_imm_ip, &i, sizeof(i))) {
                        state->amd64_ip = state->amd64_orig_ip;
                        state->amd64_fallback_to_interp = true;
                        return false;
                    }
                    imm_val = (unsigned long) (qword_t) (sqword_t) i;
                } else {
                    int32_t i;
                    if (!tlb_read(tlb, imul_imm_ip, &i, sizeof(i))) {
                        state->amd64_ip = state->amd64_orig_ip;
                        state->amd64_fallback_to_interp = true;
                        return false;
                    }
                    imm_val = (unsigned long) (qword_t) (sqword_t) i;
                }
                unsigned long packed = ((unsigned long) reg_id << 8) |
                    ((unsigned long) rm_id << 12) |
                    ((unsigned long) size << 16);
                state->amd64_ip = next_ip;
                amd64_jit_debug("imul-imm-direct ip=%llx dst=%u src=%u size=%u imm=%llx next=%llx",
                        (unsigned long long) insn.start_ip, reg_id, rm_id, size,
                        (unsigned long long) imm_val, (unsigned long long) next_ip);
                extern void gadget_amd64_imul_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_imul_imm);
                gen(state, packed);
                gen(state, imm_val);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        state->amd64_ip = next_ip;
        amd64_jit_debug("imul-imm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_imul_imm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0xa0 && insn.opcode <= 0xa3) {
        next_ip = state->amd64_ip +
            (insn.address_size_prefix ? sizeof(uint32_t) : sizeof(uint64_t));
        state->amd64_ip = next_ip;
        amd64_jit_debug("moffs-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_moffs_accum,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.fs_prefix &&
            !insn.lock_prefix &&
            ((insn.opcode >= 0xa4 && insn.opcode <= 0xa7) ||
             (insn.opcode >= 0xaa && insn.opcode <= 0xaf))) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("string-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_string_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x86 || insn.opcode == 0x87)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (!insn.lock_prefix && amd64_modrm_mod(insn.modrm) == 3) {
            unsigned reg_raw = amd64_modrm_reg(insn.modrm);
            unsigned rm_raw = amd64_modrm_rm(insn.modrm);
            unsigned reg_id = reg_raw | (insn.rex.r ? 8 : 0);
            unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
            unsigned size = insn.opcode == 0x86
                ? 8
                : (insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32));
            if (insn.opcode == 0x86 && !insn.rex.present &&
                    (reg_raw >= 4 || rm_raw >= 4))
                goto amd64_bridge_step;
            state->amd64_ip = next_ip;
            amd64_jit_debug("xchg-reg-reg-direct ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    reg_id,
                    rm_id,
                    size,
                    (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_xchg_reg_reg(void);
            gen(state, (unsigned long) gadget_amd64_xchg_reg_reg);
            gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("xchg-rm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_xchg_rm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.lock_prefix && insn.rep_mode == amd64_jit_rep_none &&
            (insn.opcode == 0x04 || insn.opcode == 0x05 ||
             insn.opcode == 0x0c || insn.opcode == 0x0d ||
             insn.opcode == 0x14 || insn.opcode == 0x15 ||
             insn.opcode == 0x1c || insn.opcode == 0x1d ||
             insn.opcode == 0x24 || insn.opcode == 0x25 ||
             insn.opcode == 0x2c || insn.opcode == 0x2d ||
             insn.opcode == 0x34 || insn.opcode == 0x35 ||
             insn.opcode == 0x3c || insn.opcode == 0x3d ||
             insn.opcode == 0xa8 || insn.opcode == 0xa9)) {
        bool imm8 = (insn.opcode & 1) == 0;
        bool is_test = insn.opcode == 0xa8 || insn.opcode == 0xa9;
        unsigned size = imm8 ? 8 : (insn.operand_size_prefix ? 16 : (insn.rex.w ? 64 : 32));
        // x86 ALU op from the opcode (0=add,1=or,2=adc,3=sbb,4=and,5=sub,6=xor,7=cmp)
        unsigned op = is_test ? 0 : ((insn.opcode >> 3) & 7);
        guest_addr_t imm_ip = state->amd64_ip;
        unsigned long value;
        if (imm8) {
            int8_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) i;
            next_ip = imm_ip + sizeof(i);
        } else if (size == 16) {
            uint16_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) i;
            next_ip = imm_ip + sizeof(i);
        } else {
            int32_t i;
            if (!tlb_read(tlb, imm_ip, &i, sizeof(i))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) i;
            next_ip = imm_ip + sizeof(i);
        }
#if defined(__aarch64__)
        // Route 32/64-bit add/sub/cmp and and/or/xor (on the accumulator, reg 0)
        // to the same validated cached reg-imm gadgets the modrm 80/81/83 path
        // uses, eliminating the amd64_jit_accum_imm_op bridge + its re-decode.
        // adc/sbb (carry-in), 8/16-bit, and test keep bridging.
        if ((size == 32 || size == 64) && !is_test) {
            bool route_arith = (op == 0 || op == 5 || op == 7);
            bool route_logic = (op == 1 || op == 4 || op == 6);
            if (route_arith || route_logic) {
                unsigned long packed = (unsigned long) insn.opcode |
                    ((unsigned long) op << 8) |
                    (0ul << 12) |
                    ((unsigned long) size << 16);
                if (insn.rex.present)
                    packed |= 1ul << 24;
                state->amd64_ip = next_ip;
                amd64_jit_debug("accum-imm-direct ip=%llx opcode=%02x op=%u size=%u value=%llx next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode, op, size,
                        (unsigned long long) value, (unsigned long long) next_ip);
                extern void gadget_amd64_cached_arith_reg_imm(void);
                extern void gadget_amd64_cached_logic_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) (route_logic
                            ? (void (*)(void)) gadget_amd64_cached_logic_reg_imm
                            : (void (*)(void)) gadget_amd64_cached_arith_reg_imm));
                gen(state, packed);
                gen(state, value);
                if (op != 7) // cmp does not write back
                    gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        state->amd64_ip = next_ip;
        amd64_jit_debug("accum-imm-helper ip=%llx opcode=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_accum_imm_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.op2 == 0xb6 || insn.op2 == 0xb7 ||
             insn.op2 == 0xbe || insn.op2 == 0xbf) &&
            !((insn.op2 == 0xb6 || insn.op2 == 0xbe) &&
              !insn.rex.present && amd64_modrm_rm(insn.modrm) >= 4)) {
        unsigned src_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned dst_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned dst_size = insn.rex.w ? 64 : 32;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("movx-reg-reg-direct ip=%llx op2=%02x src=%u dst=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                src_id,
                dst_id,
                dst_size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_movx_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_movx_reg_reg);
        gen(state, (unsigned long) insn.op2 |
                ((unsigned long) src_id << 8) |
                ((unsigned long) dst_id << 12) |
                ((unsigned long) dst_size << 16));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native MOVZX/MOVSX reg <- [mem] (0F B6/B7/BE/BF, mod!=3), 32/64-bit dst, no flags.
    // Byte (B6/BE) or word (B7/BF) memory source, zero- (B6/B7) or sign-extended (BE/BF)
    // into the dst register. The 16-bit-dst (0x66), FS, address-size, and locked forms
    // keep bridging to amd64_jit_movx below. is_signed -> meta bit 40, REX.W -> meta bit
    // 41 (free high bits; decode_mem_meta uses only 0-34).
    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.op2 == 0xb6 || insn.op2 == 0xb7 ||
             insn.op2 == 0xbe || insn.op2 == 0xbf)) {
        bool is_byte = (insn.op2 == 0xb6 || insn.op2 == 0xbe);
        bool is_signed = (insn.op2 == 0xbe || insn.op2 == 0xbf);
        unsigned opsize = is_byte ? 8 : 16;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, opsize, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (is_signed)
            meta |= 1ul << 40;
        if (insn.rex.w)
            meta |= 1ul << 41;
        state->amd64_ip = next_ip;
        amd64_jit_debug("movx-mem ip=%llx op2=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_movx_mem8(void), gadget_amd64_movx_mem16(void);
        gen(state, (unsigned long) (is_byte
                    ? gadget_amd64_movx_mem8 : gadget_amd64_movx_mem16));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            (insn.op2 == 0xb6 || insn.op2 == 0xb7 ||
             insn.op2 == 0xbe || insn.op2 == 0xbf)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("movx-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_movx,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        return true;
    }

    if (!insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            insn.op2 == 0x18 &&
            (amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0)) <= 3) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("prefetch-nop-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            insn.op2 == 0x1f &&
            amd64_modrm_reg(insn.modrm) == 0) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("nop-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // emms (0F 77): no modrm, no operands. Architecturally it just empties the
    // x87 FPU tag word; this emulator models no x87 tag state that gates MMX
    // register access (the i386 decoder likewise treats emms as ignored), so it
    // is a pure no-op. Without this the JIT raised #UD on the trailing emms that
    // every real MMX routine emits (e.g. libgcrypt SHA), independent of the
    // 0F 7F store fix. Strict prefixes: bare 0F 77 only; any 66/F2/F3 variant
    // falls through to the interpreter, which also treats it as a nop.
    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.op2 == 0x77) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("emms-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

#if defined(__aarch64__)
    // imul reg, rm (0F AF), reg form, low8 regs, 32/64-bit: native multiply with
    // overflow flags via gadget_amd64_imul_reg (shares amd64_imul_body with the
    // proven imm form). High regs / memory operand / 16-bit / fs / lock fall
    // through to the bridge below.
    //
    // This re-enables the path f2cf6452 disabled. The "latent imul_reg bug" was not
    // in the gadget: the reverted wiring omitted gen_amd64_mark_reg_cache_dirty, so
    // the result was written into the host cache register but never flagged dirty.
    // A following op that flushed/invalidated the cache (e.g. a high-reg multiply
    // on the bridge) then dropped it, reloading the stale pre-imul value -- which is
    // exactly why it was interleaving-sensitive and why an intervening flush "fixed"
    // it. Marking the cache dirty (as the imm form already does) is the fix; proven
    // by toggling that one call against the repro.
    if (!insn.operand_size_prefix && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.two_byte_opcode &&
            insn.op2 == 0xaf && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) == 3) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
            unsigned long packed = ((unsigned long) reg_id << 8) |
                ((unsigned long) rm_id << 12) |
                ((unsigned long) size << 16);
            next_ip = state->amd64_ip + 1;
            state->amd64_ip = next_ip;
            amd64_jit_debug("imul-reg-direct ip=%llx dst=%u src=%u size=%u next=%llx",
                    (unsigned long long) insn.start_ip, reg_id, rm_id, size,
                    (unsigned long long) next_ip);
            extern void gadget_amd64_imul_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_imul_reg);
            gen(state, packed);
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
    }
#endif

    // imul reg, rm (0F AF): high-reg / memory / 16-bit forms bridge to the helper.
    if (!insn.address_size_prefix &&
            (insn.rep_mode == amd64_jit_rep_none ||
             ((insn.op2 == 0xbc || insn.op2 == 0xbd) &&
              insn.rep_mode == amd64_jit_repz)) &&
            insn.two_byte_opcode &&
            insn.has_modrm &&
            (insn.op2 == 0x1f ||
             (insn.op2 >= 0x40 && insn.op2 <= 0x4f) ||
             (insn.op2 >= 0x90 && insn.op2 <= 0x9f) ||
             insn.op2 == 0xa3 ||
             insn.op2 == 0xa4 ||
             insn.op2 == 0xa5 ||
             insn.op2 == 0xab ||
             insn.op2 == 0xac ||
             insn.op2 == 0xad ||
             insn.op2 == 0xae ||
             insn.op2 == 0xaf ||
             insn.op2 == 0xba ||
             insn.op2 == 0xb3 ||
             insn.op2 == 0xb0 ||
             insn.op2 == 0xb1 ||
             insn.op2 == 0xbc ||
             insn.op2 == 0xbd ||
             insn.op2 == 0xbb ||
             insn.op2 == 0xc0 ||
             insn.op2 == 0xc1)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.op2 == 0xa4 || insn.op2 == 0xac || insn.op2 == 0xba)
            next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("0f-rm-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_0f_rm,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

#if defined(__aarch64__)
    // Native aarch64 SSE register-register gadgets. These intercept the hottest
    // reg-reg vector ops that would otherwise compile into a per-op C bridge
    // (amd64_jit_0f_vec_rm). Only the exact reg-reg cases matched here are taken
    // natively; every other form (memory operand, MMX no-prefix, other prefixes)
    // falls through to the bridge below. The gadgets touch only cpu->xmm[], so
    // the GPR reg cache is left intact (no flush) and the rip is deferred.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x6f || insn.op2 == 0xd4 || insn.op2 == 0xfb ||
             insn.op2 == 0xef || insn.op2 == 0x74 || insn.op2 == 0xf8 ||
             insn.op2 == 0xfd || insn.op2 == 0xd9 || insn.op2 == 0xeb ||
             insn.op2 == 0x60 || insn.op2 == 0x61 || insn.op2 == 0x62 ||
             insn.op2 == 0x6c)) {
        // 66 0F /r, mod==3 register-register SSE2 ops (dst=reg, src=rm):
        //   6F movdqa (128-bit copy), D4 paddq, FB psubq (packed 64-bit add/sub),
        //   EF pxor (128-bit XOR — same gadget as xorps, lane width irrelevant),
        //   74 pcmpeqb, F8 psubb (packed-byte; glibc strlen/memchr scan),
        //   FD paddw, D9 psubusw, EB por, 60/61/62/6C punpcklbw/wd/dq/qdq
        //   (the remaining hot bridge ops from the JIT stats histogram)
        extern void gadget_amd64_v_mov128_reg(void);
        extern void gadget_amd64_v_paddq_reg(void);
        extern void gadget_amd64_v_psubq_reg(void);
        extern void gadget_amd64_v_pxor_reg(void);
        extern void gadget_amd64_v_pcmpeqb_reg(void);
        extern void gadget_amd64_v_psubb_reg(void);
        extern void gadget_amd64_v_paddw_reg(void);
        extern void gadget_amd64_v_psubusw_reg(void);
        extern void gadget_amd64_v_por_reg(void);
        extern void gadget_amd64_v_punpcklbw_reg(void);
        extern void gadget_amd64_v_punpcklwd_reg(void);
        extern void gadget_amd64_v_punpckldq_reg(void);
        extern void gadget_amd64_v_punpcklqdq_reg(void);
        void (*gadget)(void) = NULL;
        switch (insn.op2) {
        case 0x6f: gadget = gadget_amd64_v_mov128_reg; break;
        case 0xd4: gadget = gadget_amd64_v_paddq_reg; break;
        case 0xfb: gadget = gadget_amd64_v_psubq_reg; break;
        case 0xef: gadget = gadget_amd64_v_pxor_reg; break;
        case 0x74: gadget = gadget_amd64_v_pcmpeqb_reg; break;
        case 0xf8: gadget = gadget_amd64_v_psubb_reg; break;
        case 0xfd: gadget = gadget_amd64_v_paddw_reg; break;
        case 0xd9: gadget = gadget_amd64_v_psubusw_reg; break;
        case 0xeb: gadget = gadget_amd64_v_por_reg; break;
        case 0x60: gadget = gadget_amd64_v_punpcklbw_reg; break;
        case 0x61: gadget = gadget_amd64_v_punpcklwd_reg; break;
        case 0x62: gadget = gadget_amd64_v_punpckldq_reg; break;
        case 0x6c: gadget = gadget_amd64_v_punpcklqdq_reg; break;
        }
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-reg op2=%02x ip=%llx src=%u dst=%u next=%llx",
                insn.op2, (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 57 xorps / 66 0F 57 xorpd, mod==3: a 128-bit bitwise XOR either way, so
    // no operand-size-prefix gating (unlike the SSE2 integer ops above). A rep
    // prefix is #UD — left to the bridge.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x57) {
        extern void gadget_amd64_v_pxor_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-xor op2=57 ip=%llx src=%u dst=%u next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget_amd64_v_pxor_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // SSE FP arithmetic, all four prefix forms x reg/mem operands:
    // 0F 58 add | 59 mul | 5C sub | 5D min | 5E div | 5F max, as
    // packed-single (no prefix), packed-double (66), scalar-single (F3),
    // scalar-double (F2). These are the instructions a compiled FP hot
    // loop is MADE of; every one previously round-tripped through the
    // interpreter bridge (a double matmul spent its whole runtime there).
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.op2 >= 0x58 && insn.op2 <= 0x5f && insn.op2 != 0x5a && insn.op2 != 0x5b &&
            !(insn.operand_size_prefix && insn.rep_mode != amd64_jit_rep_none)) {
        extern void gadget_amd64_v_addps_reg(void), gadget_amd64_v_addps_mem(void);
        extern void gadget_amd64_v_mulps_reg(void), gadget_amd64_v_mulps_mem(void);
        extern void gadget_amd64_v_subps_reg(void), gadget_amd64_v_subps_mem(void);
        extern void gadget_amd64_v_minps_reg(void), gadget_amd64_v_minps_mem(void);
        extern void gadget_amd64_v_divps_reg(void), gadget_amd64_v_divps_mem(void);
        extern void gadget_amd64_v_maxps_reg(void), gadget_amd64_v_maxps_mem(void);
        extern void gadget_amd64_v_addpd_reg(void), gadget_amd64_v_addpd_mem(void);
        extern void gadget_amd64_v_mulpd_reg(void), gadget_amd64_v_mulpd_mem(void);
        extern void gadget_amd64_v_subpd_reg(void), gadget_amd64_v_subpd_mem(void);
        extern void gadget_amd64_v_minpd_reg(void), gadget_amd64_v_minpd_mem(void);
        extern void gadget_amd64_v_divpd_reg(void), gadget_amd64_v_divpd_mem(void);
        extern void gadget_amd64_v_maxpd_reg(void), gadget_amd64_v_maxpd_mem(void);
        extern void gadget_amd64_v_addss_reg(void), gadget_amd64_v_addss_mem(void);
        extern void gadget_amd64_v_mulss_reg(void), gadget_amd64_v_mulss_mem(void);
        extern void gadget_amd64_v_subss_reg(void), gadget_amd64_v_subss_mem(void);
        extern void gadget_amd64_v_minss_reg(void), gadget_amd64_v_minss_mem(void);
        extern void gadget_amd64_v_divss_reg(void), gadget_amd64_v_divss_mem(void);
        extern void gadget_amd64_v_maxss_reg(void), gadget_amd64_v_maxss_mem(void);
        extern void gadget_amd64_v_addsd_reg(void), gadget_amd64_v_addsd_mem(void);
        extern void gadget_amd64_v_mulsd_reg(void), gadget_amd64_v_mulsd_mem(void);
        extern void gadget_amd64_v_subsd_reg(void), gadget_amd64_v_subsd_mem(void);
        extern void gadget_amd64_v_minsd_reg(void), gadget_amd64_v_minsd_mem(void);
        extern void gadget_amd64_v_divsd_reg(void), gadget_amd64_v_divsd_mem(void);
        extern void gadget_amd64_v_maxsd_reg(void), gadget_amd64_v_maxsd_mem(void);
        // [form][op][mem] — form: ps/pd/ss/sd; op: 58/59/5c/5d/5e/5f
        static void *const sse_arith[4][6][2] = {
            {{gadget_amd64_v_addps_reg, gadget_amd64_v_addps_mem},
             {gadget_amd64_v_mulps_reg, gadget_amd64_v_mulps_mem},
             {gadget_amd64_v_subps_reg, gadget_amd64_v_subps_mem},
             {gadget_amd64_v_minps_reg, gadget_amd64_v_minps_mem},
             {gadget_amd64_v_divps_reg, gadget_amd64_v_divps_mem},
             {gadget_amd64_v_maxps_reg, gadget_amd64_v_maxps_mem}},
            {{gadget_amd64_v_addpd_reg, gadget_amd64_v_addpd_mem},
             {gadget_amd64_v_mulpd_reg, gadget_amd64_v_mulpd_mem},
             {gadget_amd64_v_subpd_reg, gadget_amd64_v_subpd_mem},
             {gadget_amd64_v_minpd_reg, gadget_amd64_v_minpd_mem},
             {gadget_amd64_v_divpd_reg, gadget_amd64_v_divpd_mem},
             {gadget_amd64_v_maxpd_reg, gadget_amd64_v_maxpd_mem}},
            {{gadget_amd64_v_addss_reg, gadget_amd64_v_addss_mem},
             {gadget_amd64_v_mulss_reg, gadget_amd64_v_mulss_mem},
             {gadget_amd64_v_subss_reg, gadget_amd64_v_subss_mem},
             {gadget_amd64_v_minss_reg, gadget_amd64_v_minss_mem},
             {gadget_amd64_v_divss_reg, gadget_amd64_v_divss_mem},
             {gadget_amd64_v_maxss_reg, gadget_amd64_v_maxss_mem}},
            {{gadget_amd64_v_addsd_reg, gadget_amd64_v_addsd_mem},
             {gadget_amd64_v_mulsd_reg, gadget_amd64_v_mulsd_mem},
             {gadget_amd64_v_subsd_reg, gadget_amd64_v_subsd_mem},
             {gadget_amd64_v_minsd_reg, gadget_amd64_v_minsd_mem},
             {gadget_amd64_v_divsd_reg, gadget_amd64_v_divsd_mem},
             {gadget_amd64_v_maxsd_reg, gadget_amd64_v_maxsd_mem}},
        };
        static const int op_index[8] = {0, 1, -1, -1, 2, 3, 4, 5};
        int op = op_index[insn.op2 - 0x58];
        int form = insn.operand_size_prefix ? 1
                 : insn.rep_mode == amd64_jit_repz ? 2
                 : insn.rep_mode == amd64_jit_repnz ? 3 : 0;
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            amd64_jit_debug("v-fparith-reg op2=%02x form=%d ip=%llx src=%u dst=%u",
                    insn.op2, form, (unsigned long long) insn.start_ip, rm_id, reg_id);
            gen(state, (unsigned long) sse_arith[form][op][0]);
            gen(state, (unsigned long) (rm_id | (reg_id << 4)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        unsigned opsize = form == 0 || form == 1 ? 128 : form == 2 ? 32 : 64;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, opsize, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-fparith-mem op2=%02x form=%d ip=%llx dst=%u meta=%lx disp=%lx",
                insn.op2, form, (unsigned long long) insn.start_ip, reg_id, meta, disp);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        gen(state, (unsigned long) sse_arith[form][op][1]);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // movsd/movss (F2/F3 0F 10 load | 0F 11 store) — the scalar forms the
    // 128-bit movups branch above deliberately skips. Load forms ZERO the
    // rest of the register; reg-reg forms merge into the low lane only.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            (insn.rep_mode == amd64_jit_repnz || insn.rep_mode == amd64_jit_repz) &&
            (insn.op2 == 0x10 || insn.op2 == 0x11)) {
        extern void gadget_amd64_v_movsd_load(void), gadget_amd64_v_movsd_store(void);
        extern void gadget_amd64_v_movss_load(void), gadget_amd64_v_movss_store(void);
        extern void gadget_amd64_v_movsd_reg(void), gadget_amd64_v_movss_reg(void);
        bool is_double = insn.rep_mode == amd64_jit_repnz;
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            // register move: for 0F 10 dst=reg src=rm, for 0F 11 dst=rm src=reg
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            unsigned src = insn.op2 == 0x10 ? rm_id : reg_id;
            unsigned dst = insn.op2 == 0x10 ? reg_id : rm_id;
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            gen(state, (unsigned long) (is_double ? gadget_amd64_v_movsd_reg
                                                  : gadget_amd64_v_movss_reg));
            gen(state, (unsigned long) (src | (dst << 4)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, is_double ? 64 : 32,
                                       &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movs%c-%s ip=%llx xmm=%u meta=%lx disp=%lx",
                is_double ? 'd' : 's', insn.op2 == 0x10 ? "load" : "store",
                (unsigned long long) insn.start_ip, reg_id, meta, disp);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        void *gadget = insn.op2 == 0x10
            ? (is_double ? (void *) gadget_amd64_v_movsd_load : (void *) gadget_amd64_v_movss_load)
            : (is_double ? (void *) gadget_amd64_v_movsd_store : (void *) gadget_amd64_v_movss_store);
        gen(state, (unsigned long) gadget);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // unpcklpd/unpckhpd (66 0F 14/15) and unpcklps/unpckhps (0F 14/15):
    // NEON zip1/zip2. Both reg and m128 forms.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x14 || insn.op2 == 0x15)) {
        extern void gadget_amd64_v_unpcklpd_reg(void), gadget_amd64_v_unpcklpd_mem(void);
        extern void gadget_amd64_v_unpckhpd_reg(void), gadget_amd64_v_unpckhpd_mem(void);
        extern void gadget_amd64_v_unpcklps_reg(void), gadget_amd64_v_unpcklps_mem(void);
        extern void gadget_amd64_v_unpckhps_reg(void), gadget_amd64_v_unpckhps_mem(void);
        static void *const unpck[2][2][2] = { // [pd][hi][mem]
            {{gadget_amd64_v_unpcklps_reg, gadget_amd64_v_unpcklps_mem},
             {gadget_amd64_v_unpckhps_reg, gadget_amd64_v_unpckhps_mem}},
            {{gadget_amd64_v_unpcklpd_reg, gadget_amd64_v_unpcklpd_mem},
             {gadget_amd64_v_unpckhpd_reg, gadget_amd64_v_unpckhpd_mem}},
        };
        int pd = insn.operand_size_prefix ? 1 : 0;
        int hi = insn.op2 == 0x15 ? 1 : 0;
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            gen(state, (unsigned long) unpck[pd][hi][0]);
            gen(state, (unsigned long) (rm_id | (reg_id << 4)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        gen(state, (unsigned long) unpck[pd][hi][1]);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // movhpd/movhps (0F 16 load / 17 store) and movlpd/movlps (0F 12/13),
    // mod!=3 only: 8-byte moves into/out of one lane, other lane preserved.
    // mod==3 (movlhps/movhlps) keeps bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.op2 >= 0x12 && insn.op2 <= 0x17 && insn.op2 != 0x14 && insn.op2 != 0x15) {
        extern void gadget_amd64_v_movhp_load(void), gadget_amd64_v_movhp_store(void);
        extern void gadget_amd64_v_movlp_load(void), gadget_amd64_v_movlp_store(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 64, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        void *gadget = insn.op2 == 0x16 ? (void *) gadget_amd64_v_movhp_load
                     : insn.op2 == 0x17 ? (void *) gadget_amd64_v_movhp_store
                     : insn.op2 == 0x12 ? (void *) gadget_amd64_v_movlp_load
                     : (void *) gadget_amd64_v_movlp_store;
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        gen(state, (unsigned long) gadget);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }


    // F2 0F 2A cvtsi2sd xmm, r/m, mod==3: xmm[reg].f64[0] = (double) signed
    // GPR[rm] (REX.W -> 64-bit source, else 32-bit), high 64 bits preserved. The
    // source is a GPR, so flush the reg cache first to make cpu->amd64_regs
    // current; the gadget then reads it from memory. F2 (cvtsi2sd) only; F3
    // cvtsi2ss keeps bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            !insn.operand_size_prefix && insn.rep_mode == amd64_jit_repnz &&
            insn.op2 == 0x2a) {
        extern void gadget_amd64_v_cvtsi2sd_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-cvtsi2sd ip=%llx gpr=%u xmm=%u w=%d next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id, insn.rex.w,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_v_cvtsi2sd_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4) | ((insn.rex.w ? 1ul : 0ul) << 8)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 6E movd/movq xmm, r/m, mod==3: load a 32-bit (movd) or 64-bit (movq,
    // REX.W) GPR into xmm[reg], zeroing the rest. Reads a GPR -> flush the reg cache
    // (like cvtsi2sd). No F2/F3. (The mem form keeps bridging.)
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x6e) {
        extern void gadget_amd64_v_movd_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movd ip=%llx gpr=%u xmm=%u w=%d next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id, insn.rex.w,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_v_movd_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4) | ((insn.rex.w ? 1ul : 0ul) << 8)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F D7 pmovmskb r32, xmm, mod==3: pack the 16 byte-MSBs of xmm[rm] into a
    // 16-bit mask in GPR[reg] (zero-extended). Writes a GPR -> flush the reg cache.
    // This + pcmpeqb is the core of glibc's SSE2 strlen/memchr. (No mem form exists.)
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0xd7) {
        extern void gadget_amd64_v_pmovmskb_reg(void);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-pmovmskb ip=%llx xmm=%u gpr=%u next=%llx",
                (unsigned long long) insn.start_ip, rm_id, reg_id,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_v_pmovmskb_reg);
        gen(state, (unsigned long) (rm_id | (reg_id << 4)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 73 /2 ib psrlq | /6 ib psllq, mod==3: shift each 64-bit lane of an
    // xmm by imm8. The /digit is the op extension; the bridge folds REX.R into
    // modrm.reg and #UDs reg>=8, so REX.R cases are left to the bridge to match.
    // Only the qword shifts (/2,/6) go native; the byte shifts /3,/7 bridge.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.rex.r &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x73 &&
            (amd64_modrm_reg(insn.modrm) == 2 || amd64_modrm_reg(insn.modrm) == 6)) {
        extern void gadget_amd64_v_psrlq_imm(void);
        extern void gadget_amd64_v_psllq_imm(void);
        unsigned ext = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        uint8_t imm8;
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-shiftq op2=73 /%u ip=%llx rm=%u imm=%u next=%llx",
                ext, (unsigned long long) insn.start_ip, rm_id, imm8,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) (ext == 2
                    ? (void (*)(void)) gadget_amd64_v_psrlq_imm
                    : (void (*)(void)) gadget_amd64_v_psllq_imm));
        gen(state, (unsigned long) (rm_id | ((unsigned long) imm8 << 8)));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 73 /3 ib psrldq | /7 ib pslldq, mod==3: byte-wise shift of the full
    // 128-bit xmm. The shift amount is a compile-time constant, so this bakes a
    // tbl byte-index mask into the code stream (out-of-range index -> 0, which
    // is exactly the shifted-in zero fill) and reuses the generic tbl gadget.
    // Same REX.R exclusion as the qword shifts above.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.rex.r &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x73 &&
            (amd64_modrm_reg(insn.modrm) == 3 || amd64_modrm_reg(insn.modrm) == 7)) {
        extern void gadget_amd64_v_tbl_reg(void);
        bool is_left = amd64_modrm_reg(insn.modrm) == 7;
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        uint8_t imm8;
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        uint8_t mask[16];
        for (int i = 0; i < 16; i++) {
            int j = is_left ? i - (int) imm8 : i + (int) imm8;
            mask[i] = (j >= 0 && j < 16) ? (uint8_t) j : 0xff;
        }
        unsigned long mask_lo, mask_hi;
        memcpy(&mask_lo, mask, 8);
        memcpy(&mask_hi, mask + 8, 8);
        amd64_jit_debug("v-shiftdq op2=73 /%u ip=%llx rm=%u imm=%u next=%llx",
                is_left ? 7 : 3, (unsigned long long) insn.start_ip, rm_id, imm8,
                (unsigned long long) next_ip);
        gen(state, (unsigned long) gadget_amd64_v_tbl_reg);
        gen(state, (unsigned long) (rm_id | (rm_id << 4)));
        gen(state, mask_lo);
        gen(state, mask_hi);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // F3 0F 7E movq xmm, xmm/m64: load 64 bits into the low lane of xmm[reg],
    // zeroing the high lane. The reg-reg form is a low-lane copy; the mem form
    // reads through the 64-bit vread fast path. (66 F3 combos keep bridging.)
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            !insn.operand_size_prefix && insn.rep_mode == amd64_jit_repz &&
            insn.op2 == 0x7e) {
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            extern void gadget_amd64_v_movq_xx_reg(void);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            amd64_jit_debug("v-movq-xx ip=%llx src=%u dst=%u next=%llx",
                    (unsigned long long) insn.start_ip, rm_id, reg_id,
                    (unsigned long long) next_ip);
            gen(state, (unsigned long) gadget_amd64_v_movq_xx_reg);
            gen(state, (unsigned long) (rm_id | (reg_id << 4)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 64, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movq-load ip=%llx xmm=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, reg_id, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load64_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_load64_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 7E movd/movq r/m, xmm: store the low 32 (movd) or 64 (movq, REX.W)
    // bits of xmm[reg]. mod==3 writes a GPR (flush-style, like pmovmskb); the
    // mem form goes through the vwrite fast path.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x7e) {
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            extern void gadget_amd64_v_movd_store_reg(void);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            amd64_jit_debug("v-movd-store ip=%llx xmm=%u gpr=%u w=%d next=%llx",
                    (unsigned long long) insn.start_ip, reg_id, rm_id, insn.rex.w,
                    (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_v_movd_store_reg);
            gen(state, (unsigned long) (rm_id | (reg_id << 4) | ((insn.rex.w ? 1ul : 0ul) << 8)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, insn.rex.w ? 64 : 32,
                    &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movd-store-mem ip=%llx xmm=%u w=%d meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, reg_id, insn.rex.w, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_store64_mem(void);
        extern void gadget_amd64_v_store32_mem(void);
        gen(state, (unsigned long) (insn.rex.w
                    ? (void (*)(void)) gadget_amd64_v_store64_mem
                    : (void (*)(void)) gadget_amd64_v_store32_mem));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F 6E movd/movq xmm, m32/m64 (mem form; the reg form is native above):
    // load through the vread fast path, zero the rest of the xmm.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0x6e) {
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, insn.rex.w ? 64 : 32,
                    &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movd-load-mem ip=%llx xmm=%u w=%d meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, reg_id, insn.rex.w, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load64_mem(void);
        extern void gadget_amd64_v_load32_mem(void);
        gen(state, (unsigned long) (insn.rex.w
                    ? (void (*)(void)) gadget_amd64_v_load64_mem
                    : (void (*)(void)) gadget_amd64_v_load32_mem));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 66 0F D6 movq xmm/m64, xmm: store the low 64 bits of xmm[reg]. mod==3 is
    // a low-lane copy with zero-fill (same gadget as F3 0F 7E, operands swapped:
    // dst=rm, src=reg); the mem form goes through the vwrite fast path. F3 0F D6
    // (movq2dq, MMX source) and F2 (movdq2q) keep bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            insn.op2 == 0xd6) {
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        if (amd64_modrm_mod(insn.modrm) == 3) {
            extern void gadget_amd64_v_movq_xx_reg(void);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            amd64_jit_debug("v-movq-store-xx ip=%llx src=%u dst=%u next=%llx",
                    (unsigned long long) insn.start_ip, reg_id, rm_id,
                    (unsigned long long) next_ip);
            gen(state, (unsigned long) gadget_amd64_v_movq_xx_reg);
            gen(state, (unsigned long) (reg_id | (rm_id << 4)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 64, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movq-store-mem ip=%llx xmm=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, reg_id, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_store64_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_store64_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 70 pshufd (66) / pshufhw (F3) / pshuflw (F2) xmm, xmm/m128, imm8: the
    // shuffle imm is a compile-time constant, so bake the equivalent 16-byte tbl
    // index mask into the code stream and use the generic tbl gadgets. pshuflw
    // and pshufhw shuffle one 64-bit half and copy the other through (identity
    // indices). MMX pshufw (no prefix) keeps bridging.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.op2 == 0x70 &&
            (insn.operand_size_prefix
                 ? insn.rep_mode == amd64_jit_rep_none
                 : insn.rep_mode != amd64_jit_rep_none)) {
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        bool is_mem = amd64_modrm_mod(insn.modrm) != 3;
        unsigned long meta = 0, disp = 0;
        unsigned rm_id = 0;
        if (is_mem) {
            if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
        } else {
            rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
        }
        uint8_t imm8;
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        uint8_t mask[16];
        if (insn.operand_size_prefix) { // pshufd: four 32-bit lanes
            for (int i = 0; i < 4; i++) {
                unsigned sel = (imm8 >> (2 * i)) & 3;
                for (int b = 0; b < 4; b++)
                    mask[4 * i + b] = (uint8_t) (4 * sel + b);
            }
        } else if (insn.rep_mode == amd64_jit_repz) { // pshufhw: high 4 words
            for (int i = 0; i < 8; i++)
                mask[i] = (uint8_t) i;
            for (int i = 0; i < 4; i++) {
                unsigned sel = (imm8 >> (2 * i)) & 3;
                mask[8 + 2 * i] = (uint8_t) (8 + 2 * sel);
                mask[8 + 2 * i + 1] = (uint8_t) (8 + 2 * sel + 1);
            }
        } else { // pshuflw (F2): low 4 words
            for (int i = 0; i < 4; i++) {
                unsigned sel = (imm8 >> (2 * i)) & 3;
                mask[2 * i] = (uint8_t) (2 * sel);
                mask[2 * i + 1] = (uint8_t) (2 * sel + 1);
            }
            for (int i = 8; i < 16; i++)
                mask[i] = (uint8_t) i;
        }
        unsigned long mask_lo, mask_hi;
        memcpy(&mask_lo, mask, 8);
        memcpy(&mask_hi, mask + 8, 8);
        amd64_jit_debug("v-pshuf ip=%llx form=%s dst=%u imm=%u mem=%d next=%llx",
                (unsigned long long) insn.start_ip,
                insn.operand_size_prefix ? "d" : (insn.rep_mode == amd64_jit_repz ? "hw" : "lw"),
                reg_id, imm8, is_mem, (unsigned long long) next_ip);
        if (is_mem) {
            gen_amd64_flush_reg_cache(state);
            gen_amd64_flush_rip(state);
            extern void gadget_amd64_v_tbl_mem(void);
            gen(state, (unsigned long) gadget_amd64_v_tbl_mem);
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            gen(state, mask_lo);
            gen(state, mask_hi);
        } else {
            extern void gadget_amd64_v_tbl_reg(void);
            gen(state, (unsigned long) gadget_amd64_v_tbl_reg);
            gen(state, (unsigned long) (rm_id | (reg_id << 4)));
            gen(state, mask_lo);
            gen(state, mask_hi);
        }
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Memory-source forms of the native SSE2 binops (66 0F /r, mod!=3):
    // dst = dst OP m128 through the 128-bit vread fast path. Same op set as the
    // reg-reg clause above (paddw, psubusw, por, punpckl*, pcmpeqb, plus the
    // ones whose reg forms landed earlier).
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.operand_size_prefix && insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0xfd || insn.op2 == 0xd9 || insn.op2 == 0xeb ||
             insn.op2 == 0x60 || insn.op2 == 0x61 || insn.op2 == 0x62 ||
             insn.op2 == 0x6c || insn.op2 == 0x74)) {
        extern void gadget_amd64_v_paddw_mem(void);
        extern void gadget_amd64_v_psubusw_mem(void);
        extern void gadget_amd64_v_por_mem(void);
        extern void gadget_amd64_v_punpcklbw_mem(void);
        extern void gadget_amd64_v_punpcklwd_mem(void);
        extern void gadget_amd64_v_punpckldq_mem(void);
        extern void gadget_amd64_v_punpcklqdq_mem(void);
        extern void gadget_amd64_v_pcmpeqb_mem(void);
        void (*gadget)(void) = NULL;
        switch (insn.op2) {
        case 0xfd: gadget = gadget_amd64_v_paddw_mem; break;
        case 0xd9: gadget = gadget_amd64_v_psubusw_mem; break;
        case 0xeb: gadget = gadget_amd64_v_por_mem; break;
        case 0x60: gadget = gadget_amd64_v_punpcklbw_mem; break;
        case 0x61: gadget = gadget_amd64_v_punpcklwd_mem; break;
        case 0x62: gadget = gadget_amd64_v_punpckldq_mem; break;
        case 0x6c: gadget = gadget_amd64_v_punpcklqdq_mem; break;
        case 0x74: gadget = gadget_amd64_v_pcmpeqb_mem; break;
        }
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-binop-mem op2=%02x ip=%llx meta=%lx disp=%lx next=%llx",
                insn.op2, (unsigned long long) insn.start_ip, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        gen(state, (unsigned long) gadget);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 28 movaps / 0F 10 movups (+66 movapd/movupd), reg<-mem, mod!=3: native
    // 128-bit xmm load through a 64-bit TLB fast path. The interp does not enforce
    // movaps alignment (reads like movups), so one gadget serves both. F2/F3
    // (movsd/movss scalar) and fs/address-size forms keep bridging. The reg cache
    // and rip are flushed so a page fault re-executes this instruction correctly.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x28 || insn.op2 == 0x10)) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-load128-mem ip=%llx op2=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load128_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_load128_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // 0F 29 movaps / 0F 11 movups (+66 movapd/movupd), reg->mem, mod!=3: native
    // 128-bit xmm store through the 64-bit TLB write path (staleness + page_if_
    // writable + cross-page staging). Same caveats as the load: alignment not
    // enforced, F2/F3 scalar + fs/address-size forms bridge, cache+rip flushed.
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode == amd64_jit_rep_none &&
            (insn.op2 == 0x29 || insn.op2 == 0x11)) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-store128-mem ip=%llx op2=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_store128_mem(void);
        gen(state, (unsigned long) gadget_amd64_v_store128_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // movdqa (66 0F 6F load / 7F store) and movdqu (F3 0F 6F / 7F), reg<->mem,
    // mod!=3: a 128-bit move identical to movaps/movups, so it reuses the same
    // load128/store128 gadgets. NO-prefix 0F 6F/7F is MMX movq (64-bit mm regs)
    // and is excluded; F2 is not a valid movdq prefix. (Go's memmove path.)
    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            !insn.fs_prefix && !insn.lock_prefix &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.rep_mode != amd64_jit_repnz &&
            (insn.operand_size_prefix || insn.rep_mode == amd64_jit_repz) &&
            (insn.op2 == 0x6f || insn.op2 == 0x7f)) {
        bool is_store = insn.op2 == 0x7f;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 128, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("v-movdq-mem ip=%llx op2=%02x store=%d meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.op2, is_store, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_v_load128_mem(void);
        extern void gadget_amd64_v_store128_mem(void);
        gen(state, (unsigned long) (is_store
                    ? (void (*)(void)) gadget_amd64_v_store128_mem
                    : (void (*)(void)) gadget_amd64_v_load128_mem));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }
#endif

    if (!insn.address_size_prefix && insn.two_byte_opcode && insn.has_modrm &&
            (insn.op2 == 0x10 ||
             insn.op2 == 0x2a ||
             insn.op2 == 0x2c ||
             insn.op2 == 0x2d ||
             insn.op2 == 0x2e ||
             insn.op2 == 0x2f ||
             insn.op2 == 0x11 ||
             insn.op2 == 0x12 ||
             insn.op2 == 0x13 ||
             insn.op2 == 0x14 ||
             insn.op2 == 0x15 ||
             insn.op2 == 0x16 ||
             insn.op2 == 0x17 ||
             insn.op2 == 0x28 ||
             insn.op2 == 0x29 ||
             insn.op2 == 0x50 ||
             insn.op2 == 0x51 ||
             insn.op2 == 0x52 ||
             insn.op2 == 0x53 ||
             insn.op2 == 0x54 ||
             insn.op2 == 0x55 ||
             insn.op2 == 0x56 ||
             insn.op2 == 0x57 ||
             insn.op2 == 0x58 ||
             insn.op2 == 0x59 ||
             insn.op2 == 0x5a ||
             insn.op2 == 0x5b ||
             insn.op2 == 0x5c ||
             insn.op2 == 0x5d ||
             insn.op2 == 0x5e ||
             insn.op2 == 0x5f ||
             insn.op2 == 0x60 ||
             insn.op2 == 0x61 ||
             insn.op2 == 0x62 ||
             insn.op2 == 0x63 ||
             insn.op2 == 0x64 ||
             insn.op2 == 0x65 ||
             insn.op2 == 0x66 ||
             insn.op2 == 0x67 ||
             insn.op2 == 0x68 ||
             insn.op2 == 0x69 ||
             insn.op2 == 0x6a ||
             insn.op2 == 0x6b ||
             insn.op2 == 0x6c ||
             insn.op2 == 0x6d ||
             insn.op2 == 0x6e ||
             insn.op2 == 0x6f ||
             insn.op2 == 0x70 ||
             ((insn.op2 == 0x71 || insn.op2 == 0x72 || insn.op2 == 0x73) &&
              insn.operand_size_prefix) ||
             insn.op2 == 0x74 ||
             insn.op2 == 0x75 ||
             insn.op2 == 0x76 ||
             insn.op2 == 0x7e ||
             insn.op2 == 0x7f ||
             insn.op2 == 0xc2 ||
             insn.op2 == 0xc4 ||
             insn.op2 == 0xc5 ||
             insn.op2 == 0xc6 ||
             insn.op2 == 0xd1 ||
             insn.op2 == 0xd2 ||
             insn.op2 == 0xd3 ||
             insn.op2 == 0xd4 ||
             insn.op2 == 0xd5 ||
             insn.op2 == 0xd6 ||
             insn.op2 == 0xd7 ||
             insn.op2 == 0xd8 ||
             insn.op2 == 0xd9 ||
             insn.op2 == 0xda ||
             insn.op2 == 0xdb ||
             insn.op2 == 0xdc ||
             insn.op2 == 0xdd ||
             insn.op2 == 0xde ||
             insn.op2 == 0xdf ||
             insn.op2 == 0xe0 ||
             insn.op2 == 0xe1 ||
             insn.op2 == 0xe2 ||
             insn.op2 == 0xe3 ||
             insn.op2 == 0xe4 ||
             insn.op2 == 0xe5 ||
             insn.op2 == 0xe6 ||
             insn.op2 == 0xe7 ||
             insn.op2 == 0xe8 ||
             insn.op2 == 0xe9 ||
             insn.op2 == 0xea ||
             insn.op2 == 0xeb ||
             insn.op2 == 0xec ||
             insn.op2 == 0xed ||
             insn.op2 == 0xee ||
             insn.op2 == 0xf1 ||
             insn.op2 == 0xf2 ||
             insn.op2 == 0xf3 ||
             insn.op2 == 0xf4 ||
             insn.op2 == 0xf6 ||
             insn.op2 == 0xf8 ||
             insn.op2 == 0xf9 ||
             insn.op2 == 0xfa ||
             insn.op2 == 0xfb ||
             insn.op2 == 0xfc ||
             insn.op2 == 0xfd ||
             insn.op2 == 0xfe ||
             insn.op2 == 0xef)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.op2 == 0x70 || insn.op2 == 0x71 || insn.op2 == 0x72 ||
                insn.op2 == 0x73 || insn.op2 == 0xc2 || insn.op2 == 0xc4 ||
                insn.op2 == 0xc5 || insn.op2 == 0xc6)
            next_ip += sizeof(uint8_t);
        state->amd64_ip = next_ip;
        amd64_jit_debug("0f-vec-rm-helper ip=%llx op2=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.op2,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_0f_vec_rm,
                (unsigned long) insn.op2, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

#if defined(__aarch64__)
    // Native TEST byte [mem], imm8 (0xf6 /0), mod!=3 -- the #2 byte bridge in cc1
    // (testb $imm,mem). AND for flags, no store, 8-bit logic flag rule. Placed before
    // the grp3-test clause so it intercepts the memory case (which otherwise bridges).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0xf6 && amd64_modrm_reg(insn.modrm) == 0) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 8, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        uint8_t imm8;
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        long imm = imm8;
        next_ip += sizeof(imm8);
        state->amd64_ip = next_ip;
        amd64_jit_debug("byte-test-imm-mem ip=%llx imm=%lx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, (unsigned long) imm,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_byte_test_imm_mem(void);
        gen(state, (unsigned long) gadget_amd64_byte_test_imm_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen(state, (unsigned long) imm);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }
#endif

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0xf6 || insn.opcode == 0xf7) &&
            amd64_modrm_reg(insn.modrm) == 0) {
        size_t imm_size;
        unsigned size = insn.opcode == 0xf6
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        imm_size = insn.opcode == 0xf6
            ? sizeof(uint8_t)
            : (insn.operand_size_prefix ? sizeof(uint16_t) : sizeof(uint32_t));
        next_ip += imm_size;
        if (!insn.fs_prefix && !insn.lock_prefix &&
                amd64_modrm_mod(insn.modrm) == 3) {
            unsigned rm_raw = amd64_modrm_rm(insn.modrm);
            unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
            unsigned long value;
            guest_addr_t imm_ip = state->amd64_ip + 1;
            if (size == 8) {
                uint8_t imm8;
                if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = imm8;
            } else if (size == 16) {
                uint16_t imm16;
                if (!tlb_read(tlb, imm_ip, &imm16, sizeof(imm16))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = imm16;
            } else {
                int32_t imm32;
                if (!tlb_read(tlb, imm_ip, &imm32, sizeof(imm32))) {
                    state->amd64_ip = state->amd64_orig_ip;
                    state->amd64_fallback_to_interp = true;
                    return false;
                }
                value = insn.rex.w
                    ? (unsigned long) (qword_t) (sqword_t) imm32
                    : (unsigned long) (uint32_t) imm32;
            }
            if (!(size == 8 && !insn.rex.present && rm_raw >= 4)) {
                state->amd64_ip = next_ip;
                amd64_jit_debug("grp3-test-reg-imm-direct ip=%llx rm=%u size=%u value=%llx next=%llx",
                        (unsigned long long) insn.start_ip,
                        rm_id,
                        size,
                        (unsigned long long) value,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_test_reg_imm(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_test_reg_imm);
                    gen(state, rm_id | ((unsigned long) size << 4));
                    gen(state, value);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_test_reg_imm(void);
                gen(state, (unsigned long) gadget_amd64_test_reg_imm);
                gen(state, rm_id | ((unsigned long) size << 4));
                gen(state, value);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#if defined(__aarch64__)
            // The case excluded above: high-byte AH/CH/DH/BH TEST r/m8, imm8 (size==8,
            // !rex, rm_raw>=4). Cache-aware byte gadget reads bits 8-15 of the base reg.
            state->amd64_ip = next_ip;
            unsigned byte_id = rm_raw - 4;
            amd64_jit_debug("byte-test-imm-hi-direct ip=%llx rm=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip, byte_id,
                    (unsigned long long) value, (unsigned long long) next_ip);
            extern void gadget_amd64_byte_test_imm_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_byte_test_imm_reg);
            gen(state, (unsigned long) byte_id | (1ul << 4));
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
#endif
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("grp3-test-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_grp3_test,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    // The grp3 helper does not parse a LOCK prefix; route lock not/neg (legal
    // on memory operands) to the interpreter.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0xf6 || insn.opcode == 0xf7) &&
            amd64_modrm_reg(insn.modrm) >= 2) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
#if defined(__aarch64__)
        // Native byte NOT (0xf6 /2) / NEG (0xf6 /3), mod==3. Cache-aware; handles AH-BH
        // and r8-r15 byte. NOT writes ~src (no flags); NEG sets sub flags (0 - src).
        if (insn.opcode == 0xf6 && !insn.fs_prefix &&
                amd64_modrm_mod(insn.modrm) == 3 &&
                (amd64_modrm_reg(insn.modrm) == 2 || amd64_modrm_reg(insn.modrm) == 3)) {
            unsigned raw_rm = amd64_modrm_rm(insn.modrm);
            unsigned grp3_rm_id = raw_rm | (insn.rex.b ? 8 : 0);
            bool is_high = !insn.rex.present && raw_rm >= 4;
            unsigned byte_id = is_high ? raw_rm - 4 : grp3_rm_id;
            bool is_neg = amd64_modrm_reg(insn.modrm) == 3;
            unsigned long bpacked = (unsigned long) byte_id |
                ((unsigned long) (is_high ? 1 : 0) << 4) |
                ((unsigned long) (is_neg ? 1 : 0) << 5);
            amd64_jit_debug("byte-grp3-direct ip=%llx neg=%u rm=%u(hi%u) next=%llx",
                    (unsigned long long) insn.start_ip, is_neg, byte_id, is_high,
                    (unsigned long long) next_ip);
            extern void gadget_amd64_byte_grp3(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_byte_grp3);
            gen(state, bpacked);
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        // Native 32/64-bit NOT (0xf7 /2) / NEG (0xf7 /3), mod==3 -- the wider siblings of
        // byte_grp3. Bridged before (ending the JIT block). 16-bit + memory keep bridging.
        if (insn.opcode == 0xf7 && !insn.fs_prefix &&
                amd64_modrm_mod(insn.modrm) == 3 &&
                (amd64_modrm_reg(insn.modrm) == 2 || amd64_modrm_reg(insn.modrm) == 3)) {
            unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
            if (size == 32 || size == 64) {
                unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
                bool is_neg = amd64_modrm_reg(insn.modrm) == 3;
                unsigned long packed = (unsigned long) rm_id |
                    ((unsigned long) (is_neg ? 1 : 0) << 4);
                amd64_jit_debug("grp3-negnot-direct ip=%llx neg=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip, is_neg, rm_id, size,
                        (unsigned long long) next_ip);
                extern void gadget_amd64_negnot32(void), gadget_amd64_negnot64(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) (size == 64
                            ? gadget_amd64_negnot64 : gadget_amd64_negnot32));
                gen(state, packed);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        amd64_jit_debug("grp3-op-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_grp3_op,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        gen_exit(state);
        return false;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0xfe &&
            amd64_modrm_reg(insn.modrm) <= 1) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("fe-group-helper ip=%llx modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_fe_group,
                (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0xff) {
        unsigned group = amd64_modrm_reg(insn.modrm);
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        // Native memory-indirect call (/2) and jmp (/4): mod!=3, target is the
        // 8-byte value read from the effective address -- PLT stubs
        // `jmp/call *off(%rip)` and vtable/function-pointer dispatch, the common
        // real-code case. gen_amd64_decode_mem_meta needs amd64_ip still at the
        // opcode, so this runs before the advance below. 64-bit only (no 0x66);
        // FS-prefix and address-size forms keep bridging (the amd64_vmem_addr gadget
        // handles neither). rip is flushed so a #PF on the load/push re-executes.
        if ((group == 2 || group == 4) && amd64_modrm_mod(insn.modrm) != 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix && !insn.fs_prefix) {
            unsigned long meta, disp;
            if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 64, &meta, &disp, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            gen_amd64_flush_reg_cache(state);
            gen_amd64_flush_rip(state);
            if (group == 4) {
                amd64_jit_debug("jmp-indir-mem ip=%llx meta=%lx disp=%lx next=%llx",
                        (unsigned long long) insn.start_ip, meta, disp,
                        (unsigned long long) next_ip);
                extern void gadget_amd64_jmp_indir_mem(void);
                gen(state, (unsigned long) gadget_amd64_jmp_indir_mem);
            } else {
                amd64_jit_debug("call-indir-mem ip=%llx meta=%lx disp=%lx next=%llx",
                        (unsigned long long) insn.start_ip, meta, disp,
                        (unsigned long long) next_ip);
                extern void gadget_amd64_call_indir_mem(void);
                gen(state, (unsigned long) gadget_amd64_call_indir_mem);
            }
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            return false;
        }
        // Native INC (/0) / DEC (/1) [mem]: mod!=3, 32/64-bit. Read-modify-write that
        // preserves CF (the gadget snapshots and restores it around the add/sub flags),
        // matching amd64_jit_ff_group case 0/1. No 0x66 (16-bit OF needs a sub-32
        // width-correct overflow), no FS-prefix (vmem_addr can't add the TLS base), no
        // lock (atomic path bridges), no rex.r (keeps the reg field == the group so the
        // gadget reads is_dec from meta bit 8 bit-exactly). Runs before the amd64_ip
        // advance because decode_mem_meta needs amd64_ip at the opcode.
        if ((group == 0 || group == 1) && amd64_modrm_mod(insn.modrm) != 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix &&
                !insn.fs_prefix && !insn.rex.r) {
            unsigned size = insn.rex.w ? 64 : 32;
            unsigned long meta, disp;
            if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            state->amd64_ip = next_ip;
            amd64_jit_debug("incdec-mem ip=%llx grp=%u size=%u meta=%lx disp=%lx next=%llx",
                    (unsigned long long) insn.start_ip, group, size,
                    meta, disp, (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            gen_amd64_flush_rip(state);
            extern void gadget_amd64_incdec_mem32(void), gadget_amd64_incdec_mem64(void);
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_incdec_mem64 : gadget_amd64_incdec_mem32));
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        state->amd64_ip = next_ip;
        // Native register-indirect call (/2) and jmp (/4): mod==3, target =
        // regs[rm]. The memory forms and the other groups (inc/dec/push/far)
        // keep bridging. Always 64-bit in long mode (require no 0x66).
        if ((group == 2 || group == 4) && amd64_modrm_mod(insn.modrm) == 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix) {
            unsigned long rm = amd64_modrm_rm(insn.modrm);
            if (insn.rex.b)
                rm |= 8;
            gen_amd64_flush_reg_cache(state);
            if (group == 4) {
                amd64_jit_debug("jmp-indir-reg ip=%llx rm=%lu",
                        (unsigned long long) insn.start_ip, rm);
                state->amd64_deferred_rip_valid = false;
                extern void gadget_amd64_jmp_indir_reg(void);
                gen(state, (unsigned long) gadget_amd64_jmp_indir_reg);
                gen(state, rm);
            } else {
                amd64_jit_debug("call-indir-reg ip=%llx rm=%lu next=%llx",
                        (unsigned long long) insn.start_ip, rm,
                        (unsigned long long) next_ip);
                gen_amd64_flush_rip(state);
                extern void gadget_amd64_call_indir_reg(void);
                gen(state, (unsigned long) gadget_amd64_call_indir_reg);
                gen(state, rm);
                gen(state, (unsigned long) next_ip);
            }
            return false;
        }
#if defined(__aarch64__)
        // Native INC (/0) / DEC (/1) on a REGISTER (mod==3), 32/64-bit. The
        // counterpart of the incdec-mem path above, and the hotter one: long mode
        // reuses the one-byte 0x40+r inc/dec encodings as the REX prefixes, so
        // every `incq %rax` in compiled code arrives here as FF /0 mod==3. It was
        // the last hot FF form still bridging to amd64_jit_ff_group, which is the
        // single largest amd64 bridge helper (1.85% self time, measured in
        // 2abe9a1f); the bridge additionally forced a gadget_amd64_set_rip
        // dispatch on every execution, because the helper re-decodes its own
        // instruction out of guest memory at CPU_amd64_rip.
        //
        // Register-only, so the gadget cannot fault and the rip may be DEFERRED
        // rather than published -- that deferral is where the set_rip saving comes
        // from, and it is only sound because there is no #PF re-execution path
        // through this instruction.
        //
        // Declined (the bridge stays the oracle): 0x66, because 16-bit OF needs a
        // width-correct sub-32 overflow, exactly as for the mem form; lock, which
        // is #UD on a register operand and must keep raising it from the helper;
        // and rex.r, which amd64_decode_modrm folds into modrm.reg -- that would
        // make the helper see a group of 8/9 where this path would execute an
        // INC, so it must not be claimed here. rex.b is fine: it extends rm, and
        // the hybrid gadget accessors cover r8-r15 out of CPU_amd64_regs. The FS
        // prefix has no effect on a register operand, but is declined too rather
        // than reasoned about. Gated by /proc/ish/amd64_jit_fuse incdec_reg.
        if (group <= 1 && amd64_modrm_mod(insn.modrm) == 3 &&
                !insn.lock_prefix && !insn.operand_size_prefix &&
                !insn.fs_prefix && !insn.rex.r &&
                (amd64_jit_fuse_mask() & JIT_FUSE_AMD64_INCDEC_REG)) {
            unsigned long rm = amd64_modrm_rm(insn.modrm);
            if (insn.rex.b)
                rm |= 8;
            unsigned size = insn.rex.w ? 64 : 32;
            amd64_jit_debug("incdec-reg ip=%llx grp=%u rm=%lu size=%u next=%llx",
                    (unsigned long long) insn.start_ip, group, rm, size,
                    (unsigned long long) next_ip);
            extern void gadget_amd64_incdec_reg32(void), gadget_amd64_incdec_reg64(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_incdec_reg64 : gadget_amd64_incdec_reg32));
            gen(state, rm | ((unsigned long) group << 8));   // group 1 == is_dec
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        amd64_jit_debug("ff-group-helper ip=%llx modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_ff_group,
                (unsigned long) next_ip);
        if (group <= 1)
            return true;
        gen_exit(state);
        return false;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode >= 0xb0 && insn.opcode <= 0xb7) {
        uint8_t imm8;
        unsigned long reg_size;
        reg = (unsigned long) (insn.opcode - 0xb0);
        if (insn.rex.b)
            reg |= 8;
        if (!tlb_read(tlb, state->amd64_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        next_ip = state->amd64_ip + sizeof(imm8);
        state->amd64_ip = next_ip;
        reg_size = reg | (8ul << 8);
        if (insn.rex.present)
            reg_size |= 1ul << 16;
        if (insn.rex.present || insn.opcode <= 0xb3) {
            amd64_jit_debug("mov-imm8-reg-direct ip=%llx reg=%lu value=%x next=%llx",
                    (unsigned long long) insn.start_ip,
                    reg,
                    imm8,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg((unsigned) reg)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, reg | (8ul << 4));
                gen(state, (unsigned long) imm8);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm8_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm8_reg);
            gen(state, reg);
            gen(state, (unsigned long) imm8);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        amd64_jit_debug("mov-imm-helper ip=%llx reg=%lu size=8 value=%x next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                imm8,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_mov_imm,
                reg_size, (unsigned long) imm8, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0xb8 && insn.opcode <= 0xbf) {
        uint64_t value;
        uint32_t imm32;
        uint16_t imm16;
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        reg = (unsigned long) (insn.opcode - 0xb8);
        if (insn.rex.b)
            reg |= 8;
        if (size == 64) {
            if (!tlb_read(tlb, state->amd64_ip, &value, sizeof(value))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            next_ip = state->amd64_ip + sizeof(value);
        } else if (size == 16) {
            if (!tlb_read(tlb, state->amd64_ip, &imm16, sizeof(imm16))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm16;
            next_ip = state->amd64_ip + sizeof(imm16);
        } else {
            if (!tlb_read(tlb, state->amd64_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm32;
            next_ip = state->amd64_ip + sizeof(imm32);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov-imm-reg-direct ip=%llx reg=%lu size=%u value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) value,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg((unsigned) reg)) {
            extern void gadget_amd64_cached_mov_imm_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
            gen(state, reg | ((unsigned long) size << 4));
            gen(state, (unsigned long) value);
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov_imm_reg(void);
        gen(state, (unsigned long) gadget_amd64_mov_imm_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen(state, (unsigned long) value);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_push_pop_prefixes_ok(&insn) &&
            insn.opcode >= 0x50 && insn.opcode <= 0x57) {
        reg = (unsigned long) (insn.opcode - 0x50);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        amd64_jit_debug("push-reg ip=%llx reg=%lu next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                (unsigned long long) next_ip);
        state->amd64_ip = next_ip;
        // Native 64-bit stack push. Flush the reg cache + rip so the gadget reads
        // guest registers from CPU_amd64_regs and a #PF re-executes this insn.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_reg(void);
        gen(state, (unsigned long) gadget_amd64_push_reg);
        gen(state, reg);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_push_pop_prefixes_ok(&insn) &&
            insn.opcode >= 0x58 && insn.opcode <= 0x5f) {
        reg = (unsigned long) (insn.opcode - 0x58);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        amd64_jit_debug("pop-reg ip=%llx reg=%lu next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                (unsigned long long) next_ip);
        state->amd64_ip = next_ip;
        // Native 64-bit stack pop. Same reg-cache/rip flush contract as push.
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_pop_reg(void);
        gen(state, (unsigned long) gadget_amd64_pop_reg);
        gen(state, reg);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            insn.opcode == 0x8f) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("pop-rm-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_1_retint(state, amd64_jit_pop_rm,
                (unsigned long) next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            (insn.opcode == 0x68 || insn.opcode == 0x6a)) {
        unsigned long value;
        if (insn.opcode == 0x68) {
            int32_t imm32;
            if (!tlb_read(tlb, state->amd64_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm32;
            next_ip = state->amd64_ip + sizeof(imm32);
        } else {
            int8_t imm8;
            if (!tlb_read(tlb, state->amd64_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm8;
            next_ip = state->amd64_ip + sizeof(imm8);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("push-imm ip=%llx value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) value,
                (unsigned long long) next_ip);
        // Native push of a sign-extended 64-bit immediate (value computed above).
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_push_imm(void);
        gen(state, (unsigned long) gadget_amd64_push_imm);
        gen(state, value);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode == 0x9c) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("pushf-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_push_flags,
                64, (unsigned long) next_ip);
        return true;
    }

    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            insn.opcode == 0x9d) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("popf-helper ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_pop_flags,
                64, (unsigned long) next_ip);
        return true;
    }

#if defined(__aarch64__)
    // Native SAHF (0x9e) / LAHF (0x9f). One byte, no operand, no memory; they only move
    // bits between AH (rax bits 8-15) and the low-byte flags (SF/ZF/AF/PF/CF). Neither
    // was implemented in the amd64 engine before -- the interp lacked case 0x9e/0x9f and
    // the JIT had no gadget, so guest sahf/lahf SIGILL'd. Flush-style (like the rotate/
    // shift-count gadgets): flush the reg cache so the gadget reads/writes rax + the
    // eager flags straight from CPU state.
    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            (insn.opcode == 0x9e || insn.opcode == 0x9f)) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("flag-ah-op ip=%llx op=%02x next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_sahf(void), gadget_amd64_lahf(void);
        gen(state, (unsigned long) (insn.opcode == 0x9e
                    ? gadget_amd64_sahf : gadget_amd64_lahf));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }
#endif

    // Native carry-flag ops: CLC (0xf8), STC (0xf9), CMC (0xf5). One byte, no operand,
    // no memory, no register -- only CF changes. Cached-style (no flush): the gadget
    // rewrites CPU_cf + eflags bit0 and the reg cache stays live. Previously these
    // bridged, forcing an interp fallback for the rest of the block (bignum stc;adc).
    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            (insn.opcode == 0xf8 || insn.opcode == 0xf9 || insn.opcode == 0xf5)) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("carry-flag-op ip=%llx op=%02x next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode,
                (unsigned long long) next_ip);
        extern void gadget_amd64_clc(void), gadget_amd64_stc(void), gadget_amd64_cmc(void);
        gen(state, (unsigned long) (insn.opcode == 0xf8 ? gadget_amd64_clc
                    : insn.opcode == 0xf9 ? gadget_amd64_stc
                    : gadget_amd64_cmc));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native direction-flag ops: CLD (0xfc), STD (0xfd). Only DF (eflags bit10) and
    // df_offset change, exactly the interp (cld: df=0/off=+1; std: df=1/off=-1). These
    // touch only shared CPU state, so reuse the i386 gadget_cld/gadget_std (byte-
    // identical: ldr/bic|orr DF_FLAG/str eflags + str +-1 to df_offset, gret 0); no
    // reg-cache flush. Previously bridged -> interp fallback for the rest of the block
    // (any function that does cld;...;rep movs, i.e. most of glibc's mem/str routines).
    if (amd64_jit_one_byte_plain_prefixes(&insn) &&
            (insn.opcode == 0xfc || insn.opcode == 0xfd)) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("df-flag-op ip=%llx op=%02x next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode,
                (unsigned long long) next_ip);
        extern void gadget_cld(void), gadget_std(void);
        gen(state, (unsigned long) (insn.opcode == 0xfc ? gadget_cld : gadget_std));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.operand_size_prefix &&
            !insn.address_size_prefix && !insn.fs_prefix &&
            !insn.lock_prefix && !insn.rex.present &&
            insn.rep_mode == amd64_jit_repz && insn.opcode == 0x90) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("pause-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.rex.present &&
            insn.rep_mode == amd64_jit_rep_none && insn.opcode == 0x90) {
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("nop90-direct ip=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                (unsigned long long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.opcode >= 0x90 && insn.opcode <= 0x97) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        reg = (unsigned long) (insn.opcode - 0x90);
        if (insn.rex.b)
            reg |= 8;
        next_ip = insn.end_ip;
        state->amd64_ip = next_ip;
        amd64_jit_debug("xchg-rax-direct ip=%llx reg=%lu size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                reg,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_xchg_rax_reg(void);
        gen(state, (unsigned long) gadget_amd64_xchg_rax_reg);
        gen(state, reg | ((unsigned long) size << 4));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0x8d) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        unsigned long meta;
        unsigned long disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp,
                &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("lea-reg-mem-direct ip=%llx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip,
                meta,
                disp,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        extern void gadget_amd64_cached_lea_reg_mem(void);
        gen_amd64_ensure_reg_cache(state);
        gen(state, (unsigned long) gadget_amd64_cached_lea_reg_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        if (amd64_jit_low8_reg((unsigned) ((meta >> AMD64_JIT_MEM_REG_SHIFT) & 0xf)))
            gen_amd64_mark_reg_cache_dirty(state);
        gen_amd64_defer_rip(state, next_ip);
        return true;
#else
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_lea_reg_mem(void);
        gen(state, (unsigned long) gadget_amd64_lea_reg_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
#endif
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0xd0 || insn.opcode == 0xd1 ||
             insn.opcode == 0xd2 || insn.opcode == 0xd3)) {
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("shift-rm-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_shift,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0xd0 || insn.opcode == 0xd1 ||
             insn.opcode == 0xd2 || insn.opcode == 0xd3)) {
        unsigned size = (insn.opcode == 0xd0 || insn.opcode == 0xd2)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned group = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned long count = (insn.opcode == 0xd0 || insn.opcode == 0xd1) ? 1ul : (1ul << 63);
        unsigned long packed = (unsigned long) insn.opcode |
            ((unsigned long) group << 8) |
            ((unsigned long) rm_id << 12) |
            ((unsigned long) size << 16);
        if (insn.rex.present)
            packed |= 1ul << 24;
        (void) count;
        (void) packed;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        if ((group == 4 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_shift_reg_count(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_shift_reg_count);
                gen(state, packed);
                gen(state, count);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
        }
#if defined(__aarch64__)
        // Native reg,CL (0xd3) / reg,1 (0xd1), 32/64-bit, flush style. Covers what the
        // cached low-8 shift path above does NOT: ROL/ROR (group 0/1) for ALL regs (they
        // had no native path -- always bridged), and SHL/SHR/SAR (group 4/5/7) for the
        // high regs r8-r15. Byte (0xd0/0xd2, size 8), 16-bit, RCL/RCR (group 2/3, through
        // CF) keep bridging. Count source: use_cl bit (set for 0xd3 = CL, clear for 0xd1 =
        // literal 1) packed at bit 8; rm bits 0-3, group bits 4-7.
        if (size == 32 || size == 64) {
            unsigned long rcount = (unsigned long) rm_id |
                ((unsigned long) group << 4) |
                ((insn.opcode == 0xd3) ? (1ul << 8) : 0ul);
            if (group == 0 || group == 1) {
                amd64_jit_debug("rotate-count ip=%llx op=%02x grp=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode, group, rm_id,
                        size, (unsigned long long) next_ip);
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_rotate_count32(void), gadget_amd64_rotate_count64(void);
                gen(state, (unsigned long) (size == 64
                            ? gadget_amd64_rotate_count64 : gadget_amd64_rotate_count32));
                gen(state, rcount);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if (group == 4 || group == 5 || group == 7) {
                amd64_jit_debug("shift-count ip=%llx op=%02x grp=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode, group, rm_id,
                        size, (unsigned long long) next_ip);
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_shift_count32(void), gadget_amd64_shift_count64(void);
                gen(state, (unsigned long) (size == 64
                            ? gadget_amd64_shift_count64 : gadget_amd64_shift_count32));
                gen(state, rcount);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
        }
#endif
        amd64_jit_debug("shift-helper ip=%llx opcode=%02x group=%u rm=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                group,
                rm_id,
                size,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_shift,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0x88 || insn.opcode == 0x8a)) {
        unsigned reg_raw = amd64_modrm_reg(insn.modrm);
        unsigned rm_raw = amd64_modrm_rm(insn.modrm);
        unsigned reg_id = reg_raw | (insn.rex.r ? 8 : 0);
        unsigned rm_id = rm_raw | (insn.rex.b ? 8 : 0);
        unsigned src_id = insn.opcode == 0x88 ? reg_id : rm_id;
        unsigned dst_id = insn.opcode == 0x88 ? rm_id : reg_id;
        unsigned long packed = src_id | (dst_id << 4);
        if (!insn.rex.present && (reg_raw >= 4 || rm_raw >= 4))
            goto amd64_bridge_step;
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov8-reg-reg-direct ip=%llx src=%u dst=%u next=%llx",
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg(src_id) && amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_cached_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_reg_reg);
            gen(state, packed | (8ul << 8));
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (amd64_jit_low8_reg(src_id) || amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_hybrid_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_hybrid_mov_reg_reg);
            gen(state, packed | (8ul << 8));
            if (amd64_jit_low8_reg(dst_id))
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov8_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_mov8_reg_reg);
        gen(state, packed);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Only the REX.W form goes direct: the movsxd gadgets implement 64-bit
    // semantics only. 16/32-bit movsxd falls through to the reg-reg helper.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.rex.w &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            insn.opcode == 0x63) {
        unsigned size = 64;
        unsigned src_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned dst_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("movsxd-reg-reg-direct ip=%llx src=%u dst=%u size=%u next=%llx",
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                size,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_movsxd_reg_reg(void);
        gen(state, (unsigned long) gadget_amd64_movsxd_reg_reg);
        gen(state, src_id | (dst_id << 4) | ((unsigned long) size << 8));
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm && amd64_modrm_mod(insn.modrm) == 3 &&
            (insn.opcode == 0x89 || insn.opcode == 0x8b)) {
        unsigned size = insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32);
        unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned src_id = insn.opcode == 0x89 ? reg_id : rm_id;
        unsigned dst_id = insn.opcode == 0x89 ? rm_id : reg_id;
        unsigned long packed = src_id | (dst_id << 4);
        next_ip = state->amd64_ip + 1;
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov%u-reg-reg-direct ip=%llx src=%u dst=%u next=%llx",
                size,
                (unsigned long long) insn.start_ip,
                src_id,
                dst_id,
                (unsigned long long) next_ip);
#if defined(__aarch64__)
        if (amd64_jit_low8_reg(src_id) && amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_cached_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_cached_mov_reg_reg);
            gen(state, packed | ((unsigned long) size << 8));
            gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (amd64_jit_low8_reg(src_id) || amd64_jit_low8_reg(dst_id)) {
            extern void gadget_amd64_hybrid_mov_reg_reg(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_hybrid_mov_reg_reg);
            gen(state, packed | ((unsigned long) size << 8));
            if (amd64_jit_low8_reg(dst_id))
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        gen_amd64_flush_reg_cache(state);
        extern void gadget_amd64_mov16_reg_reg(void);
        extern void gadget_amd64_mov32_reg_reg(void);
        extern void gadget_amd64_mov64_reg_reg(void);
        gen(state, size == 64
                ? (unsigned long) gadget_amd64_mov64_reg_reg
                : size == 32
                ? (unsigned long) gadget_amd64_mov32_reg_reg
                : (unsigned long) gadget_amd64_mov16_reg_reg);
        gen(state, packed);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            !insn.fs_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) == 3 &&
            !insn.rex.r &&
            (insn.opcode == 0x80 || insn.opcode == 0x81 ||
             insn.opcode == 0x83 || insn.opcode == 0xc0 ||
             insn.opcode == 0xc1 || insn.opcode == 0xc6 ||
             insn.opcode == 0xc7)) {
        unsigned size = (insn.opcode == 0x80 || insn.opcode == 0xc0 ||
                insn.opcode == 0xc6)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned group = amd64_modrm_reg(insn.modrm);
        unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
        unsigned long value;
        unsigned long packed;
        guest_addr_t imm_ip = state->amd64_ip + 1;

        if ((insn.opcode == 0xc6 || insn.opcode == 0xc7) && group != 0)
            goto amd64_bridge_step;

        if (insn.opcode == 0x80 || insn.opcode == 0xc0 ||
                insn.opcode == 0xc1 || insn.opcode == 0xc6) {
            uint8_t imm8;
            if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = imm8;
            next_ip = imm_ip + sizeof(imm8);
        } else if (insn.opcode == 0x83) {
            int8_t imm8;
            if (!tlb_read(tlb, imm_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) (qword_t) (sqword_t) imm8;
            next_ip = imm_ip + sizeof(imm8);
        } else if (size == 16) {
            uint16_t imm16;
            if (!tlb_read(tlb, imm_ip, &imm16, sizeof(imm16))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            value = (unsigned long) imm16;
            next_ip = imm_ip + sizeof(imm16);
        } else {
            int32_t imm32;
            if (!tlb_read(tlb, imm_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            if (insn.opcode == 0xc7 && !insn.rex.w)
                value = (unsigned long) (uint32_t) imm32;
            else
                value = (unsigned long) (qword_t) (sqword_t) imm32;
            next_ip = imm_ip + sizeof(imm32);
        }

        state->amd64_ip = next_ip;
        packed = (unsigned long) insn.opcode |
            ((unsigned long) group << 8) |
            ((unsigned long) rm_id << 12) |
            ((unsigned long) size << 16);
        if (insn.rex.present)
            packed |= 1ul << 24;
#if defined(__aarch64__)
        // Native byte (8-bit) ALU r/m8, imm8 (0x80 /0,/1,/4,/5,/6,/7), mod==3 -- the #1
        // byte bridge in cc1 (amd64_jit_reg_imm_op/modrm_imm). ADD/OR/AND/SUB/XOR/CMP.
        // Cache-aware, handles AH-BH and r8-r15 byte. ADC/SBB (/2,/3) keep bridging.
        if (insn.opcode == 0x80 &&
                (group == 0 || group == 1 || group == 4 ||
                 group == 5 || group == 6 || group == 7)) {
            unsigned raw_rm = amd64_modrm_rm(insn.modrm);
            bool is_high = !insn.rex.present && raw_rm >= 4;
            unsigned byte_id = is_high ? raw_rm - 4 : rm_id;
            unsigned long bpacked = (unsigned long) group |
                ((unsigned long) byte_id << 4) | ((unsigned long) (is_high ? 1 : 0) << 8);
            amd64_jit_debug("byte-alu-reg-imm-direct ip=%llx group=%u rm=%u(hi%u) value=%llx next=%llx",
                    (unsigned long long) insn.start_ip, group, byte_id, is_high,
                    (unsigned long long) value, (unsigned long long) next_ip);
            extern void gadget_amd64_byte_alu_reg_imm(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_byte_alu_reg_imm);
            gen(state, bpacked);
            gen(state, value);
            if (group != 7)
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        // Native 16-bit ALU r/m16, imm16 (0x81) / imm8-sign-ext (0x83), mod==3, ARITH
        // ADD/SUB/CMP (groups 0/5/7). The 32/64 arith clause below gates on size 32/64
        // (its CF/OF come from the host's 32-bit flags); 16-bit needs the by-hand width-16
        // flag math in gadget_amd64_w16_alu_reg_imm. 16-bit OR/AND/XOR (1/4/6) are already
        // native via the size-agnostic logic-reg-imm clause; ADC/SBB (2/3) bridge.
        if ((insn.opcode == 0x81 || insn.opcode == 0x83) && size == 16 &&
                (group == 0 || group == 5 || group == 7)) {
            unsigned long wpacked = (unsigned long) group | ((unsigned long) rm_id << 4);
            amd64_jit_debug("w16-alu-reg-imm-direct ip=%llx group=%u rm=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip, group, rm_id,
                    (unsigned long long) value, (unsigned long long) next_ip);
            extern void gadget_amd64_w16_alu_reg_imm(void);
            gen_amd64_ensure_reg_cache(state);
            gen(state, (unsigned long) gadget_amd64_w16_alu_reg_imm);
            gen(state, wpacked);
            gen(state, value);
            if (group != 7)
                gen_amd64_mark_reg_cache_dirty(state);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        if (insn.opcode == 0xc6) {
            if (!insn.rex.present && amd64_modrm_rm(insn.modrm) >= 4)
                goto amd64_bridge_step;
            state->amd64_ip = next_ip;
            amd64_jit_debug("mov-imm8-rmreg-direct ip=%llx rm=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    rm_id,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, rm_id | (8ul << 4));
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm8_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm8_reg);
            gen(state, rm_id);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if (insn.opcode == 0xc7) {
            state->amd64_ip = next_ip;
            amd64_jit_debug("mov-imm-rmreg-direct ip=%llx rm=%u size=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    rm_id,
                    size,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_mov_imm_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_mov_imm_reg);
                gen(state, rm_id | ((unsigned long) size << 4));
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_mov_imm_reg(void);
            gen(state, (unsigned long) gadget_amd64_mov_imm_reg);
            gen(state, rm_id | ((unsigned long) size << 4));
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        if ((insn.opcode == 0x80 || insn.opcode == 0x81 ||
                    insn.opcode == 0x83) &&
                (group == 0 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_arith_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_arith_reg_imm);
                gen(state, packed);
                gen(state, value);
                if (group != 7)
                    gen_amd64_mark_reg_cache_dirty(state);
                else { // cmp: fusable with a directly-following jcc
                    state->x86_fuse_op = 3;
                    state->x86_fuse_end = state->size;
                }
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            // Any operand in r8-r15: flush-style native arith reg-imm (cached is
            // low-8 only; arith had no flush fallback, unlike logic_reg_imm).
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_arith_reg_imm(void);
            gen(state, (unsigned long) gadget_amd64_arith_reg_imm);
            gen(state, packed);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
#endif
        }
#if defined(__aarch64__)
        // Native register adc/sbb reg-imm (0x81 /2,/3 + 0x83 sign-ext), mod==3, 32/64.
        // Group 2=ADC, 3=SBB bridged before (the arith clause above is groups 0/5/7).
        // Flush-style, ARM carry trick. 16-bit + byte (0x80) keep bridging.
        if ((insn.opcode == 0x81 || insn.opcode == 0x83) &&
                (group == 2 || group == 3) && (size == 32 || size == 64)) {
            bool is_sbb = group == 3;
            unsigned long packed2 = (unsigned long) (is_sbb ? 1 : 0) |
                ((unsigned long) rm_id << 4);
            amd64_jit_debug("adcsbb-ri-direct ip=%llx grp=%u rm=%u size=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip, group, rm_id, size,
                    (unsigned long long) value, (unsigned long long) next_ip);
            extern void gadget_amd64_adcsbb_ri32(void), gadget_amd64_adcsbb_ri64(void);
            gen_amd64_flush_reg_cache(state);
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_adcsbb_ri64 : gadget_amd64_adcsbb_ri32));
            gen(state, packed2);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        if ((insn.opcode == 0xc0 || insn.opcode == 0xc1) &&
                (group == 4 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_shift_reg_count(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_shift_reg_count);
                gen(state, packed);
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
        }
        // ROL (/0) / ROR (/1) reg, imm8 (0xc1), 32/64-bit -- native, the SHA-512 hot
        // op (Sigma/sigma ror-chains) that was bridging via amd64_jit_reg_imm_op.
        // Flush style so it handles r8-r15 (where SHA-512 lives), not just the cache.
        // RCL/RCR (/2,/3, carry-rotates) and byte/16-bit keep bridging.
#if defined(__aarch64__)
        if (insn.opcode == 0xc1 && (group == 0 || group == 1) &&
                (size == 32 || size == 64)) {
            extern void gadget_amd64_rotate_imm32(void), gadget_amd64_rotate_imm64(void);
            amd64_jit_debug("rotate-imm ip=%llx rm=%u sub=%u size=%u imm=%llx next=%llx",
                    (unsigned long long) insn.start_ip, rm_id, group, size,
                    (unsigned long long) value, (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_rotate_imm64 : gadget_amd64_rotate_imm32));
            gen(state, (unsigned long) (rm_id | (group << 4) | ((value & 0xff) << 8)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        // SHL (/4) / SHR (/5) / SAR (/7) reg, imm8 (0xc1), 32/64-bit -- native for
        // HIGH regs (r8-r15); the cached_shift clause above already took the low-8
        // case and returned, so this catches the r8-r15 shifts that were bridging
        // (SHA-512's sigma `shr r8,imm`). Flush style, same flag math as cached_shift.
        if (insn.opcode == 0xc1 && (group == 4 || group == 5 || group == 7) &&
                (size == 32 || size == 64)) {
            extern void gadget_amd64_shift_imm32(void), gadget_amd64_shift_imm64(void);
            amd64_jit_debug("shift-imm-hi ip=%llx rm=%u grp=%u size=%u imm=%llx next=%llx",
                    (unsigned long long) insn.start_ip, rm_id, group, size,
                    (unsigned long long) value, (unsigned long long) next_ip);
            gen_amd64_flush_reg_cache(state);
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_shift_imm64 : gadget_amd64_shift_imm32));
            gen(state, (unsigned long) (rm_id | (group << 4) | ((value & 0xff) << 8)));
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
#endif
        if ((insn.opcode == 0x80 || insn.opcode == 0x81 ||
                    insn.opcode == 0x83) &&
                (group == 1 || group == 4 || group == 6) &&
                !(size == 8 && !insn.rex.present &&
                    amd64_modrm_rm(insn.modrm) >= 4)) {
            amd64_jit_debug("logic-reg-imm-direct ip=%llx opcode=%02x group=%u rm=%u size=%u value=%llx next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    group,
                    rm_id,
                    size,
                    (unsigned long long) value,
                    (unsigned long long) next_ip);
#if defined(__aarch64__)
            if (amd64_jit_low8_reg(rm_id)) {
                extern void gadget_amd64_cached_logic_reg_imm(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_cached_logic_reg_imm);
                gen(state, packed);
                gen(state, value);
                gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            gen_amd64_flush_reg_cache(state);
            extern void gadget_amd64_logic_reg_imm(void);
            gen(state, (unsigned long) gadget_amd64_logic_reg_imm);
            gen(state, packed);
            gen(state, value);
            gen_amd64_defer_rip(state, next_ip);
            return true;
        }
        amd64_jit_debug("reg-imm-helper ip=%llx opcode=%02x group=%u rm=%u size=%u value=%llx next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                group,
                rm_id,
                size,
                (unsigned long long) value,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_reg_imm_op,
                packed, value, (unsigned long) next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            !insn.fs_prefix &&
            !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none &&
            insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) == 3) {
        switch (insn.opcode) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x18:
        case 0x19:
        case 0x1a:
        case 0x1b:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x28:
        case 0x29:
        case 0x2a:
        case 0x2b:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b:
        case 0x63:
        case 0x84:
        case 0x85:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b: {
            unsigned size = ((insn.opcode & 1) == 0 ||
                    insn.opcode == 0x38 || insn.opcode == 0x3a ||
                    insn.opcode == 0x84 || insn.opcode == 0x88 ||
                    insn.opcode == 0x8a ||
                    insn.opcode == 0x08 || insn.opcode == 0x20)
                ? 8
                : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
            unsigned reg_id = amd64_modrm_reg(insn.modrm) | (insn.rex.r ? 8 : 0);
            unsigned rm_id = amd64_modrm_rm(insn.modrm) | (insn.rex.b ? 8 : 0);
            unsigned long packed = (unsigned long) insn.opcode |
                ((unsigned long) reg_id << 8) |
                ((unsigned long) rm_id << 12) |
                ((unsigned long) size << 16);
            if (insn.rex.present)
                packed |= 1ul << 24;
            next_ip = state->amd64_ip + 1;
            state->amd64_ip = next_ip;
#if defined(__aarch64__)
            // Native byte (8-bit) ALU reg-reg + TEST (mod==3): ADD/OR/AND/SUB/XOR/CMP
            // (0x00/02/08/0a/20/22/28/2a/30/32/38/3a) and TEST (0x84). These bridged via
            // amd64_jit_reg_reg_op -- byte CMP 0x38 was the top byte reg-reg bridge in cc1.
            // Cache-aware (handles AH-BH and r8-r15 byte); ADC/SBB (0x10-0x1b) keep bridging.
            if (size == 8 &&
                    (insn.opcode == 0x00 || insn.opcode == 0x02 ||
                     insn.opcode == 0x08 || insn.opcode == 0x0a ||
                     insn.opcode == 0x20 || insn.opcode == 0x22 ||
                     insn.opcode == 0x28 || insn.opcode == 0x2a ||
                     insn.opcode == 0x30 || insn.opcode == 0x32 ||
                     insn.opcode == 0x38 || insn.opcode == 0x3a ||
                     insn.opcode == 0x84)) {
                unsigned op = insn.opcode == 0x84 ? 4 : ((insn.opcode >> 3) & 7);
                bool nowrite = insn.opcode == 0x84;
                bool d = (insn.opcode & 2) != 0 && insn.opcode != 0x84;
                unsigned raw_reg = amd64_modrm_reg(insn.modrm);
                unsigned raw_rm = amd64_modrm_rm(insn.modrm);
                bool reg_high = !insn.rex.present && raw_reg >= 4;
                bool rm_high = !insn.rex.present && raw_rm >= 4;
                unsigned reg_bid = reg_high ? raw_reg - 4 : reg_id;
                unsigned rm_bid = rm_high ? raw_rm - 4 : rm_id;
                unsigned dst_id = d ? reg_bid : rm_bid;
                unsigned dst_hi = d ? reg_high : rm_high;
                unsigned src_id = d ? rm_bid : reg_bid;
                unsigned src_hi = d ? rm_high : reg_high;
                unsigned long bpacked = (unsigned long) op |
                    ((unsigned long) dst_id << 4) | ((unsigned long) dst_hi << 8) |
                    ((unsigned long) src_id << 9) | ((unsigned long) src_hi << 13) |
                    ((unsigned long) (nowrite ? 1 : 0) << 16);
                amd64_jit_debug("byte-alu-reg-reg-direct ip=%llx opcode=%02x dst=%u(hi%u) src=%u(hi%u) next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode,
                        dst_id, dst_hi, src_id, src_hi, (unsigned long long) next_ip);
                extern void gadget_amd64_byte_alu_reg_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_byte_alu_reg_reg);
                gen(state, bpacked);
                if (op != 7 && !nowrite)
                    gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            if ((insn.opcode == 0x85) ||
                    (insn.opcode == 0x84 &&
                     (insn.rex.present ||
                      (amd64_modrm_reg(insn.modrm) < 4 && amd64_modrm_rm(insn.modrm) < 4)))) {
                amd64_jit_debug("test-reg-reg-direct ip=%llx reg=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        reg_id,
                        rm_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_test_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_test_reg_reg);
                    gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
                if (amd64_jit_low8_reg(reg_id) || amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_hybrid_test_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_hybrid_test_reg_reg);
                    gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_test_reg_reg(void);
                gen(state, (unsigned long) gadget_amd64_test_reg_reg);
                gen(state, rm_id | (reg_id << 4) | ((unsigned long) size << 8));
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if ((insn.opcode == 0x31 || insn.opcode == 0x33) &&
                    reg_id == rm_id && size != 8) {
                amd64_jit_debug("xor-zero-direct ip=%llx reg=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        reg_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id)) {
                    extern void gadget_amd64_cached_xor_zero_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_xor_zero_reg);
                    gen(state, reg_id | ((unsigned long) size << 4));
                    gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_xor_zero_reg(void);
                gen(state, (unsigned long) gadget_amd64_xor_zero_reg);
                gen(state, reg_id | ((unsigned long) size << 4));
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
            if ((insn.opcode >= 0x08 && insn.opcode <= 0x0b) ||
                    (insn.opcode >= 0x20 && insn.opcode <= 0x23) ||
                    (insn.opcode >= 0x30 && insn.opcode <= 0x33)) {
                if (size == 8 && !insn.rex.present &&
                        (amd64_modrm_reg(insn.modrm) >= 4 ||
                         amd64_modrm_rm(insn.modrm) >= 4))
                    goto amd64_bridge_step;
                amd64_jit_debug("logic-reg-reg-direct ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip,
                        insn.opcode,
                        reg_id,
                        rm_id,
                        size,
                        (unsigned long long) next_ip);
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_logic_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_logic_reg_reg);
                    gen(state, packed);
                    gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
                if (amd64_jit_low8_reg(reg_id) || amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_hybrid_logic_reg_reg(void);
                    unsigned dst_id = (insn.opcode & 2) == 0 ? rm_id : reg_id;
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_hybrid_logic_reg_reg);
                    gen(state, packed);
                    if (amd64_jit_low8_reg(dst_id))
                        gen_amd64_mark_reg_cache_dirty(state);
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
#endif
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_logic_reg_reg(void);
                gen(state, (unsigned long) gadget_amd64_logic_reg_reg);
                gen(state, packed);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#if defined(__aarch64__)
            // Native 16-bit ALU reg-reg ADD/SUB/CMP (0x01/03/29/2b/39/3b, mod==3). The
            // 32/64 arith clause below gates on size 32/64 (its CF/OF come from the host's
            // 32-bit flags); 16-bit needs the by-hand width-16 flag math in
            // gadget_amd64_w16_alu_reg_reg. 16-bit OR/AND/XOR reg-reg are already native
            // (the logic clause above is size-agnostic), as are TEST (0x85) and xor-zero.
            if (size == 16 &&
                    (insn.opcode <= 0x03 ||
                     (insn.opcode >= 0x28 && insn.opcode <= 0x2b) ||
                     (insn.opcode >= 0x38 && insn.opcode <= 0x3b))) {
                unsigned op = (insn.opcode >> 3) & 7;   /* 0=ADD, 5=SUB, 7=CMP */
                bool d = (insn.opcode & 2) != 0;        /* opcode bit1: dst=reg vs dst=rm */
                unsigned dst_id = d ? reg_id : rm_id;
                unsigned src_id = d ? rm_id : reg_id;
                unsigned long wpacked = (unsigned long) op |
                    ((unsigned long) dst_id << 4) | ((unsigned long) src_id << 8);
                amd64_jit_debug("w16-alu-reg-reg-direct ip=%llx opcode=%02x dst=%u src=%u next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode,
                        dst_id, src_id, (unsigned long long) next_ip);
                extern void gadget_amd64_w16_alu_reg_reg(void);
                gen_amd64_ensure_reg_cache(state);
                gen(state, (unsigned long) gadget_amd64_w16_alu_reg_reg);
                gen(state, wpacked);
                if (op != 7)
                    gen_amd64_mark_reg_cache_dirty(state);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            if ((insn.opcode <= 0x03 ||
                     (insn.opcode >= 0x28 && insn.opcode <= 0x2b) ||
                     (insn.opcode >= 0x38 && insn.opcode <= 0x3b)) &&
                    (size == 32 || size == 64)) {
#if defined(__aarch64__)
                if (amd64_jit_low8_reg(reg_id) && amd64_jit_low8_reg(rm_id)) {
                    extern void gadget_amd64_cached_arith_reg_reg(void);
                    gen_amd64_ensure_reg_cache(state);
                    gen(state, (unsigned long) gadget_amd64_cached_arith_reg_reg);
                    gen(state, packed);
                    if (insn.opcode < 0x38)
                        gen_amd64_mark_reg_cache_dirty(state);
                    else { // cmp: fusable with a directly-following jcc
                        state->x86_fuse_op = 4;
                        state->x86_fuse_end = state->size;
                    }
                    gen_amd64_defer_rip(state, next_ip);
                    return true;
                }
                // Any operand in r8-r15: flush-style native arith (cached_arith is
                // low-8 only). SHA-512's `add r8,r9` was the top reg-reg C bridge.
                gen_amd64_flush_reg_cache(state);
                extern void gadget_amd64_arith_reg_reg(void);
                gen(state, (unsigned long) gadget_amd64_arith_reg_reg);
                gen(state, packed);
                gen_amd64_defer_rip(state, next_ip);
                return true;
#endif
            }
#if defined(__aarch64__)
            // Native register adc/sbb reg-reg (0x11/13 adc, 0x19/1b sbb), mod==3, 32/64.
            // The memory forms were native (loadop/opstore_addc); the register forms
            // bridged (ending the block) -- multi-precision arith + the sbb-reg-reg
            // carry-materialize idiom. Flush-style, ARM carry trick. Byte (0x10/12/18/1a)
            // + 16-bit keep bridging.
            if ((size == 32 || size == 64) &&
                    (insn.opcode == 0x11 || insn.opcode == 0x13 ||
                     insn.opcode == 0x19 || insn.opcode == 0x1b)) {
                bool is_sbb = insn.opcode >= 0x18;
                bool d = (insn.opcode & 2) != 0;     /* dst=reg vs dst=rm */
                unsigned dst_id = d ? reg_id : rm_id;
                unsigned src_id = d ? rm_id : reg_id;
                unsigned long packed2 = (unsigned long) (is_sbb ? 1 : 0) |
                    ((unsigned long) dst_id << 4) | ((unsigned long) src_id << 8);
                amd64_jit_debug("adcsbb-rr-direct ip=%llx opcode=%02x dst=%u src=%u size=%u next=%llx",
                        (unsigned long long) insn.start_ip, insn.opcode, dst_id, src_id,
                        size, (unsigned long long) next_ip);
                extern void gadget_amd64_adcsbb_rr32(void), gadget_amd64_adcsbb_rr64(void);
                gen_amd64_flush_reg_cache(state);
                gen(state, (unsigned long) (size == 64
                            ? gadget_amd64_adcsbb_rr64 : gadget_amd64_adcsbb_rr32));
                gen(state, packed2);
                gen_amd64_defer_rip(state, next_ip);
                return true;
            }
#endif
            amd64_jit_debug("reg-reg-helper ip=%llx opcode=%02x reg=%u rm=%u size=%u next=%llx",
                    (unsigned long long) insn.start_ip,
                    insn.opcode,
                    reg_id,
                    rm_id,
                    size,
                    (unsigned long long) next_ip);
            gen_amd64_helper_tlb_2_retint(state, amd64_jit_reg_reg_op,
                    packed, (unsigned long) next_ip);
            return true;
        }
        default:
            break;
        }
    }

    // Native MOV reg<->mem (0x8a/0x8b load, 0x88/0x89 store), mod!=3: the most
    // common memory instruction and flag-free. Like the vector mem ops, the reg
    // cache and rip are flushed so a #PF re-executes -- base/index are read from
    // CPU_amd64_regs and the destination reg is written there too. Per-size gadget
    // keeps the vread/vwrite size literal (correct cross-page staging). The byte
    // gadgets handle the AH/CH/DH/BH high-byte aliasing (modrm.reg 4-7 without REX)
    // via meta's REX_PRESENT bit. FS-prefix and address-size forms keep bridging
    // (amd64_vmem_addr adds no tls_ptr).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x88 || insn.opcode == 0x89 ||
             insn.opcode == 0x8a || insn.opcode == 0x8b)) {
        bool is_load = insn.opcode == 0x8a || insn.opcode == 0x8b;
        bool is_byte = insn.opcode == 0x88 || insn.opcode == 0x8a;
        unsigned size = is_byte ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mov-mem ip=%llx op=%02x load=%d size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, is_load, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_mov_load8(void), gadget_amd64_mov_load16(void),
                gadget_amd64_mov_load32(void), gadget_amd64_mov_load64(void),
                gadget_amd64_mov_store8(void), gadget_amd64_mov_store16(void),
                gadget_amd64_mov_store32(void), gadget_amd64_mov_store64(void);
        void (*g)(void);
        if (is_load)
            g = size == 8 ? gadget_amd64_mov_load8
              : size == 16 ? gadget_amd64_mov_load16
              : size == 32 ? gadget_amd64_mov_load32 : gadget_amd64_mov_load64;
        else
            g = size == 8 ? gadget_amd64_mov_store8
              : size == 16 ? gadget_amd64_mov_store16
              : size == 32 ? gadget_amd64_mov_store32 : gadget_amd64_mov_store64;
        gen(state, (unsigned long) g);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native load-op arith: reg <op>= [mem] for the d=1 forms ADD (0x03), SUB (0x2b),
    // CMP (0x3b), mod!=3, 32/64-bit (no 0x66). reg is the dst/lhs, [mem] is the rhs;
    // flags are set eagerly. Mirrors the cached_arith_reg_reg coverage (ADC/SBB and
    // byte forms 0x02/2a/3a keep bridging; 16-bit bridges). Same flush+#PF-reexec
    // discipline as MOV; FS-prefix and address-size forms bridge.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x03 || insn.opcode == 0x2b || insn.opcode == 0x3b)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("loadop-arith ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_arith32(void), gadget_amd64_loadop_arith64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_loadop_arith64 : gadget_amd64_loadop_arith32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native load-op logic: reg <op>= [mem] for OR (0x0b), AND (0x23), XOR (0x33),
    // mod!=3, 32/64-bit (no 0x66). Same as load-op arith but the logic flag rule
    // (CF=OF=0, ZF/SF/PF from result). Byte forms (0x0a/22/32), TEST, 16-bit bridge.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x0b || insn.opcode == 0x23 || insn.opcode == 0x33)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("loadop-logic ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_logic32(void), gadget_amd64_loadop_logic64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_loadop_logic64 : gadget_amd64_loadop_logic32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native op-store/RMW arith: [mem] <op>= reg for ADD (0x01), SUB (0x29), CMP
    // (0x39), mod!=3, 32/64-bit (no 0x66). [mem] is both source and destination
    // (read-modify-write); modrm.reg is the rhs. Flags eager; CMP is flags-only (no
    // store). Byte forms (0x00/28/38), ADC/SBB, 16-bit, and locked forms keep
    // bridging. Same flush + #PF-reexec discipline (a fault on the store re-reads).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x01 || insn.opcode == 0x29 || insn.opcode == 0x39)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("opstore-arith ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_opstore_arith32(void), gadget_amd64_opstore_arith64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_opstore_arith64 : gadget_amd64_opstore_arith32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native op-store/RMW logic + TEST: [mem] <op>= reg for OR (0x09), AND (0x21),
    // XOR (0x31) -- read-modify-write -- and TEST (0x85, = [mem] AND reg, flags only,
    // no store), mod!=3, 32/64-bit (no 0x66). Logic flags. Byte forms (0x08/20/30/84),
    // 16-bit, and locked forms keep bridging.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x09 || insn.opcode == 0x21 ||
             insn.opcode == 0x31 || insn.opcode == 0x85)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("opstore-logic ip=%llx op=%02x size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_opstore_logic32(void), gadget_amd64_opstore_logic64(void);
        gen(state, (unsigned long) (size == 64
                    ? gadget_amd64_opstore_logic64 : gadget_amd64_opstore_logic32));
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native MOVSXD reg64 <- [mem]32 (movslq, 0x63 with REX.W), mod!=3: sign-extend a
    // 32-bit memory operand into a 64-bit register, no flags. Only the REX.W form is
    // routed here (the rare non-W 0x63 32-bit form, and FS/address-size, keep bridging).
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0x63 && insn.rex.w) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 32, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("movsxd-mem ip=%llx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, meta, disp,
                (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_movsxd_mem(void);
        gen(state, (unsigned long) gadget_amd64_movsxd_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native adc/sbb with a memory operand, 32/64-bit (no 0x66), mod!=3 -- completes
    // the arith family. Load-op (reg <op>= [mem] + carry): ADC 0x13, SBB 0x1b. Op-
    // store/RMW ([mem] <op>= reg + carry): ADC 0x11, SBB 0x19. Carry-in = CPU_cf; the
    // gadgets use ARM adcs/sbcs for the result + CF/OF, and compute AF/ZF/SF/PF via
    // amd64_cached_set_addsub_flags from the original rhs. Byte (0x10/12/18/1a), 16-bit, and
    // locked forms keep bridging.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x13 || insn.opcode == 0x1b ||
             insn.opcode == 0x11 || insn.opcode == 0x19)) {
        bool is_store = insn.opcode == 0x11 || insn.opcode == 0x19;
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("addc-mem ip=%llx op=%02x store=%d size=%u meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, insn.opcode, is_store, size,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_loadop_addc32(void), gadget_amd64_loadop_addc64(void),
                gadget_amd64_opstore_addc32(void), gadget_amd64_opstore_addc64(void);
        void (*g)(void);
        if (is_store)
            g = size == 64 ? gadget_amd64_opstore_addc64 : gadget_amd64_opstore_addc32;
        else
            g = size == 64 ? gadget_amd64_loadop_addc64 : gadget_amd64_loadop_addc32;
        gen(state, (unsigned long) g);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    if (!insn.two_byte_opcode &&
            !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x00 || insn.opcode == 0x01 ||
             insn.opcode == 0x02 || insn.opcode == 0x03 ||
             insn.opcode == 0x08 ||
             insn.opcode == 0x09 || insn.opcode == 0x0a || insn.opcode == 0x0b ||
             insn.opcode == 0x10 || insn.opcode == 0x12 ||
             insn.opcode == 0x11 || insn.opcode == 0x13 ||
             insn.opcode == 0x18 || insn.opcode == 0x1a ||
             insn.opcode == 0x19 || insn.opcode == 0x1b ||
             insn.opcode == 0x20 || insn.opcode == 0x21 || insn.opcode == 0x22 || insn.opcode == 0x23 ||
             insn.opcode == 0x28 || insn.opcode == 0x2a ||
             insn.opcode == 0x29 || insn.opcode == 0x2b ||
             insn.opcode == 0x30 || insn.opcode == 0x32 ||
             insn.opcode == 0x31 || insn.opcode == 0x33 ||
             insn.opcode == 0x38 || insn.opcode == 0x39 ||
             insn.opcode == 0x3a || insn.opcode == 0x3b ||
             insn.opcode == 0x84 || insn.opcode == 0x85 ||
             insn.opcode == 0x88 || insn.opcode == 0x89 ||
             insn.opcode == 0x8a || insn.opcode == 0x8b ||
             insn.opcode == 0x8d || insn.opcode == 0x63)) {
        unsigned size = ((insn.opcode & 1) == 0 ||
                insn.opcode == 0x38 || insn.opcode == 0x3a ||
                insn.opcode == 0x84 || insn.opcode == 0x88 ||
                insn.opcode == 0x8a ||
                insn.opcode == 0x08 || insn.opcode == 0x20)
            ? 8
            : (insn.rex.w ? 64 : (insn.operand_size_prefix ? 16 : 32));
        unsigned long meta;
        unsigned long disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp,
                &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("mem-op-helper ip=%llx opcode=%02x meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                meta,
                disp,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_3_retint(state, amd64_jit_mem_op,
                meta, disp, (unsigned long) next_ip);
        return true;
    }

    // Native imm-to-mem ALU: [mem] <op>= imm for ADD (/0), OR (/1), AND (/4), SUB (/5),
    // XOR (/6) -- RMW -- and CMP (/7) -- flags only, no store -- mod!=3, 32/64-bit
    // (0x81 imm32, 0x83 imm8 sign-extended; no 0x66). CMP [mem],imm was the SHA-512/
    // crypt hot-loop bottleneck (it block-bridged); its flags use the same
    // amd64_cached_set_addsub_flags as native CMP 0x3b/0x39 so they are bit-exact.
    // adc/sbb-imm (/2,/3), byte (0x80) and 16-bit keep bridging.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix && !insn.operand_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            (insn.opcode == 0x81 || insn.opcode == 0x83) &&
            (amd64_modrm_reg(insn.modrm) == 0 || amd64_modrm_reg(insn.modrm) == 1 ||
             amd64_modrm_reg(insn.modrm) == 4 || amd64_modrm_reg(insn.modrm) == 5 ||
             amd64_modrm_reg(insn.modrm) == 6 || amd64_modrm_reg(insn.modrm) == 7)) {
        unsigned size = insn.rex.w ? 64 : 32;
        unsigned group = amd64_modrm_reg(insn.modrm);
        bool is_logic = group == 1 || group == 4 || group == 6;
        bool is_cmp = group == 7;
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, size, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        long imm;  // next_ip points at the immediate after the addressing bytes
        if (insn.opcode == 0x83) {
            int8_t imm8;
            if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            imm = imm8;
            next_ip += sizeof(imm8);
        } else {
            int32_t imm32;
            if (!tlb_read(tlb, next_ip, &imm32, sizeof(imm32))) {
                state->amd64_ip = state->amd64_orig_ip;
                state->amd64_fallback_to_interp = true;
                return false;
            }
            imm = imm32;
            next_ip += sizeof(imm32);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("imm-alu ip=%llx grp=%u logic=%d cmp=%d size=%u imm=%lx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, group, is_logic, is_cmp, size,
                (unsigned long) imm, meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_imm_arith32(void), gadget_amd64_imm_arith64(void),
                gadget_amd64_imm_logic32(void), gadget_amd64_imm_logic64(void),
                gadget_amd64_imm_cmp32(void), gadget_amd64_imm_cmp64(void);
        if (is_cmp) {
            // CMP: flags only, no store -> imm_cmp gadget (meta/disp/next_ip/imm).
            gen(state, (unsigned long) (size == 64
                        ? gadget_amd64_imm_cmp64 : gadget_amd64_imm_cmp32));
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            gen(state, (unsigned long) imm);
        } else {
            void (*g)(void);
            if (is_logic)
                g = size == 64 ? gadget_amd64_imm_logic64 : gadget_amd64_imm_logic32;
            else
                g = size == 64 ? gadget_amd64_imm_arith64 : gadget_amd64_imm_arith32;
            gen(state, (unsigned long) g);
            gen(state, meta);
            gen(state, disp);
            gen(state, (unsigned long) next_ip);
            gen(state, (unsigned long) imm);
            gen(state, (unsigned long) group);
        }
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

    // Native CMP byte [mem], imm8 (0x80 /7), mod!=3 -- the byte sibling of the imm-cmp
    // above and the other crypt hot-loop block-bridge. Flags only, 8-bit.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 &&
            insn.opcode == 0x80 && amd64_modrm_reg(insn.modrm) == 7) {
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 8, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        int8_t imm8;
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        long imm = imm8;
        next_ip += sizeof(imm8);
        state->amd64_ip = next_ip;
        amd64_jit_debug("imm-cmp8 ip=%llx imm=%lx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, (unsigned long) imm,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_imm_cmp8(void);
        gen(state, (unsigned long) gadget_amd64_imm_cmp8);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen(state, (unsigned long) imm);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }

#if defined(__aarch64__)
    // Native op byte [mem], imm8 (0x80 /0,/1,/4,/5,/6 = ADD/OR/AND/SUB/XOR), mod!=3 RMW.
    // CMP (/7) is the imm_cmp8 clause above; ADC/SBB (/2,/3) bridge.
    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            !insn.fs_prefix && !insn.lock_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            amd64_modrm_mod(insn.modrm) != 3 && insn.opcode == 0x80 &&
            (amd64_modrm_reg(insn.modrm) == 0 || amd64_modrm_reg(insn.modrm) == 1 ||
             amd64_modrm_reg(insn.modrm) == 4 || amd64_modrm_reg(insn.modrm) == 5 ||
             amd64_modrm_reg(insn.modrm) == 6)) {
        unsigned group = amd64_modrm_reg(insn.modrm);
        unsigned long meta, disp;
        if (!gen_amd64_decode_mem_meta(state, tlb, &insn, 8, &meta, &disp, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        uint8_t imm8;
        if (!tlb_read(tlb, next_ip, &imm8, sizeof(imm8))) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        long imm = imm8;
        next_ip += sizeof(imm8);
        state->amd64_ip = next_ip;
        amd64_jit_debug("byte-imm-mem ip=%llx grp=%u imm=%lx meta=%lx disp=%lx next=%llx",
                (unsigned long long) insn.start_ip, group, (unsigned long) imm,
                meta, disp, (unsigned long long) next_ip);
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
        extern void gadget_amd64_byte_imm_mem(void);
        gen(state, (unsigned long) gadget_amd64_byte_imm_mem);
        gen(state, meta);
        gen(state, disp);
        gen(state, (unsigned long) next_ip);
        gen(state, (unsigned long) imm);
        gen(state, (unsigned long) group);
        gen_amd64_defer_rip(state, next_ip);
        return true;
    }
#endif

    if (!insn.two_byte_opcode && !insn.address_size_prefix &&
            insn.rep_mode == amd64_jit_rep_none && insn.has_modrm &&
            (insn.opcode == 0x80 || insn.opcode == 0x81 ||
             insn.opcode == 0x83 || insn.opcode == 0xc0 ||
             insn.opcode == 0xc1 || insn.opcode == 0xc6 ||
             insn.opcode == 0xc7)) {
        // Memory-form cmp imm (groups /7) is now native above for all of 0x80/0x81/
        // 0x83 (it was the crypt/login hot-loop bottleneck). The remaining memory-form
        // 0x80/0x81/0x83 groups here go through the amd64_jit_modrm_imm helper, which
        // (unlike the old cmp bridge) does not end the JIT block.
        if (!gen_amd64_decode_rm_extent(state, tlb, &insn, &next_ip)) {
            state->amd64_ip = state->amd64_orig_ip;
            state->amd64_fallback_to_interp = true;
            return false;
        }
        if (insn.opcode == 0x80 || insn.opcode == 0x83 ||
                insn.opcode == 0xc0 || insn.opcode == 0xc1 ||
                insn.opcode == 0xc6) {
            next_ip += sizeof(uint8_t);
        } else {
            next_ip += insn.operand_size_prefix ? sizeof(uint16_t) : sizeof(uint32_t);
        }
        state->amd64_ip = next_ip;
        amd64_jit_debug("modrm-imm-helper ip=%llx opcode=%02x modrm=%02x next=%llx",
                (unsigned long long) insn.start_ip,
                insn.opcode,
                insn.modrm,
                (unsigned long long) next_ip);
        gen_amd64_helper_tlb_2_retint(state, amd64_jit_modrm_imm,
                (unsigned long) insn.opcode, (unsigned long) next_ip);
        return true;
    }

amd64_bridge_step:
    amd64_jit_debug("helper-step ip=%llx opcode=%02x two_byte=%d op2=%02x rex=%d%d%d%d%d opsz=%d addrsz=%d fs=%d lock=%d rep=%d has_modrm=%d modrm=%02x",
            (unsigned long long) insn.start_ip,
            insn.opcode,
            insn.two_byte_opcode,
            insn.op2,
            insn.rex.present,
            insn.rex.w,
            insn.rex.r,
            insn.rex.x,
            insn.rex.b,
            insn.operand_size_prefix,
            insn.address_size_prefix,
            insn.fs_prefix,
            insn.lock_prefix,
            insn.rep_mode,
            insn.has_modrm,
            insn.modrm);
    state->amd64_fallback_to_interp = true;
    return false;
}
#else
static int gen_step64(struct gen_state *state, struct tlb *tlb) {
    // The amd64 JIT gadgets are implemented only for aarch64 hosts. On other
    // hosts (x86_64: mint's Linux VM, CI's build-linux) every amd64 instruction
    // bridges to the interpreter, mirroring the decode-fail fallback above; this
    // keeps the binary linking without the aarch64-only gadget symbols.
    (void) tlb;
    state->amd64_orig_ip = state->amd64_ip;
    state->orig_ip_extra = 0;
    state->amd64_fallback_to_interp = true;
    state->amd64_fallback_ip = state->amd64_ip;
    state->amd64_fallback_opcode = 0xff;
    state->amd64_fallback_op2 = 0;
    state->amd64_fallback_flags = 0x80;
    return false;
}
#endif

void gen_end(struct gen_state *state) {
    if (state->amd64) {
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
    }
    struct jit_block *block = state->block;
    for (int i = 0; i <= 1; i++) {
        if (state->jump_ip[i] != 0) {
            block->jump_ip[i] = &block->code[state->jump_ip[i]];
            block->old_jump_ip[i] = *block->jump_ip[i];
        } else {
            block->jump_ip[i] = NULL;
        }

        list_init(&block->jumps_from[i]);
        list_init(&block->jumps_from_links[i]);
    }
    if (state->block_patch_ip != 0) {
        block->code[state->block_patch_ip] = (unsigned long) block;
    }
    if (state->amd64) {
        if (block->addr != state->amd64_ip)
            block->end_addr = state->amd64_ip - 1;
        else
            block->end_addr = block->addr;
    } else if (state->arm64) {
        // arm64 advances arm64_ip, not the i386 state->ip — without this
        // branch every arm64 block claimed to span a single byte, so
        // page-registration/invalidation only saw the block's first byte
        // (a landmine for any code-page invalidation, flagged in
        // aarch64_guest_plan.md before it ever misfired).
        if (block->addr != state->arm64_ip)
            block->end_addr = state->arm64_ip - 1;
        else
            block->end_addr = block->addr;
    } else if (state->riscv64) {
        // Same rule as arm64: riscv64 advances riscv64_ip.
        if (block->addr != state->riscv64_ip)
            block->end_addr = state->riscv64_ip - 1;
        else
            block->end_addr = block->addr;
    } else if (block->addr != state->ip)
        block->end_addr = state->ip - 1;
    else
        block->end_addr = block->addr;
    list_init(&block->chain);
    block->is_jetsam = false;
    for (int i = 0; i <= 1; i++) {
        list_init(&block->page[i]);
    }
}

void gen_exit(struct gen_state *state) {
    extern void gadget_exit(void);
    if (state->arm64) {
#ifdef ISH_JIT_ARM64_GUEST
        // The block hit the size cap mid-straight-line. Continue at the
        // next instruction through the normal branch gadget (tagged
        // chainable target), exactly as an unconditional branch to
        // arm64_ip would. The old path fell through to gadget_exit with
        // state->ip — the i386 field, which arm64 never advances — so any
        // straight-line run past the cap (big unrolled loops, generated
        // code) resumed at garbage and crashed.
        extern void gadget_arm64_b(void);
        gen(state, (unsigned long) gadget_arm64_b);
        gen(state, state->arm64_ip | 0x8000000000000000ULL);
        state->jump_ip[0] = state->size - 1;
        return;
#else
        // Unreachable: gen_step_arm64's unsupported stub die()s before any
        // arm64 block can be built on a host without the aarch64 gadgets.
        die("arm64 guest block on a host without the aarch64 guest JIT");
#endif
    }
    if (state->riscv64) {
#ifdef ISH_JIT_RISCV64_GUEST
        // Size-cap continuation through the branch gadget, same design
        // (and same gadget_exit-would-use-the-wrong-ip bug avoided) as the
        // arm64 case above.
        extern void gadget_riscv64_b(void);
        gen(state, (unsigned long) gadget_riscv64_b);
        gen(state, state->riscv64_ip | 0x8000000000000000ULL);
        state->jump_ip[0] = state->size - 1;
        return;
#else
        die("riscv64 guest block on a host without the riscv64 guest JIT");
#endif
    }
    if (state->amd64) {
        gen_amd64_flush_reg_cache(state);
        gen_amd64_flush_rip(state);
    }
    // in case the last instruction didn't end the block
    gen(state, (unsigned long) gadget_exit);
    gen(state, state->amd64 ? (unsigned long) (addr_t) state->amd64_ip : state->ip);
}

#define DECLARE_LOCALS \
    dword_t addr_offset = 0; \
    bool end_block = false; \
    bool seg_tls = false

#define FINISH \
    return !end_block

#define RESTORE_IP state->ip = state->orig_ip
#define _READIMM(name, size) do {\
    state->ip += size/8; \
    if (!tlb_read(tlb, state->ip - size/8, &name, size/8)) SEGFAULT; \
} while (0)

#define READMODRM if (!modrm_decode32(&state->ip, tlb, &modrm)) SEGFAULT
#define READADDR _READIMM(addr_offset, 32)
#define SEG_GS() seg_tls = true
#define SEG_FS() seg_tls = true

// This should stay in sync with the definition of .gadget_array in gadgets.h
enum arg {
    arg_reg_a, arg_reg_c, arg_reg_d, arg_reg_b, arg_reg_sp, arg_reg_bp, arg_reg_si, arg_reg_di,
    arg_reg_ah = arg_reg_sp, arg_reg_ch = arg_reg_bp, arg_reg_dh = arg_reg_si, arg_reg_bh = arg_reg_di,
    arg_imm, arg_mem, arg_addr, arg_gs,
    arg_count, arg_invalid,
    // the following should not be synced with the list mentioned above (no gadgets implement them)
    arg_modrm_val, arg_modrm_reg,
    arg_xmm_modrm_val, arg_xmm_modrm_reg,
    arg_mm_modrm_val, arg_mm_modrm_reg,
    arg_mem_addr, arg_1,
};

enum size {
    size_8, size_16, size_32,
    size_count,
    size_64, size_80, size_128, // bonus sizes
};

// sync with COND_LIST in control.S
enum cond {
    cond_O, cond_B, cond_E, cond_BE, cond_S, cond_P, cond_L, cond_LE,
    cond_count,
};

enum repeat {
    rep_once, rep_repz, rep_repnz,
    rep_count,
    rep_rep = rep_repz,
};

typedef void (*gadget_t)(void);

#define GEN(thing) gen(state, (unsigned long) (thing))
#define g(g) do { extern void gadget_##g(void); GEN(gadget_##g); } while (0)
#define gg(_g, a) do { g(_g); GEN(a); } while (0)
#define ggg(_g, a, b) do { g(_g); GEN(a); GEN(b); } while (0)
#define gggg(_g, a, b, c) do { g(_g); GEN(a); GEN(b); GEN(c); } while (0)
#define ggggg(_g, a, b, c, d) do { g(_g); GEN(a); GEN(b); GEN(c); GEN(d); } while (0)
#define gggggg(_g, a, b, c, d, e) do { g(_g); GEN(a); GEN(b); GEN(c); GEN(d); GEN(e); } while (0)
#define ga(g, i) do { extern gadget_t g##_gadgets[]; if (g##_gadgets[i] == NULL) UNDEFINED; GEN(g##_gadgets[i]); } while (0)
#define gag(g, i, a) do { ga(g, i); GEN(a); } while (0)
#define gagg(g, i, a, b) do { ga(g, i); GEN(a); GEN(b); } while (0)
#define gz(g, z) ga(g, sz(z))
#define h(h) gg(helper_0, h)
#define hh(h, a) ggg(helper_1, h, a)
#define hhh(h, a, b) gggg(helper_2, h, a, b)
#define ht_retint(h) gg(helper_tlb_0_retint, h)
#define h_read(h, z) do { g_addr(); ggg(helper_read##z, state->orig_ip, h##z); } while (0)
#define h_write(h, z) do { g_addr(); ggg(helper_write##z, state->orig_ip, h##z); } while (0)
#define UNDEFINED do { gggg(interrupt, INT_UNDEFINED, state->orig_ip, state->orig_ip); return false; } while (0)
#define SEGFAULT do { gggg(interrupt, INT_GPF, state->orig_ip, tlb->segfault_addr); return false; } while (0)
#define SYSCALL_AMD64 do { gggg(interrupt, INT_AMD64_SYSCALL, state->ip, 0); return false; } while (0)

static inline int sz(int size) {
    switch (size) {
        case 8: return size_8;
        case 16: return size_16;
        case 32: return size_32;
        default: return -1;
    }
}

// x86 cmp/test + jcc fusion. The CMP/TEST macros note the stream position
// right after their flag-op gadget; if the very next thing emitted is a
// jcc (nothing in between moved state->size), the op gadget is rewritten
// in place to its fused twin from gadgets-aarch64/math.S, which deposits
// the identical lazy-flag state and then branches on live host flags:
// [load][sub32_imm][imm] + [jmp_z][to][else] becomes
// [load][fused_cmp32_z_imm][imm][to][else] -- one dispatch and no
// CPU_res/CPU_cf reload in do_jump. Measured on Alpine i386/amd64
// binaries: 93-95% of jcc directly follow their flag setter.
// Zero-emission instructions between the pair (mov reg,same-reg) don't
// defeat the position check and are flag-transparent, so fusing across
// them is still exact.
static inline void gen_note_flag_op_fuse(struct gen_state *state, int size, int op) {
    state->x86_fuse_op = size == 32 ? op : 0;
    state->x86_fuse_end = state->size;
}

static inline bool gen_try_fuse_jcc(struct gen_state *state, int cond) {
#if defined(__aarch64__)
    if (state->x86_fuse_op == 0 || state->x86_fuse_end != state->size)
        return false;
    extern gadget_t sub_gadgets[], and_gadgets[];
    extern gadget_t fused_cmp32_gadgets[], fused_test32_gadgets[];
    gadget_t *ops = state->x86_fuse_op == 1 ? sub_gadgets : and_gadgets;
    gadget_t *fused = state->x86_fuse_op == 1 ? fused_cmp32_gadgets : fused_test32_gadgets;
    // The op gadget is the last word (reg source) or second-to-last
    // (imm/mem source, which trail one operand word). Identify the source
    // form by pointer-matching against the op's own gadget table; only a
    // CMP/TEST can end an instruction with a bare flag-op gadget (every
    // other ALU form stores its result afterwards).
    for (unsigned trailing = 0; trailing <= 1; trailing++) {
        if (state->size < trailing + 1)
            break;
        unsigned slot = state->size - 1 - trailing;
        unsigned long g = state->block->code[slot];
        for (int arg = 0; arg < arg_count; arg++) {
            if ((unsigned long) ops[size_32 * arg_count + arg] != g)
                continue;
            bool has_trailing = arg == arg_imm || arg == arg_mem;
            if (has_trailing != (trailing == 1))
                continue;
            gadget_t f = fused[cond * arg_count + arg];
            if (f == NULL)
                return false; // no fused form (parity, test+jb, ...)
            state->block->code[slot] = (unsigned long) f;
            return true;
        }
    }
#else
    (void) state; (void) cond;
#endif
    return false;
}

bool gen_addr(struct gen_state *state, struct modrm *modrm, bool seg_tls) {
    if (modrm->base == reg_none)
        gg(addr_none, modrm->offset);
    else
        gag(addr, modrm->base, modrm->offset);
    if (modrm->type == modrm_mem_si)
        ga(si, modrm->index * 4 + modrm->shift);
    if (seg_tls)
        g(seg_gs);
    return true;
}
#define g_addr() gen_addr(state, &modrm, seg_tls)

// Fold the address computation into the memory-operand gadget, replacing
//     [addr_<base>][disp]  [<op>32_mem][orig_ip]
// with
//     [fused_<op>32_mem_<base>][disp][orig_ip]
// one word shorter and, the point of the exercise, one gadget dispatch shorter.
// The fused gadgets live in jit/gadgets-aarch64/memory.S and branch into the
// unmodified op body, so semantics come from the same code either way.
//
// Declines to the unfused path for anything the simple form cannot express: a
// scaled index (which needs its own si gadget), a TLS segment (needs seg_gs), a
// non-32-bit operand, and the no-base form (that is addr_none, whose whole body
// is the displacement). On a real gcc compile 81.3% of memory operands emitted
// are the fusable shape, and load/store are 74% of those.
//
// The op is identified by pointer-matching the caller's table, the same trick
// gen_try_fuse_jcc uses. Note gen_op has already been handed the UNADJUSTED
// table base here, before the size stride is applied.
// ---- Live fusion mask (see jit/jit.h and /proc/ish/i386_jit_fuse) ----------
//
// -1 means "not yet seeded from the environment". Every gate below reads this
// through i386_jit_fuse_mask() on each call rather than caching the answer in a
// per-site `static`, which is the whole point: a cached gate would consult a
// stale value after the first block compiled and silently ignore every later
// write to the proc node, turning the knob into a liar. That is exactly the
// silent-plumbing failure mode this facility exists to eliminate, so it must not
// be reintroduced here for the sake of one relaxed atomic load per translated
// operand (translation-time, not execution-time).
static atomic_int i386_fuse_mask = -1;
static atomic_int arm64_fuse_mask = -1;
static atomic_int riscv64_fuse_mask = -1;
static atomic_int amd64_fuse_mask = -1;

// One table-driven implementation for all three guests. Each domain carries its
// live mask, its full-on value, its name table, and a seeder that reads the
// arch's existing env vars so their documented behaviour is untouched.
//
// -1 means "not yet seeded". Every gate reads through jit_fuse_mask_get() on each
// call and NEVER caches the answer in a per-site static. A cached gate consults a
// stale value after the first block compiles and then silently ignores every later
// write to the proc node -- the knob becomes a liar, which is the exact failure
// class this facility exists to remove. arm64's old arm64_fusion_disabled() had
// precisely that cache. Worth one relaxed atomic load per translated instruction,
// which is translation time, not execution time.
struct jit_fuse_entry { const char *name; unsigned bit; };
struct jit_fuse_domain {
    atomic_int *mask;
    unsigned all;
    const struct jit_fuse_entry *names;
    unsigned n_names;
    unsigned (*seed)(void);
};

static unsigned i386_fuse_seed(void) {
    unsigned v = JIT_FUSE_ALL;
    // Existing semantics preserved exactly: set to ANY value disables.
    // ISH_NO_MOVMR_FUSE still covers LEA, which shared movmr's gate before LEA
    // was given its own bit for independent A/B.
    if (getenv("ISH_NO_ADDR_FUSE") != NULL)    v &= ~JIT_FUSE_ADDR;
    if (getenv("ISH_NO_MOVMR_FUSE") != NULL)   v &= ~(JIT_FUSE_MOVMR | JIT_FUSE_LEA);
    if (getenv("ISH_NO_ALU_FUSE") != NULL)     v &= ~JIT_FUSE_ALU;
    if (getenv("ISH_NO_PUSHPOP_FUSE") != NULL) v &= ~JIT_FUSE_PUSHPOP;
    return v;
}
static unsigned arm64_fuse_seed(void) {
    // ISH_ARM64_NO_FUSE was the single bisection hatch for all three passes and
    // keeps that meaning; the per-pass bits are new granularity, not a change.
    return getenv("ISH_ARM64_NO_FUSE") != NULL ? 0 : JIT_FUSE_A64_ALL;
}
static unsigned riscv64_fuse_seed(void) {
    return getenv("ISH_RISCV64_NO_FUSE") != NULL ? 0 : JIT_FUSE_RV_ALL;
}
static unsigned amd64_fuse_seed(void) {
    // New namespace, so ISH_AMD64_NO_FUSE has no prior meaning to preserve; it
    // clears every native-vs-bridge switch, i.e. sends all of them back through
    // the C helpers, which is the bisection hatch.
    return getenv("ISH_AMD64_NO_FUSE") != NULL ? 0 : JIT_FUSE_AMD64_ALL;
}

static const struct jit_fuse_entry i386_fuse_names[] = {
    {"addr", JIT_FUSE_ADDR}, {"movmr", JIT_FUSE_MOVMR}, {"lea", JIT_FUSE_LEA},
    {"alu", JIT_FUSE_ALU}, {"pushpop", JIT_FUSE_PUSHPOP},
};
static const struct jit_fuse_entry arm64_fuse_names[] = {
    {"bcond", JIT_FUSE_A64_BCOND}, {"ldst", JIT_FUSE_A64_LDST},
    {"ldcmp", JIT_FUSE_A64_LDCMP}, {"retcache", JIT_FUSE_A64_RETCACHE},
};
static const struct jit_fuse_entry riscv64_fuse_names[] = {
    {"fold", JIT_FUSE_RV_FOLD}, {"jal", JIT_FUSE_RV_JAL},
    {"retcache", JIT_FUSE_RV_RETCACHE},
};
static const struct jit_fuse_entry amd64_fuse_names[] = {
    {"incdec_reg", JIT_FUSE_AMD64_INCDEC_REG},
};

static const struct jit_fuse_domain jit_fuse_domains[] = {
    [JIT_FUSE_ARCH_I386] = {&i386_fuse_mask, JIT_FUSE_ALL, i386_fuse_names,
        sizeof(i386_fuse_names) / sizeof(i386_fuse_names[0]), i386_fuse_seed},
    [JIT_FUSE_ARCH_ARM64] = {&arm64_fuse_mask, JIT_FUSE_A64_ALL, arm64_fuse_names,
        sizeof(arm64_fuse_names) / sizeof(arm64_fuse_names[0]), arm64_fuse_seed},
    [JIT_FUSE_ARCH_RISCV64] = {&riscv64_fuse_mask, JIT_FUSE_RV_ALL, riscv64_fuse_names,
        sizeof(riscv64_fuse_names) / sizeof(riscv64_fuse_names[0]), riscv64_fuse_seed},
    [JIT_FUSE_ARCH_AMD64] = {&amd64_fuse_mask, JIT_FUSE_AMD64_ALL, amd64_fuse_names,
        sizeof(amd64_fuse_names) / sizeof(amd64_fuse_names[0]), amd64_fuse_seed},
};
#define JIT_FUSE_N_DOMAINS (sizeof(jit_fuse_domains) / sizeof(jit_fuse_domains[0]))

unsigned jit_fuse_mask_get(enum jit_fuse_arch arch) {
    if ((unsigned) arch >= JIT_FUSE_N_DOMAINS)
        return 0;
    const struct jit_fuse_domain *d = &jit_fuse_domains[arch];
    int m = atomic_load_explicit(d->mask, memory_order_relaxed);
    if (m < 0) {
        // Benign race: concurrent seeders derive the same value from the same
        // environment, so whoever wins stores an identical mask.
        m = (int) d->seed();
        atomic_store_explicit(d->mask, m, memory_order_relaxed);
    }
    return (unsigned) m;
}

void jit_fuse_mask_set(enum jit_fuse_arch arch, unsigned mask) {
    if ((unsigned) arch >= JIT_FUSE_N_DOMAINS)
        return;
    const struct jit_fuse_domain *d = &jit_fuse_domains[arch];
    atomic_store_explicit(d->mask, (int) (mask & d->all), memory_order_relaxed);
}

const char *jit_fuse_name(enum jit_fuse_arch arch, unsigned index, unsigned *bit_out) {
    if ((unsigned) arch >= JIT_FUSE_N_DOMAINS)
        return NULL;
    const struct jit_fuse_domain *d = &jit_fuse_domains[arch];
    if (index >= d->n_names)
        return NULL;
    if (bit_out != NULL)
        *bit_out = d->names[index].bit;
    return d->names[index].name;
}

bool jit_fuse_set_by_name(enum jit_fuse_arch arch, const char *name, bool on) {
    if ((unsigned) arch >= JIT_FUSE_N_DOMAINS)
        return false;
    const struct jit_fuse_domain *d = &jit_fuse_domains[arch];
    if (strcmp(name, "all") == 0) {
        jit_fuse_mask_set(arch, on ? d->all : 0);
        return true;
    }
    for (unsigned i = 0; i < d->n_names; i++) {
        if (strcmp(name, d->names[i].name) != 0)
            continue;
        unsigned m = jit_fuse_mask_get(arch);
        if (on)
            m |= d->names[i].bit;
        else
            m &= ~d->names[i].bit;
        jit_fuse_mask_set(arch, m);
        return true;
    }
    return false;
}

unsigned i386_jit_fuse_mask(void) { return jit_fuse_mask_get(JIT_FUSE_ARCH_I386); }
unsigned arm64_jit_fuse_mask(void) { return jit_fuse_mask_get(JIT_FUSE_ARCH_ARM64); }
unsigned riscv64_jit_fuse_mask(void) { return jit_fuse_mask_get(JIT_FUSE_ARCH_RISCV64); }
unsigned amd64_jit_fuse_mask(void) { return jit_fuse_mask_get(JIT_FUSE_ARCH_AMD64); }

static inline bool gen_try_fuse_addr(struct gen_state *state, gadget_t *table,
        struct modrm *modrm, int size, bool seg_tls) {
#if defined(__aarch64__)
    // ISH_NO_ADDR_FUSE=1, or `addr=0` written to /proc/ish/i386_jit_fuse, emits
    // the old two-gadget form so the fusion can be A/B'd and bisected from one
    // binary. Read live, never cached -- see i386_jit_fuse_mask above.
    if (!(i386_jit_fuse_mask() & JIT_FUSE_ADDR))
        return false;
    if (seg_tls)
        return false;
    if (modrm->type == modrm_mem_si)
        return false;
    if (modrm->base == reg_none || modrm->base >= reg_count)
        return false;
    extern gadget_t load_gadgets[], store_gadgets[];
    extern gadget_t fused_load8_mem_gadgets[], fused_load16_mem_gadgets[],
                    fused_load32_mem_gadgets[];
    extern gadget_t fused_store8_mem_gadgets[], fused_store16_mem_gadgets[],
                    fused_store32_mem_gadgets[];
    gadget_t *fused = NULL;
    if (table == load_gadgets) {
        switch (size) {
            case size_8:  fused = fused_load8_mem_gadgets; break;
            case size_16: fused = fused_load16_mem_gadgets; break;
            case size_32: fused = fused_load32_mem_gadgets; break;
        }
    } else if (table == store_gadgets) {
        switch (size) {
            case size_8:  fused = fused_store8_mem_gadgets; break;
            case size_16: fused = fused_store16_mem_gadgets; break;
            case size_32: fused = fused_store32_mem_gadgets; break;
        }
    }
    if (fused == NULL)
        return false;
    // A missing entry is a NULL from the .gadget_list filler, not a bug: fall
    // back rather than emitting a null gadget pointer.
    if (fused[modrm->base] == NULL)
        return false;
    GEN(fused[modrm->base]);
    GEN(modrm->offset);
    GEN(state->orig_ip | state->orig_ip_extra);
    return true;
#else
    (void) state; (void) table; (void) modrm; (void) size; (void) seg_tls;
    return false;
#endif
}

// this really wants to use all the locals of the decoder, which we can do
// really nicely in gcc using nested functions, but that won't work in clang,
// so we explicitly pass 500 arguments. sorry for the mess
static inline bool gen_op(struct gen_state *state, gadget_t *gadgets, enum arg arg, struct modrm *modrm, uint64_t *imm, int size, bool seg_tls, dword_t addr_offset) {
    size = sz(size);
    gadget_t *table = gadgets; // unadjusted base, for the fusion's op identification
    gadgets = gadgets + size * arg_count;

    switch (arg) {
        case arg_modrm_reg:
            // TODO find some way to assert that this won't overflow?
            arg = modrm->reg + arg_reg_a; break;
        case arg_modrm_val:
            if (modrm->type == modrm_reg)
                arg = modrm->base + arg_reg_a;
            else
                arg = arg_mem;
            break;
        case arg_mem_addr:
            arg = arg_mem;
            modrm->type = modrm_mem;
            modrm->base = reg_none;
            modrm->offset = addr_offset;
            break;
        case arg_1:
            arg = arg_imm;
            *imm = 1;
            break;
    }
    if (arg >= arg_count || gadgets[arg] == NULL) {
        UNDEFINED;
    }
    if (arg == arg_mem || arg == arg_addr) {
        // arg_mem only: arg_addr wants the address itself as the value, and has
        // no [orig_ip] word, so the fused layout does not apply to it.
        if (arg == arg_mem && gen_try_fuse_addr(state, table, modrm, size, seg_tls))
            return true;
        if (!gen_addr(state, modrm, seg_tls))
            return false;
    }
    GEN(gadgets[arg]);
    if (arg == arg_imm)
        GEN(*imm);
    else if (arg == arg_mem)
        GEN(state->orig_ip | state->orig_ip_extra);
    return true;
}

static inline enum arg gen_reg_arg(enum arg arg, struct modrm *modrm) {
    switch (arg) {
        case arg_modrm_reg:
            return modrm->reg + arg_reg_a;
        case arg_modrm_val:
            return modrm->type == modrm_reg ? modrm->base + arg_reg_a : arg_invalid;
        default:
            return arg >= arg_reg_a && arg <= arg_reg_di ? arg : arg_invalid;
    }
}

static inline bool gen_mov(struct gen_state *state, enum arg src, enum arg dst, struct modrm *modrm, uint64_t *imm, int size, bool seg_tls, dword_t addr_offset) {
#if defined(__aarch64__) || defined(__x86_64__)
    enum arg src_reg = gen_reg_arg(src, modrm);
    enum arg dst_reg = gen_reg_arg(dst, modrm);

    if (size == 32 && src_reg != arg_invalid && dst_reg != arg_invalid) {
        if (src_reg != dst_reg) {
            extern gadget_t mov32_reg_reg_gadgets[];
            GEN(mov32_reg_reg_gadgets[(dst_reg - arg_reg_a) * 8 + (src_reg - arg_reg_a)]);
        }
        return true;
    }
#endif

#if defined(__aarch64__)
    // `mov <reg>, [<base>+disp]` in one gadget instead of two. The most-emitted
    // shape in the stream: 96709 of 207139 MOVs on a real gcc compile (46.7%).
    // Even with the address fusion the load still lands in _tmp and needs a
    // separate store32_reg_<dst> to move it out; this loads straight into the
    // destination register. See fused_movmr32 in jit/gadgets-aarch64/memory.S.
    //
    // The modrm test is deliberately `== modrm_mem`, not `!= modrm_reg`: that
    // excludes modrm_mem_si (a scaled index needs its own si gadget) in the same
    // breath, and it excludes arg_mem_addr, whose modrm fields gen_op rewrites
    // later, so inspecting them here would be reading pre-mutation state.
    if (sz(size) == size_32 && (i386_jit_fuse_mask() & JIT_FUSE_MOVMR) &&
            dst_reg != arg_invalid &&
            src == arg_modrm_val && modrm->type == modrm_mem && !seg_tls &&
            modrm->base != reg_none && modrm->base < reg_count) {
        extern gadget_t fused_movmr32_gadgets[];
        gadget_t g = fused_movmr32_gadgets[(dst_reg - arg_reg_a) * 8 + modrm->base];
        if (g != NULL) {
            GEN(g);
            GEN(modrm->offset);
            GEN(state->orig_ip | state->orig_ip_extra);
            return true;
        }
    }

    // The mirror: `mov [<base>+disp], <reg>`, 39815 of 207139 MOVs (19.2%).
    // Same conditions with the operands swapped. Shares ISH_NO_MOVMR_FUSE and the
    // JIT_FUSE_MOVMR bit, since the two are one change and are measured together.
    if ((i386_jit_fuse_mask() & JIT_FUSE_MOVMR) && sz(size) == size_32 && src_reg != arg_invalid &&
            dst == arg_modrm_val && modrm->type == modrm_mem && !seg_tls &&
            modrm->base != reg_none && modrm->base < reg_count) {
        extern gadget_t fused_movrm32_gadgets[];
        gadget_t g = fused_movrm32_gadgets[(src_reg - arg_reg_a) * 8 + modrm->base];
        if (g != NULL) {
            GEN(g);
            GEN(modrm->offset);
            GEN(state->orig_ip | state->orig_ip_extra);
            return true;
        }
    }

    // LEA: `lea <reg>, [<base>+disp]` in one gadget instead of three. This is the
    // only i386 MOV site whose src is arg_addr (emu/decode.h:1171), which is why
    // the movmr fusion above declines it, and it was the unexplained "src=other"
    // 14.3% of the MOV emission histogram -- 18.4% of instructions reaching
    // gen_mov in this repo's i386 rootfs.
    //
    // Worst work-to-dispatch ratio in the engine: LEA never touches memory, yet it
    // emitted addr_<base> + load32_addr + store32_reg_<dst>, and load32_addr's
    // whole body is `mov w0, w3`. No memory access also means the fused gadget
    // needs no read_prep, no orig_ip word and no fault path -- three instructions
    // total, and two stream words instead of four.
    //
    // 32-bit only: at oz=16 LEA writes just the low half of the destination.
    // seg_tls declines: gen_addr folds a segment base in via seg_gs, and rather
    // than decide whether LEA should do that, this leaves that case exactly as it
    // was. modrm_mem (not != modrm_reg) excludes the scaled-index form, which
    // needs its own si gadget.
    // Own bit (JIT_FUSE_LEA) so LEA can be A/B'd independently; ISH_NO_MOVMR_FUSE
    // still clears it too, preserving the env var's original scope.
    if ((i386_jit_fuse_mask() & JIT_FUSE_LEA) && sz(size) == size_32 && dst_reg != arg_invalid &&
            src == arg_addr && modrm->type == modrm_mem && !seg_tls &&
            modrm->base != reg_none && modrm->base < reg_count) {
        extern gadget_t fused_lea32_gadgets[];
        gadget_t g = fused_lea32_gadgets[(dst_reg - arg_reg_a) * 8 + modrm->base];
        if (g != NULL) {
            GEN(g);
            GEN(modrm->offset);
            return true;
        }
    }
#endif

    extern gadget_t load_gadgets[];
    extern gadget_t store_gadgets[];
    return gen_op(state, load_gadgets, src, modrm, imm, size, seg_tls, addr_offset) &&
           gen_op(state, store_gadgets, dst, modrm, imm, size, seg_tls, addr_offset);
}

// Collapse load(dst_reg) + op(imm) + store(dst_reg) into one fused gadget. See
// jit/gadgets-aarch64/math.S for the gadget family and why reg,imm rather than
// reg,reg (measured: reg,imm is ~60% of ALU load-op-store trios and needs 56
// gadgets; reg,reg is ~21% and would need 448).
//
// Deliberately NOT hooked inside gen_op, for two reasons that each cost real
// performance if ignored:
//   - gen_op cannot distinguish CMP/TEST from SUB/AND (they share sub_gadgets and
//     and_gadgets), so hooking there would fuse CMP and silently disable the
//     cmp/test+jcc fusion, which is worth more than this.
//   - gen_try_fuse_addr identifies its op by pointer-comparing gen_op's table
//     argument, so changing what gen_op receives can disable the address fusion.
// Deciding here, at the los() call site, means both operand kinds are known
// statically and neither existing fusion is perturbed.
//
// Requires BOTH that the source is a true immediate and that the destination
// resolves to a register: los() also emits load32_mem/op/store32_mem for a MEMORY
// destination, and fusing that shape would silently drop the memory load and store.
static inline bool gen_alu_imm_fused(struct gen_state *state, gadget_t *fused,
        enum arg src, enum arg dst, struct modrm *modrm, uint64_t *imm, int size) {
#if defined(__aarch64__)
    // ISH_NO_ALU_FUSE=1, or `alu=0` written to /proc/ish/i386_jit_fuse, falls back
    // to the three-gadget form so both sides live in one binary for A/B. Read
    // live, never cached -- see i386_jit_fuse_mask.
    if (!(i386_jit_fuse_mask() & JIT_FUSE_ALU))
        return false;
    if (sz(size) != size_32)
        return false;
    if (src != arg_imm)
        return false;
    enum arg dst_reg = gen_reg_arg(dst, modrm);
    if (dst_reg == arg_invalid)
        return false;
    gadget_t g = fused[dst_reg - arg_reg_a];
    if (g == NULL)
        return false;
    // Always emits two words. Never short-circuit to zero emission (even for a
    // no-op immediate): gen_try_fuse_jcc requires state->size to advance past a
    // flag-setting instruction, and an ALU op emitting nothing would leave a stale
    // cmp/test fuse note live for the next jcc.
    GEN(g);
    GEN(*imm);
    return true;
#else
    (void) state; (void) fused; (void) src; (void) dst;
    (void) modrm; (void) imm; (void) size;
    return false;
#endif
}

// `push <reg>` / `pop <reg>` in one dispatch instead of two.
//
// PUSH and POP move their value through _tmp, so each needs a staging gadget on
// one side: load32_reg_<r> before push, store32_reg_<r> after pop. Both shapes
// are extremely common (every call sequence) and the fused table is only ONE
// dimensional -- 16 gadgets, against the 448 that fusing ALU reg,reg would need
// for a comparable share. In an i386 profile taken after the earlier fusions
// landed, push+pop were 4.7% of gadget samples and the load32_reg/store32_reg
// staging they drive was another 21.1% (shared with other shapes).
//
// Register destination only. `push <mem>` and `pop <mem>` keep the generic form:
// their staging gadget is doing a real memory access, not just moving a value,
// and fusing that away would drop it.
static inline bool gen_push_reg_fused(struct gen_state *state, enum arg thing,
        struct modrm *modrm, int size) {
#if defined(__aarch64__)
    // ISH_NO_PUSHPOP_FUSE=1, or `pushpop=0` written to /proc/ish/i386_jit_fuse,
    // falls back to the two-gadget form so both sides live in one binary for A/B.
    // Read live, never cached -- see i386_jit_fuse_mask.
    if (!(i386_jit_fuse_mask() & JIT_FUSE_PUSHPOP))
        return false;
    if (sz(size) != size_32)
        return false;
    enum arg reg = gen_reg_arg(thing, modrm);
    if (reg == arg_invalid)
        return false;
    extern gadget_t fused_push_gadgets[];
    gadget_t g = fused_push_gadgets[reg - arg_reg_a];
    if (g == NULL)
        return false;
    GEN(g);
    GEN(state->orig_ip);
    return true;
#else
    (void) state; (void) thing; (void) modrm; (void) size;
    return false;
#endif
}

static inline bool gen_pop_reg_fused(struct gen_state *state, enum arg thing,
        struct modrm *modrm, int size) {
#if defined(__aarch64__)
    if (!(i386_jit_fuse_mask() & JIT_FUSE_PUSHPOP))
        return false;
    if (sz(size) != size_32)
        return false;
    enum arg reg = gen_reg_arg(thing, modrm);
    if (reg == arg_invalid)
        return false;
    extern gadget_t fused_pop_gadgets[];
    // reg_sp is a deliberate 0 in that table: see the comment on fused_pop in
    // jit/gadgets-aarch64/memory.S. `pop esp` falls back here.
    gadget_t g = fused_pop_gadgets[reg - arg_reg_a];
    if (g == NULL)
        return false;
    GEN(g);
    // Plain orig_ip, matching the generic `gg(pop, state->orig_ip)`: the bit-62
    // "adjust esp on segfault" marker is set AFTER that gadget is emitted, so it
    // only ever reaches a following memory store's orig_ip word -- i.e. only the
    // `pop <mem>` form, which is not fused here. Set below anyway so this path
    // leaves gen_state identical to the generic one.
    GEN(state->orig_ip);
    state->orig_ip_extra = 1ul << 62;
    return true;
#else
    (void) state; (void) thing; (void) modrm; (void) size;
    return false;
#endif
}

#define op(type, thing, z) do { \
    extern gadget_t type##_gadgets[]; \
    if (!gen_op(state, type##_gadgets, arg_##thing, &modrm, &imm, z, seg_tls, addr_offset)) return false; \
} while (0)

#define load(thing, z) op(load, thing, z)
#define store(thing, z) op(store, thing, z)
// load-op-store
#define los(o, src, dst, z) load(dst, z); op(o, src, z); store(dst, z)
#define lo(o, src, dst, z) load(dst, z); op(o, src, z)

#define MOV(src, dst,z) do { if (!gen_mov(state, arg_##src, arg_##dst, &modrm, &imm, z, seg_tls, addr_offset)) return false; } while (0)
#define MOVZX(src, dst,zs,zd) load(src, zs); gz(zero_extend, zs); store(dst, zd)
#define MOVSX(src, dst,zs,zd) load(src, zs); gz(sign_extend, zs); store(dst, zd)
// xchg must generate in this order to be atomic
#define XCHG(src, dst,z) load(src, z); op(xchg, dst, z); store(src, z)

// load-op-store, but try the fused reg,imm gadget first (one dispatch and two
// stream words instead of three and four). Falls back to the plain los()
// expansion for every shape the fused family does not cover: non-32-bit, a
// memory destination, or a non-immediate source.
//
// Applied ONLY to the seven store-back ALU ops. CMP and TEST keep lo() verbatim
// below -- they have no store to save, and their op word must stay a literal entry
// of sub_gadgets/and_gadgets for gen_try_fuse_jcc to pointer-match.
#define losf(o, src, dst, z) do { \
    extern gadget_t fused_##o##32_imm_gadgets[]; \
    if (!gen_alu_imm_fused(state, fused_##o##32_imm_gadgets, arg_##src, arg_##dst, &modrm, &imm, z)) { \
        los(o, src, dst, z); \
    } \
} while (0)

#define ADD(src, dst,z) losf(add, src, dst, z)
#define OR(src, dst,z) losf(or, src, dst, z)
#define ADC(src, dst,z) losf(adc, src, dst, z)
#define SBB(src, dst,z) losf(sbb, src, dst, z)
#define AND(src, dst,z) losf(and, src, dst, z)
#define SUB(src, dst,z) losf(sub, src, dst, z)
#define XOR(src, dst,z) losf(xor, src, dst, z)
#define CMP(src, dst,z) lo(sub, src, dst, z); gen_note_flag_op_fuse(state, z, 1)
#define TEST(src, dst,z) lo(and, src, dst, z); gen_note_flag_op_fuse(state, z, 2)
#define NOT(val,z) load(val,z); gz(not, z); store(val,z)
#define NEG(val,z) imm = 0; load(imm,z); op(sub, val,z); store(val,z)

#define POP(thing,z) do { \
    if (!gen_pop_reg_fused(state, arg_##thing, &modrm, z)) { \
        gg(pop, state->orig_ip); \
        state->orig_ip_extra = 1ul << 62; /* marks that on segfault the stack pointer should be adjusted */\
        store(thing, z); \
    } \
} while (0)
#define PUSH(thing,z) do { \
    if (!gen_push_reg_fused(state, arg_##thing, &modrm, z)) { \
        load(thing, z); gg(push, state->orig_ip); \
    } \
} while (0)

#define INC(val,z) load(val, z); gz(inc, z); store(val, z)
#define DEC(val,z) load(val, z); gz(dec, z); store(val, z)

#define fake_ip (state->ip | (1ul << 63))

#define jump_ips(off1, off2) \
    state->jump_ip[0] = state->size + off1; \
    if (off2 != 0) \
        state->jump_ip[1] = state->size + off2
#define JMP(loc) load(loc, OP_SIZE); g(jmp_indir); end_block = true
#define JMP_REL(off) gg(jmp, fake_ip + off); jump_ips(-1, 0); end_block = true
#define JCXZ_REL(off) ggg(jcxz, fake_ip + off, fake_ip); jump_ips(-2, -1); end_block = true

void helper_loop_dec_ecx(struct cpu_state *cpu);

void helper_aaa(struct cpu_state *cpu);
void helper_aas(struct cpu_state *cpu);
void helper_daa(struct cpu_state *cpu);
void helper_das(struct cpu_state *cpu);
void helper_aam(struct cpu_state *cpu, uint32_t base);
void helper_aad(struct cpu_state *cpu, uint32_t base);
#define AAA() h(helper_aaa)
#define AAS() h(helper_aas)
#define DAA() h(helper_daa)
#define DAS() h(helper_das)
// aam with base 0 is a divide error. The base is an immediate, so that is
// decidable here rather than in the helper.
#define AAM(base) do { \
    if ((base) == 0) { gggg(interrupt, INT_DIV, state->orig_ip, state->orig_ip); return false; } \
    hh(helper_aam, base); \
} while (0)
#define AAD(base) hh(helper_aad, base)
// LOOP rel8 (0xe2): decrement ECX without touching flags, then branch if the
// result is nonzero. That is jcxz with its two ip slots swapped, so the
// existing gadget covers it on both hosts.
#define LOOP_REL(off) h(helper_loop_dec_ecx); ggg(jcxz, fake_ip, fake_ip + off); jump_ips(-2, -1); end_block = true
// LOOPZ/LOOPE (0xe1) and LOOPNZ/LOOPNE (0xe0): same decrement, but the branch
// also tests ZF, which needs a gadget -- the taken/not-taken decision can't be
// expressed by swapping jcxz's targets. Operand 0 = taken, operand 1 = else.
#define LOOPZ_REL(off)  h(helper_loop_dec_ecx); ggg(loopz,  fake_ip + off, fake_ip); jump_ips(-2, -1); end_block = true
#define LOOPNZ_REL(off) h(helper_loop_dec_ecx); ggg(loopnz, fake_ip + off, fake_ip); jump_ips(-2, -1); end_block = true
#define jcc(cc, to, otherwise) do { \
    if (gen_try_fuse_jcc(state, cond_##cc)) { GEN(to); GEN(otherwise); } \
    else { gagg(jmp, cond_##cc, to, otherwise); } \
    jump_ips(-2, -1); end_block = true; } while (0)
#define J_REL(cc, off)  jcc(cc, fake_ip + off, fake_ip)
#define JN_REL(cc, off) jcc(cc, fake_ip, fake_ip + off)

// state->orig_ip: for use with page fault handler;
// -1: will be patched to block address in gen_end();
// fake_ip: the first one is the return address, used for saving to stack and verifying the cached ip in return cache is correct;
// fake_ip: the second one is the return target, patchable by return chaining.
#define CALL(loc) do { \
    load(loc, OP_SIZE); \
    ggggg(call_indir, state->orig_ip, -1, fake_ip, fake_ip); \
    state->block_patch_ip = state->size - 3; \
    jump_ips(-1, 0); \
    end_block = true; \
} while (0)
// the first four arguments are the same with CALL,
// the last one is the call target, patchable by return chaining.
#define CALL_REL(off) do { \
    gggggg(call, state->orig_ip, -1, fake_ip, fake_ip, fake_ip + off); \
    state->block_patch_ip = state->size - 4; \
    jump_ips(-2, -1); \
    end_block = true; \
} while (0)
#define RET_NEAR(imm) ggg(ret, state->orig_ip, 4 + imm); end_block = true
#define INT(code) gggg(interrupt, (uint8_t) code, state->ip, 0); end_block = true

// in/out (decode.h's 0xe4-0xe7, 0xec-0xef). Port I/O is ring-0, so from user
// mode the only correct outcome is #GP(0) -- SIGSEGV -- rather than the SIGILL
// the unhandled-opcode path used to give. That mattered because faulting on a
// port access is a deliberate hypervisor-probe idiom: util-linux's lscpu arms
// a SIGSEGV handler, runs the "VMXh"/port-0x5658 VMware backdoor, and
// siglongjmps out of the fault. It does not handle SIGILL, so lscpu died at
// startup on the x86 guests instead of printing anything.
//
// state->orig_ip, not state->ip: a fault reports the instruction that caused
// it, unlike INT above, whose traps report the instruction after. The
// segfault_addr slot is 0 -- this is not a memory fault, and the kernel's
// i386 GPF fixups only decode load/store opcodes, so they decline it and it
// reaches the SIGSEGV/SI_KERNEL delivery the same way INT_PRIV does.
#define PORT_IO() gggg(interrupt, INT_GPF, state->orig_ip, 0); end_block = true

#define SET(cc, dst) ga(set, cond_##cc); store(dst, 8)
#define SETN(cc, dst) ga(setn, cond_##cc); store(dst, 8)
// wins the prize for the most annoying instruction to generate
#define CMOV(cc, src, dst,z) do { \
    gag(skipn, cond_##cc, 0); \
    int start = state->size; \
    load(src, z); store(dst, z); \
    state->block->code[start - 1] = (state->size - start) * sizeof(long); \
} while (0)
#define CMOVN(cc, src, dst,z) do { \
    gag(skip, cond_##cc, 0); \
    int start = state->size; \
    load(src, z); store(dst, z); \
    state->block->code[start - 1] = (state->size - start) * sizeof(long); \
} while (0)

#define PUSHF() g(pushf)
#define POPF() g(popf)
#define SAHF g(sahf)
#define LAHF g(lahf)
#define CMC g(cmc)
#define CLC g(clc)
#define STC g(stc)
#define CLD g(cld)
#define STD g(std)

#define MUL18(val,z) MUL1(val,z)
#define MUL1(val,z) load(val, z); gz(mul, z)
#define IMUL1(val,z) load(val, z); gz(imul1, z)
// The div/idiv #DE gadget that consumes this IP operand (and does `gret 1`) is
// implemented only for aarch64; the x86_64 i386 div gadget takes no operand, so
// emitting the IP there desyncs the gadget stream (crash). Guard it to aarch64.
#if defined(__aarch64__)
#define DIV(val, z) load(val, z); gz(div, z); GEN(state->orig_ip)
#define IDIV(val, z) load(val, z); gz(idiv, z); GEN(state->orig_ip)
#else
#define DIV(val, z) load(val, z); gz(div, z)
#define IDIV(val, z) load(val, z); gz(idiv, z)
#endif
#define IMUL3(times, src, dst,z) load(src, z); op(imul, times, z); store(dst, z)
#define IMUL2(val, reg,z) IMUL3(val, reg, reg, z)

#define CVT ga(cvt, sz(oz))
#define CVTE ga(cvte, sz(oz))

#define ROL(count, val,z) los(rol, count, val, z)
#define ROR(count, val,z) los(ror, count, val, z)
#define RCL(count, val,z) los(rcl, count, val, z)
#define RCR(count, val,z) los(rcr, count, val, z)
#define SHL(count, val,z) los(shl, count, val, z)
#define SHR(count, val,z) los(shr, count, val, z)
#define SAR(count, val,z) los(sar, count, val, z)

#define SHLD(count, extra, dst,z) \
    load(dst,z); \
    if (arg_##count == arg_reg_c) op(shld_cl, extra,z); \
    else { op(shld_imm, extra,z); GEN(imm); } \
    store(dst,z)
#define SHRD(count, extra, dst,z) \
    load(dst,z); \
    if (arg_##count == arg_reg_c) op(shrd_cl, extra,z); \
    else { op(shrd_imm, extra,z); GEN(imm); } \
    store(dst,z)

#define BT(bit, val,z) lo(bt, val, bit, z)
#define BTC(bit, val,z) lo(btc, val, bit, z)
#define BTS(bit, val,z) lo(bts, val, bit, z)
#define BTR(bit, val,z) lo(btr, val, bit, z)
#define BSF(src, dst,z) los(bsf, src, dst, z)
#define BSR(src, dst,z) los(bsr, src, dst, z)
#define POPCNT(src, dst,z) los(popcnt, src, dst, z)
#define TZCNT(src, dst,z) los(tzcnt, src, dst, z)
#define LZCNT(src, dst,z) los(lzcnt, src, dst, z)

#define BSWAP(dst) ga(bswap, arg_##dst)

#define strop(op, rep, z) gag(op, sz(z) * size_count + rep_##rep, state->orig_ip)
#define STR(op, z) strop(op, once, z)
#define REP(op, z) strop(op, rep, z)
#define REPZ(op, z) strop(op, repz, z)
#define REPNZ(op, z) strop(op, repnz, z)

#define CMPXCHG(src, dst,z) load(src, z); op(cmpxchg, dst, z)
#define CMPXCHG8B(dst,z) g_addr(); gg(cmpxchg8b, state->orig_ip)
#define XADD(src, dst,z) XCHG(src, dst,z); ADD(src, dst,z)

void helper_rdtsc(struct cpu_state *cpu);
#define RDTSC h(helper_rdtsc)
#define CPUID() g(cpuid)
#define XGETBV() g(xgetbv)

// atomic
#define atomic_op(type, src, dst,z) load(src, z); op(atomic_##type, dst, z)
#define ATOMIC_ADD(src, dst,z) atomic_op(add, src, dst, z)
#define ATOMIC_OR(src, dst,z) atomic_op(or, src, dst, z)
#define ATOMIC_ADC(src, dst,z) atomic_op(adc, src, dst, z)
#define ATOMIC_SBB(src, dst,z) atomic_op(sbb, src, dst, z)
#define ATOMIC_AND(src, dst,z) atomic_op(and, src, dst, z)
#define ATOMIC_SUB(src, dst,z) atomic_op(sub, src, dst, z)
#define ATOMIC_XOR(src, dst,z) atomic_op(xor, src, dst, z)
#define ATOMIC_INC(val,z) op(atomic_inc, val, z)
#define ATOMIC_DEC(val,z) op(atomic_dec, val, z)
#define ATOMIC_CMPXCHG(src, dst,z) atomic_op(cmpxchg, src, dst, z)
#define ATOMIC_XADD(src, dst,z) load(src, z); op(atomic_xadd, dst, z); store(src, z)
#define ATOMIC_BTC(bit, val,z) lo(atomic_btc, val, bit, z)
#define ATOMIC_BTS(bit, val,z) lo(atomic_bts, val, bit, z)
#define ATOMIC_BTR(bit, val,z) lo(atomic_btr, val, bit, z)
#define ATOMIC_CMPXCHG8B(dst,z) g_addr(); gg(atomic_cmpxchg8b, state->orig_ip)

// fpu
#define st_0 0
#define st_i modrm.rm_opcode
#define FLD() hh(fpu_ld, st_i);
#define FILD(val,z) h_read(fpu_ild, z)
#define FLDM(val,z) h_read(fpu_ldm, z)
#define FSTM(dst,z) h_write(fpu_stm, z)
#define FIST(dst,z) h_write(fpu_ist, z)
#define FISTT(dst,z) h_write(fpu_istt, z)
#define FXCH() hh(fpu_xch, st_i)
#define FCOM() hh(fpu_com, st_i)
#define FCOMM(val,z) h_read(fpu_comm, z)
#define FICOM(val,z) h_read(fpu_icom, z)
#define FUCOM() hh(fpu_ucom, st_i)
#define FUCOMI() hh(fpu_ucomi, st_i)
#define FCOMI() hh(fpu_comi, st_i)
#define FTST() h(fpu_tst)
#define FXAM() h(fpu_xam)
#define FST() hh(fpu_st, st_i)
#define FCHS() h(fpu_chs)
#define FABS() h(fpu_abs)
#define FLDC(what) hh(fpu_ldc, fconst_##what)
#define FPREM() h(fpu_prem)
#define FRNDINT() h(fpu_rndint)
#define FSCALE() h(fpu_scale)
#define FSQRT() h(fpu_sqrt)
#define FYL2X() h(fpu_yl2x)
#define F2XM1() h(fpu_2xm1)
#define FSTSW(dst) if (arg_##dst == arg_reg_a) g(fstsw_ax); else h_write(fpu_stsw, 16)
#define FSTCW(dst) if (arg_##dst == arg_reg_a) UNDEFINED; else h_write(fpu_stcw, 16)
#define FLDCW(dst) if (arg_##dst == arg_reg_a) UNDEFINED; else h_read(fpu_ldcw, 16)
#define FSTENV(val,z) h_write(fpu_stenv, z)
#define FLDENV(val,z) h_write(fpu_ldenv, z)
#define FSAVE(val,z) h_write(fpu_save, z)
#define FRESTORE(val,z) h_write(fpu_restore, z)
#define FINIT() h(fpu_init)
#define FCLEX() h(fpu_clex)
#define FPOP h(fpu_pop)
#define FINCSTP() h(fpu_incstp)
#define FADD(src, dst) hhh(fpu_add, src, dst)
#define FIADD(val,z) h_read(fpu_iadd, z)
#define FADDM(val,z) h_read(fpu_addm, z)
#define FSUB(src, dst) hhh(fpu_sub, src, dst)
#define FSUBM(val,z) h_read(fpu_subm, z)
#define FISUB(val,z) h_read(fpu_isub, z)
#define FISUBR(val,z) h_read(fpu_isubr, z)
#define FSUBR(src, dst) hhh(fpu_subr, src, dst)
#define FSUBRM(val,z) h_read(fpu_subrm, z)
#define FMUL(src, dst) hhh(fpu_mul, src, dst)
#define FIMUL(val,z) h_read(fpu_imul, z)
#define FMULM(val,z) h_read(fpu_mulm, z)
#define FDIV(src, dst) hhh(fpu_div, src, dst)
#define FIDIV(val,z) h_read(fpu_idiv, z)
#define FDIVM(val,z) h_read(fpu_divm, z)
#define FDIVR(src, dst) hhh(fpu_divr, src, dst)
#define FIDIVR(val,z) h_read(fpu_idivr, z)
#define FDIVRM(val,z) h_read(fpu_divrm, z)
#define FPATAN() h(fpu_patan)
#define FSIN() h(fpu_sin)
#define FCOS() h(fpu_cos)
#define FSINCOS() h(fpu_sincos)
#define FXTRACT() h(fpu_xtract)
#define FCMOVB(src) hh(fpu_cmovb, src)
#define FCMOVE(src) hh(fpu_cmove, src)
#define FCMOVBE(src) hh(fpu_cmovbe, src)
#define FCMOVU(src) hh(fpu_cmovu, src)
#define FCMOVNB(src) hh(fpu_cmovnb, src)
#define FCMOVNE(src) hh(fpu_cmovne, src)
#define FCMOVNBE(src) hh(fpu_cmovnbe, src)
#define FCMOVNU(src) hh(fpu_cmovnu, src)

// vector

static inline bool could_be_memory(enum arg arg) {
    return arg == arg_modrm_val || arg == arg_mm_modrm_val || arg == arg_xmm_modrm_val;
}

static inline uint16_t cpu_reg_offset(enum arg arg, int index) {
    if (arg == arg_xmm_modrm_reg || arg == arg_xmm_modrm_val)
        return CPU_OFFSET(xmm[index]);
    if (arg == arg_mm_modrm_reg || arg == arg_mm_modrm_val)
        return CPU_OFFSET(mm[index]);
    if (arg == arg_modrm_reg || arg == arg_modrm_val)
        return CPU_OFFSET(regs[index]);
    return 0;
}

// VEX-encoded (AVX/AVX2) instructions for the i386 guest (GH #525).
//
// i386 is JIT-only -- there is no interpreter to bail to the way amd64 does --
// so the prefix is decoded here at codegen time and the whole instruction is
// described to the runtime by a single 32-bit word carried in the vec-helper
// gadgets' existing immediate slot. See emu/avx.h for the descriptor layout
// and vec_avx32 (emu/avx.c) for execution.
//
// 32-bit mode makes this simpler than amd64: VEX.R/X/B are ignored, so every
// operand is xmm0-7, and there is no REX interaction to worry about.
static inline bool gen_vex32(struct gen_state *state, struct tlb *tlb, struct modrm *modrm,
        bool seg_tls, unsigned map, unsigned pp, unsigned op, unsigned l, unsigned w,
        unsigned vvvv, uint8_t imm8) {
    unsigned vlen = l ? 256 : 128;

    // VEX.vvvv is 4 bits but 32-bit mode has only 8 vector registers, so the
    // top bit must be zero once un-inverted.
    if (vvvv > 7)
        UNDEFINED;

    unsigned mem_bits = avx32_mem_bits(map, op, pp, w, vlen);
    bool rm_mem = modrm->type != modrm_reg;
    bool rm_dst = avx32_rm_is_dst(map, op, pp);

    // mem_bits == 0 means "not implemented by this front-end" -- but it is
    // also the honest answer for the register-only forms (the shift-by-imm8
    // group, VZEROUPPER, VPMOVMSKB), so only reject it when there really is a
    // memory operand to size.
    if (rm_mem && mem_bits == 0)
        UNDEFINED;
    // Register-only forms legitimately report mem_bits == 0: the shift-by-imm8
    // group, VZEROUPPER/VZEROALL, and the mask-to-GPR extracts.
    if (!rm_mem && mem_bits == 0 && !(map == 1 && (op == 0x71 || op == 0x72 || op == 0x73 ||
                                                   op == 0x77 || op == 0xd7 || op == 0x50)))
        UNDEFINED;

    uint32_t desc = AVX32_DESC(map, pp, op, l, w, vvvv, modrm->opcode,
                               rm_mem ? 1 : 0, rm_mem ? 0 : modrm->rm_opcode, imm8);
    uint16_t reg_off = CPU_OFFSET(xmm[modrm->opcode]);

    if (!rm_mem) {
        uint16_t rm_off = CPU_OFFSET(xmm[modrm->rm_opcode]);
        g(vec_helper_reg_imm);
        GEN(vec_avx32);
        GEN((uint64_t) rm_off | ((uint64_t) reg_off << 16) | ((uint64_t) desc << 32));
        return true;
    }

    gen_addr(state, modrm, seg_tls);
    // The gadget size bakes in the access width, so it has to match the
    // instruction's actual memory operand rather than the operation width.
    switch (mem_bits) {
        case 8:   if (rm_dst) g(vec_helper_write8_imm);   else g(vec_helper_read8_imm);   break;
        case 16:  if (rm_dst) g(vec_helper_write16_imm);  else g(vec_helper_read16_imm);  break;
        case 32:  if (rm_dst) g(vec_helper_write32_imm);  else g(vec_helper_read32_imm);  break;
        case 64:  if (rm_dst) g(vec_helper_write64_imm);  else g(vec_helper_read64_imm);  break;
        case 128: if (rm_dst) g(vec_helper_write128_imm); else g(vec_helper_read128_imm); break;
        case 256: if (rm_dst) g(vec_helper_write256_imm); else g(vec_helper_read256_imm); break;
        default: UNDEFINED;
    }
    GEN(state->orig_ip);
    GEN(vec_avx32);
    GEN((uint64_t) reg_off | ((uint64_t) desc << 32));
    (void) tlb;
    return true;
}

static inline bool gen_vec(enum arg src, enum arg dst, void (*helper)(), gadget_t read_mem_gadget, gadget_t write_mem_gadget, struct gen_state *state, struct modrm *modrm, uint8_t imm, bool seg_tls, bool has_imm) {
    bool rm_is_src = !could_be_memory(dst);
    enum arg rm = rm_is_src ? src : dst;
    enum arg reg = rm_is_src ? dst : src;

    uint16_t reg_offset = cpu_reg_offset(reg, modrm->opcode);
    uint16_t rm_reg_offset = cpu_reg_offset(rm, modrm->rm_opcode);
    assert(reg_offset != 0);

    if (could_be_memory(rm) && modrm->type != modrm_reg)
        rm = arg_mem;

    uint64_t imm_arg = 0;
    if (has_imm)
        imm_arg = (uint64_t) imm << 32;

    switch (rm) {
        case arg_xmm_modrm_val:
        case arg_mm_modrm_val:
        case arg_modrm_val:
            assert(rm_reg_offset != 0);
            if (!has_imm)
                g(vec_helper_reg);
            else
                g(vec_helper_reg_imm);
            GEN(helper);
            // first byte is src, second byte is dst
            uint64_t arg;
            if (rm_is_src)
                arg = rm_reg_offset | (reg_offset << 16);
            else
                arg = reg_offset | (rm_reg_offset << 16);
            GEN(arg | imm_arg);
            break;

        case arg_mem:
            gen_addr(state, modrm, seg_tls);
            GEN(rm_is_src ? read_mem_gadget : write_mem_gadget);
            GEN(state->orig_ip);
            GEN(helper);
            GEN(reg_offset | imm_arg);
            break;

        case arg_imm:
            // TODO: support immediates and opcode
            g(vec_helper_imm);
            GEN(helper);
            // This is rm_opcode instead of opcode because PSRLQ is weird like that
            GEN(((uint16_t) imm) | (cpu_reg_offset(reg, modrm->rm_opcode) << 16));
            break;

        default:
            printk("jit: unimplemented vector op at ip %#x\n", (unsigned) state->orig_ip);
            UNDEFINED;
    }
    return true;
}

#define has_imm_ false
#define has_imm__imm true
#define _v(src, dst, helper, _imm, z) do { \
    extern void gadget_vec_helper_read##z##_imm(void); \
    extern void gadget_vec_helper_write##z##_imm(void); \
    if (!gen_vec(src, dst, (void (*)()) helper, gadget_vec_helper_read##z##_imm, gadget_vec_helper_write##z##_imm, state, &modrm, imm, seg_tls, has_imm_##_imm)) return false; \
} while (0)
#define v_(op, src, dst, _imm,z) _v(arg_##src, arg_##dst, vec_##op##z, _imm,z)
#define v(op, src, dst,z) v_(op, src, dst,,z)
#define v_imm(op, src, dst,z) v_(op, src, dst, _imm,z)

#define vec_dst_size_modrm_val 32
#define vec_dst_size_mm_modrm_val 64
#define vec_dst_size_mm_modrm_reg 64
#define vec_dst_size_xmm_modrm_val 128
#define vec_dst_size_xmm_modrm_reg 128
// you always want to merge when storing to memory
// default is to never merge otherwise
#define VMOV(src, dst, z) \
    if (could_be_memory(arg_##dst) && modrm.type != modrm_reg) { \
        v(merge, src, dst,z); \
    } else { \
        v(glue3(zero, vec_dst_size_##dst, _copy), src, dst,z); \
    }
// this will additionally merge if both src and dst are registers, e.g. movss
#define VMOV_MERGE_REG(src, dst, z) \
    if (modrm.type == modrm_reg || could_be_memory(arg_##dst)) { \
        v(merge, src, dst,z); \
    } else { \
        v(glue3(zero, vec_dst_size_##dst, _copy), src, dst,z); \
    }

#define VCOMPARE(src, dst,z) v(compare, src, dst,z)
#define V_OP(op, src, dst, z) v(op, src, dst, z)
#define V_OP_IMM(op, src, dst, z) v_imm(op, src, dst, z)

#define DECODER_RET static int
#define DECODER_NAME gen_step
#define DECODER_ARGS struct gen_state *state, struct tlb *tlb
#define DECODER_PASS_ARGS state, tlb

#define OP_SIZE 32
#include "emu/decode.h"
#undef OP_SIZE
#define OP_SIZE 16
#include "emu/decode.h"
#undef OP_SIZE
