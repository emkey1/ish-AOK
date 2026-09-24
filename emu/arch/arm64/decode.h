#ifndef EMU_ARCH_ARM64_DECODE_H
#define EMU_ARCH_ARM64_DECODE_H

// Adapted from OpenMinis/ish-arm64 (emu/arch/arm64/decode.h), a GPLv3 fork of
// ish-app/ish. See /CREDITS-aarch64.md. Field names below are renamed with
// an arm64_ prefix (cpu->arm64_nf instead of cpu->nf, etc.) because this
// codebase's struct cpu_state is a single struct shared across i386, amd64,
// and arm64 tasks (not a per-arch build like theirs), and bare `zf`/`cf`
// already name the i386 eflags-bitfield fields.

#include "misc.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "kernel/abi.h"

/*
 * ARM64 Instruction Decoding
 *
 * ARM64 uses fixed 4-byte (32-bit) instructions. The instruction encoding
 * is relatively straightforward compared to x86's variable-length encoding.
 *
 * Top-level encoding groups (bits 28:25):
 *   0000 - Reserved
 *   100x - Data processing (immediate)
 *   101x - Branches, exceptions, system
 *   x1x0 - Loads and stores
 *   x101 - Data processing (register)
 *   x111 - Data processing (SIMD and FP)
 */

// Instruction group extraction
#define ARM64_OP0(insn) (((insn) >> 25) & 0xf)

// Common field extractions
#define ARM64_RD(insn)  (((insn) >>  0) & 0x1f)  // Destination register
#define ARM64_RN(insn)  (((insn) >>  5) & 0x1f)  // First source register
#define ARM64_RM(insn)  (((insn) >> 16) & 0x1f)  // Second source register
#define ARM64_RT(insn)  (((insn) >>  0) & 0x1f)  // Transfer register (load/store)
#define ARM64_RT2(insn) (((insn) >> 10) & 0x1f)  // Second transfer register

// Instruction size (sf bit, bit 31)
#define ARM64_SF(insn)  (((insn) >> 31) & 0x1)   // 0=32-bit, 1=64-bit

// Condition codes (used in conditional branches, select, etc.)
enum arm64_cond {
    COND_EQ = 0,   // Equal (Z==1)
    COND_NE = 1,   // Not equal (Z==0)
    COND_CS = 2,   // Carry set / unsigned higher or same (C==1)
    COND_CC = 3,   // Carry clear / unsigned lower (C==0)
    COND_MI = 4,   // Minus / negative (N==1)
    COND_PL = 5,   // Plus / positive or zero (N==0)
    COND_VS = 6,   // Overflow (V==1)
    COND_VC = 7,   // No overflow (V==0)
    COND_HI = 8,   // Unsigned higher (C==1 && Z==0)
    COND_LS = 9,   // Unsigned lower or same (C==0 || Z==1)
    COND_GE = 10,  // Signed greater or equal (N==V)
    COND_LT = 11,  // Signed less than (N!=V)
    COND_GT = 12,  // Signed greater than (Z==0 && N==V)
    COND_LE = 13,  // Signed less or equal (Z==1 || N!=V)
    COND_AL = 14,  // Always
    COND_NV = 15,  // Always (same as AL)
};

// Evaluate a condition code against the current flags. Reads the packed
// NZCV field (bits 31:28) rather than decoded bools — see emu/cpu.h's
// arm64_nzcv comment for why this changed from the original design.
static inline bool arm64_cond_check(struct cpu_state *cpu, enum arm64_cond cond) {
    bool nf = (cpu->arm64_nzcv >> 31) & 1;
    bool zf = (cpu->arm64_nzcv >> 30) & 1;
    bool cf = (cpu->arm64_nzcv >> 29) & 1;
    bool vf = (cpu->arm64_nzcv >> 28) & 1;
    bool result;
    switch (cond >> 1) {
        case 0: result = zf; break;                    // EQ/NE
        case 1: result = cf; break;                    // CS/CC
        case 2: result = nf; break;                    // MI/PL
        case 3: result = vf; break;                    // VS/VC
        case 4: result = cf && !zf; break;              // HI/LS
        case 5: result = nf == vf; break;               // GE/LT
        case 6: result = !zf && (nf == vf); break;       // GT/LE
        case 7: result = true; break;                    // AL/NV
        default: result = false; break;
    }
    // Invert result for odd condition codes (except AL/NV)
    if ((cond & 1) && cond != COND_AL && cond != COND_NV)
        result = !result;
    return result;
}

// Instruction type classification
enum arm64_insn_type {
    INSN_UNKNOWN = 0,
    INSN_DP_IMM,        // Data processing (immediate)
    INSN_BRANCH,        // Branches
    INSN_EXCEPTION,     // Exception generation
    INSN_SYSTEM,        // System instructions
    INSN_LD_ST,         // Loads and stores
    INSN_DP_REG,        // Data processing (register)
    INSN_SIMD_FP,       // SIMD and floating-point
};

