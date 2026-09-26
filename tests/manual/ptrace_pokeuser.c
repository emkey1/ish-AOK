// PTRACE_POKEUSER, and the per-register rules SETREGS shares with it.
//
// iSH-AOK had no PTRACE_POKEUSER at all, on any ABI: the request fell through
// to the default and failed. gdb writes the debug registers through it (its
// hardware watchpoints) and some tools poke single registers. SETREGS, for its
// part, took any fs_base and gs_base and any eflags bit, and a bad selector was
// dropped silently rather than refused.
//
// Linux (arch/x86/kernel/ptrace.c) writes a user-area word through putreg --
// the path SETREGS takes for every word, in struct order, stopping at the first
// refusal:
//  - a selector is its low 16 bits, and EIO unless null or RPL 3; a null CS or
//    SS is EIO;
//  - eflags changes only CF PF AF ZF SF TF DF OF NT RF AC (0x54dd5, measured:
//    writing every bit gives 0x54fd7, none 0x202); the rest, IF among them,
//    stays as it was;
//  - fs_base and gs_base at or above TASK_SIZE_MAX are EIO;
//  - an unaligned offset, one past the user area, or (x86_64) one between the
//    registers and u_debugreg is EIO; on i386 a word elsewhere in struct user
//    is accepted and ignored -- and so, measured, is the word at 284, just past
//    its 284 bytes (compat's bound is one word generous); 288 is EIO;
//  - u_debugreg: DR4 and DR5 are EIO, DR0-3 take an address, DR6 and DR7 a
//    value, and each reads back what was written.
//
// One difference, on purpose: iSH-AOK has no hardware breakpoints, so a DR7
// that would arm one is ENOSPC -- Linux's own answer when every debug register
// is taken -- and gdb says "Couldn't write debug register" instead of placing a
// watchpoint that would never fire. Every other assertion here holds on Linux
// 6.12 (camd, gcc and gcc -m32) as written.

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#if defined(__x86_64__) || defined(__i386__)

typedef unsigned long word;   // a user-area word: 8 bytes on x86_64, 4 on i386

static pid_t child;
static int on_ish;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%#lx want=%#lx\n", label, (unsigned long) got, (unsigned long) want);
}

// 0, or -errno.
static long poke(size_t offset, word value) {
    errno = 0;
    long r = ptrace(PTRACE_POKEUSER, child, (void *) offset, (void *) value);
    return r < 0 ? -errno : 0;
}

static word peek(size_t offset) {
    errno = 0;
    word v = (word) ptrace(PTRACE_PEEKUSER, child, (void *) offset, NULL);
    if (errno != 0)
        failf("PEEKUSER", (uint64_t) offset, errno, 0, 0, 0, 0);
    return v;
}

#define REG(field) offsetof(struct user_regs_struct, field)
#define DR(n) (offsetof(struct user, u_debugreg) + (n) * sizeof(word))
#define FLAG_MASK 0x54dd5UL   // CF PF AF ZF SF TF DF OF NT RF AC
#define EFLAGS_IF 0x200UL

static void check_common(size_t ds, size_t cs, size_t ss, size_t eflags, size_t gpr) {
    ck("an ordinary register takes the word", poke(gpr, 0x1234567), 0);
    ck("  and reads it back", (long) peek(gpr), 0x1234567);
    ck("an unaligned offset is EIO", poke(gpr + 1, 0), -EIO);

    ck("a selector with RPL 0 is EIO", poke(ds, 0x28), -EIO);
    ck("a selector with RPL 3 is taken", poke(ds, 0x2b), 0);
    ck("  and reads back", (long) peek(ds), 0x2b);
    ck("a selector is its low 16 bits", poke(ds, 0x1002b), 0);
    ck("  so it reads back as them", (long) peek(ds), 0x2b);
    ck("a null DS is taken", poke(ds, 0), 0);
    ck("a null CS is EIO", poke(cs, 0), -EIO);
    ck("a null SS is EIO", poke(ss, 0), -EIO);
    ck("the CS it has is taken", poke(cs, peek(cs)), 0);
    ck("the SS it has is taken", poke(ss, peek(ss)), 0);

    word before = peek(eflags);
    ck("eflags takes a write of every bit", poke(eflags, ~0UL), 0);
    word after = peek(eflags);
    ck("  and sets every bit FLAG_MASK covers", (long) (after & FLAG_MASK), (long) FLAG_MASK);
    ck("  and no other", (long) (after & ~FLAG_MASK), (long) (before & ~FLAG_MASK));
    ck("eflags takes a write of none", poke(eflags, 0), 0);
    after = peek(eflags);
    ck("  and clears FLAG_MASK's", (long) (after & FLAG_MASK), 0);
    ck("  and leaves IF set", (long) (after & EFLAGS_IF), (long) EFLAGS_IF);
    poke(eflags, before);
}

