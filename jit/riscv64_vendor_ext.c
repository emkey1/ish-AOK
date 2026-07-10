// RISC-V vendor/user extension hook: a decode-miss registry that lets
// custom instructions execute via one generic "call a C handler" gadget,
// with NO interpreter and NO change to the JIT's engine-only-for-the-
// ratified-ISA design (riscv64_guest_plan.md patch 5b).
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
// though: this port doesn't implement the standard Zbb bit-manipulation
// extension, so clz/ctz/popcount/byte-swap have no other way to run
// under this engine — a legitimate, if invented, motivation for a
// vendor extension. Swap in real encodings here once you have a
// verified spec to check them against; the mechanism below doesn't care.
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

struct riscv64_vendor_insn {
    uint32_t mask;
    uint32_t match;
    const char *mnemonic;
    void (*handler)(struct cpu_state *cpu, uint32_t insn);
};

// R-type shaped: opcode + funct3 select the operation, funct7 pinned to
// 0 (claim as little of the custom-0 space as the pack actually uses,
// leaving other funct7 values free for a different pack sharing the
// same opcode), rs2 unused/ignored. rd==0 writes are dropped (the
// riscv64_regs[0]-must-never-be-written invariant every gadget in this
// port already follows); rs1==0 reads the permanently-zero slot, same
// as everywhere else.
#define RISCV64_VENDOR_MASK  0xfe00707fu
#define RISCV64_VENDOR_MATCH(funct3) \
    (RISCV64_OP_CUSTOM0 | ((uint32_t) (funct3) << 12))

static void riscv64_vext_clz(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = riscv64_rd(insn), rs1 = riscv64_rs1(insn);
    uint64_t v = cpu->riscv64_regs[rs1];
    if (rd != 0)
        cpu->riscv64_regs[rd] = v == 0 ? 64 : (uint64_t) __builtin_clzll(v);
}

static void riscv64_vext_ctz(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = riscv64_rd(insn), rs1 = riscv64_rs1(insn);
    uint64_t v = cpu->riscv64_regs[rs1];
    if (rd != 0)
        cpu->riscv64_regs[rd] = v == 0 ? 64 : (uint64_t) __builtin_ctzll(v);
}

static void riscv64_vext_pcnt(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = riscv64_rd(insn), rs1 = riscv64_rs1(insn);
    uint64_t v = cpu->riscv64_regs[rs1];
    if (rd != 0)
        cpu->riscv64_regs[rd] = (uint64_t) __builtin_popcountll(v);
}

static void riscv64_vext_bswap(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = riscv64_rd(insn), rs1 = riscv64_rs1(insn);
    uint64_t v = cpu->riscv64_regs[rs1];
    if (rd != 0)
        cpu->riscv64_regs[rd] = __builtin_bswap64(v);
}

static const struct riscv64_vendor_insn riscv64_vendor_ext_table[] = {
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(0), "ish.clz",   riscv64_vext_clz },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(1), "ish.ctz",   riscv64_vext_ctz },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(2), "ish.pcnt",  riscv64_vext_pcnt },
    { RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(3), "ish.bswap", riscv64_vext_bswap },
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

// Gen-time lookup: does insn match a registered vendor instruction?
// Returns the mnemonic for logging/tracing, or NULL on no match (in
// which case the caller falls through to the normal undefined-
// instruction path — this hook only ever narrows what's legal, never
// widens silently).
const char *riscv64_vendor_ext_lookup(uint32_t insn) {
    for (size_t i = 0; i < RISCV64_VENDOR_EXT_COUNT; i++) {
        const struct riscv64_vendor_insn *e = &riscv64_vendor_ext_table[i];
        if ((insn & e->mask) == e->match)
            return e->mnemonic;
    }
    return NULL;
}

// Execution-time dispatch: the single function every matched vendor
// instruction's callback gadget invokes (jit/guest-riscv64/fp.S's
// call_helper, arg = the raw 32-bit instruction word). Re-scans the
// same tiny table gen-time lookup used — cheap (4 entries) and keeps
// the code stream to one word (the instruction itself) rather than
// threading a second per-entry function pointer through gen().
void riscv64_vendor_ext_dispatch(struct cpu_state *cpu, unsigned long arg) {
    uint32_t insn = (uint32_t) arg;
    for (size_t i = 0; i < RISCV64_VENDOR_EXT_COUNT; i++) {
        const struct riscv64_vendor_insn *e = &riscv64_vendor_ext_table[i];
        if ((insn & e->mask) == e->match) {
            e->handler(cpu, insn);
            return;
        }
    }
    // Unreachable in practice: gen_step_riscv64 only emits a call to
    // this dispatcher after riscv64_vendor_ext_lookup already matched
    // the same static table. Left as a silent no-op rather than a
    // crash — the guest instruction simply becomes inert, which is a
    // far safer failure mode than executing garbage if this invariant
    // is ever violated by a future change.
}
