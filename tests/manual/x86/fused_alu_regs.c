// Every register pairing of the 32-bit ALU reg,reg ops, inc/dec reg and
// shl/shr/sar reg,imm, with the full register file and the arithmetic flags
// printed after each. The i386 JIT emits these as one fused gadget per
// register (JIT_FUSE_ALURR / INCDEC / SHIFT); the oracle is the same binary
// with those fusions switched off, which runs the long-standing
// load/op/store gadget sequence:
//
//   gcc -O1 -o /tmp/far fused_alu_regs.c
//   /tmp/far > on.txt
//   echo "alurr=0 shift=0 incdec=0" > /proc/ish/i386_jit_fuse   (fresh ish)
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

static uint32_t in_regs[8], out_regs[8], out_flags, saved_esp;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t) rng; }

static uint8_t *p;
static void b(uint8_t v) { *p++ = v; }
static void d(uint32_t v) { memcpy(p, &v, 4); p += 4; }

struct test_case {
    void (*fn)(void);
    const char *name;
    int dst, src, imm;
    uint32_t regs[8];
};
static struct test_case cases[8192];
static int ncases;

static const char *const reg_names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

// insn: the bytes of the one instruction under test.
static void emit_case(const char *name, int dst, int src, int imm, const uint8_t *insn, int len, uint32_t entry_flags) {
    struct test_case *c = &cases[ncases++];
    c->fn = (void (*)(void)) p;
    c->name = name; c->dst = dst; c->src = src; c->imm = imm;
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

static uint32_t random_flags(void) {
    return (rnd() & FLAGS_MASK) | 0x2;
}

int main(void) {
    uint8_t *code = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
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
        printf(" fl %03x\n", out_flags & FLAGS_MASK);
    }
    fprintf(stderr, "%d cases\n", ncases);
    return 0;
}
