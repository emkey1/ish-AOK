// Every register pairing of the 32-bit ALU reg,reg ops, imul reg,reg, inc/dec
// reg, shl/shr/sar reg,imm and mov reg,imm32, and every (register, base)
// pairing of movzx reg32,byte/word [base+disp] and mov word [base+disp],reg16
// -- including a word that straddles a page boundary -- with the full register
// file, the arithmetic flags and the touched memory printed after each, and
// add/mov byte/cmp byte through every [base+index*scale+disp] combination,
// and cmp/test of every register pair followed by each of the 16 jcc. The
// i386 JIT emits these as one fused gadget each (JIT_FUSE_ALURR / IMUL /
// INCDEC / SHIFT / MOVI / MOVX); the oracle is the same binary with those
// fusions switched off, which runs the long-standing load/op/store sequence:
//
//   gcc -O1 -o /tmp/far fused_alu_regs.c
//   /tmp/far > on.txt
//   echo "alurr=0 shift=0 incdec=0 movimm=0 movx=0 imul=0 addrsi=0 cmprr=0" > /proc/ish/i386_jit_fuse
//   /tmp/far > off.txt; cmp on.txt off.txt
//
// Each case is generated as raw machine code so the register choice is
// exact, ESP included: the stub parks the real ESP in memory, loads all eight
// registers (ESP too) from a table, runs the one instruction, stores all
// eight, and only then restores ESP for PUSHFD. MOV never touches the flags,
// so the flags read are the instruction's. The entry flags (CF for adc/sbb,
// and what a zero-count shift must leave alone) come from PUSH imm; POPFD.
// The table of cases is built before any of it runs, so no code is
// rewritten after it has been translated.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define FLAGS_MASK 0x8d5 // OF SF ZF AF PF CF

static uint32_t in_regs[8], out_regs[8], out_flags, saved_esp, taken;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t) rng; }

static uint8_t *p;
static void b(uint8_t v) { *p++ = v; }
static void d(uint32_t v) { memcpy(p, &v, 4); p += 4; }

struct test_case {
    void (*fn)(void);
    const char *name;
    int dst, src, imm;
    int mem_off; // offset into mem[] of the access, or -1
    int branchy; // ends in a jcc whose outcome lands in `taken`
    uint32_t regs[8];
};
// Two pages; a word at offset 4095 straddles them.
static uint8_t *mem;
static struct test_case cases[12288];
static int ncases;

static const char *const reg_names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

// insn: the bytes of the one instruction under test.
static void emit_case(const char *name, int dst, int src, int imm, const uint8_t *insn, int len, uint32_t entry_flags) {
    struct test_case *c = &cases[ncases++];
    c->fn = (void (*)(void)) p;
    c->name = name; c->dst = dst; c->src = src; c->imm = imm; c->mem_off = -1;
    static const uint32_t special[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x80000001, 0x0000ffff, 0xfffffffe};
    for (int r = 0; r < 8; r++)
        c->regs[r] = (rnd() & 3) == 0 ? special[rnd() & 7] : rnd();
    b(0x53); b(0x56); b(0x57); b(0x55);                       // push ebx/esi/edi/ebp
    b(0x89); b(0x25); d((uint32_t) &saved_esp);                // mov [saved_esp], esp
    b(0x68); d(entry_flags); b(0x9d);                          // push flags; popfd
    for (int r = 0; r < 8; r++) {                              // mov r, [in_regs+4r]
        b(0x8b); b(0x05 | r << 3); d((uint32_t) &in_regs[r]);
    }
    memcpy(p, insn, len); p += len;
    for (int r = 0; r < 8; r++) {                              // mov [out_regs+4r], r
        b(0x89); b(0x05 | r << 3); d((uint32_t) &out_regs[r]);
    }
    b(0x8b); b(0x25); d((uint32_t) &saved_esp);                // mov esp, [saved_esp]
    b(0x9c); b(0x58); b(0xa3); d((uint32_t) &out_flags);       // pushfd; pop eax; mov [out_flags], eax
    b(0x5d); b(0x5f); b(0x5e); b(0x5b); b(0xc3);               // pop ebp/edi/esi/ebx; ret
}

// A case whose instruction addresses mem[off] through register `base` with a
// 32-bit displacement `disp`: the base register's value is chosen to make it so.
static void emit_mem_case(const char *name, int reg, int base, int off, uint32_t disp,
        const uint8_t *insn, int len, uint32_t entry_flags) {
    emit_case(name, reg, base, (int) disp, insn, len, entry_flags);
    struct test_case *c = &cases[ncases - 1];
    c->mem_off = off;
    c->regs[base] = (uint32_t) (uintptr_t) (mem + off) - disp;
}

