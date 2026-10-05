#!/usr/bin/env python3
"""Generate exact-word test vectors for emu/arch/riscv64/decode.h.

Uses llvm-mc (Homebrew LLVM; Apple clang has no riscv backend) as the
oracle: each RVC instruction is assembled to its 16-bit word, its
architecturally defined expansion is assembled to its 32-bit word, and
decode_check.c asserts riscv64_expand_rvc(c) == expected word-for-word.
Immediate-extractor vectors are generated the same way: assemble an
instruction with a known immediate, assert the extractor recovers it.

Run manually when the corpus changes; decode_vectors.h is checked in so
the test needs no llvm at build time:
    python3 tests/riscv64/gen_decode_vectors.py > tests/riscv64/decode_vectors.h
"""

import subprocess
import sys

LLVM_MC = "/opt/homebrew/opt/llvm/bin/llvm-mc"

def assemble(text, want_len):
    out = subprocess.run(
        [LLVM_MC, "--triple=riscv64", "-mattr=+c,+d,+m,+zcb,+zba,+zbb,+zcmop", "--show-encoding"],
        input=f".option exact\n{text}\n", capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"llvm-mc failed on {text!r}:\n{out.stderr}")
    for line in out.stdout.splitlines():
        if "encoding:" in line:
            bytes_str = line.split("encoding:")[1].strip().strip("[]")
            data = [int(b, 16) for b in bytes_str.split(",")]
            if len(data) != want_len:
                sys.exit(f"{text!r}: expected {want_len} bytes, got {data}")
            word = 0
            for i, b in enumerate(data):
                word |= b << (8 * i)
            return word
    sys.exit(f"llvm-mc produced no encoding for {text!r}:\n{out.stdout}")

# (compressed asm, defined 32-bit expansion asm)
# Operand values chosen to exercise immediate sign/extreme bits.
RVC_PAIRS = [
    # quadrant 0
    ("c.addi4spn s0, sp, 4",    "addi s0, sp, 4"),
    ("c.addi4spn a5, sp, 1020", "addi a5, sp, 1020"),
    ("c.fld fs0, 0(a0)",        "fld fs0, 0(a0)"),
    ("c.fld fa5, 248(s1)",      "fld fa5, 248(s1)"),
    ("c.lw a0, 0(a1)",          "lw a0, 0(a1)"),
    ("c.lw s1, 124(a4)",        "lw s1, 124(a4)"),
    ("c.ld a2, 0(a3)",          "ld a2, 0(a3)"),
    ("c.ld s0, 248(s1)",        "ld s0, 248(s1)"),
    ("c.fsd fs1, 8(a2)",        "fsd fs1, 8(a2)"),
    ("c.sw a3, 4(a2)",          "sw a3, 4(a2)"),
    ("c.sw s0, 124(s1)",        "sw s0, 124(s1)"),
    ("c.sd a4, 16(a5)",         "sd a4, 16(a5)"),
    ("c.sd s1, 248(s0)",        "sd s1, 248(s0)"),
    # Zcb (quadrant 0, funct3 100: RV128's c.lq slot)
    ("c.lbu a0, 1(a1)",         "lbu a0, 1(a1)"),
    ("c.lbu s1, 2(a5)",         "lbu s1, 2(a5)"),
    ("c.lhu a2, 2(s0)",         "lhu a2, 2(s0)"),
    ("c.lh a3, 0(a4)",          "lh a3, 0(a4)"),
    ("c.lh s0, 2(a0)",          "lh s0, 2(a0)"),
    ("c.sb a0, 3(a1)",          "sb a0, 3(a1)"),
    ("c.sh a5, 2(s1)",          "sh a5, 2(s1)"),
    # quadrant 1
    ("c.addi a0, 1",            "addi a0, a0, 1"),
    ("c.addi s3, -32",          "addi s3, s3, -32"),
    ("c.addiw a1, -1",          "addiw a1, a1, -1"),
    ("c.addiw t0, 31",          "addiw t0, t0, 31"),
    ("c.li a2, -32",            "addi a2, zero, -32"),
    ("c.li t1, 31",             "addi t1, zero, 31"),
    ("c.addi16sp sp, 16",       "addi sp, sp, 16"),
    ("c.addi16sp sp, -512",     "addi sp, sp, -512"),
    ("c.addi16sp sp, 496",      "addi sp, sp, 496"),
    ("c.lui a3, 1",             "lui a3, 1"),
    ("c.lui s4, 0x1f",          "lui s4, 0x1f"),
    ("c.lui a4, 0xfffe0",       "lui a4, 0xfffe0"),
    # Zcmop: c.lui x(odd < 16), 0 is c.mop.n, which writes nothing
    ("c.mop.1",                 "addi zero, zero, 0"),
    ("c.mop.15",                "addi zero, zero, 0"),
    ("c.srli a0, 1",            "srli a0, a0, 1"),
    ("c.srli s1, 63",           "srli s1, s1, 63"),
    ("c.srai a1, 32",           "srai a1, a1, 32"),
    ("c.srai s0, 63",           "srai s0, s0, 63"),
    ("c.andi a2, -1",           "andi a2, a2, -1"),
    ("c.andi s1, 31",           "andi s1, s1, 31"),
    ("c.sub a0, a1",            "sub a0, a0, a1"),
    ("c.xor a2, a3",            "xor a2, a2, a3"),
    ("c.or a4, a5",             "or a4, a4, a5"),
    ("c.and s0, s1",            "and s0, s0, s1"),
    ("c.subw a0, a1",           "subw a0, a0, a1"),
    ("c.addw a2, a3",           "addw a2, a2, a3"),
    # Zcb (quadrant 1, the reserved c.subw/c.addw neighbours)
    ("c.mul s0, a1",            "mul s0, s0, a1"),
    ("c.zext.b a0",             "andi a0, a0, 255"),
    ("c.sext.b s1",             "sext.b s1, s1"),
    ("c.zext.h a2",             "zext.h a2, a2"),
    ("c.sext.h a3",             "sext.h a3, a3"),
    ("c.zext.w a4",             "add.uw a4, a4, zero"),
    ("c.not a5",                "xori a5, a5, -1"),
    ("c.j 2046",                "jal zero, 2046"),
    ("c.j -2048",               "jal zero, -2048"),
    ("c.j 16",                  "jal zero, 16"),
    ("c.beqz a0, 254",          "beq a0, zero, 254"),
    ("c.beqz s1, -256",         "beq s1, zero, -256"),
    ("c.bnez a5, 32",           "bne a5, zero, 32"),
    ("c.bnez s0, -2",           "bne s0, zero, -2"),
    # quadrant 2
    ("c.slli a0, 1",            "slli a0, a0, 1"),
    ("c.slli t6, 63",           "slli t6, t6, 63"),
    ("c.fldsp fa0, 0(sp)",      "fld fa0, 0(sp)"),
    ("c.fldsp fs11, 504(sp)",   "fld fs11, 504(sp)"),
    ("c.lwsp a0, 0(sp)",        "lw a0, 0(sp)"),
    ("c.lwsp t2, 252(sp)",      "lw t2, 252(sp)"),
    ("c.ldsp a1, 8(sp)",        "ld a1, 8(sp)"),
    ("c.ldsp s11, 504(sp)",     "ld s11, 504(sp)"),
    ("c.jr ra",                 "jalr zero, 0(ra)"),
    ("c.jr a5",                 "jalr zero, 0(a5)"),
    ("c.mv a0, a1",             "add a0, zero, a1"),
    ("c.mv t6, t5",             "add t6, zero, t5"),
    ("c.ebreak",                "ebreak"),
    ("c.jalr a0",               "jalr ra, 0(a0)"),
    ("c.jalr t1",               "jalr ra, 0(t1)"),
    ("c.add a0, a1",            "add a0, a0, a1"),
    ("c.add s11, t6",           "add s11, s11, t6"),
    ("c.fsdsp fs0, 0(sp)",      "fsd fs0, 0(sp)"),
    ("c.fsdsp fa7, 504(sp)",    "fsd fa7, 504(sp)"),
    ("c.swsp a0, 0(sp)",        "sw a0, 0(sp)"),
    ("c.swsp t0, 252(sp)",      "sw t0, 252(sp)"),
    ("c.sdsp a1, 8(sp)",        "sd a1, 8(sp)"),
    ("c.sdsp s2, 504(sp)",      "sd s2, 504(sp)"),
]

