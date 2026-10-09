// amd64_addr32.c -- the 0x67 address-size prefix in 64-bit code. Where an
// instruction has no effective address it does nothing, and toolchains rely
// on that: the linker relaxes `call *foo@GOTPCREL(%rip)` to `addr32 call foo`
// (spelled in bytes here: gas drops the prefix from CALL and JMP; a 32-bit
// base register, `(%k1)`, is how gas spells it on a memory operand)
// and OpenSSL's perlasm pads register-only instructions with it. Where there
// is one, the address is computed in 32 bits and zero-extended: a pointer
// with junk above bit 31 reaches the low 4 GB, and a segment base is added
// after the truncation. Checked on an AMD Ryzen (camd); the EVEX forms
// under SDE (camd has no AVX-512).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cpuid.h>
#include <asm/prctl.h>

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

    // MASKMOVDQU's [rdi] is [edi]; a VEX gather's element addresses are 32 bits
    uint8_t mdata[16], mmask[16];
    for (int i = 0; i < 16; i++) { mdata[i] = (uint8_t) (0xa0 + i); mmask[i] = i & 1 ? 0x80 : 0; }
    unsigned long mdi = 0x7777000000000000ul | ((uintptr_t) low + 300);
    __asm__ volatile("movdqu %1, %%xmm5\n movdqu %2, %%xmm6\n .byte 0x67\n maskmovdqu %%xmm6, %%xmm5"
                     :: "D"(mdi), "m"(mdata), "m"(mmask) : "xmm5", "xmm6", "memory");
    CHECK(low[300] == 0 && low[301] == 0xa1 && low[315] == 0xaf, "addr32 maskmovdqu: %#x %#x", low[300], low[301]);
    uint32_t gidx[8] = {0, 1, 2, 3, 4, 5, 6, 7}, gout[8];
    for (int i = 0; i < 8; i++)
        memcpy(low + 512 + 4 * i, &(uint32_t) {100u + (unsigned) i}, 4);
    __asm__ volatile("vmovdqu %1, %%ymm7\n vpcmpeqd %%ymm8, %%ymm8, %%ymm8\n vpgatherdd %%ymm8, 512(%k2,%%ymm7,4), %%ymm9\n"
                     " vmovdqu %%ymm9, %0\n vzeroupper" : "=m"(gout) : "m"(gidx), "r"(junk) : "xmm7", "xmm8", "xmm9", "memory");
    CHECK(gout[0] == 100 && gout[7] == 107, "addr32 vpgatherdd: %u %u", gout[0], gout[7]);
    // ... and with a segment override: the 32-bit truncation comes first, then
    // the segment base (GS here, set by arch_prctl): base + index * scale +
    // disp wraps at 4 GB, and the base is added to that
    unsigned long gsb = 0x1000;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, gsb) == 0) {
        unsigned long gbase = 0x3333000000000000ul | (uint32_t) ((uintptr_t) low - gsb);
        uint32_t gout2[8] = {0};
        __asm__ volatile("vmovdqu %1, %%ymm7\n vpcmpeqd %%ymm8, %%ymm8, %%ymm8\n vpgatherdd %%ymm8, %%gs:512(%k2,%%ymm7,4), %%ymm9\n"
                         " vmovdqu %%ymm9, %0\n vzeroupper" : "=m"(gout2) : "m"(gidx), "r"(gbase) : "xmm7", "xmm8", "xmm9", "memory");
        CHECK(gout2[0] == 100 && gout2[7] == 107, "addr32 gs: vpgatherdd: %u %u", gout2[0], gout2[7]);
        unsigned has512;
        { unsigned a7, b7, c7, d7; __cpuid_count(7, 0, a7, b7, c7, d7); has512 = (b7 >> 16) & 1; }
        if (has512) {
            uint32_t eout[8] = {0}, sval[8] = {9, 8, 7, 6, 5, 4, 3, 2};
            __asm__ volatile("vmovdqu %1, %%ymm7\n kxnorb %%k0, %%k0, %%k1\n vpgatherdd %%gs:512(%k2,%%ymm7,4), %%ymm9%{%%k1%}\n"
                             " vmovdqu %%ymm9, %0\n vzeroupper" : "=m"(eout) : "m"(gidx), "r"(gbase) : "xmm7", "xmm9", "memory");   // (k1: unused without -mavx512f)
            CHECK(eout[0] == 100 && eout[7] == 107, "addr32 gs: evex vpgatherdd: %u %u", eout[0], eout[7]);
            __asm__ volatile("vmovdqu %0, %%ymm7\n vmovdqu %1, %%ymm9\n kxnorb %%k0, %%k0, %%k1\n"
                             " vpscatterdd %%ymm9, %%gs:768(%k2,%%ymm7,4)%{%%k1%}\n vzeroupper"
                             :: "m"(gidx), "m"(sval), "r"(gbase) : "xmm7", "xmm9", "memory");   // (k1: unused without -mavx512f)
            uint32_t sv0, sv7;
            memcpy(&sv0, low + 768, 4);
            memcpy(&sv7, low + 768 + 28, 4);
            CHECK(sv0 == 9 && sv7 == 2, "addr32 gs: evex vpscatterdd: %u %u", sv0, sv7);
        }
        syscall(SYS_arch_prctl, ARCH_SET_GS, 0ul);
    } else {
        CHECK(0, "arch_prctl(ARCH_SET_GS) failed");
    }
    // 0F 01's register forms: 0x67 inert (XGETBV)
    unsigned xlo, xhi;
    __asm__ volatile(".byte 0x67, 0x0f, 0x01, 0xd0" : "=a"(xlo), "=d"(xhi) : "c"(0));
    CHECK(xlo & 3, "addr32 xgetbv: %#x", xlo);

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
