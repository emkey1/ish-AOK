// popf_ac_id.c -- what a user-mode PUSHF/POPF may change, in both operand
// sizes, on both x86 guests.
//
// A full-width POPF changes EFLAGS.AC (bit 18) and EFLAGS.ID (bit 21).
// Toggling those two bits is how a program tells a 386 (AC fixed) and a 486
// (ID fixed) from a CPU with CPUID, and HotSpot does exactly that before it
// will read CPUID at all. The amd64 engine masked every POPF down to the
// arithmetic flags and DF, so both bits read back unchanged, HotSpot decided it
// was on a 386, and `java -version` died with "Unknown x64 processor: SSE2 not
// supported".
//
// The i386 engine had the opposite problem. It ignored the operand-size prefix
// on PUSHF/POPF, so PUSHFW and POPFW moved ESP by four, and POPF stored every
// popped bit, so IF, IOPL and the reserved bits read back as whatever was
// popped. Linux lets a user-mode POPF change only CF PF AF ZF SF TF DF OF NT AC
// ID, and a 16-bit POPF only the ones in the low word.
//
// TF and NT are not exercised: TF single-steps the process, and the two
// engines deliberately differ on both (the amd64 one does not let POPF set
// them).
#include <stdint.h>
#include "../test_common.h"

#define AC (1ul << 18)
#define ID (1ul << 21)
#define IF (1ul << 9)
#define IOPL (3ul << 12)
// Not writable from user mode: RF, VM, VIF, VIP, and the reserved bits 3, 5,
// 15 and 22-31. Bit 1 is reserved too, but reads as one.
#define FIXED_ZERO ((1ul << 3) | (1ul << 5) | (1ul << 15) | (1ul << 16) | \
                    (1ul << 17) | (1ul << 19) | (1ul << 20) | 0xffc00000ul)

// On x86_64 the asm below pushes into the compiler's red zone; step over it.
#if defined(__x86_64__)
#define ENTER "leaq -128(%%rsp), %%rsp;"
#define LEAVE "leaq 128(%%rsp), %%rsp;"
#else
#define ENTER
#define LEAVE
#endif

// Flip `bit` with a full-width POPF and report which bits of EFLAGS changed.
static unsigned long flip(unsigned long bit) {
    unsigned long before, after;
#if defined(__x86_64__)
    __asm__ volatile(ENTER "pushfq; popq %0; movq %0, %%rax; xorq %2, %%rax; pushq %%rax; popfq;"
                     "pushfq; popq %1; pushq %0; popfq;" LEAVE
                     : "=&r"(before), "=&r"(after) : "r"(bit) : "rax", "cc", "memory");
#else
    __asm__ volatile("pushfl; popl %0; movl %0, %%eax; xorl %2, %%eax; pushl %%eax; popfl;"
                     "pushfl; popl %1; pushl %0; popfl"
                     : "=&r"(before), "=&r"(after) : "r"(bit) : "eax", "cc", "memory");
#endif
    return (before ^ after) & (AC | ID);
}

// Set ID with a full-width POPF, then run a 16-bit POPF of 0x0002 (every
// arithmetic flag clear), and return the full EFLAGS that follows. ID is
// above bit 15, so it must survive.
static unsigned long popfw_keeps_high(void) {
    unsigned long saved, after;
#if defined(__x86_64__)
    __asm__ volatile(ENTER "pushfq; popq %0; movq %0, %%rax; orq %2, %%rax; pushq %%rax; popfq;"
                     "pushw $2; popfw; pushfq; popq %1; pushq %0; popfq;" LEAVE
                     : "=&r"(saved), "=&r"(after) : "r"(ID) : "rax", "cc", "memory");
#else
    __asm__ volatile("pushfl; popl %0; movl %0, %%eax; orl %2, %%eax; pushl %%eax; popfl;"
                     "pushw $2; popfw; pushfl; popl %1; pushl %0; popfl"
                     : "=&r"(saved), "=&r"(after) : "r"(ID) : "eax", "cc", "memory");
#endif
    (void) saved;
    return after;
}

// PUSHFW then POPFW: how far each moved the stack pointer, and what PUSHFW
// stored beside the low word of a full PUSHF taken just before it.
struct fw { unsigned long push_delta, pop_delta, word, full; };

