// x86_bt_mem_index.c -- the bit index of BT/BTS/BTR/BTC on a memory operand.
// A register index is signed and reaches past the operand: the bit is at
// floor(index / width) operands from it, index mod width within -- a 16-bit
// index being the register's low half, sign-extended. An imm8 index is taken
// mod the width and never moves the address. Every form, LOCK and not, at
// indices on both sides; each must change exactly that bit (CF its old value)
// and nothing else in the buffer. The i386 JIT shifted the index unsigned (a
// negative one went 512 MB away and faulted) and used an imm8 unreduced.
// Checked on an AMD Ryzen (camd), 32- and 64-bit builds.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
static uint8_t buf[512] __attribute__((aligned(64)));
#define MID 256

static void expect(const char *what, long idx, int width, const uint8_t *before, unsigned cf,
                   int want_cf, int set) {
    // the bit, from the operand's address at buf + MID
    long op = idx >= 0 ? idx / width : -((-idx + width - 1) / width);
    long bit = idx - op * width;
    long byte = MID + op * (width / 8) + bit / 8;
    uint8_t want[sizeof buf];
    memcpy(want, before, sizeof buf);
    if (set == 1) want[byte] |= (uint8_t) (1 << (bit % 8));
    if (set == 0) want[byte] &= (uint8_t) ~(1 << (bit % 8));
    if (set == 2) want[byte] ^= (uint8_t) (1 << (bit % 8));
    checks++;
    if (memcmp(buf, want, sizeof buf) != 0 || cf != (unsigned) want_cf) {
        if (failures++ < 30)
            printf("FAIL %s width %d index %ld: cf %u want %d, memory %s\n", what, width, idx, cf,
                   want_cf, memcmp(buf, want, sizeof buf) ? "differs" : "right");
    }
}

#define REG(name, insn, W, T, set) \
static void name(long idx) { \
    uint8_t before[sizeof buf]; \
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t) (i * 37 + 11); \
    memcpy(before, buf, sizeof buf); \
    long op = idx >= 0 ? idx / W : -((-idx + W - 1) / W); \
    long bit = idx - op * W; \
    int old = (before[MID + op * (W / 8) + bit / 8] >> (bit % 8)) & 1; \
    unsigned cf = 0; T ix = (T) idx; \
    __asm__ volatile(insn " %1, (%2)\n setc %b0" : "+q"(cf) : "r"(ix), "r"(buf + MID) : "memory", "cc"); \
    expect(insn, idx, W, before, cf, old, set); \
}
REG(bt16, "btw", 16, short, -1)
REG(bts16, "btsw", 16, short, 1)
REG(btr16, "btrw", 16, short, 0)
REG(btc16, "btcw", 16, short, 2)
REG(lbts16, "lock btsw", 16, short, 1)
REG(lbtr16, "lock btrw", 16, short, 0)
REG(lbtc16, "lock btcw", 16, short, 2)
REG(bt32, "btl", 32, int, -1)
REG(bts32, "btsl", 32, int, 1)
REG(btr32, "btrl", 32, int, 0)
REG(btc32, "btcl", 32, int, 2)
REG(lbts32, "lock btsl", 32, int, 1)
REG(lbtr32, "lock btrl", 32, int, 0)
REG(lbtc32, "lock btcl", 32, int, 2)
#if defined(__x86_64__)
REG(bt64, "btq", 64, long, -1)
REG(bts64, "btsq", 64, long, 1)
REG(lbtc64, "lock btcq", 64, long, 2)
#endif

// imm8: the index mod the width, at the operand itself
#define IMM(name, insn, W, imm, set) \
static void name(void) { \
    uint8_t before[sizeof buf]; \
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t) (i * 53 + 7); \
    memcpy(before, buf, sizeof buf); \
    long bit = (imm) % W; \
    int old = (before[MID + bit / 8] >> (bit % 8)) & 1; \
    unsigned cf = 0; \
    __asm__ volatile(insn " $" #imm ", (%1)\n setc %b0" : "+q"(cf) : "r"(buf + MID) : "memory", "cc"); \
    expect(insn " imm", bit, W, before, cf, old, set); \
}
IMM(i_bt16, "btw", 16, 37, -1)
IMM(i_bts16, "btsw", 16, 255, 1)
IMM(i_lbtr16, "lock btrw", 16, 200, 0)
IMM(i_bt32, "btl", 32, 39, -1)
IMM(i_bts32, "btsl", 32, 37, 1)
IMM(i_btc32, "btcl", 32, 128, 2)
IMM(i_lbts32, "lock btsl", 32, 101, 1)
IMM(i_lbtc32, "lock btcl", 32, 254, 2)
#if defined(__x86_64__)
IMM(i_bts64, "btsq", 64, 200, 1)
IMM(i_lbtr64, "lock btrq", 64, 127, 0)
#endif

int main(void) {
    static void (*const regs[])(long) = {
        bt16, bts16, btr16, btc16, lbts16, lbtr16, lbtc16,
        bt32, bts32, btr32, btc32, lbts32, lbtr32, lbtc32,
#if defined(__x86_64__)
        bt64, bts64, lbtc64,
#endif
    };
    // all inside the buffer: |index| / 8 < MID
    static const long idx[] = {0, 1, 15, 16, 31, 32, 63, 64, 100, 1000, 2000,
                               -1, -2, -15, -16, -17, -31, -32, -33, -64, -65, -100, -1000, -2000};
    for (unsigned f = 0; f < sizeof regs / sizeof regs[0]; f++)
        for (unsigned i = 0; i < sizeof idx / sizeof idx[0]; i++)
            regs[f](idx[i]);
    i_bt16(); i_bts16(); i_lbtr16(); i_bt32(); i_bts32(); i_btc32(); i_lbts32(); i_lbtc32();
#if defined(__x86_64__)
    i_bts64(); i_lbtr64();
#endif
    printf("x86_bt_mem_index: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
