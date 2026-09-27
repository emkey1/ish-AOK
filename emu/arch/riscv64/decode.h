#ifndef EMU_ARCH_RISCV64_DECODE_H
#define EMU_ARCH_RISCV64_DECODE_H

// RV64GC decode helpers: pure functions over instruction words, no state.
// Shared by the JIT generator (jit/gen.c, docs/historical/riscv64_guest_plan.md patch 5).
// Style follows emu/arch/arm64/decode.h.
//
// The C (compressed) extension is handled by expansion: every 16-bit
// encoding maps to a defined 32-bit equivalent (riscv64_expand_rvc), so
// the rest of the decoder only ever sees 32-bit words. The immediate
// reassemblers below are THE classic riscv decode bug surface (B/J bit
// scrambles); they are verified by exact-word comparison against llvm-mc
// in tests/riscv64/decode_check (see that harness before touching these).

#include <stdint.h>
#include <stdbool.h>

// ---- major opcodes (bits [6:0] of a 32-bit instruction) ----
#define RISCV64_OP_LOAD      0x03
#define RISCV64_OP_LOAD_FP   0x07
#define RISCV64_OP_CUSTOM0   0x0b // vendor/user extension space (plan 5b)
#define RISCV64_OP_MISC_MEM  0x0f // fence, fence.i
#define RISCV64_OP_OP_IMM    0x13
#define RISCV64_OP_AUIPC     0x17
#define RISCV64_OP_OP_IMM_32 0x1b
#define RISCV64_OP_STORE     0x23
#define RISCV64_OP_STORE_FP  0x27
#define RISCV64_OP_CUSTOM1   0x2b // vendor/user extension space
#define RISCV64_OP_AMO       0x2f
#define RISCV64_OP_OP        0x33
#define RISCV64_OP_LUI       0x37
#define RISCV64_OP_OP_32     0x3b
#define RISCV64_OP_MADD      0x43
#define RISCV64_OP_MSUB      0x47
#define RISCV64_OP_NMSUB     0x4b
#define RISCV64_OP_NMADD     0x4f
#define RISCV64_OP_OP_FP     0x53
#define RISCV64_OP_CUSTOM2   0x5b // vendor/user extension space
#define RISCV64_OP_BRANCH    0x63
#define RISCV64_OP_JALR      0x67
#define RISCV64_OP_JAL       0x6f
#define RISCV64_OP_SYSTEM    0x73 // ecall, ebreak, csr*
#define RISCV64_OP_CUSTOM3   0x7b // vendor/user extension space

// ---- field extractors ----
static inline unsigned riscv64_opcode(uint32_t insn) { return insn & 0x7f; }
static inline unsigned riscv64_rd(uint32_t insn)     { return (insn >> 7) & 0x1f; }
static inline unsigned riscv64_funct3(uint32_t insn) { return (insn >> 12) & 0x7; }
static inline unsigned riscv64_rs1(uint32_t insn)    { return (insn >> 15) & 0x1f; }
static inline unsigned riscv64_rs2(uint32_t insn)    { return (insn >> 20) & 0x1f; }
static inline unsigned riscv64_funct7(uint32_t insn) { return (insn >> 25) & 0x7f; }

static inline int64_t riscv64_sext(uint64_t value, unsigned bits) {
    return (int64_t) (value << (64 - bits)) >> (64 - bits);
}

// ---- immediate reassembly, one function per instruction format ----
// I-type: imm[11:0] = insn[31:20]
static inline int64_t riscv64_imm_i(uint32_t insn) {
    return riscv64_sext(insn >> 20, 12);
}
// S-type: imm[11:5|4:0] = insn[31:25|11:7]
static inline int64_t riscv64_imm_s(uint32_t insn) {
    uint64_t imm = ((uint64_t) (insn >> 25) << 5) | ((insn >> 7) & 0x1f);
    return riscv64_sext(imm, 12);
}
// B-type: imm[12|10:5|4:1|11] = insn[31|30:25|11:8|7], imm[0]=0
static inline int64_t riscv64_imm_b(uint32_t insn) {
    uint64_t imm = ((uint64_t) (insn >> 31) << 12)
                 | (((insn >> 7) & 1) << 11)
                 | (((insn >> 25) & 0x3f) << 5)
                 | (((insn >> 8) & 0xf) << 1);
    return riscv64_sext(imm, 13);
}
// U-type: imm[31:12] = insn[31:12], low 12 bits zero (sign-extended to 64)
static inline int64_t riscv64_imm_u(uint32_t insn) {
    return (int64_t) (int32_t) (insn & 0xfffff000u);
}
// J-type: imm[20|10:1|11|19:12] = insn[31|30:21|20|19:12], imm[0]=0
static inline int64_t riscv64_imm_j(uint32_t insn) {
    uint64_t imm = ((uint64_t) (insn >> 31) << 20)
                 | (((insn >> 12) & 0xff) << 12)
                 | (((insn >> 20) & 1) << 11)
                 | (((insn >> 21) & 0x3ff) << 1);
    return riscv64_sext(imm, 21);
}

