// amd64_addr32.c -- the 0x67 address-size prefix in 64-bit code. Where an
// instruction has no effective address it does nothing, and toolchains rely
// on that: the linker relaxes `call *foo@GOTPCREL(%rip)` to `addr32 call foo`
// (spelled in bytes here: gas drops the prefix from CALL and JMP; a 32-bit
// base register, `(%k1)`, is how gas spells it on a memory operand)
// and OpenSSL's perlasm pads register-only instructions with it. Where there
// is one, the address is computed in 32 bits and zero-extended: a pointer
// with junk above bit 31 reaches the low 4 GB. Checked on an AMD Ryzen
// (camd).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile int called;
__attribute__((used, noinline)) static void target(void) { called++; }

int main(void) {
    // inert: near CALL and JMP, register-only ALU, SSE and AVX
    called = 0;
    __asm__ volatile(".byte 0x67, 0xe8\n .long target - . - 4" ::: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(called == 1, "addr32 call: called %d", called);
    unsigned long r = 0;
    __asm__ volatile(".byte 0x67, 0xe9\n .long 1f - . - 4\n mov $1, %0\n1:" : "+r"(r));
    CHECK(r == 0, "addr32 jmp: %lu", r);
    unsigned long a = 0x123456789abcdef0ul, b = 0x1111111111111111ul;
    __asm__ volatile("addr32 add %1, %0" : "+r"(a) : "r"(b) : "cc");
    CHECK(a == 0x23456789abcdf001ul, "addr32 add r64: %#lx", a);
    __asm__ volatile("addr32 shr $4, %0" : "+r"(a) :: "cc");
    CHECK(a == 0x023456789abcdf00ul, "addr32 shr: %#lx", a);
    uint32_t xin[4] = {1, 2, 3, 4}, yin[8] = {1, 2, 3, 4, 5, 6, 7, 8}, out[8];
    __asm__ volatile("movdqu %1, %%xmm1\n addr32 movdqa %%xmm1, %%xmm2\n addr32 paddd %%xmm1, %%xmm2\n movdqu %%xmm2, %0"
                     : "=m"(out) : "m"(xin) : "xmm1", "xmm2");
    CHECK(out[0] == 2 && out[3] == 8, "addr32 sse: %u %u", out[0], out[3]);
    __asm__ volatile("vmovdqu %1, %%ymm1\n addr32 vpaddd %%ymm1, %%ymm1, %%ymm2\n vmovdqu %%ymm2, %0\n vzeroupper"
                     : "=m"(out) : "m"(yin) : "xmm1", "xmm2");
    CHECK(out[0] == 2 && out[7] == 16, "addr32 avx: %u %u", out[0], out[7]);

    // effective: a 32-bit address, the pointer's upper half ignored
    uint8_t *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(low != MAP_FAILED && (uintptr_t) low < (1ul << 32), "MAP_32BIT: %p", (void *) low);
    for (int i = 0; i < 256; i++)
        low[i] = (uint8_t) i;
    unsigned long junk = 0xdead00000000ul | (uintptr_t) low;
    unsigned v = 0;
    __asm__ volatile("movl (%k1), %0" : "=r"(v) : "r"(junk) : "memory");
    CHECK(v == 0x03020100, "addr32 load: %#x", v);
    __asm__ volatile("movl 8(%k1,%k2,4), %0" : "=r"(v) : "r"(junk), "r"(0xffff000000000001ul) : "memory");
    CHECK(v == 0x0f0e0d0c, "addr32 load base+index*4+8: %#x", v);
    __asm__ volatile("addl $0x01010101, 16(%k0)" :: "r"(junk) : "memory", "cc");
    CHECK(low[16] == 17 && low[19] == 20, "addr32 add to memory: %u %u", low[16], low[19]);
    unsigned long l = 0;
    __asm__ volatile("lea 0x10(%k1), %0" : "=r"(l) : "r"(0xfffffffffffffffful) );
    CHECK(l == 0xf, "addr32 lea wraps at 4G: %#lx", l);
    uint8_t xb[16];
    __asm__ volatile("movdqu (%k1), %%xmm3\n movdqu %%xmm3, %0" : "=m"(xb) : "r"(junk) : "xmm3", "memory");
    CHECK(xb[0] == 0 && xb[15] == 15, "addr32 movdqu: %u", xb[15]);
    // across the arm families: VEX, x87, PUSH and CALL through memory, LOCK
    // CMPXCHG, MOVBE, FXSAVE
    uint8_t yb[32];
    __asm__ volatile("vmovdqu (%k1), %%ymm4\n vmovdqu %%ymm4, %0\n vzeroupper" : "=m"(yb) : "r"(junk) : "xmm4", "memory");
    CHECK(yb[0] == 0 && yb[31] == 31, "addr32 vmovdqu: %u", yb[31]);
    float f = 2.5f;
    memcpy(low + 200, &f, 4);
    float fo = 0;
    __asm__ volatile("flds 200(%k1)\n fstps %0" : "=m"(fo) : "r"(junk) : "memory");
    CHECK(fo == 2.5f, "addr32 flds: %f", fo);
    unsigned long pv = 0;
    memcpy(low + 72, &(uint64_t) {0x1122334455667788ull}, 8);
    __asm__ volatile("pushq 72(%k1)\n popq %0" : "=r"(pv) : "r"(junk) : "memory");
    CHECK(pv == 0x1122334455667788ul, "addr32 push [mem]: %#lx", pv);
    uint64_t fp = (uintptr_t) target;
    memcpy(low + 80, &fp, 8);
    called = 0;
    __asm__ volatile("call *80(%k0)" :: "r"(junk) : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory");
    CHECK(called == 1, "addr32 call [mem]: called %d", called);
    memcpy(low + 88, &(uint32_t) {7}, 4);
    unsigned old = 7;
    __asm__ volatile("lock cmpxchgl %2, 88(%k1)" : "+a"(old) : "r"(junk), "r"(9u) : "memory", "cc");
    CHECK(low[88] == 9 && old == 7, "addr32 lock cmpxchg: %u", low[88]);
    unsigned be = 0;
    __asm__ volatile("movbe (%k1), %0" : "=r"(be) : "r"(junk) : "memory");
    CHECK(be == 0x00010203, "addr32 movbe: %#x", be);
    uint8_t *area = low + 1024;                          // 16-aligned
    unsigned long jarea = 0x5a5a00000000ul | (uintptr_t) area;
    __asm__ volatile("fxsave (%k0)" :: "r"(jarea) : "memory");
    CHECK(area[0] | area[1], "addr32 fxsave wrote nothing");

    // the string instructions and XLAT take EDI/ESI/ECX/EBX
    unsigned long rdi = 0xbeef00000000ul | ((uintptr_t) low + 100), rcx = 0xffff000000000004ul;
    __asm__ volatile("mov $0x5a, %%al\n addr32 rep stosb" : "+D"(rdi), "+c"(rcx) :: "rax", "memory");
    CHECK(low[100] == 0x5a && low[103] == 0x5a && low[104] == 104, "addr32 rep stosb: %u %u", low[103], low[104]);
    // (EDI and ECX written as 32-bit registers: their upper halves zero)
    CHECK(rdi == (uintptr_t) low + 104 && rcx == 0, "addr32 rep stosb: rdi %#lx rcx %#lx", rdi, rcx);
    unsigned long rbx = 0xabcd00000000ul | (uintptr_t) low, rax = 0xff00000000000040ul;
    __asm__ volatile("addr32 xlat" : "+a"(rax) : "b"(rbx) : "memory");
    CHECK((rax & 0xff) == 0x40 && (rax >> 8) == 0xff000000000000ul, "addr32 xlat: %#lx", rax);
    printf("amd64_addr32: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
