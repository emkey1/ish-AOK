// The ppc64 vDSO: vdso/arm64/vdso.S's functions for a ppc64le guest, under
// the names and version (LINUX_2.6.15) Linux's powerpc vDSO exports and its
// C libraries look up. The arm64 file says why a vDSO, and why it asks the
// JIT for the time instead of computing it from the timebase and a page of
// numbers.
//
// AOK_VCLOCK here is `mfspr rT, 777`, an SPR no POWER processor has and only
// the JIT implements (jit/gen_ppc64.c, the vclock gadget): rT names a clock
// on the way in and holds its reading on the way out, in nanoseconds --
// kernel/time.c vdso_clock_ns, the computation clock_gettime(2) itself makes
// -- or a negative value when the system call has to answer instead, which
// the fallbacks below then make.
//
// A powerpc vDSO function returns as a system call does: 0 with CR0[SO]
// clear, or an errno with it set (glibc tests SO after the call). So the
// fast paths clear SO, and each fallback is the system call itself.
//
// Leaf functions, no stack, no TOC, volatile registers only.

#define AOK_VCLOCK 777

#define NR_time 13
#define NR_gettimeofday 78
#define NR_clock_gettime 246
#define NR_clock_getres 247

    .abiversion 2
    .text

// r6 = 1e9
.macro billion
    lis     6, 0x3b9a
    ori     6, 6, 0xca00
.endm

// int __kernel_clock_gettime(clockid_t clock, struct timespec *ts)
    .balign 16
    .global __kernel_clock_gettime
    .type __kernel_clock_gettime, @function
__kernel_clock_gettime:
    .cfi_startproc
    mr      5, 3
    mfspr   5, AOK_VCLOCK
    cmpdi   5, 0
    blt-    1f
    billion
    divdu   7, 5, 6
    mulld   8, 7, 6
    subf    8, 8, 5
    std     7, 0(4)
    std     8, 8(4)
    li      3, 0
    crclr   3                               // CR0[SO]
    blr
1:  li      0, NR_clock_gettime
    sc
    blr
    .cfi_endproc
    .size __kernel_clock_gettime, . - __kernel_clock_gettime

// int __kernel_gettimeofday(struct timeval *tv, struct timezone *tz)
//
// A timezone is the kernel's to fill, so asking for one takes the system call.
    .balign 16
    .global __kernel_gettimeofday
    .type __kernel_gettimeofday, @function
__kernel_gettimeofday:
    .cfi_startproc
    cmpdi   4, 0
    bne-    2f
    cmpdi   3, 0
    beq-    1f
    li      5, 0                            // CLOCK_REALTIME
    mfspr   5, AOK_VCLOCK
    cmpdi   5, 0
    blt-    2f
    billion
    divdu   7, 5, 6
    mulld   8, 7, 6
    subf    8, 8, 5
    li      9, 1000
    divdu   8, 8, 9
    std     7, 0(3)
    std     8, 8(3)
1:  li      3, 0
    crclr   3
    blr
2:  li      0, NR_gettimeofday
    sc
    blr
    .cfi_endproc
    .size __kernel_gettimeofday, . - __kernel_gettimeofday

// time_t __kernel_time(time_t *t)
    .balign 16
    .global __kernel_time
    .type __kernel_time, @function
__kernel_time:
    .cfi_startproc
    li      5, 0                            // CLOCK_REALTIME
    mfspr   5, AOK_VCLOCK
    cmpdi   5, 0
    blt-    2f
    billion
    divdu   4, 5, 6
    cmpdi   3, 0
    beq-    1f
    std     4, 0(3)
1:  mr      3, 4
    crclr   3
    blr
2:  li      0, NR_time
    sc
    blr
    .cfi_endproc
    .size __kernel_time, . - __kernel_time

// int __kernel_clock_getres(clockid_t clock, struct timespec *res): the
// system call itself, as on arm64.
    .balign 16
    .global __kernel_clock_getres
    .type __kernel_clock_getres, @function
__kernel_clock_getres:
    .cfi_startproc
    li      0, NR_clock_getres
    sc
    blr
    .cfi_endproc
    .size __kernel_clock_getres, . - __kernel_clock_getres

// The kernel version, as Linux's vDSO carries it: uname(2)'s release.
    .pushsection .note.Linux, "a", @note
    .balign 4
    .long 2f - 1f                           // namesz
    .long 4f - 3f                           // descsz
    .long 0                                 // type
1:  .asciz "Linux"
2:  .balign 4
3:  .long (5 << 16) | (10 << 8) | 0         // 5.10.0
4:  .balign 4
    .popsection