// ---- instruction length ----
// RV64GC only has 16-bit (bits [1:0] != 11) and 32-bit ([1:0] == 11 and
// [4:2] != 111) encodings; longer formats are reserved and can't appear in
// RV64GC binaries, so callers may treat !=2 as 4.
static inline unsigned riscv64_insn_length(uint16_t low16) {
    return (low16 & 3) != 3 ? 2 : 4;
}

// ---- 32-bit encoders (used by the RVC expander; also handy in tests) ----
static inline uint32_t riscv64_enc_r(unsigned opcode, unsigned rd, unsigned funct3,
                                     unsigned rs1, unsigned rs2, unsigned funct7) {
    return opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20)
        | ((uint32_t) funct7 << 25);
}
static inline uint32_t riscv64_enc_i(unsigned opcode, unsigned rd, unsigned funct3,
                                     unsigned rs1, int64_t imm) {
    return opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15)
        | ((uint32_t) (imm & 0xfff) << 20);
}
static inline uint32_t riscv64_enc_s(unsigned opcode, unsigned funct3,
                                     unsigned rs1, unsigned rs2, int64_t imm) {
    return opcode | (((uint32_t) imm & 0x1f) << 7) | (funct3 << 12)
        | (rs1 << 15) | (rs2 << 20) | (((uint32_t) (imm >> 5) & 0x7f) << 25);
}
static inline uint32_t riscv64_enc_b(unsigned opcode, unsigned funct3,
                                     unsigned rs1, unsigned rs2, int64_t imm) {
    uint32_t i = (uint32_t) imm;
    return opcode
        | (((i >> 11) & 1) << 7)
        | (((i >> 1) & 0xf) << 8)
        | (funct3 << 12) | (rs1 << 15) | (rs2 << 20)
        | (((i >> 5) & 0x3f) << 25)
        | (((i >> 12) & 1) << 31);
}
static inline uint32_t riscv64_enc_u(unsigned opcode, unsigned rd, int64_t imm) {
    return opcode | (rd << 7) | ((uint32_t) imm & 0xfffff000u);
}
static inline uint32_t riscv64_enc_j(unsigned opcode, unsigned rd, int64_t imm) {
    uint32_t i = (uint32_t) imm;
    return opcode | (rd << 7)
        | (((i >> 12) & 0xff) << 12)
        | (((i >> 11) & 1) << 20)
        | (((i >> 1) & 0x3ff) << 21)
        | (((i >> 20) & 1) << 31);
}

// ---- RVC (compressed) expansion ----
// Expands a 16-bit RV64C encoding to its architecturally defined 32-bit
// equivalent. Returns 0 (a guaranteed-illegal 32-bit encoding: all-zero) for
// reserved/illegal compressed encodings, including the all-zeros pattern the
// spec defines as illegal. HINT encodings (e.g. C.ADDI with imm=0, C.LUI
// with rd=0) expand to their nop-equivalent 32-bit forms rather than
// faulting, matching hardware behavior.
//
// x8 + r3 register decompression for the prime (3-bit) register fields.
static inline unsigned riscv64_rvc_reg(unsigned r3) { return 8 + (r3 & 7); }

