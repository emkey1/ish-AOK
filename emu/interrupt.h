// Intel standard interrupts
// Any interrupt not handled specially becomes a SIGSEGV
#define INT_NONE -1
#define INT_DIV 0
#define INT_DEBUG 1
#define INT_NMI 2
#define INT_BREAKPOINT 3
#define INT_OVERFLOW 4
#define INT_BOUND 5
#define INT_UNDEFINED 6
#define INT_FPU 7 // do not try to use the fpu. instead, try to realize the truth: there is no fpu.
#define INT_DOUBLE 8 // interrupt during interrupt, i.e. interruptception
#define INT_GPF 13
#define INT_PF 14
#define INT_TIMER 32
#define INT_SYSCALL 0x80
// Synthetic interrupt used for privileged instructions that should fault in
// userspace as #GP but are not memory faults.
#define INT_PRIV 0x100
// Synthetic interrupt for amd64 SYSCALL entry. This is separate from int 0x80
// so the kernel can select the amd64 syscall ABI and preserve rcx/r11 entry
// semantics without disturbing the legacy i386 path.
#define INT_AMD64_SYSCALL 0x101
// Synthetic interrupt for AArch64 SVC (syscall) entry. Not wired to a
// syscall table yet (aarch64_guest_plan.md patch 4) — for now this lets the
// interpreter and its tests observe that an SVC instruction was correctly
// reached and decoded without a real dispatch target existing.
#define INT_ARM64_SVC 0x102
// Synthetic interrupt for a HOST bad-access (SIGBUS) taken while the JIT was
// executing a guest memory access whose backing host page is no longer valid --
// e.g. a file-backed guest mmap (tmpfs host_fd or realfs real_fd) whose backing
// file was ftruncate'd smaller under a live mapping. The host fault is caught
// (POSIX SIGBUS on the CLI, EXC_BAD_ACCESS via Mach on device), reverse-mapped
// to the guest address, and reported here so the kernel delivers a guest SIGBUS
// (BUS_ADRERR) exactly like Linux, instead of the emulator process dying. This
// is distinct from INT_PF: the guest page is validly mapped in iSH's page table
// (only the host backing shrank), so it must NOT retry through mem_ptr_fault.
#define INT_BUS 0x103
// Synthetic interrupt for RISC-V ECALL (syscall) entry. Not wired to a
// syscall table yet (riscv64_guest_plan.md patch 4) — same staging as
// INT_ARM64_SVC above.
#define INT_RISCV64_ECALL 0x104
// Synthetic interrupt for an instruction fetch from a page that is mapped but
// not executable (tlb_fetch): SIGSEGV with SEGV_ACCERR at segfault_addr, never
// retried as a read. Chosen when the fault is emitted -- for a JIT, when the
// block is translated -- because a read of that page would succeed, and the
// ordinary fault path's retry would only run into the same fetch again.
#define INT_PF_EXEC 0x105