// Classify an instruction by its top-level encoding
static inline enum arm64_insn_type arm64_classify_insn(uint32_t insn) {
    uint32_t op0 = ARM64_OP0(insn);

    switch (op0) {
        case 0b1000: case 0b1001:
            return INSN_DP_IMM;
        case 0b1010: case 0b1011:
            // System instructions: 1101_0101_00 (bits 31-22 = 0x354)
            if ((insn >> 22) == 0x354)
                return INSN_SYSTEM;
            // Exception generation: 1101_0100 (bits 31-24 = 0xd4)
            if ((insn >> 24) == 0xd4)
                return INSN_EXCEPTION;
            // Everything else is a branch
            return INSN_BRANCH;
        case 0b0100: case 0b0110: case 0b1100: case 0b1110:
            return INSN_LD_ST;
        case 0b0101: case 0b1101:
            return INSN_DP_REG;
        case 0b0111: case 0b1111:
            return INSN_SIMD_FP;
        default:
            return INSN_UNKNOWN;
    }
}

// Read a 32-bit instruction from memory. guest_addr_t (64-bit), not addr_t
// (32-bit) — the latter is i386-specific in this codebase (misc.h:121),
// unlike OpenMinis' arm64-only build where there was only one address width.
static inline bool arm64_read_insn(guest_addr_t *ip, struct tlb *tlb, uint32_t *insn) {
    if (!guest_abi_range_valid(GUEST_ABI_ARM64, *ip, sizeof(*insn)))
        return false;
    // A fetch: refused for a page without PROT_EXEC (emu/tlb.h).
    if (!tlb_fetch(tlb, *ip, insn, sizeof(*insn)))
        return false;
    *ip += 4;
    return true;
}

// Sign-extend a value
static inline int64_t arm64_sign_extend(uint64_t val, unsigned bits) {
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (val & sign_bit)
        return (int64_t) (val | (~0ULL << bits));
    return (int64_t) val;
}

// Extract immediate from PC-relative branch instructions (26-bit signed offset)
static inline int64_t arm64_branch_imm26(uint32_t insn) {
    return arm64_sign_extend((insn & 0x03ffffff) << 2, 28);
}

// Extract immediate from conditional branch instructions (19-bit signed offset)
static inline int64_t arm64_branch_imm19(uint32_t insn) {
    return arm64_sign_extend(((insn >> 5) & 0x7ffff) << 2, 21);
}

// Extract immediate from test/compare-and-branch instructions (14-bit signed offset)
static inline int64_t arm64_branch_imm14(uint32_t insn) {
    return arm64_sign_extend(((insn >> 5) & 0x3fff) << 2, 16);
}

// Extract immediate from ADR/ADRP instructions
static inline int64_t arm64_adr_imm(uint32_t insn) {
    uint64_t immlo = (insn >> 29) & 0x3;
    uint64_t immhi = (insn >> 5) & 0x7ffff;
    return arm64_sign_extend((immhi << 2) | immlo, 21);
}

// Bitmask immediate decode (AArch64 "DecodeBitMasks", immN:imms:immr form).
// Adapted from OpenMinis/ish-arm64's asbestos/guest-arm64/gen.c:1018 (GPLv3,
// see /CREDITS-aarch64.md) -- moved here (was file-local to
// emu/arm64_interp.c) so gen_step_arm64 (jit/gen.c) can call the same
// proven-correct implementation at JIT-compile time instead of
// reimplementing it a second time -- the exact "one implementation, not two
// chances to get it wrong" lesson the branch-offset sign-extension bug
// (aarch64_guest_plan.md's Phase B writeup) already taught this port.
static inline bool arm64_decode_bitmask_imm(uint32_t n, uint32_t imms, uint32_t immr, bool is64, uint64_t *result) {
    uint32_t len = 0;
    uint32_t combined = (n << 6) | (~imms & 0x3f);
    bool found = false;
    for (int i = 6; i >= 0; i--) {
        if (combined & (1u << i)) {
            len = (uint32_t) i;
            found = true;
            break;
        }
    }
    if (!found || len < 1)
        return false;

    uint32_t size = 1u << len;
    uint32_t s = imms & (size - 1);
    uint32_t r = immr & (size - 1);
    if (s == size - 1)
        return false;

    uint64_t pattern = (1ULL << (s + 1)) - 1;
    if (r > 0) {
        pattern = (pattern >> r) | (pattern << (size - r));
        pattern &= (size < 64) ? ((1ULL << size) - 1) : ~0ULL;
    }

    *result = 0;
    for (uint32_t i = 0; i < 64; i += size)
        *result |= pattern << i;
    if (!is64)
        *result &= 0xffffffffu;
    return true;
}

#endif