// [base + index*scale + disp32]: index gets a small value, base whatever makes
// the sum land on mem[off].
static void emit_sib_case(const char *name, int reg, int base, int index, int shift, int off,
        uint32_t disp, const uint8_t *insn, int len, uint32_t entry_flags) {
    emit_case(name, reg, base, (int) disp, insn, len, entry_flags);
    struct test_case *c = &cases[ncases - 1];
    c->mem_off = off;
    c->regs[index] = rnd() & 15;
    c->regs[base] = (uint32_t) (uintptr_t) (mem + off) - disp - (c->regs[index] << shift);
}

// cmp/test reg,reg then jcc: the taken path stores 1 to `taken`, the other 2
// (mov to memory touches no flags and no register).
static void emit_branch_case(const char *name, uint8_t op, int dst, int src, int cc) {
    uint8_t insn[32];
    int n = 0;
    insn[n++] = op;
    insn[n++] = (uint8_t) (0xc0 | src << 3 | dst);
    insn[n++] = (uint8_t) (0x70 + cc);
    insn[n++] = 12;                                   // jcc over the next 12 bytes
    insn[n++] = 0xc7; insn[n++] = 0x05;               // mov dword [taken], 2
    uint32_t a = (uint32_t) (uintptr_t) &taken, two = 2, one = 1;
    memcpy(insn + n, &a, 4); n += 4; memcpy(insn + n, &two, 4); n += 4;
    insn[n++] = 0xeb; insn[n++] = 10;                 // jmp over the next 10
    insn[n++] = 0xc7; insn[n++] = 0x05;               // mov dword [taken], 1
    memcpy(insn + n, &a, 4); n += 4; memcpy(insn + n, &one, 4); n += 4;
    emit_case(name, dst, src, cc, insn, n, (rnd() & FLAGS_MASK) | 0x2);
    struct test_case *c = &cases[ncases - 1];
    c->branchy = 1;
    if (rnd() & 1) // equal operands half the time, so z/c/cz go both ways
        c->regs[src] = c->regs[dst];
}

static uint32_t random_flags(void) {
    return (rnd() & FLAGS_MASK) | 0x2;
}

