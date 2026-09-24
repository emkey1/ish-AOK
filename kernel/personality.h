#ifndef KERNEL_PERSONALITY_H
#define KERNEL_PERSONALITY_H

#define ADDR_NO_RANDOMIZE_ 0x0040000
// Memory that can be read can be executed: the NX-less world a binary from
// before PT_GNU_STACK expects. Set by exec for an i386 binary with no
// PT_GNU_STACK, as Linux's elf_read_implies_exec does, and by personality(2);
// mmap, mprotect, brk and shmat then add PROT_EXEC to anything readable.
#define READ_IMPLIES_EXEC_ 0x0400000
// Linux's PER_CLEAR_ON_SETID: a set-id exec drops what would weaken the
// privileged program -- READ_IMPLIES_EXEC, ADDR_NO_RANDOMIZE,
// ADDR_COMPAT_LAYOUT, MMAP_PAGE_ZERO -- or its caller could hand it an
// executable heap or a predictable layout.
#define PER_CLEAR_ON_SETID_ 0x0740000

// /proc/sys/kernel/randomize_va_space: 0 lays every process out the same way
// each time, 1 randomizes the stack, the mmap base (so the libraries, the
// loader and every anonymous mapping), the PIE base and the vDSO, and 2 -- the
// default, as on Linux -- the heap's start as well. ISH_RANDOMIZE_VA_SPACE in
// the host environment sets the starting value. Defined in kernel/exec.c.
int aslr_randomize_va_space(void);
void aslr_set_randomize_va_space(int value);

#endif /* KERNEL_PERSONALITY_H */
