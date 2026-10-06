// amd64_xchg_mem.c -- XCHG reg, [mem] (86, 87) in 8/16/32/64 bits: the register
// gets the old value (8/16 keep the rest of it, AH..BH for a byte, 32
// zero-extends), memory the register's; aligned, misaligned within a 16-byte
// block and across one; and atomic -- a spinlock taken with XCHG (aligned and
// misaligned lock words) protects a plain counter four threads increment. The
// gadget is math.S's amd64_xchg_mem*.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks, bad;
static void check(const char *what, long k, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%ld): %#llx, want %#llx\n", what, k, (unsigned long long) got, (unsigned long long) want);
}
static uint64_t rs = 0xdeadbeefcafef00dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint8_t buf[64] __attribute__((aligned(16)));

static uint8_t *lockw;
static volatile uint64_t shared;
static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < 20000; i++) {
        uint32_t v;
        do { v = 1; __asm__ volatile("xchgl %0, (%1)" : "+r"(v) : "r"(lockw) : "memory"); } while (v);
        shared++;
        v = 0;
        __asm__ volatile("xchgl %0, (%1)" : "+r"(v) : "r"(lockw) : "memory");
    }
    return NULL;
}

int main(void) {
    static const int offs[] = {0, 1, 3, 7, 13, 15};
    for (int k = 0; k < 3000; k++) {
        int o = offs[k % 6];
        uint64_t mem = rnd(), reg = rnd(), reg0 = reg, cur;
        uint8_t *p = buf + 16 + o;
        memcpy(p, &mem, 8);
        switch (k % 4) {
        case 0: __asm__ volatile("xchgb %b0, (%1)" : "+q"(reg) : "r"(p) : "memory"); break;
        case 1: __asm__ volatile("xchgw %w0, (%1)" : "+r"(reg) : "r"(p) : "memory"); break;
        case 2: __asm__ volatile("xchgl %k0, (%1)" : "+r"(reg) : "r"(p) : "memory"); break;
        default: __asm__ volatile("xchgq %0, (%1)" : "+r"(reg) : "r"(p) : "memory"); break;
        }
        int bits = 8 << (k % 4);
        uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1;
        memcpy(&cur, p, 8);
        check("memory = the register", k, cur, (mem & ~m) | (reg0 & m));
        check("register = old memory", k, reg, bits == 32 ? (mem & m) : bits == 64 ? mem : (reg0 & ~m) | (mem & m));
    }
    {
        uint8_t b = 0x5a;
        uint64_t rax = 0x1111110522ull;
        __asm__ volatile("xchgb %%ah, (%1)" : "+a"(rax) : "b"(&b) : "memory");   /* not r8-r15: no REX with %ah */
        check("xchg %ah, (mem): memory", 0, b, 0x05);
        check("xchg %ah, (mem): ah = old, the rest kept", 0, rax, 0x1111115a22ull);
    }
    static const int loffs[] = {0, 1, 13};
    for (int c = 0; c < 3; c++) {
        lockw = buf + 32 + loffs[c];
        memset(lockw, 0, 4);
        shared = 0;
        pthread_t t[4];
        for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        check("xchg spinlock: no lost increment", loffs[c], shared, 80000);
    }
    printf("amd64_xchg_mem: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