int main(void) {
    uint8_t *code = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mem = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || mem == MAP_FAILED) { perror("mmap"); return 1; }
    p = code;

    static const struct { const char *name; uint8_t opcode; } alu[] = {
        {"add", 0x01}, {"or", 0x09}, {"adc", 0x11}, {"sbb", 0x19},
        {"and", 0x21}, {"sub", 0x29}, {"xor", 0x31},
    };
    for (unsigned o = 0; o < sizeof(alu) / sizeof(alu[0]); o++)
        for (int dst = 0; dst < 8; dst++)
            for (int src = 0; src < 8; src++)
                for (int rep = 0; rep < 4; rep++) {
                    uint8_t insn[] = {alu[o].opcode, (uint8_t) (0xc0 | src << 3 | dst)};
                    emit_case(alu[o].name, dst, src, 0, insn, 2, random_flags());
                }
    for (int r = 0; r < 8; r++)
        for (int rep = 0; rep < 16; rep++) {
            uint8_t inc[] = {(uint8_t) (0x40 + r)}, dec[] = {(uint8_t) (0x48 + r)};
            emit_case("inc", r, -1, 0, inc, 1, random_flags());
            emit_case("dec", r, -1, 0, dec, 1, random_flags());
        }
    static const struct { const char *name; uint8_t ext; } shifts[] = {{"shl", 4}, {"shr", 5}, {"sar", 7}};
    for (int s = 0; s < 3; s++)
        for (int r = 0; r < 8; r++)
            for (int count = 0; count < 40; count++) { // 32..39 check the count mask
                uint8_t insn[] = {0xc1, (uint8_t) (0xc0 | shifts[s].ext << 3 | r), (uint8_t) count};
                emit_case(shifts[s].name, r, -1, count, insn, 3, random_flags());
            }

    for (int r = 0; r < 8; r++)
        for (int rep = 0; rep < 4; rep++) {
            uint8_t insn[] = {(uint8_t) (0xb8 + r), 0, 0, 0, 0};
            uint32_t v = rnd();
            memcpy(insn + 1, &v, 4);
            emit_case("movi", r, -1, (int) v, insn, 5, random_flags());
        }
    for (int dst = 0; dst < 8; dst++)
        for (int src = 0; src < 8; src++)
            for (int rep = 0; rep < 4; rep++) {
                uint8_t insn[] = {0x0f, 0xaf, (uint8_t) (0xc0 | dst << 3 | src)};
                emit_case("imul", dst, src, 0, insn, 3, random_flags());
            }
    // [base+disp32]: modrm mod=10, rm=base; esp as a base needs a SIB byte.
    static const int offs[] = {64, 1000, 4094, 4095};
    static const struct { const char *name; uint8_t pre, op1, op2; } memops[] = {
        {"movzx8", 0, 0x0f, 0xb6}, {"movzx16", 0, 0x0f, 0xb7}, {"movw_store", 0x66, 0x89, 0},
    };
    for (unsigned m = 0; m < 3; m++)
        for (int reg = 0; reg < 8; reg++)
            for (int base = 0; base < 8; base++)
                for (unsigned o = 0; o < 4; o++) {
                    if (memops[m].op1 == 0x0f && o == 3 && memops[m].op2 == 0xb6)
                        continue; // a byte cannot straddle
                    uint8_t insn[12];
                    int n = 0;
                    if (memops[m].pre) insn[n++] = memops[m].pre;
                    insn[n++] = memops[m].op1;
                    if (memops[m].op2) insn[n++] = memops[m].op2;
                    insn[n++] = (uint8_t) (0x80 | reg << 3 | (base == 4 ? 4 : base));
                    if (base == 4) insn[n++] = 0x24;
                    uint32_t disp = (rnd() & 0xfff) - 0x800;
                    memcpy(insn + n, &disp, 4); n += 4;
                    emit_mem_case(memops[m].name, reg, base, offs[o], disp, insn, n, random_flags());
                }

    // add r32 / mov r8 / cmp r8 through a SIB operand: every base, index
    // (esp cannot be one) and scale. base == index has no exact solution here
    // and is skipped.
    static const struct { const char *name; uint8_t op; } sibops[] = {
        {"add_sib", 0x03}, {"movb_sib", 0x8a}, {"cmpb_sib", 0x3a},
    };
    for (unsigned m = 0; m < 3; m++)
        for (int base = 0; base < 8; base++)
            for (int index = 0; index < 8; index++)
                for (int shift = 0; shift < 4; shift++) {
                    if (index == 4 || index == base)
                        continue;
                    int reg = (int) (rnd() & 7);
                    uint32_t disp = (rnd() & 0xfff) - 0x800;
                    uint8_t insn[8] = {sibops[m].op, (uint8_t) (0x84 | reg << 3),
                                       (uint8_t) (shift << 6 | index << 3 | base)};
                    memcpy(insn + 3, &disp, 4);
                    emit_sib_case(sibops[m].name, reg, base, index, shift, 100 + (int) (rnd() & 1023),
                                  disp, insn, 7, random_flags());
                }

    for (int cc = 0; cc < 16; cc++)
        for (int dst = 0; dst < 8; dst++)
            for (int src = 0; src < 8; src++) {
                emit_branch_case("cmp_jcc", 0x39, dst, src, cc);
                if ((dst + src + cc) % 2 == 0)
                    emit_branch_case("test_jcc", 0x85, dst, src, cc);
            }

    for (int i = 0; i < 8192; i++)
        mem[i] = (uint8_t) (i * 7 + 3);
    for (int i = 0; i < ncases; i++) {
        struct test_case *c = &cases[i];
        memcpy(in_regs, c->regs, sizeof(in_regs));
        c->fn();
        printf("%s %s", c->name, reg_names[c->dst]);
        if (c->src >= 0)
            printf(",%s", reg_names[c->src]);
        else if (strcmp(c->name, "inc") && strcmp(c->name, "dec"))
            printf(",%d", c->imm);
        printf(" in");
        for (int r = 0; r < 8; r++)
            printf(" %08x", c->regs[r]);
        printf(" out");
        for (int r = 0; r < 8; r++)
            printf(" %08x", out_regs[r]);
        printf(" fl %03x", out_flags & FLAGS_MASK);
        if (c->branchy)
            printf(" taken %u", taken);
        if (c->mem_off >= 0) {
            int o = c->mem_off;
            printf(" mem %02x%02x%02x%02x", mem[o - 1], mem[o], mem[o + 1], mem[o + 2]);
            for (int k = -1; k <= 2; k++) // restore for the next case
                mem[o + k] = (uint8_t) ((o + k) * 7 + 3);
        }
        printf("\n");
    }
    fprintf(stderr, "%d cases\n", ncases);
    return 0;
}