static void check_debugregs(void) {
    ck("DR7 reads 0 with nothing armed", (long) peek(DR(7)), 0);
    ck("DR0 takes an address", poke(DR(0), 0x1000), 0);
    ck("  and reads it back", (long) peek(DR(0)), 0x1000);
    ck("DR4 is EIO", poke(DR(4), 0), -EIO);
    ck("DR5 is EIO", poke(DR(5), 0), -EIO);
#ifdef __x86_64__
    ck("DR0 at TASK_SIZE_MAX is EINVAL", poke(DR(0), 0x7ffffffff000UL), -EINVAL);
    ck("  and leaves DR0 as it was", (long) peek(DR(0)), 0x1000);
#endif
    ck("DR6 takes a value", poke(DR(6), 0xffff0ff0UL), 0);
    ck("  and reads it back", (long) peek(DR(6)), (long) 0xffff0ff0UL);
    ck("DR7 takes type and length bits with nothing enabled", poke(DR(7), 0xd0000UL), 0);
    ck("  and reads them back", (long) peek(DR(7)), 0xd0000L);
    ck("DR7 takes 0", poke(DR(7), 0), 0);
    // L0, then G0: arm DR0 as a one-byte execute breakpoint.
    ck(on_ish ? "DR7 arming a breakpoint (L0) is ENOSPC (none on iSH-AOK)"
              : "DR7 arming a breakpoint (L0) is taken",
       poke(DR(7), 1), on_ish ? -ENOSPC : 0);
    ck(on_ish ? "DR7 arming a breakpoint (G0) is ENOSPC (none on iSH-AOK)"
              : "DR7 arming a breakpoint (G0) is taken",
       poke(DR(7), 2), on_ish ? -ENOSPC : 0);
    ck("DR7 back to 0 is taken", poke(DR(7), 0), 0);
    ck("DR0 back to 0 is taken", poke(DR(0), 0), 0);
    ck("  and reads it back", (long) peek(DR(0)), 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    struct stat st;
    on_ish = stat("/proc/ish", &st) == 0;

    child = fork();
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        for (;;)
            pause();
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
        printf("ptrace_pokeuser: FAIL (the child did not stop)\n");
        return 1;
    }

#ifdef __x86_64__
    check_common(REG(ds), REG(cs), REG(ss), REG(eflags), REG(rbx));
    ck("a word between the registers and u_debugreg is EIO",
       poke(sizeof(struct user_regs_struct), 0), -EIO);
    ck("a word past the user area is EIO", poke(sizeof(struct user), 0), -EIO);
    ck("orig_rax takes the word", poke(REG(orig_rax), 42), 0);
    ck("  and reads it back", (long) peek(REG(orig_rax)), 42);

    const word kernel_half = 0xffff800000000000UL;
    const word task_size_max = 0x7ffffffff000UL;   // 4-level paging
    word fs = peek(REG(fs_base)), gs = peek(REG(gs_base));
    ck("an fs_base in the kernel half is EIO", poke(REG(fs_base), kernel_half), -EIO);
    ck("an fs_base at TASK_SIZE_MAX is EIO", poke(REG(fs_base), task_size_max), -EIO);
    ck("an fs_base just below it is taken", poke(REG(fs_base), task_size_max - 0x1000), 0);
    ck("  and reads it back", (long) peek(REG(fs_base)), (long) (task_size_max - 0x1000));
    ck("a gs_base in the kernel half is EIO", poke(REG(gs_base), kernel_half), -EIO);
    ck("a gs_base just below TASK_SIZE_MAX is taken", poke(REG(gs_base), task_size_max - 0x1000), 0);
    poke(REG(fs_base), fs);
    poke(REG(gs_base), gs);

    // SETREGS: struct order, stopping at the first refusal -- rbx (early) is
    // set, the DS after the refused fs_base is not.
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, child, NULL, &regs);
    word ds = regs.ds;
    regs.rbx = 0x55;
    regs.fs_base = kernel_half;
    regs.ds = 0x2b;
    errno = 0;
    long r = ptrace(PTRACE_SETREGS, child, NULL, &regs);
    ck("SETREGS with a bad fs_base is EIO", r < 0 ? -errno : r, -EIO);
    ptrace(PTRACE_GETREGS, child, NULL, &regs);
    ck("  having set what came before it", (long) regs.rbx, 0x55);
    ck("  and not what came after", (long) regs.ds, (long) ds);
    ck("  and not the fs_base", (long) regs.fs_base, (long) fs);
#else
    check_common(REG(xds), REG(xcs), REG(xss), REG(eflags), REG(ebx));
    ck("a word elsewhere in struct user is taken and ignored",
       poke(offsetof(struct user, u_comm), 7), 0);
    ck("the word just past struct user is taken, as compat has it",
       poke(sizeof(struct user), 0), 0);
    ck("  and the one after it is EIO", poke(sizeof(struct user) + 4, 0), -EIO);
    ck("orig_eax takes the word", poke(REG(orig_eax), 42), 0);
    ck("  and reads it back", (long) peek(REG(orig_eax)), 42);
#endif
    check_debugregs();

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return finish_suite("ptrace_pokeuser");
}

#else
int main(void) {
    printf("ptrace_pokeuser: SKIP (x86 only: arm64 and riscv64 have no user area)\n");
    return 0;
}
#endif
