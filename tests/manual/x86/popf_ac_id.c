// popf_ac_id.c -- a user-mode POPF changes EFLAGS.AC (bit 18) and EFLAGS.ID
// (bit 21), and a 16-bit POPF leaves them alone.
//
// Toggling those two bits is how a program tells a 386 (AC fixed) and a 486
// (ID fixed) from a CPU with CPUID, and HotSpot does exactly that before it
// will read CPUID at all. The amd64 engine masked every POPF down to the
// arithmetic flags and DF, so both bits read back unchanged, HotSpot decided it
// was on a 386, and `java -version` died with "Unknown x64 processor: SSE2 not
// supported". The i386 engine already let them through.
//
// The 16-bit POPF check runs on amd64 only: the i386 engine ignores the
// operand-size prefix on PUSHF/POPF altogether (four bytes, all of EFLAGS),
// which is a separate gap.
#include <stdint.h>
#include "../test_common.h"

#define AC (1ul << 18)
#define ID (1ul << 21)

// Flip `bit` with a full-width POPF and report which bits of EFLAGS changed.
static unsigned long flip(unsigned long bit) {
    unsigned long before, after;
#if defined(__x86_64__)
    __asm__ volatile("pushfq; popq %0; movq %0, %%rax; xorq %2, %%rax; pushq %%rax; popfq;"
                     "pushfq; popq %1; pushq %0; popfq"
                     : "=&r"(before), "=&r"(after) : "r"(bit) : "rax", "cc", "memory");
#else
    __asm__ volatile("pushfl; popl %0; movl %0, %%eax; xorl %2, %%eax; pushl %%eax; popfl;"
                     "pushfl; popl %1; pushl %0; popfl"
                     : "=&r"(before), "=&r"(after) : "r"(bit) : "eax", "cc", "memory");
#endif
    return (before ^ after) & (AC | ID);
}

#if defined(__x86_64__)
// Set ID with a full-width POPF, then run a 16-bit POPF of 0x0002 (every
// arithmetic flag clear), and return the full EFLAGS that follows. ID is
// above bit 15, so it must survive.
static unsigned long popfw_keeps_high(void) {
    unsigned long saved, after;
    __asm__ volatile("pushfq; popq %0; movq %0, %%rax; orq %2, %%rax; pushq %%rax; popfq;"
                     "pushw $2; popfw; pushfq; popq %1; pushq %0; popfq"
                     : "=&r"(saved), "=&r"(after) : "r"(ID) : "rax", "cc", "memory");
    (void) saved;
    return after;
}
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);
    unsigned long ac = flip(AC), id = flip(ID);
    test_logf("AC toggles: %s, ID toggles: %s\n", ac == AC ? "yes" : "no", id == ID ? "yes" : "no");
    if (ac != AC)
        failf("popf AC", ac, 0, 0, AC, 0, 0);
    if (id != ID)
        failf("popf ID", id, 0, 0, ID, 0, 0);
#if defined(__x86_64__)
    unsigned long f = popfw_keeps_high();
    test_logf("after popfw: %#lx\n", f);
    if (!(f & ID))
        failf("popfw kept ID", f & ID, 0, 0, ID, 0, 0);
    if (f & 0xcd5)
        failf("popfw cleared the arithmetic flags", f & 0xcd5, 0, 0, 0, 0, 0);
#endif
    return finish_suite("popf_ac_id");
}
