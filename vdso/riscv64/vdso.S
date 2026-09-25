// The riscv64 vDSO: vdso/arm64/vdso.S's functions for a riscv64 guest,
// under the names and version (LINUX_4.15) its C libraries look up. The
// arm64 file says why a vDSO, and why it asks the JIT for the time instead
// of computing it from a counter and a page of numbers.
//
// AOK_VCLOCK here is a CSR only the JIT implements (jit/gen.c,
// riscv64_csr_helper), in the range the ISA reserves for custom read-only
// user CSRs, where a real core raises an illegal instruction:
// `csrrs rd, 0xcc0, rs1` puts in rd the reading of the clock rs1 names, in
// nanoseconds -- kernel/time.c vdso_clock_ns, the computation clock_gettime(2)
// itself makes -- or a value with bit 63 set when the system call has to
// answer instead, which the fallbacks below then make. csrrs with a nonzero
// rs1 would SET bits of an ordinary CSR; this one is read-only, so rs1 is
// free to carry the argument.
//
// Leaf functions, no stack, caller-saved registers only.

#define AOK_VCLOCK 0xcc0

#define NR_clock_gettime 113
#define NR_clock_getres 114
#define NR_rt_sigreturn 139
#define NR_gettimeofday 169

    .text

// int __vdso_clock_gettime(clockid_t clock, struct timespec *ts)
    .balign 16
    .global __vdso_clock_gettime
    .type __vdso_clock_gettime, %function
__vdso_clock_gettime:
    .cfi_startproc
    csrrs   a2, AOK_VCLOCK, a0
    bltz    a2, 1f
    li      a3, 1000000000
    divu    a4, a2, a3
    remu    a5, a2, a3
    sd      a4, 0(a1)
    sd      a5, 8(a1)
    li      a0, 0
    ret
1:  li      a7, NR_clock_gettime
    ecall
    ret
    .cfi_endproc
    .size __vdso_clock_gettime, . - __vdso_clock_gettime

// int __vdso_gettimeofday(struct timeval *tv, struct timezone *tz)
//
// A timezone is the kernel's to fill, so asking for one takes the system call.
    .balign 16
    .global __vdso_gettimeofday
    .type __vdso_gettimeofday, %function
__vdso_gettimeofday:
    .cfi_startproc
    bnez    a1, 2f
    beqz    a0, 1f
    csrrs   a2, AOK_VCLOCK, zero            // CLOCK_REALTIME
    bltz    a2, 2f
    li      a3, 1000000000
    divu    a4, a2, a3
    remu    a5, a2, a3
    li      a3, 1000
    divu    a5, a5, a3
    sd      a4, 0(a0)
    sd      a5, 8(a0)
1:  li      a0, 0
    ret
2:  li      a7, NR_gettimeofday
    ecall
    ret
    .cfi_endproc
    .size __vdso_gettimeofday, . - __vdso_gettimeofday

// int __vdso_clock_getres(clockid_t clock, struct timespec *res): the system
// call itself, as on arm64.
    .balign 16
    .global __vdso_clock_getres
    .type __vdso_clock_getres, %function
__vdso_clock_getres:
    .cfi_startproc
    li      a7, NR_clock_getres
    ecall
    ret
    .cfi_endproc
    .size __vdso_clock_getres, . - __vdso_clock_getres

// Linux's riscv64 vDSO carries the signal return trampoline; the kernel here
// returns handlers through the [sigpage] (kernel/exec.c map_sigpage), which
// holds these same two instructions.
    .balign 16
    nop
    .global __vdso_rt_sigreturn
    .type __vdso_rt_sigreturn, %function
__vdso_rt_sigreturn:
    li      a7, NR_rt_sigreturn
    ecall
    .size __vdso_rt_sigreturn, . - __vdso_rt_sigreturn

// The kernel version, as Linux's vDSO carries it: uname(2)'s release.
    .pushsection .note.Linux, "a", %note
    .balign 4
    .long 2f - 1f                           // namesz
    .long 4f - 3f                           // descsz
    .long 0                                 // type
1:  .asciz "Linux"
2:  .balign 4
3:  .long (5 << 16) | (10 << 8) | 0         // 5.10.0
4:  .balign 4
    .popsection