static struct fw pushfw_popfw(void) {
    unsigned long sp0, sp1, sp2, word, full;
#if defined(__x86_64__)
    __asm__ volatile(ENTER "pushfq; popq %4; movq %%rsp, %0; pushfw; movq %%rsp, %1;"
                     "movzwq (%%rsp), %3; popfw; movq %%rsp, %2;" LEAVE
                     : "=&r"(sp0), "=&r"(sp1), "=&r"(sp2), "=&r"(word), "=&r"(full)
                     : : "cc", "memory");
#else
    __asm__ volatile("pushfl; popl %4; movl %%esp, %0; pushfw; movl %%esp, %1;"
                     "movzwl (%%esp), %3; popfw; movl %%esp, %2"
                     : "=&r"(sp0), "=&r"(sp1), "=&r"(sp2), "=&r"(word), "=&r"(full)
                     : : "cc", "memory");
#endif
    return (struct fw) { sp0 - sp1, sp2 - sp1, word, full };
}

// A full-width POPF of `value`, and the EFLAGS that follows. The caller's
// flags come back afterwards.
static unsigned long popf_readback(unsigned long value) {
    unsigned long saved, after;
#if defined(__x86_64__)
    __asm__ volatile(ENTER "pushfq; popq %0; pushq %2; popfq; pushfq; popq %1; pushq %0; popfq;" LEAVE
                     : "=&r"(saved), "=&r"(after) : "r"(value) : "cc", "memory");
#else
    __asm__ volatile("pushfl; popl %0; pushl %2; popfl; pushfl; popl %1; pushl %0; popfl"
                     : "=&r"(saved), "=&r"(after) : "r"(value) : "cc", "memory");
#endif
    (void) saved;
    return after;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    unsigned long ac = flip(AC), id = flip(ID);
    test_logf("AC toggles: %s, ID toggles: %s\n", ac == AC ? "yes" : "no", id == ID ? "yes" : "no");
    if (ac != AC)
        failf("popf AC", ac, 0, 0, AC, 0, 0);
    if (id != ID)
        failf("popf ID", id, 0, 0, ID, 0, 0);

    unsigned long f = popfw_keeps_high();
    test_logf("after popfw: %#lx\n", f);
    if (!(f & ID))
        failf("popfw kept ID", f & ID, 0, 0, ID, 0, 0);
    if (f & 0xcd5)
        failf("popfw cleared the arithmetic flags", f & 0xcd5, 0, 0, 0, 0, 0);

    struct fw fw = pushfw_popfw();
    test_logf("pushfw moved sp by %lu, popfw by %lu; pushfw stored %#lx, pushf %#lx\n",
              fw.push_delta, fw.pop_delta, fw.word, fw.full);
    if (fw.push_delta != 2)
        failf("pushfw sp delta", fw.push_delta, 0, 0, 2, 0, 0);
    if (fw.pop_delta != 2)
        failf("popfw sp delta", fw.pop_delta, 0, 0, 2, 0, 0);
    // Only the flags an intervening MOV cannot touch: all of them, since the
    // two reads have nothing but MOVs between them.
    if (fw.word != (fw.full & 0xffff))
        failf("pushfw stored the low word", fw.word, 0, 0, fw.full & 0xffff, 0, 0);

    // Ask for IF clear, IOPL 3 and every fixed-zero bit set, arithmetic flags
    // clear. Only the arithmetic flags may move.
    unsigned long r = popf_readback(0x2 | IOPL | FIXED_ZERO);
    test_logf("popf of %#lx reads back %#lx\n", 0x2 | IOPL | FIXED_ZERO, r);
    if (!(r & IF))
        failf("popf left IF set", r & IF, 0, 0, IF, 0, 0);
    if (r & IOPL)
        failf("popf left IOPL at 0", r & IOPL, 0, 0, 0, 0, 0);
    if (r & FIXED_ZERO)
        failf("popf left the fixed bits clear", r & FIXED_ZERO, 0, 0, 0, 0, 0);
    if (!(r & 0x2))
        failf("bit 1 reads as one", r & 0x2, 0, 0, 0x2, 0, 0);
    if (r & 0xcd5)
        failf("popf cleared the arithmetic flags", r & 0xcd5, 0, 0, 0, 0, 0);
    return finish_suite("popf_ac_id");
}
