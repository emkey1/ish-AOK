// Random straight-line x86-64 sequences mixing flag producers (add/sub/and/
// or/xor/cmp/adc/sbb reg,reg and reg,imm8 at 64 and 32 bits, inc/dec, shl/
// shr/sar by an immediate or by one, test), flag-neutral instructions (mov, lea) and
// flag readers in the MIDDLE of a sequence (setcc, cmovcc, adc/sbb, a short
// jcc over a mov), ending in pushfq. Prints every register and the flags
// after each sequence.
//
// The amd64 JIT skips flags that a later instruction overwrites before any
// reader (jit/gen.c amd64_flags_note); a reader it fails to recognise shows as
// a difference between
//   echo deadflags=1 > /proc/ish/amd64_jit_fuse;  ./amd64_flag_liveness > on.txt
//   echo deadflags=0 > /proc/ish/amd64_jit_fuse;  ./amd64_flag_liveness > off.txt
//   echo 0 > /proc/ish/amd64_jit;                 ./amd64_flag_liveness > interp.txt
// which must all be identical (with ISH_RANDOMIZE_VA_SPACE=0: rip-relative
// lea results are code addresses). Each sequence is generated into an executable
// buffer before any runs, so no code is rewritten after translation.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

// Registers the sequences use: rax rcx rdx rbx rsi rdi r8 r9 r10 r11 r12 r13.
// rsp/rbp stay out; r15 holds the state pointer, r14 is the pushfq scratch
// and the index register of the indexed memory operands.
static const int usable[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13};
#define NUSABLE 12

// r[14] carries the flags in and out (r14 itself is only the pushfq scratch).
struct state { uint64_t r[16]; uint64_t mem[8]; };
// [r15 + 128 + 8*k], k = 0..7: the memory operands (disp32 addressing).

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int pick(void) { return usable[rnd() % NUSABLE]; }

static uint8_t *p;
static void b(uint8_t v) { *p++ = v; }

// REX for (reg field, rm field) with W
static void rex(int w, int reg, int rm) {
    uint8_t v = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);
    if (v != 0x40 || w)
        b(v);
    else if ((reg | rm) & 8)
        b(v);
}
static void modrm_rr(int reg, int rm) { b((uint8_t) (0xc0 | (reg & 7) << 3 | (rm & 7))); }

// mov reg64, [r15 + 8*i]  /  mov [r15 + 8*i], reg64
static void load_reg(int reg) { b((uint8_t) (0x49 | ((reg & 8) ? 4 : 0))); b(0x8b); b((uint8_t) (0x47 | (reg & 7) << 3)); b((uint8_t) (8 * reg)); }
static void store_reg(int reg) { b((uint8_t) (0x49 | ((reg & 8) ? 4 : 0))); b(0x89); b((uint8_t) (0x47 | (reg & 7) << 3)); b((uint8_t) (8 * reg)); }

// modrm (mod=10, rm=r15) + disp32 into mem[], for register `reg`
static void mem_operand(int reg) {
    b((uint8_t) (0x87 | (reg & 7) << 3));
    uint32_t d = 128 + 8 * (uint32_t) (rnd() % 8) + (uint32_t) (rnd() % 4);
    memcpy(p, &d, 4); p += 4;
}
// REX with B = 1 (r15 base) and R from reg
static void rex_mem(int w, int reg) { b((uint8_t) (0x41 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0))); }
// Half the memory operands go through rbp, which points at mem[0] and is one
// of the eight registers the amd64 JIT caches -- the register-cache memory
// gadgets (math.S amd64_sld/sst) need a cached base. rex_mem_any/mem_any pick.
static int use_rbp;
static void rex_mem_any(int w, int reg) {
    if (use_rbp) b((uint8_t) (0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0)));
    else rex_mem(w, reg);
}
static void mem_any(int reg) {
    if (!use_rbp) { mem_operand(reg); return; }
    b((uint8_t) (0x85 | (reg & 7) << 3)); // mod=10 rm=rbp: [rbp + disp32]
    uint32_t d = 8 * (uint32_t) (rnd() % 8) + (uint32_t) (rnd() % 4);
    memcpy(p, &d, 4); p += 4;
}