# (asm, format letter, expected immediate) for the imm reassemblers
IMM_VECTORS = [
    ("addi a0, a1, -2048", "i", -2048),
    ("addi a0, a1, 2047",  "i", 2047),
    ("addi a0, a1, -1",    "i", -1),
    ("lb t0, -7(t1)",      "i", -7),
    ("jalr ra, 100(a0)",   "i", 100),
    ("sd a2, -2048(a3)",   "s", -2048),
    ("sw a2, 2047(a3)",    "s", 2047),
    ("sb a2, -1(a3)",      "s", -1),
    ("sh a2, 0(a3)",       "s", 0),
    ("beq a0, a1, -4096",  "b", -4096),
    ("bne a0, a1, 4094",   "b", 4094),
    ("blt a0, a1, -2",     "b", -2),
    ("bgeu a0, a1, 16",    "b", 16),
    ("lui a0, 0xfffff",    "u", -4096),
    ("lui a0, 1",          "u", 4096),
    ("auipc a0, 0x80000",  "u", -2147483648),
    ("jal ra, -1048576",   "j", -1048576),
    ("jal ra, 1048574",    "j", 1048574),
    ("jal zero, -2",       "j", -2),
    ("jal ra, 2048",       "j", 2048),
]

def main():
    print("// GENERATED by gen_decode_vectors.py -- do not edit by hand.")
    print("// Oracle: llvm-mc --triple=riscv64 -mattr=+c,+d,+m,+zcb,+zba,+zbb,+zcmop --show-encoding")
    print()
    print("struct rvc_vector { uint16_t c; uint32_t expanded; const char *name; };")
    print("static const struct rvc_vector rvc_vectors[] = {")
    for c_asm, full_asm in RVC_PAIRS:
        c_word = assemble(c_asm, 2)
        full_word = assemble(full_asm, 4)
        print(f'    {{0x{c_word:04x}, 0x{full_word:08x}, "{c_asm}"}},')
    print("};")
    print()
    print("struct imm_vector { uint32_t insn; char format; int64_t imm; const char *name; };")
    print("static const struct imm_vector imm_vectors[] = {")
    for asm_text, fmt, imm in IMM_VECTORS:
        word = assemble(asm_text, 4)
        print(f'    {{0x{word:08x}, \'{fmt}\', {imm}LL, "{asm_text}"}},')
    print("};")

if __name__ == "__main__":
    main()
