// AArch64 guest interpreter. Per aarch64_guest_plan.md patch 3: a bounded
// but real instruction set — enough for a hand-assembled function prologue/
// epilogue, a PLT-style ADRP+load+branch sequence, LDXR/STXR/CAS atomic
// update loops, and SVC entry — mirroring emu/amd64_interp.c's structure
// (interpreter first, JIT later) and I/O conventions (tlb_read/tlb_write,
// segfault_addr on fault, INT_* return codes).
//
// Bit-field masks for the trickier encodings (load/store exclusive, CAS,
// LDP/STP indexing modes) are adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gen.c, a GPLv3 fork of ish-app/ish — see
// /CREDITS-aarch64.md. That file is a JIT code generator (decode + gadget
// selection fused together); the actual arithmetic/flag semantics here are
// original, since a same-architecture JIT has no portable-C expression of
// "what ADD means" to adapt — the real hardware executes it. Only the
// decode.h helpers (kernel/abi.h-adjacent bit extraction, condition-code
// evaluation) and the specific mask/field layouts cited in comments below
// are adapted; the interpreter dispatch loop, register/memory helpers, and
// flag computation are written against this codebase's own conventions.
//
// Deliberately NOT implemented in this patch (all follow-up work):
//   - Logical (immediate) AND/ORR/EOR/ANDS — needs the ARM "DecodeBitMasks"
//     bitmask-immediate algorithm, substantial enough to warrant its own
//     pass rather than being rushed into this one.
//   - Data-processing (register): logical-shifted-register, add/subtract
//     shifted/extended register, conditional select, 1-/2-source (MUL,
//     UDIV/SDIV, CLZ, etc). Function prologues in practice route around
//     needing these (MOV Xd,SP is ADD-immediate with imm=0; simple
//     register moves can be avoided in hand-written test code).
//   - STLR/LDAR (non-exclusive acquire/release), LSE atomic RMW
//     (LDADD/LDCLR/etc, distinct from CAS). LDXP/STXP (pair exclusives)
//     ARE implemented now, via the arm64_ldxp/arm64_stxp helpers shared
//     with the JIT gadgets.
//   - LDR (literal, PC-relative), sign-extending LDRSB/LDRSH/LDRSW.
//   - All SIMD/FP instructions.
// Anything not decoded below raises INT_UNDEFINED, matching how i386/amd64
// handle unimplemented opcodes — no silent misexecution.

#include "misc.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "emu/arch/arm64/decode.h"
#include "kernel/abi.h"

// ---- Register access -------------------------------------------------
//
// AArch64's register field 31 is context-dependent: in most data-processing
// instructions it means XZR (reads as zero, writes discarded); in the
// immediate add/subtract family and in load/store base-register position it
// means SP. Each decode site below picks the right accessor explicitly
// rather than trying to infer it generically.

static inline uint64_t arm64_reg_get_zr(const struct cpu_state *cpu, unsigned n) {
    if (n == 31)
        return 0;
    return cpu->arm64_regs[n];
}

static inline uint64_t arm64_reg_get_sp(const struct cpu_state *cpu, unsigned n) {
    if (n == 31)
        return cpu->arm64_sp;
    return cpu->arm64_regs[n];
}

// sf: true = 64-bit (Xn), false = 32-bit (Wn, zero-extended on read here
// only insofar as the stored value is always the full 64-bit register —
// callers that want the 32-bit view mask the result themselves).
static inline uint64_t arm64_reg_get(const struct cpu_state *cpu, unsigned n, bool sf) {
    uint64_t v = arm64_reg_get_zr(cpu, n);
    return sf ? v : (v & 0xffffffffu);
}

// Writing Wd (sf=false) zero-extends into the full Xd register — an
// architectural rule, not an implementation choice (ARM ARM, "Register
// arguments" preamble to A64 instruction descriptions).
static inline void arm64_reg_set_discard(struct cpu_state *cpu, unsigned n, bool sf, uint64_t val) {
    if (n == 31)
        return; // XZR: write discarded
    cpu->arm64_regs[n] = sf ? val : (val & 0xffffffffu);
}

