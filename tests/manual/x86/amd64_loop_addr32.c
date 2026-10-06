// amd64_loop_addr32.c -- LOOP, LOOPE, LOOPNE and JECXZ with the 0x67 prefix
// (67 E0-E3), which count in ECX: the loop runs ECX times whatever RCX's upper
// half holds, the decrement is a 32-bit write (RCX's upper half cleared),
// LOOPE/LOOPNE also stop on ZF, and JECXZ jumps when ECX alone is zero. The
// gadgets are jit/gadgets-aarch64/control.S's amd64_loop32/loope32/loopne32/
// amd64_jecxz.
#include <stdint.h>
#include <stdio.h>

static unsigned long checks, bad;
static void check(const char *what, uint64_t in, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 30)
        printf("FAIL %s (%#llx): %#llx, want %#llx\n", what, (unsigned long long) in,
               (unsigned long long) got, (unsigned long long) want);
}

int main(void) {
    static const uint32_t counts[] = {1, 2, 3, 7, 100, 1000};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint64_t rcx = 0xabcd000000000000ull | counts[i], iters = 0;
        // addr32 loop: body increments iters
        __asm__ volatile("1: inc %1\n .byte 0x67\n loop 1b" : "+c"(rcx), "+r"(iters) :: "cc");
        check("67 loop: ECX iterations", counts[i], iters, counts[i]);
        check("67 loop: RCX upper half cleared", counts[i], rcx, 0);
    }
    // LOOPE: stops when ZF clears (cmp against a value reached at iteration 5)
    {
        uint64_t rcx = 0xff00000000000010ull, n = 0;
        __asm__ volatile("1: inc %1\n cmp $5, %1\n setne %%al\n test %%al, %%al\n .byte 0x67\n loope 1b"
                         : "+c"(rcx), "+r"(n) :: "rax", "cc");
        // ZF = (n != 5 test) -> al = 1 while n != 5 -> test al,al gives ZF=0 -> stops at once
        check("67 loope: stops when ZF is clear", 0, n, 1);
        check("67 loope: ECX decremented once", 0, rcx, 0xf);
    }
    {
        uint64_t rcx = 0xff00000000000010ull, n = 0;
        __asm__ volatile("1: inc %1\n cmp $5, %1\n .byte 0x67\n loopne 1b"
                         : "+c"(rcx), "+r"(n) :: "cc");
        check("67 loopne: stops when ZF is set", 0, n, 5);
        check("67 loopne: ECX counted down by 5", 0, rcx, 0x10 - 5);
    }
    {
        uint64_t rcx = 0xff00000000000003ull, n = 0;
        __asm__ volatile("1: inc %1\n cmp %1, %1\n .byte 0x67\n loope 1b" : "+c"(rcx), "+r"(n) :: "cc");
        check("67 loope: runs out on ECX", 0, n, 3);
    }
    // JECXZ: ECX zero with RCX's upper half set -> taken; ECX nonzero -> not
    for (int k = 0; k < 2; k++) {
        uint64_t rcx0 = k ? 0x0000000100000000ull : 0x0000000100000001ull, rcx = rcx0, taken = 0;
        __asm__ volatile(".byte 0x67\n jrcxz 1f\n jmp 2f\n 1: mov $1, %0\n 2:" : "+r"(taken), "+c"(rcx));
        check(k ? "jecxz: ECX 0 (RCX not) taken" : "jecxz: ECX 1 not taken", rcx0, taken, k ? 1 : 0);
        check("jecxz: RCX untouched", rcx0, rcx, rcx0);
    }
    printf("amd64_loop_addr32: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
