#include "emu/modrm.h"
#include "undefined-flags.h"
#include "ptutil.h"

#define C (1 << 0)
#define P (1 << 2)
#define A (1 << 4)
#define Z (1 << 6)
#define S (1 << 7)
#define O (1 << 11)

int undefined_flags_mask(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t ip = cpu->eip;
    byte_t opcode;
#define read(x) tlb_read(tlb, ip++, &x, sizeof(x));
skip:
    read(opcode);
    switch (opcode) {
        // shift or rotate, of is undefined if shift count is greater than 1
        case 0x0f:
            read(opcode);
            switch(opcode) {
                // shrd/shld
                case 0xa4:
                case 0xa5:
                case 0xac:
                case 0xad: {
                    ip++;
                    byte_t shift = -1;
                    if (opcode == 0xad)
                        shift = cpu->cl;
                    else
                        read(shift);
                    if (shift == 1)
                        return O|A;
                    else if (shift > 1)
                        return O|A;
                    break;
                }
                case 0xaf: return S|Z|A|P; // imul
                case 0xbd: case 0xbc: return O|S|A|P|C; // bsr/bsf
            }
            break;

        case 0x69:
        case 0x6b: return S|Z|A|P; // imul

        // Group 2: rotates in reg 0-3, shifts in reg 4-7. They do not leave
        // the same flags undefined, so the modrm has to be read rather than
        // skipped.
        //
        // AF was the omission. Intel's SDM says the AF flag is undefined for
        // SHL/SHR/SAL/SAR at any non-zero count -- the SHRD/SHLD case above
        // already returns O|A for exactly that reason -- and this case
        // returned only O. So every shift in a program was compared on a flag
        // the architecture does not define, and ptraceomatic reported a
        // divergence on the first `shr` it met: 422 instructions into libc
        // start-up, on `c1 ea 08` (shr edx, 8), real eflags 0x356 against fake
        // 0x346, differing in bit 4 and nothing else.
        case 0xc0:
        case 0xc1:
        case 0xd0:
        case 0xd1:
        case 0xd2:
        case 0xd3: {
            byte_t modrm = -1;
            read(modrm);
            byte_t shift_count = -1;
            if (opcode == 0xd0 || opcode == 0xd1)
                shift_count = 1;
            else if (opcode == 0xd2 || opcode == 0xd3)
                shift_count = cpu->cl;
            else
                // Approximation inherited from the `ip++` this replaced: the
                // imm8 is read as though the modrm had no SIB or displacement,
                // which is true for a register operand and wrong for something
                // like `shl dword [ebx+8], 3`. Correct for every shift a
                // compiler emits on a register, which is what this has been
                // used on; if a memory-operand shift ever reports a bogus
                // divergence, this is why.
                read(shift_count);
            // x86 masks the count to 5 bits, and a count of zero affects no
            // flags at all -- so nothing is undefined either.
            shift_count &= 0x1f;
            if (shift_count == 0)
                break;
            int undefined = 0;
            if (REG(modrm) >= 4)
                undefined |= A;      // SHL/SHR/SAL/SAR, not the rotates
            if (shift_count > 1)
                undefined |= O;      // OF is defined only for a 1-bit shift
            return undefined;
        }

        case 0xf6: case 0xf7: {
            // group 3
            byte_t modrm = -1;
            read(modrm);
            switch (REG(modrm)) {
                case 4: return S|Z|A|P; // mul
                case 5: return S|Z|A|P; // imul
                // DIV and IDIV were missing, and they are the most permissive
                // instructions in the set: Intel leaves CF, OF, SF, ZF, AF and
                // PF all undefined. Found the same way as the shift case, one
                // divergence further in -- `f7 f6` (div esi), differing in AF
                // and nothing else.
                case 6: return C|O|S|Z|A|P; // div
                case 7: return C|O|S|Z|A|P; // idiv
            }
            break;
        }
        case 0x66: goto skip;
    }
    return 0;
}