static inline uint32_t riscv64_expand_rvc(uint16_t c) {
    unsigned quadrant = c & 3;
    unsigned funct3 = (c >> 13) & 7;
    // full-width register fields (quadrants 1/2)
    unsigned rd = (c >> 7) & 0x1f;   // also rs1
    unsigned rs2 = (c >> 2) & 0x1f;
    // prime register fields (3-bit, x8-x15)
    unsigned rdp = riscv64_rvc_reg(c >> 2);
    unsigned rs1p = riscv64_rvc_reg(c >> 7);

    if (c == 0)
        return 0; // defined illegal

    switch (quadrant) {
    case 0:
        switch (funct3) {
        case 0: { // c.addi4spn -> addi rd', x2, nzuimm
            uint32_t imm = (((c >> 11) & 3) << 4)   // nzuimm[5:4] = c[12:11]
                         | (((c >> 7) & 0xf) << 6)  // nzuimm[9:6] = c[10:7]
                         | (((c >> 6) & 1) << 2)    // nzuimm[2]   = c[6]
                         | (((c >> 5) & 1) << 3);   // nzuimm[3]   = c[5]
            if (imm == 0)
                return 0; // reserved (covers the all-zeros case too)
            return riscv64_enc_i(RISCV64_OP_OP_IMM, rdp, 0, 2, imm);
        }
        case 1: { // c.fld -> fld rd', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD_FP, rdp, 3, rs1p, imm);
        }
        case 2: { // c.lw -> lw rd', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 6) & 1) << 2)
                         | (((c >> 5) & 1) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD, rdp, 2, rs1p, imm);
        }
        case 3: { // c.ld (RV64) -> ld rd', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD, rdp, 3, rs1p, imm);
        }
        case 5: { // c.fsd -> fsd rs2', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE_FP, 3, rs1p, rdp, imm);
        }
        case 6: { // c.sw -> sw rs2', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 6) & 1) << 2)
                         | (((c >> 5) & 1) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE, 2, rs1p, rdp, imm);
        }
        case 7: { // c.sd -> sd rs2', uimm(rs1')
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE, 3, rs1p, rdp, imm);
        }
        default:
            return 0; // funct3=4 reserved (c.lq is RV128)
        }

    case 1: {
        int64_t imm6 = riscv64_sext((((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f), 6);
        switch (funct3) {
        case 0: // c.addi -> addi rd, rd, imm (rd=0 or imm=0: HINT/NOP forms)
            return riscv64_enc_i(RISCV64_OP_OP_IMM, rd, 0, rd, imm6);
        case 1: // c.addiw (RV64) -> addiw rd, rd, imm; rd=0 reserved
            if (rd == 0)
                return 0;
            return riscv64_enc_i(RISCV64_OP_OP_IMM_32, rd, 0, rd, imm6);
        case 2: // c.li -> addi rd, x0, imm
            return riscv64_enc_i(RISCV64_OP_OP_IMM, rd, 0, 0, imm6);
        case 3:
            if (rd == 2) { // c.addi16sp -> addi x2, x2, nzimm
                uint64_t imm = (((uint64_t) (c >> 12) & 1) << 9)
                             | (((c >> 6) & 1) << 4)
                             | (((c >> 5) & 1) << 6)
                             | (((c >> 3) & 3) << 7)
                             | (((c >> 2) & 1) << 5);
                if (imm == 0)
                    return 0; // reserved
                return riscv64_enc_i(RISCV64_OP_OP_IMM, 2, 0, 2, riscv64_sext(imm, 10));
            } else { // c.lui -> lui rd, nzimm (rd=0: HINT, expand anyway)
                int64_t imm = riscv64_sext((((uint64_t) (c >> 12) & 1) << 17)
                                         | (((c >> 2) & 0x1f) << 12), 18);
                if (imm == 0)
                    return 0; // reserved
                return riscv64_enc_u(RISCV64_OP_LUI, rd, imm);
            }
        case 4: { // misc-alu on rs1'/rd'
            unsigned op2 = (c >> 10) & 3;
            unsigned shamt = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f);
            switch (op2) {
            case 0: // c.srli -> srli rd', rd', shamt
                return riscv64_enc_i(RISCV64_OP_OP_IMM, rs1p, 5, rs1p, shamt);
            case 1: // c.srai -> srai rd', rd', shamt (funct7 bit 0x20)
                return riscv64_enc_i(RISCV64_OP_OP_IMM, rs1p, 5, rs1p, shamt | 0x400);
            case 2: // c.andi -> andi rd', rd', imm (sign-extended)
                return riscv64_enc_i(RISCV64_OP_OP_IMM, rs1p, 7, rs1p, imm6);
            case 3: {
                unsigned op3 = (c >> 5) & 3;
                if (((c >> 12) & 1) == 0) {
                    static const struct { unsigned funct3, funct7; } ops[4] = {
                        {0, 0x20}, // c.sub -> sub
                        {4, 0},    // c.xor -> xor
                        {6, 0},    // c.or  -> or
                        {7, 0},    // c.and -> and
                    };
                    return riscv64_enc_r(RISCV64_OP_OP, rs1p, ops[op3].funct3,
                                         rs1p, rdp, ops[op3].funct7);
                } else {
                    if (op3 == 0) // c.subw -> subw
                        return riscv64_enc_r(RISCV64_OP_OP_32, rs1p, 0, rs1p, rdp, 0x20);
                    if (op3 == 1) // c.addw -> addw
                        return riscv64_enc_r(RISCV64_OP_OP_32, rs1p, 0, rs1p, rdp, 0);
                    return 0; // reserved
                }
            }
            }
            return 0; // unreachable
        }
        case 5: { // c.j -> jal x0, offset
            uint64_t imm = (((uint64_t) (c >> 12) & 1) << 11)
                         | (((c >> 11) & 1) << 4)
                         | (((c >> 9) & 3) << 8)
                         | (((c >> 8) & 1) << 10)
                         | (((c >> 7) & 1) << 6)
                         | (((c >> 6) & 1) << 7)
                         | (((c >> 3) & 7) << 1)
                         | (((c >> 2) & 1) << 5);
            return riscv64_enc_j(RISCV64_OP_JAL, 0, riscv64_sext(imm, 12));
        }
        case 6: case 7: { // c.beqz/c.bnez -> beq/bne rs1', x0, offset
            uint64_t imm = (((uint64_t) (c >> 12) & 1) << 8)
                         | (((c >> 10) & 3) << 3)
                         | (((c >> 5) & 3) << 6)
                         | (((c >> 3) & 3) << 1)
                         | (((c >> 2) & 1) << 5);
            return riscv64_enc_b(RISCV64_OP_BRANCH, funct3 == 6 ? 0 : 1,
                                 rs1p, 0, riscv64_sext(imm, 9));
        }
        }
        return 0; // unreachable
    }

    case 2:
        switch (funct3) {
        case 0: { // c.slli -> slli rd, rd, shamt (rd=0 or shamt=0: HINTs)
            unsigned shamt = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f);
            return riscv64_enc_i(RISCV64_OP_OP_IMM, rd, 1, rd, shamt);
        }
        case 1: { // c.fldsp -> fld rd, uimm(x2)
            uint32_t imm = (((c >> 12) & 1) << 5) | (((c >> 5) & 3) << 3)
                         | (((c >> 2) & 7) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD_FP, rd, 3, 2, imm);
        }
        case 2: { // c.lwsp -> lw rd, uimm(x2); rd=0 reserved
            if (rd == 0)
                return 0;
            uint32_t imm = (((c >> 12) & 1) << 5) | (((c >> 4) & 7) << 2)
                         | (((c >> 2) & 3) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD, rd, 2, 2, imm);
        }
        case 3: { // c.ldsp (RV64) -> ld rd, uimm(x2); rd=0 reserved
            if (rd == 0)
                return 0;
            uint32_t imm = (((c >> 12) & 1) << 5) | (((c >> 5) & 3) << 3)
                         | (((c >> 2) & 7) << 6);
            return riscv64_enc_i(RISCV64_OP_LOAD, rd, 3, 2, imm);
        }
        case 4:
            if (((c >> 12) & 1) == 0) {
                if (rs2 == 0) { // c.jr -> jalr x0, 0(rs1); rs1=0 reserved
                    if (rd == 0)
                        return 0;
                    return riscv64_enc_i(RISCV64_OP_JALR, 0, 0, rd, 0);
                }
                // c.mv -> add rd, x0, rs2 (rd=0: HINT)
                return riscv64_enc_r(RISCV64_OP_OP, rd, 0, 0, rs2, 0);
            } else {
                if (rs2 == 0) {
                    if (rd == 0) // c.ebreak -> ebreak
                        return riscv64_enc_i(RISCV64_OP_SYSTEM, 0, 0, 0, 1);
                    // c.jalr -> jalr x1, 0(rs1)
                    return riscv64_enc_i(RISCV64_OP_JALR, 1, 0, rd, 0);
                }
                // c.add -> add rd, rd, rs2 (rd=0: HINT)
                return riscv64_enc_r(RISCV64_OP_OP, rd, 0, rd, rs2, 0);
            }
        case 5: { // c.fsdsp -> fsd rs2, uimm(x2)
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 7) & 7) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE_FP, 3, 2, rs2, imm);
        }
        case 6: { // c.swsp -> sw rs2, uimm(x2)
            uint32_t imm = (((c >> 9) & 0xf) << 2) | (((c >> 7) & 3) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE, 2, 2, rs2, imm);
        }
        case 7: { // c.sdsp -> sd rs2, uimm(x2); uimm[8:6] is THREE bits
            // (c[9:7]), unlike c.swsp's two-bit uimm[7:6] just above
            uint32_t imm = (((c >> 10) & 7) << 3) | (((c >> 7) & 7) << 6);
            return riscv64_enc_s(RISCV64_OP_STORE, 3, 2, rs2, imm);
        }
        }
        return 0; // unreachable
    }
    return 0; // quadrant 3 is a 32-bit instruction; caller shouldn't get here
}

#endif