// [base + r14 * scale + disp32], base r15 (+128) or rbp, r14 = 0..3 set just
// before: the indexed forms (math.S amd64_ea + the memory families' base 17).
// rex_x adds REX.X for r14; the offset into mem[] stays at or below 32.
static void rex_sib(int w, int reg) {
    b((uint8_t) (0x42 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | (use_rbp ? 0 : 1)));
}
static int sib_k; // r14's value, from set_r14
static void sib_operand(int reg) {
    int sc = (int) (rnd() % 4);
    uint32_t off = (uint32_t) (sib_k << sc), d = (uint32_t) (rnd() % (33 - off)) + (use_rbp ? 0 : 128);
    b((uint8_t) (0x84 | (reg & 7) << 3));                        // mod=10, rm=100: SIB
    b((uint8_t) (sc << 6 | (14 & 7) << 3 | (use_rbp ? 5 : 7)));    // index r14
    memcpy(p, &d, 4); p += 4;
}
static void set_r14(void) { // mov $k, %r14d before an indexed operand
    sib_k = (int) (rnd() % 4);
    b(0x41); b((uint8_t) (0xb8 | (14 & 7))); uint32_t v = (uint32_t) sib_k; memcpy(p, &v, 4); p += 4;
}

static void emit_mem_one(void) {
    int a = pick(), w = (int) (rnd() & 1);
    use_rbp = (int) (rnd() & 1);
    if (rnd() % 4 == 0) { // the indexed and the 16-bit / byte forms
        int lowbyte_rex = (a & 8) || (rnd() & 1); // REX makes a's low byte addressable
        switch (rnd() % 18) {
            case 16: case 17: { // cmp m8, imm8 (80 /7) / test m8, imm8 (F6 /0): plain, indexed, %fs:0
                int t = (int) (rnd() & 1), op = t ? 0xf6 : 0x80, ext = t ? 0 : 7;
                switch (rnd() % 3) {
                    case 0: if (!use_rbp) rex_mem(0, 0); b((uint8_t) op); mem_any(ext); break;
                    case 1: set_r14(); rex_sib(0, 0); b((uint8_t) op); sib_operand(ext); break;
                    default: b(0x64); b((uint8_t) op); b((uint8_t) (0x04 | ext << 3)); b(0x25);
                        uint32_t z = (uint32_t) (rnd() % 8); memcpy(p, &z, 4); p += 4; break;
                }
                b((uint8_t) rnd());
                break;
            }
            case 9: { // inc/dec [m] (FF /0, /1), plain or indexed
                int ext = (int) (rnd() & 1);
                if (rnd() & 1) { set_r14(); rex_sib(w, 0); b(0xff); sib_operand(ext); }
                else { rex_mem_any(w, 0); b(0xff); mem_any(ext); }
                break;
            }
            case 10: { // push a (or push imm32); pop c (net rsp change 0)
                int c = pick();
                if (rnd() & 1) { b(0x68); uint32_t v = (uint32_t) rnd(); memcpy(p, &v, 4); p += 4; }
                else { if (a & 8) b(0x41); b((uint8_t) (0x50 + (a & 7))); }
                if (c & 8) b(0x41);
                b((uint8_t) (0x58 + (c & 7)));
                break;
            }
            case 15: { // mov/add/sub/cmp/xor a, %fs:0 (the thread pointer; fixed with ASLR off)
                static const uint8_t fo[] = {0x8b, 0x03, 0x2b, 0x3b, 0x33};
                b(0x64); b((uint8_t) (0x48 | ((a & 8) ? 4 : 0))); b(fo[rnd() % 5]);
                b((uint8_t) (0x04 | (a & 7) << 3)); b(0x25); // SIB: no base, no index
                uint32_t z = 0; memcpy(p, &z, 4); p += 4;
                break;
            }
            case 13: { // mov [m], imm (C7 /0 imm32, or C6 /0 imm8), plain or indexed
                int byte = (int) (rnd() & 1), idx = (int) (rnd() & 1);
                if (idx) { set_r14(); rex_sib(byte ? 0 : w, 0); b(byte ? 0xc6 : 0xc7); sib_operand(0); }
                else { rex_mem_any(byte ? 0 : w, 0); b(byte ? 0xc6 : 0xc7); mem_any(0); }
                if (byte) b((uint8_t) rnd());
                else { uint32_t v = (uint32_t) rnd(); memcpy(p, &v, 4); p += 4; }
                break;
            }
            case 14: { // add/or/and/sub/xor [m], imm8 (83 /0,1,4,5,6), plain or indexed
                static const int ext[] = {0, 1, 4, 5, 6};
                int e = ext[rnd() % 5];
                if (rnd() & 1) { set_r14(); rex_sib(w, 0); b(0x83); sib_operand(e); }
                else { rex_mem_any(w, 0); b(0x83); mem_any(e); }
                b((uint8_t) rnd());
                break;
            }
            case 11: // cmp [m], a or cmp [m], imm8 (83 /7)
                if (rnd() & 1) { rex_mem_any(w, a); b(0x39); mem_any(a); }
                else { rex_mem_any(w, 0); b(0x83); mem_any(7); b((uint8_t) rnd()); }
                break;
            case 12: { // jmp *a / call *a to the next instruction (a = lea [rip + len])
                int is_call = (int) (rnd() & 1), len = (a & 8) ? 3 : 2;
                b((uint8_t) (0x48 | ((a & 8) ? 4 : 0))); b(0x8d); b((uint8_t) (0x05 | (a & 7) << 3));
                uint32_t d = (uint32_t) len; memcpy(p, &d, 4); p += 4;
                if (a & 8) b(0x41);
                b(0xff); b((uint8_t) (0xc0 | (is_call ? 2 : 4) << 3 | (a & 7)));
                if (is_call) { // the pushed return address is the target: pop it into a
                    if (a & 8) b(0x41);
                    b((uint8_t) (0x58 + (a & 7)));
                }
                break;
            }
            case 0: set_r14(); rex_sib(w, a); b(0x8b); sib_operand(a); break;             // mov a, [b+i*s+d]
            case 1: set_r14(); rex_sib(w, a); b(0x89); sib_operand(a); break;             // mov [b+i*s+d], a
            case 2: set_r14(); rex_sib(w, a); b(rnd() & 1 ? 0x03 : 0x3b); sib_operand(a); break; // add/cmp a, [..]
            case 3: set_r14(); rex_sib(w, a); b(0x8d); sib_operand(a); break;             // lea a, [b+i*s+d]
            case 4: b(0x66); rex_mem_any(0, a); b(rnd() & 1 ? 0x89 : 0x8b); mem_any(a); break; // mov m16 <-> a16
            case 5: rex_mem_any(w, a); b(0x0f); b(0xb7); mem_any(a); break;               // movzx a, word [m]
            case 6: // mov byte [m] <-> a8, low byte only (no REX: al..bl)
                if (!lowbyte_rex && (a & 7) >= 4) a &= 3;
                if (lowbyte_rex) rex_mem_any(0, a);
                else if (!use_rbp) rex_mem(0, a);
                b(rnd() & 1 ? 0x88 : 0x8a); mem_any(a); break;
            case 7: // cmp a8, [b+i*s+d] or test [m], a8
                if (!lowbyte_rex && (a & 7) >= 4) a &= 3;
                if (rnd() & 1) { set_r14(); rex_sib(0, a); b(0x3a); sib_operand(a); }
                else { rex_mem_any(0, a); b(0x84); mem_any(a); }
                break;
            case 8: // movsxd a, [m]: plain, indexed, or %fs:0
                switch (rnd() % 3) {
                    case 0: rex_mem_any(1, a); b(0x63); mem_any(a); break;
                    case 1: set_r14(); rex_sib(1, a); b(0x63); sib_operand(a); break;
                    default: b(0x64); b((uint8_t) (0x48 | ((a & 8) ? 4 : 0))); b(0x63);
                        b((uint8_t) (0x04 | (a & 7) << 3)); b(0x25);
                        uint32_t z = 0; memcpy(p, &z, 4); p += 4; break;
                }
                break;
            default: set_r14(); rex_sib(0, a); b(0x0f); b(0xb6); sib_operand(a); break;   // movzx a, byte [b+i*s+d]
        }
        return;
    }
    if (rnd() % 12 == 0) { // mov a, [rip + d]: reads back code bytes just emitted
        b((uint8_t) (0x40 | (w ? 8 : 0) | ((a & 8) ? 4 : 0))); b(0x8b); b((uint8_t) (0x05 | (a & 7) << 3));
        uint32_t d = (uint32_t) -(int32_t) (16 + rnd() % 32);
        memcpy(p, &d, 4); p += 4;
        return;
    }
    switch (rnd() % 10) {
        case 0: rex_mem_any(w, a); b(0x8b); mem_any(a); break;            // mov a, [m]
        case 1: rex_mem_any(w, a); b(0x89); mem_any(a); break;            // mov [m], a
        case 2: rex_mem_any(w, a); b(rnd() & 1 ? 0x03 : 0x2b); mem_any(a); break; // add/sub a, [m]
        case 3: rex_mem_any(w, a); b(0x3b); mem_any(a); break;            // cmp a, [m]
        case 4: { static const uint8_t lo[] = {0x0b, 0x23, 0x33};                 // or/and/xor a, [m]
            rex_mem_any(w, a); b(lo[rnd() % 3]); mem_any(a); break; }
        case 5: rex_mem_any(w, a); b(rnd() & 1 ? 0x01 : 0x39); mem_any(a); break; // add/cmp [m], a
        case 6: rex_mem(w, a); b(0x0f); b(0xb6); mem_operand(a); break;   // movzx a, byte [m]
        case 7: rex_mem(0, a); b(0x02); mem_operand(a); break;            // add a8, [m] (REX: no ah..bh)
        case 8: rex_mem(0, a); b(0x38); mem_operand(a); break;            // cmp [m], a8
        default: rex_mem_any(1, a); b(0x8b); mem_any(a); break;           // mov a64, [m]
    }
}