static inline void arm64_reg_set_sp(struct cpu_state *cpu, unsigned n, bool sf, uint64_t val) {
    uint64_t masked = sf ? val : (val & 0xffffffffu);
    if (n == 31)
        cpu->arm64_sp = masked;
    else
        cpu->arm64_regs[n] = masked;
}

// ---- Memory access -----------------------------------------------------
// Mirrors amd64_mem_read/amd64_mem_write's fault-reporting contract
// (emu/amd64_interp.c:3488-3529): validate the address range for this ABI,
// then tlb_read/tlb_write, recording segfault_addr/segfault_was_write and
// returning false on either failure so the caller can return INT_PF.

static inline bool arm64_mem_read(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, void *out, unsigned size) {
    if (!guest_abi_range_valid(GUEST_ABI_ARM64, addr, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = false;
        return false;
    }
    tlb->segfault_addr = 0;
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = tlb->segfault_addr != 0 ? tlb->segfault_addr : addr;
        cpu->segfault_was_write = false;
        return false;
    }
    return true;
}

static inline bool arm64_mem_write(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, const void *value, unsigned size) {
    if (!guest_abi_range_valid(GUEST_ABI_ARM64, addr, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = true;
        return false;
    }
    tlb->segfault_addr = 0;
    if (!tlb_write(tlb, addr, value, size)) {
        cpu->segfault_addr = tlb->segfault_addr != 0 ? tlb->segfault_addr : addr;
        cpu->segfault_was_write = true;
        return false;
    }
    return true;
}

// ---- Flag computation ---------------------------------------------------
// NZCV for ADD/SUB, computed generically over a 32- or 64-bit operand width.
// a, b, and result are the *unmasked* 64-bit values; width comes from sf.
// Packs directly into cpu->arm64_nzcv (bits 31:28) — see emu/cpu.h.

static inline void arm64_set_nzcv(struct cpu_state *cpu, bool n, bool z, bool c, bool v) {
    cpu->arm64_nzcv = ((dword_t) n << 31) | ((dword_t) z << 30) | ((dword_t) c << 29) | ((dword_t) v << 28);
}

static inline void arm64_set_flags_add(struct cpu_state *cpu, uint64_t a, uint64_t b, bool sf) {
    unsigned width = sf ? 64 : 32;
    uint64_t mask = sf ? UINT64_MAX : 0xffffffffu;
    uint64_t am = a & mask, bm = b & mask;
    uint64_t res = (am + bm) & mask;
    unsigned __int128 wide = (unsigned __int128) am + (unsigned __int128) bm;
    bool sa = (am >> (width - 1)) & 1, sb = (bm >> (width - 1)) & 1, sr = (res >> (width - 1)) & 1;
    arm64_set_nzcv(cpu,
        (res >> (width - 1)) & 1,
        res == 0,
        ((wide >> width) & 1) != 0,
        (sa == sb) && (sr != sa));
}

static inline void arm64_set_flags_sub(struct cpu_state *cpu, uint64_t a, uint64_t b, bool sf) {
    unsigned width = sf ? 64 : 32;
    uint64_t mask = sf ? UINT64_MAX : 0xffffffffu;
    uint64_t am = a & mask, bm = b & mask;
    uint64_t res = (am - bm) & mask;
    bool sa = (am >> (width - 1)) & 1, sb = (bm >> (width - 1)) & 1, sr = (res >> (width - 1)) & 1;
    arm64_set_nzcv(cpu,
        (res >> (width - 1)) & 1,
        res == 0,
        am >= bm, // SUB's carry is "NOT borrow": set when no borrow was needed
        (sa != sb) && (sr != sa));
}

// ---- Register shift application (LSL/LSR/ASR/ROR, shift_type 0-3) -----
// Shared by Logical(shifted-register) and Add/subtract(shifted-register).
// val is assumed already masked to `sf`'s width by the caller.
static uint64_t arm64_apply_shift(uint64_t val, unsigned shift_type, unsigned amount, bool sf) {
    unsigned width = sf ? 64 : 32;
    uint64_t wmask = sf ? UINT64_MAX : 0xffffffffu;
    if (amount == 0)
        return val;
    switch (shift_type) {
    case 0b00: // LSL
        return (val << amount) & wmask;
    case 0b01: // LSR
        return val >> amount;
    case 0b10: { // ASR
        int64_t sv = sf ? (int64_t) val : (int64_t) (int32_t) val;
        return ((uint64_t) (sv >> amount)) & wmask;
    }
    case 0b11: // ROR
        return ((val >> amount) | (val << (width - amount))) & wmask;
    default:
        return val;
    }
}

// arm64_decode_bitmask_imm() moved to emu/arch/arm64/decode.h so
// gen_step_arm64 (jit/gen.c) can share the same implementation at JIT-compile
// time instead of reimplementing it — see that header for the full comment.

// ---- Instruction execution ----------------------------------------------
// Returns INT_NONE on success, or an interrupt code (INT_UNDEFINED,
// INT_PF, INT_ARM64_SVC) to unwind to the caller. cpu->arm64_pc has already
// been advanced past the just-fetched instruction; branch/call handlers
// overwrite it explicitly.

static int arm64_execute(struct cpu_state *cpu, struct tlb *tlb, uint32_t insn, guest_addr_t insn_addr) {
    // ADR/ADRP — adapted mask (OpenMinis gen.c:1314): bits[28:24]=10000.
    if ((insn & 0x1f000000) == 0x10000000) {
        unsigned rd = ARM64_RD(insn);
        bool is_adrp = (insn >> 31) & 1;
        int64_t imm = arm64_adr_imm(insn);
        uint64_t target = is_adrp
            ? (insn_addr & ~(uint64_t) 0xfff) + ((uint64_t) imm << 12)
            : insn_addr + (uint64_t) imm;
        arm64_reg_set_discard(cpu, rd, true, target);
        return INT_NONE;
    }

    // Add/subtract (immediate) — adapted mask (OpenMinis gen.c:1372, flattened
    // to a standalone bits[28:24]=10001 test — see this file's header comment
    // for why the two-stage form in their code is equivalent).
    if ((insn & 0x1f000000) == 0x11000000) {
        bool sf = ARM64_SF(insn);
        bool op_sub = (insn >> 30) & 1;
        bool S = (insn >> 29) & 1;
        bool sh = (insn >> 22) & 1;
        uint64_t imm12 = (insn >> 10) & 0xfff;
        unsigned rn = ARM64_RN(insn);
        unsigned rd = ARM64_RD(insn);
        uint64_t value = imm12 << (sh ? 12 : 0);
        uint64_t base = arm64_reg_get_sp(cpu, rn);
        uint64_t result = op_sub ? (base - value) : (base + value);
        if (S) {
            if (op_sub)
                arm64_set_flags_sub(cpu, base, value, sf);
            else
                arm64_set_flags_add(cpu, base, value, sf);
            // Flag-setting variant: Rd=31 is the CMP/CMN alias (discard to ZR).
            arm64_reg_set_discard(cpu, rd, sf, result);
        } else {
            // Non-flag-setting variant: Rd=31 is a real destination (SP).
            arm64_reg_set_sp(cpu, rd, sf, result);
        }
        return INT_NONE;
    }

    // Logical (immediate): AND/ORR/EOR/ANDS — mask derived to match
    // bits[28:23]=100100 (adjacent to, but distinct from, MOVN/MOVZ/MOVK's
    // 100101 below — the two differ only in bit 23). Field layout per
    // OpenMinis gen.c:1469-1476; the ORR-with-Rn=XZR case is the "MOV
    // (bitmask immediate)" alias, handled for free since arm64_reg_get_zr
    // already returns 0 for Rn=31 — no special-casing needed. Added after
    // patch 5 testing showed this is the very first instruction musl's
    // dynamic linker executes, not a rarely-hit case.
    if ((insn & 0x1f800000) == 0x12000000) {
        bool sf = ARM64_SF(insn);
        unsigned opc = (insn >> 29) & 0x3;
        unsigned N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3f;
        unsigned imms = (insn >> 10) & 0x3f;
        unsigned rn = ARM64_RN(insn);
        unsigned rd = ARM64_RD(insn);
        if (!sf && N != 0)
            return INT_UNDEFINED; // N=1 with sf=0 is unallocated
        uint64_t imm;
        if (!arm64_decode_bitmask_imm(N, imms, immr, sf, &imm))
            return INT_UNDEFINED;
        uint64_t src = arm64_reg_get(cpu, rn, sf);
        uint64_t result;
        switch (opc) {
        case 0b00: result = src & imm; break;  // AND
        case 0b01: result = src | imm; break;  // ORR (Rn=31 -> MOV alias)
        case 0b10: result = src ^ imm; break;  // EOR
        case 0b11:                              // ANDS
            result = src & imm;
            arm64_set_nzcv(cpu,
                (result >> (sf ? 63 : 31)) & 1,
                sf ? result == 0 : (result & 0xffffffffu) == 0,
                false, false); // ANDS always clears C and V
            break;
        default:
            return INT_UNDEFINED;
        }
        // Rd=31 means SP for the non-flag-setting forms (AND/ORR/EOR,
        // opc != 0b11) -- same SP-capable destination as ADD/SUB immediate
        // just above. Only ANDS (opc==0b11) can't target SP: its Rd=31
        // form is the TST alias and genuinely discards the result. Found
        // via a real on-device crash: LuaJIT's `and sp, x1, #mask` stack-
        // realignment (arm64 GC64 coroutine/call-return path) was
        // silently a no-op here, leaving SP stale so the next SP-relative
        // load read garbage -- manifested as a NaN-boxed double or a GC
        // string's raw bytes ending up dereferenced as a pointer several
        // instructions later.
        if (opc == 0b11)
            arm64_reg_set_discard(cpu, rd, sf, result);
        else
            arm64_reg_set_sp(cpu, rd, sf, result);
        return INT_NONE;
    }

    // Move wide (immediate): MOVN/MOVZ/MOVK — adapted mask (OpenMinis
    // gen.c:1566): bits[28:23]=100101.
    if ((insn & 0x1f800000) == 0x12800000) {
        bool sf = ARM64_SF(insn);
        unsigned opc = (insn >> 29) & 0x3;
        unsigned hw = (insn >> 21) & 0x3;
        uint64_t imm16 = (insn >> 5) & 0xffff;
        unsigned rd = ARM64_RD(insn);
        if (!sf && hw >= 2)
            return INT_UNDEFINED; // hw=2,3 only valid for 64-bit
        unsigned shift = hw * 16;
        switch (opc) {
        case 0b00: // MOVN
            arm64_reg_set_discard(cpu, rd, sf, ~(imm16 << shift));
            break;
        case 0b10: // MOVZ
            arm64_reg_set_discard(cpu, rd, sf, imm16 << shift);
            break;
        case 0b11: { // MOVK — replace one 16-bit field, keep the rest
            uint64_t cur = arm64_reg_get(cpu, rd, sf);
            uint64_t field_mask = (uint64_t) 0xffff << shift;
            arm64_reg_set_discard(cpu, rd, sf, (cur & ~field_mask) | (imm16 << shift));
            break;
        }
        default:
            return INT_UNDEFINED; // opc=01 is unallocated
        }
        return INT_NONE;
    }

    // B, BL — adapted mask (OpenMinis gen.c:1726): bits[30:26]=00101.
    if ((insn & 0x7c000000) == 0x14000000) {
        bool is_bl = (insn >> 31) & 1;
        int64_t offset = arm64_branch_imm26(insn);
        if (is_bl)
            arm64_reg_set_discard(cpu, arm64_x30, true, cpu->arm64_pc);
        cpu->arm64_pc = insn_addr + (uint64_t) offset;
        return INT_NONE;
    }

    // B.cond — adapted mask (OpenMinis gen.c:1765): bits[31:25]=0101010, bit4=0.
    if ((insn & 0xff000010) == 0x54000000) {
        enum arm64_cond cond = insn & 0xf;
        if (arm64_cond_check(cpu, cond))
            cpu->arm64_pc = insn_addr + (uint64_t) arm64_branch_imm19(insn);
        return INT_NONE;
    }

    // CBZ, CBNZ — adapted mask (OpenMinis gen.c:1815): bits[30:25]=011010.
    if ((insn & 0x7e000000) == 0x34000000) {
        bool sf = ARM64_SF(insn);
        bool is_cbnz = (insn >> 24) & 1;
        unsigned rt = insn & 0x1f;
        uint64_t val = arm64_reg_get(cpu, rt, sf);
        if ((val == 0) != is_cbnz)
            cpu->arm64_pc = insn_addr + (uint64_t) arm64_branch_imm19(insn);
        return INT_NONE;
    }

    // TBZ, TBNZ — adapted mask (OpenMinis gen.c:1850): bits[30:25]=011011.
    if ((insn & 0x7e000000) == 0x36000000) {
        bool is_tbnz = (insn >> 24) & 1;
        unsigned b5 = (insn >> 31) & 1;
        unsigned b40 = (insn >> 19) & 0x1f;
        unsigned bit_pos = (b5 << 5) | b40;
        unsigned rt = insn & 0x1f;
        uint64_t val = arm64_reg_get_zr(cpu, rt);
        bool bit_set = (val >> bit_pos) & 1;
        if (bit_set == is_tbnz)
            cpu->arm64_pc = insn_addr + (uint64_t) arm64_branch_imm14(insn);
        return INT_NONE;
    }

    // BR, BLR, RET — adapted mask (OpenMinis gen.c:1879): bits[31:25]=1101011.
    if ((insn & 0xfe000000) == 0xd6000000) {
        unsigned opc = (insn >> 21) & 0xf;
        unsigned rn = (insn >> 5) & 0x1f;
        uint64_t target = arm64_reg_get_zr(cpu, rn);
        switch (opc) {
        case 0: // BR
            cpu->arm64_pc = target;
            break;
        case 1: // BLR
            arm64_reg_set_discard(cpu, arm64_x30, true, cpu->arm64_pc);
            cpu->arm64_pc = target;
            break;
        case 2: // RET
            cpu->arm64_pc = target;
            break;
        default:
            return INT_UNDEFINED;
        }
        return INT_NONE;
    }

    // SVC — adapted mask (OpenMinis gen.c:1710): fixed encoding 0xd4000001
    // (imm16 must be 0 for the Linux syscall convention; other values are
    // unallocated/reserved here, not decoded).
    if ((insn & 0xffe0001f) == 0xd4000001) {
        // Not wired to a syscall table yet (patch 4) — see emu/interrupt.h.
        return INT_ARM64_SVC;
    }

    // Load/store register (unsigned immediate), GPR only (V=0) — adapted
    // mask (OpenMinis gen.c:2218): bits[29:24]=111001 with bit26(V)=0 folded
    // into the mask/value pair below (0x3b000000/0x39000000 already forces
    // V=0; V=1 would match 0x3d000000, a different value).
    if ((insn & 0x3b000000) == 0x39000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned opc = (insn >> 22) & 0x3;
        uint64_t imm12 = (insn >> 10) & 0xfff;
        unsigned rn = ARM64_RN(insn);
        unsigned rt = ARM64_RT(insn);
        if (opc > 1)
            return INT_UNDEFINED; // sign-extending loads (LDRSB/H/W) not yet implemented
        bool is_load = opc == 1;
        unsigned bytes = 1u << size;
        uint64_t offset = imm12 << size;
        uint64_t addr = arm64_reg_get_sp(cpu, rn) + offset;
        if (is_load) {
            uint64_t val = 0;
            if (!arm64_mem_read(cpu, tlb, addr, &val, bytes))
                return INT_PF;
            arm64_reg_set_discard(cpu, rt, size == 3, val);
        } else {
            uint64_t val = arm64_reg_get_zr(cpu, rt);
            if (!arm64_mem_write(cpu, tlb, addr, &val, bytes))
                return INT_PF;
        }
        return INT_NONE;
    }

    // Load/store pair (LDP/STP), GPR only (V=0) — adapted mask (OpenMinis
    // gen.c:2516): bits[29:25]=10100, V=bit26=0.
    if ((insn & 0x3a000000) == 0x28000000 && ((insn >> 26) & 1) == 0) {
        unsigned opc = (insn >> 30) & 0x3;
        unsigned mode = (insn >> 23) & 0x7; // 1=post, 2=offset, 3=pre
        bool is_load = (insn >> 22) & 1;
        int64_t imm7 = (insn >> 15) & 0x7f;
        if (imm7 & 0x40)
            imm7 |= ~(int64_t) 0x7f; // sign-extend 7 bits
        unsigned rt2 = ARM64_RT2(insn);
        unsigned rn = ARM64_RN(insn);
        unsigned rt = ARM64_RT(insn);
        if (opc != 0b00 && opc != 0b10)
            return INT_UNDEFINED; // opc=01 unallocated, opc=11 is LDPSW (not yet implemented)
        bool sf = opc == 0b10;
        unsigned bytes = sf ? 8 : 4;
        int64_t offset = imm7 * bytes;
        if (mode != 1 && mode != 2 && mode != 3)
            return INT_UNDEFINED;
        uint64_t base = arm64_reg_get_sp(cpu, rn);
        uint64_t addr = (mode == 1) ? base : (uint64_t) ((int64_t) base + offset);
        if (is_load) {
            uint64_t v1 = 0, v2 = 0;
            if (!arm64_mem_read(cpu, tlb, addr, &v1, bytes))
                return INT_PF;
            if (!arm64_mem_read(cpu, tlb, addr + bytes, &v2, bytes))
                return INT_PF;
            arm64_reg_set_discard(cpu, rt, sf, v1);
            arm64_reg_set_discard(cpu, rt2, sf, v2);
        } else {
            uint64_t v1 = arm64_reg_get_zr(cpu, rt);
            uint64_t v2 = arm64_reg_get_zr(cpu, rt2);
            if (!arm64_mem_write(cpu, tlb, addr, &v1, bytes))
                return INT_PF;
            if (!arm64_mem_write(cpu, tlb, addr + bytes, &v2, bytes))
                return INT_PF;
        }
        if (mode == 1 || mode == 3) // post- or pre-index: writeback
            arm64_reg_set_sp(cpu, rn, true, (uint64_t) ((int64_t) base + offset));
        return INT_NONE;
    }

    // CAS (LSE atomic compare-and-swap) — adapted mask and field layout
    // (OpenMinis gen.c:2134): size:001000:1:A:1:Rs:R:11111:Rn:Rt. Acquire/
    // release (A/R) barrier semantics are not modeled — see this file's
    // header comment. MUST be checked before the load/store-exclusive
    // family below: CAS's encoding (bits 29:24 = 001000, same top-level
    // "atomic" group) also matches that check's broader 0x3f000000 mask —
    // OpenMinis' gen.c checks CAS first (line 2134) for exactly this
    // reason (their load/store-exclusive check is at line 2942, later in
    // the same file); getting this ordering backwards was an actual bug
    // caught by running a real CAS instruction through this interpreter
    // (it fell into the load/store-exclusive handler's `o2 != 0` reject
    // path and raised INT_UNDEFINED instead of executing the CAS).
    // The mask pins bit23=1 per the field layout documented above —
    // bit23=0 in this slot is CASP (compare-and-swap PAIR), handled just
    // below; the old mask left bit23 free and executed CASP as a
    // byte/halfword single-register CAS, silently corrupting memory
    // (same bug, same fix as gen_step_arm64's).
    if ((insn & 0x3fa00c00) == 0x08a00c00) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned rs = (insn >> 16) & 0x1f; // expected value in, old value out
        unsigned rn = ARM64_RN(insn);
        unsigned rt = ARM64_RT(insn); // new value
        unsigned bytes = 1u << size;
        uint64_t addr = arm64_reg_get_sp(cpu, rn);
        uint64_t cur = 0;
        if (!arm64_mem_read(cpu, tlb, addr, &cur, bytes))
            return INT_PF;
        uint64_t mask = bytes == 8 ? UINT64_MAX : ((uint64_t) 1 << (bytes * 8)) - 1;
        uint64_t expected = arm64_reg_get_zr(cpu, rs) & mask;
        if (cur == expected) {
            uint64_t newval = arm64_reg_get_zr(cpu, rt);
            if (!arm64_mem_write(cpu, tlb, addr, &newval, bytes))
                return INT_PF;
        }
        arm64_reg_set_discard(cpu, rs, size == 3, cur); // CAS always writes old value back
        return INT_NONE;
    }

    // CASP/CASPA/CASPL/CASPAL (LSE compare-and-swap pair):
    // 0:sz:001000:0:L:1:Rs:o0:11111:Rn:Rt — bit23=0 vs. CAS's 1, bit31=0
    // vs. LDXP/STXP's 1, bit30 picks a 32- or 64-bit register pair.
    // Same must-precede-the-exclusive-family ordering constraint as CAS.
    // Implemented via the arm64_casp helper shared with the JIT gadgets
    // (emu/tlb.c), which is host-atomic and enforces the 2*sz alignment
    // requirement. Rs/Rt must be even and Rt2 must be 11111 (ARM ARM).
    if ((insn & 0xbfa00000) == 0x08200000) {
        unsigned sz = ((insn >> 30) & 1) ? 8 : 4;
        unsigned rs = (insn >> 16) & 0x1f; // expected pair in, old pair out
        unsigned rt2 = (insn >> 10) & 0x1f;
        unsigned rn = ARM64_RN(insn);
        unsigned rt = ARM64_RT(insn); // new (desired) pair
        if (rt2 != 0x1f || (rs & 1) || (rt & 1))
            return INT_UNDEFINED;
        uint64_t addr = arm64_reg_get_sp(cpu, rn);
        // Rs/Rt are even, so never 31; Rs+1/Rt+1 are 31 only for Rs/Rt=30,
        // where register 31 reads as XZR / discards the write.
        uint64_t expected[2] = { arm64_reg_get_zr(cpu, rs),
                                 arm64_reg_get_zr(cpu, rs + 1) };
        uint64_t desired[2] = { arm64_reg_get_zr(cpu, rt),
                                arm64_reg_get_zr(cpu, rt + 1) };
        uint64_t old[2];
        uint32_t swapped;
        int err = arm64_casp(cpu, tlb, addr, sz, expected, desired, old, &swapped);
        if (err != 0)
            return err;
        arm64_reg_set_discard(cpu, rs, sz == 8, old[0]);
        arm64_reg_set_discard(cpu, rs + 1, sz == 8, old[1]);
        return INT_NONE;
    }

    // Load/store exclusive register (LDXR/STXR) and pair (LDXP/STXP) —
    // adapted mask and field layout (OpenMinis gen.c:2942):
    // size:001000:o2:L:o1:Rs:o0:Rt2:Rn:Rt. The non-exclusive
    // acquire/release family (o2=1: STLR/LDAR/etc) is not implemented
    // here — see this file's header comment.
    if ((insn & 0x3f000000) == 0x08000000) {
        unsigned size = (insn >> 30) & 0x3;
        unsigned o2 = (insn >> 23) & 1;
        unsigned L = (insn >> 22) & 1;
        unsigned o1 = (insn >> 21) & 1;
        unsigned rs = (insn >> 16) & 0x1f;
        unsigned rn = ARM64_RN(insn);
        unsigned rt = ARM64_RT(insn);
        if (o2 != 0)
            return INT_UNDEFINED; // STLR-family: not yet implemented
        if (o1 != 0) {
            // LDXP/STXP/LDAXP/STLXP: bit31 must be 1 (the bit31=0 half of
            // this slot is CASP, consumed above); sz (bit30) picks a
            // 32- or 64-bit register pair. Same shared-helper
            // implementation as the JIT gadgets (emu/tlb.c); o0
            // (acquire/release) accepted and ignored like the rest of
            // the family. Rt/Rt2/Rs are independent (31 = XZR/discard).
            if (!(insn >> 31))
                return INT_UNDEFINED;
            unsigned rt2 = (insn >> 10) & 0x1f;
            unsigned sz = (size & 1) ? 8 : 4;
            uint64_t addr = arm64_reg_get_sp(cpu, rn);
            if (L) {
                uint64_t val[2];
                int err = arm64_ldxp(cpu, tlb, addr, sz, val);
                if (err != 0)
                    return err;
                arm64_reg_set_discard(cpu, rt, sz == 8, val[0]);
                arm64_reg_set_discard(cpu, rt2, sz == 8, val[1]);
            } else {
                uint32_t status;
                int err = arm64_stxp(cpu, tlb, addr, sz,
                                     arm64_reg_get_zr(cpu, rt),
                                     arm64_reg_get_zr(cpu, rt2), &status);
                if (err != 0)
                    return err;
                arm64_reg_set_discard(cpu, rs, false, status);
            }
            return INT_NONE;
        }
        unsigned bytes = 1u << size;
        uint64_t addr = arm64_reg_get_sp(cpu, rn);
        if (L) {
            // LDXR: record the address and the value read for the local
            // exclusive monitor. Single-guest-thread-step semantics only —
            // see the header comment on struct cpu_state's arm64_excl_*
            // fields regarding real cross-thread atomicity, which is a
            // differential-testing follow-up, not covered by this patch.
            uint64_t val = 0;
            if (!arm64_mem_read(cpu, tlb, addr, &val, bytes))
                return INT_PF;
            arm64_reg_set_discard(cpu, rt, size == 3, val);
            cpu->arm64_excl_addr = addr;
            cpu->arm64_excl_val = val;
        } else {
            // STXR: succeeds only if the monitor is still valid for this
            // address and the memory value hasn't changed since the LDXR.
            // Rs receives the status (0 = success, 1 = failure) per the
            // architecture; the store itself uses the low `bytes` of Rt.
            bool ok = false;
            if (cpu->arm64_excl_addr == addr) {
                uint64_t cur = 0;
                if (!arm64_mem_read(cpu, tlb, addr, &cur, bytes))
                    return INT_PF;
                uint64_t mask = bytes == 8 ? UINT64_MAX : ((uint64_t) 1 << (bytes * 8)) - 1;
                if ((cur & mask) == (cpu->arm64_excl_val & mask)) {
                    uint64_t val = arm64_reg_get_zr(cpu, rt);
                    if (!arm64_mem_write(cpu, tlb, addr, &val, bytes))
                        return INT_PF;
                    ok = true;
                }
            }
            cpu->arm64_excl_addr = UINT64_MAX; // monitor clears on any STXR, success or not
            arm64_reg_set_discard(cpu, rs, false, ok ? 0 : 1);
        }
        return INT_NONE;
    }

    return INT_UNDEFINED;
}

static inline int arm64_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    guest_addr_t insn_addr = cpu->arm64_pc;
    uint32_t insn;
    if (!arm64_read_insn(&cpu->arm64_pc, tlb, &insn)) {
        cpu->arm64_pc = insn_addr;
        cpu->segfault_addr = insn_addr;
        cpu->segfault_was_write = false;
        // Mapped but not executable: SEGV_ACCERR, not a retried read.
        return tlb->fetch_denied ? INT_PF_EXEC : INT_PF;
    }
    return arm64_execute(cpu, tlb, insn, insn_addr);
}

// Mirrors cpu_run_to_interrupt_amd64's driver loop (emu/amd64_interp.c:13663)
// minus its extensive per-binary debug tracing, which arm64 has no
// equivalent of yet (nothing to trace against until later patches land).
int cpu_run_to_interrupt_arm64(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);

    int steps = 0;
    while (true) {
        int interrupt = arm64_step_to_interrupt(cpu, tlb);
        if (interrupt == INT_NONE && __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++steps >= 1024) {
            steps = 0;
            interrupt = INT_TIMER;
        }
        if (interrupt != INT_NONE) {
            cpu->trapno = interrupt;
            return interrupt;
        }
    }
}
