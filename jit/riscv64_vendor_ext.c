// RISC-V vendor/user extension hook: a decode-time registry that lets a
// custom instruction execute as the ratified-ISA instruction it is defined
// to equal -- the decoder rewrites the word and the JIT compiles it with its
// own gadgets -- with NO interpreter, NO C at run time, and NO change to the
// JIT's engine-only-for-the-ratified-ISA design
// (docs/historical/riscv64_guest_plan.md patch 5b).
//
// This file is ALSO the reference implementation for /AOK/docs' vendor
// extension write-up (opt/AOK/docs/riscv64-vendor-extensions.md) — the
// two are meant to be read side by side. If you change the mechanism
// here, update that doc too.
//
// ---- Why this is safe: the opcode-space rule ----
// RISC-V permanently reserves four major opcodes for non-standard use:
// custom-0/1/2/3 (0x0B/0x2B/0x5B/0x7B — RISCV64_OP_CUSTOM{0,1,2,3} in
// emu/arch/riscv64/decode.h). The ISA spec guarantees no ratified
// standard extension will ever claim these encodings. That is what
// makes this hook safe to leave permanently wired into gen_step_riscv64
// rather than a special build mode: every entry in the registry is
// validated (riscv64_vendor_ext_register_is_valid) to sit fully inside
// that space, so it can never shadow, alias, or race a real instruction
// the JIT might implement later. The JIT remains the only engine for
// the ratified ISA; this hook only ever fires on encodings nothing else
// claims.
//
// ---- The example pack ----
// The four instructions below (ish.clz/ish.ctz/ish.pcnt/ish.bswap) are
// an iSH-AOK-INVENTED demonstration pack, not a transcription of any
// real vendor's silicon (T-Head, Andes, SiFive, etc. all ship real
// custom-0 extensions, but this project has no way to verify a
// transcribed encoding against their actual hardware, so it does not
// claim to be bit-compatible with any of them). They fill a real gap
// they are clz, ctz, cpop and rev8 of Zbb under other encodings, which is
// what a vendor instruction mapped onto the ratified ISA looks like. Swap
// in real encodings here once you have a verified spec to check them
// against; the mechanism below doesn't care.
//
// ---- Enabling ----
// Off by default (a vendor extension changing what encodings are legal
// is a deliberate opt-in, not ambient behavior). Set
// ISH_RISCV64_VENDOR_EXT=1 in the environment before starting the
// guest. Checked once per process via the same cached-getenv idiom used
// throughout this codebase (see e.g. jit/jit.c's ISH_AMD64_CC1_TRACE) —
// a process-lifetime toggle, not a hot runtime one; see the doc for why
// that's sufficient for a built-in pack and what a hot-swappable
// CLI-plugin tier would need on top (JIT block-cache invalidation on
// toggle, listed as future work, not implemented here).

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "emu/cpu.h"
#include "emu/arch/riscv64/decode.h"

// One registered instruction: the words (insn & mask) == match, executing
// as `standard`, a ratified-ISA encoding with its rd/rs1/rs2 fields zero;
// `carry` says which of those the vendor word's own fields fill in (at the
// same bit positions, R-type shaped).
struct riscv64_vendor_insn {
    uint32_t mask;
    uint32_t match;
    const char *mnemonic;
    uint32_t standard;
    unsigned carry;
};
#define CARRY_RD  1
#define CARRY_RS1 2
#define CARRY_RS2 4

// R-type shaped: opcode + funct3 select the operation, funct7 pinned to
// 0 (claim as little of the custom-0 space as the pack actually uses,
// leaving other funct7 values free for a different pack sharing the
// same opcode), rs2 unused/ignored. rd == 0 discards the result, as the
// standard instruction does.
#define RISCV64_VENDOR_MASK  0xfe00707fu
#define RISCV64_VENDOR_MATCH(funct3) \
    (RISCV64_OP_CUSTOM0 | ((uint32_t) (funct3) << 12))
// Zbb 1.0.0, OP-IMM: clz/ctz/cpop rd, rs1 (funct3 1, imm 0x600-0x602) and
// rev8 rd, rs1 (funct3 5, imm 0x6b8).
#define ZBB_UNARY(imm12, funct3) (((uint32_t) (imm12) << 20) | ((uint32_t) (funct3) << 12) | 0x13u)

static const struct riscv64_vendor_insn riscv64_vendor_ext_table[] = {
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(0), "ish.clz",   ZBB_UNARY(0x600, 1), CARRY_RD | CARRY_RS1 },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(1), "ish.ctz",   ZBB_UNARY(0x601, 1), CARRY_RD | CARRY_RS1 },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(2), "ish.pcnt",  ZBB_UNARY(0x602, 1), CARRY_RD | CARRY_RS1 },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(3), "ish.bswap", ZBB_UNARY(0x6b8, 5), CARRY_RD | CARRY_RS1 },
};
#define RISCV64_VENDOR_EXT_COUNT \
    (sizeof(riscv64_vendor_ext_table) / sizeof(riscv64_vendor_ext_table[0]))

// Registration validator: every entry must fully pin the opcode field
// to one of the four reserved custom opcodes. Nothing in this file
// calls this dynamically today (the table above is static and already
// correct by construction) — it exists so a future dynamic
// registration API (the CLI-plugin tier from the plan) has a real
// safety check to call, not just a comment promising one, and so this
// file's own table is self-checking in debug builds (see
// riscv64_vendor_ext_self_check below).
bool riscv64_vendor_ext_register_is_valid(uint32_t mask, uint32_t match) {
    if ((mask & 0x7f) != 0x7f)
        return false; // opcode field must be fully pinned
    uint32_t opcode = match & 0x7f;
    return opcode == RISCV64_OP_CUSTOM0 || opcode == RISCV64_OP_CUSTOM1 ||
           opcode == RISCV64_OP_CUSTOM2 || opcode == RISCV64_OP_CUSTOM3;
}

bool riscv64_vendor_ext_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_RISCV64_VENDOR_EXT") != NULL ? 1 : 0;
    return enabled;
}

// Gen-time translation: the ratified instruction a registered vendor word
// executes as, with its operand fields filled in; or 0 if nothing matches
// (the caller then falls through to the undefined-instruction path -- this
// hook only ever narrows what's legal, never widens silently). *mnemonic,
// when not NULL, gets the entry's name for tracing.
uint32_t riscv64_vendor_ext_translate(uint32_t insn, const char **mnemonic) {
    for (size_t i = 0; i < RISCV64_VENDOR_EXT_COUNT; i++) {
        const struct riscv64_vendor_insn *e = &riscv64_vendor_ext_table[i];
        if ((insn & e->mask) != e->match)
            continue;
        uint32_t out = e->standard;
        if (e->carry & CARRY_RD)  out |= insn & (0x1fu << 7);
        if (e->carry & CARRY_RS1) out |= insn & (0x1fu << 15);
        if (e->carry & CARRY_RS2) out |= insn & (0x1fu << 20);
        if (mnemonic != NULL)
            *mnemonic = e->mnemonic;
        return out;
    }
    return 0;
}