static void emit_one(void) {
    int a = pick(), c = pick(), w = (int) (rnd() & 1);
    if (rnd() % 3 == 0) {
        emit_mem_one();
        return;
    }
    switch (rnd() % 24) {
        case 0: case 1: case 2: case 3: { // alu reg,reg: add or adc sbb and sub xor cmp
            static const uint8_t ops[] = {0x01, 0x09, 0x11, 0x19, 0x21, 0x29, 0x31, 0x39};
            rex(w, c, a); b(ops[rnd() % 8]); modrm_rr(c, a); break;
        }
        case 4: case 5: { // alu reg,imm8 (83 /n)
            rex(w, 0, a); b(0x83); modrm_rr((int) (rnd() % 8), a); b((uint8_t) rnd()); break;
        }
        case 6: rex(w, 0, a); b(0xff); modrm_rr((int) (rnd() & 1), a); break; // inc/dec
        case 7: { // shl/shr/sar imm8 (0 included), or 1 time in 4 by one (D1)
            static const int ext[] = {4, 5, 7};
            if ((rnd() & 3) == 0) { rex(w, 0, a); b(0xd1); modrm_rr(ext[rnd() % 3], a); break; }
            rex(w, 0, a); b(0xc1); modrm_rr(ext[rnd() % 3], a); b((uint8_t) (rnd() % 40)); break;
        }
        case 8: rex(w, c, a); b(0x85); modrm_rr(c, a); break; // test
        case 9: rex(w, c, a); b(rnd() & 1 ? 0x89 : 0x8b); modrm_rr(c, a); break; // mov r64/r32, both directions
        case 10: // lea a, [c + disp8] (64 or 32), or 1 time in 4 lea a, [rip + disp32]
            if ((rnd() & 3) == 0) {
                rex(w, a, 0); b(0x8d); b((uint8_t) (0x05 | (a & 7) << 3));
                uint32_t d = (uint32_t) rnd();
                memcpy(p, &d, 4); p += 4;
                break;
            }
            rex(w, a, c); b(0x8d);
            if ((c & 7) == 4) { b((uint8_t) (0x44 | (a & 7) << 3)); b(0x24); }
            else b((uint8_t) (0x40 | (a & 7) << 3 | (c & 7)));
            b((uint8_t) rnd()); break;
        case 11: // setcc a8 (needs REX for sil/dil/r8b..)
            b((uint8_t) (0x40 | ((a & 8) ? 1 : 0))); b(0x0f); b((uint8_t) (0x90 + rnd() % 16)); modrm_rr(0, a); break;
        case 12: // cmovcc a, c (32: written, zero-extended, even when not taken; or 64)
            rex(w, a, c); b(0x0f); b((uint8_t) (0x40 + rnd() % 16)); modrm_rr(a, c); break;
        case 13: { // jcc over a 3-byte mov (rex.w 89 modrm); 1 time in 4 jrcxz,
            // after setting ecx to 0 or 1 (jrcxz reads rcx, not the flags)
            if ((rnd() & 3) == 0) {
                if (a == 1) a = 0;
                b(0xb9); uint32_t v = (uint32_t) (rnd() & 1); memcpy(p, &v, 4); p += 4; // mov $v, %ecx
                b(0xe3); b(3);
            } else {
                if (rnd() % 3 == 0) { // test a, c or test a, imm32 right before: fused with the jcc
                    if (rnd() & 1) { rex(w, c, a); b(0x85); modrm_rr(c, a); }
                    else {
                        uint32_t v = (rnd() & 1) ? (uint32_t) rnd() : (uint32_t) (1u << (rnd() % 32));
                        rex(w, 0, a); b(0xf7); modrm_rr(0, a); memcpy(p, &v, 4); p += 4;
                    }
                }
                b((uint8_t) (0x70 + rnd() % 16)); b(3);
            }
            rex(1, c, a); b(0x89); modrm_rr(c, a); break;
        }
        case 16: { // movzx/movsx a, c8/c16; 1 time in 3 from ah..bh (no REX)
            static const uint8_t mx[] = {0xb6, 0xb7, 0xbe, 0xbf};
            if (rnd() % 3 == 0) {
                int d = (int) (rnd() % 8);
                if (d == 4 || d == 5) d = 0; // rsp/rbp stay out
                b(0x0f); b(rnd() & 1 ? 0xb6 : 0xbe); b((uint8_t) (0xc0 | d << 3 | (4 + rnd() % 4)));
            } else {
                b((uint8_t) (0x40 | (w ? 8 : 0) | ((a & 8) ? 4 : 0) | ((c & 8) ? 1 : 0))); // REX: low bytes
                b(0x0f); b(mx[rnd() % 4]); modrm_rr(a, c);
            }
            break;
        }
        case 15: // neg a / not a (F7 /3, /2)
            rex(w, 0, a); b(0xf7); modrm_rr(rnd() & 1 ? 3 : 2, a); break;
        case 14: // imul a, c (0F AF) or imul a, c, imm8 (6B): CF/OF only
            if (rnd() & 1) { rex(w, a, c); b(0x0f); b(0xaf); modrm_rr(a, c); }
            else { rex(w, a, c); b(0x6b); modrm_rr(a, c); b((uint8_t) rnd()); }
            break;
        case 17: { // test a, imm: F7 /0, F6 /0 on a low byte (REX, or al..bl), A9/A8
            uint32_t v = (rnd() & 1) ? (uint32_t) rnd() : (uint32_t) (1u << (rnd() % 32));
            switch (rnd() % 4) {
                case 0: rex(w, 0, a); b(0xf7); modrm_rr(0, a); memcpy(p, &v, 4); p += 4; break;
                case 1: b((uint8_t) (0x40 | ((a & 8) ? 1 : 0))); b(0xf6); modrm_rr(0, a); b((uint8_t) v); break;
                case 2: b(0xf6); modrm_rr(0, a & 3); b((uint8_t) v); break;
                default:
                    if (rnd() & 1) { if (w) b(0x48); b(0xa9); memcpy(p, &v, 4); p += 4; }
                    else { b(0xa8); b((uint8_t) v); }
                    break;
            }
            break;
        }
        case 18: { // mov a, imm32 (B8+r), movabs (REX.W B8+r), C7 /0 imm32; movsxd a, c
            uint64_t v = (rnd() & 1) ? rnd() : (uint64_t) (int64_t) (int8_t) rnd();
            switch (rnd() % 4) {
                case 0: if (a & 8) b(0x41); b((uint8_t) (0xb8 + (a & 7))); memcpy(p, &v, 4); p += 4; break;
                case 1: b((uint8_t) (0x48 | ((a & 8) ? 1 : 0))); b((uint8_t) (0xb8 + (a & 7))); memcpy(p, &v, 8); p += 8; break;
                case 2: rex(w, 0, a); b(0xc7); modrm_rr(0, a); memcpy(p, &v, 4); p += 4; break;
                default: rex(1, a, c); b(0x63); modrm_rr(a, c); break;
            }
            break;
        }
        case 19: { // rol/ror a, imm8 (C1 /0, /1): counts 0 and 1 included, and past the size
            static const uint8_t cnt[] = {0, 1, 1, 2, 31, 32, 33, 63, 64, 65};
            rex(w, 0, a); b(0xc1); modrm_rr((int) (rnd() & 1), a);
            b((uint8_t) ((rnd() & 1) ? cnt[rnd() % 10] : rnd() % 70)); break;
        }
        case 21: rex(w, a, a); b(rnd() & 1 ? 0x31 : 0x33); modrm_rr(a, a); break; // xor a, a
        case 20: // setcc without REX: al..bl (rm 0-3) or ah..bh (rm 4-7)
            b(0x0f); b((uint8_t) (0x90 + rnd() % 16)); b((uint8_t) (0xc0 | (rnd() % 8))); break;
        default: { // a second producer right away, to give the scan something to skip
            static const uint8_t ops[] = {0x01, 0x29, 0x31};
            rex(w, c, a); b(ops[rnd() % 3]); modrm_rr(c, a); break;
        }
    }
}

#define NSEQ 3000
static void (*seqs[NSEQ])(struct state *);
static struct state inputs[NSEQ];

int main(void) {
    uint8_t *code = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    p = code;
    for (int s = 0; s < NSEQ; s++) {
        seqs[s] = (void (*)(struct state *)) p;
        b(0x41); b(0x57); b(0x41); b(0x56); b(0x41); b(0x55); b(0x41); b(0x54); b(0x53); // push r15 r14 r13 r12 rbx
        b(0x55);                                       // push rbp
        b(0x49); b(0x89); b(0xff);                     // mov r15, rdi
        b(0x49); b(0x8d); b(0xaf); { uint32_t d = 128; memcpy(p, &d, 4); p += 4; } // lea rbp, [r15 + 128]
        b(0x41); b(0xff); b(0x77); b(8 * 14);          // push qword [r15 + 112] (initial flags)
        b(0x9d);                                       // popfq
        for (int i = 0; i < NUSABLE; i++) load_reg(usable[i]);
        int n = 3 + (int) (rnd() % 12);
        for (int i = 0; i < n; i++) emit_one();
        b(0x9c);                                       // pushfq
        b(0x41); b(0x5e);                              // pop r14
        b(0x4d); b(0x89); b(0x77); b(8 * 14);          // mov [r15 + 112], r14
        for (int i = 0; i < NUSABLE; i++) store_reg(usable[i]);
        b(0x5d);                                       // pop rbp
        b(0x5b); b(0x41); b(0x5c); b(0x41); b(0x5d); b(0x41); b(0x5e); b(0x41); b(0x5f); // pop rbx r12 r13 r14 r15
        b(0xc3);
        for (int i = 0; i < 16; i++)
            inputs[s].r[i] = (rnd() & 3) == 0 ? (uint64_t) (int64_t) (int8_t) rnd() : rnd();
        inputs[s].r[14] = (rnd() & 0x8d5) | 0x202; // arithmetic flags only: never TF or DF
        for (int i = 0; i < 8; i++)
            inputs[s].mem[i] = rnd();
    }
    for (int s = 0; s < NSEQ; s++) {
        struct state st = inputs[s];
        seqs[s](&st);
        printf("%d", s);
        for (int i = 0; i < NUSABLE; i++)
            printf(" %016llx", (unsigned long long) st.r[usable[i]]);
        printf(" fl %03llx m", (unsigned long long) (st.r[14] & 0x8d5));
        for (int i = 0; i < 8; i++)
            printf(" %016llx", (unsigned long long) st.mem[i]);
        printf("\n");
    }
    return 0;
}
