// The aarch64 vDSO: what an arm64 guest's C library calls instead of making
// a system call to read the time. kernel/exec.c maps a private copy of it
// into every arm64 process and hands its address over as AT_SYSINFO_EHDR;
// glibc and musl then look these symbols up by name, version LINUX_2.6.39,
// exactly as they do on Linux.
//
// A system call is the expensive part of reading the clock here, not the
// clock: the guest leaves the JIT, the kernel dispatches the call, checks
// seccomp and ptrace and pending signals, and the guest comes back -- about
// 700 ns on Apple silicon, the same as getppid, where the host reads its own
// clock in 30. Linux avoids its equivalent round trip by reading a hardware
// counter in user space and scaling it with numbers the kernel publishes on
// a [vvar] page. Here the counter would be the host's, and the numbers under
// it -- the guest's clock origins and the gap between the host's wall clock
// and its monotonic one -- are the kernel's to change: a checkpoint restore
// moves the origins, and the host stepping its wall clock (NTP, a time zone
// trip, a user in Settings) moves the gap without telling anyone. A copy of
// them on a page would be stale until something noticed, and in that window
// the guest's CLOCK_REALTIME would disagree with the kernel's -- a file the
// kernel had just stamped could look like it was written in the future.
//
// So the numbers never leave the kernel. AOK_VCLOCK below is a system
// register only the JIT implements (jit/gen.c, jit/guest-arm64/dpextra.S):
// reading it runs the same code clock_gettime(2) runs, vdso_clock_ns in
// kernel/time.c, without leaving the JIT. The vDSO and the system call
// cannot disagree, because they are one computation.
//
// AOK_VCLOCK is MRS of S3_3_C15_C0_0, an IMPLEMENTATION DEFINED encoding --
// on real hardware an EL0 read of it is UNDEFINED -- with one departure from
// an ordinary MRS: the register is read as well as written. Wt on the way in
// names the clock, a Linux clockid_t; Xt on the way out is that clock's
// reading in nanoseconds, or has bit 63 set when the kernel has to answer
// instead -- a CPU-time clock, an id Linux does not have, a time before the
// epoch -- and then this falls back to the system call, which gives the
// answer, or the error, that it always did.
//
// Every function is a leaf that touches no stack and only the caller-saved
// x0-x8, which is all the AAPCS64 lets it assume; Go calls it from a small
// system stack and relies on that.
//
// One thing changes for the guest, and it is the same thing a vDSO changes
// on Linux: these reads are not system calls, so seccomp filters and ptrace
// (strace included) no longer see them. The fallbacks still are.

#define AOK_VCLOCK s3_3_c15_c0_0

#define NR_clock_gettime 113
#define NR_clock_getres 114
#define NR_rt_sigreturn 139
#define NR_gettimeofday 169

    .text

// int __kernel_clock_gettime(clockid_t clock, struct timespec *ts)
//
// 0, or a negative errno from the system call -- a raw call's convention,
// which is the one both C libraries expect from a vDSO.
    .balign 16
    .global __kernel_clock_gettime
    .type __kernel_clock_gettime, %function
__kernel_clock_gettime:
    .cfi_startproc
    mov     w2, w0
    mrs     x2, AOK_VCLOCK
    tbnz    x2, #63, 1f
    mov     x3, #0xca00                     // 1000000000
    movk    x3, #0x3b9a, lsl #16
    udiv    x4, x2, x3
    msub    x5, x4, x3, x2
    stp     x4, x5, [x1]
    mov     w0, #0
    ret
1:  mov     x8, #NR_clock_gettime
    svc     #0
    ret
    .cfi_endproc
    .size __kernel_clock_gettime, . - __kernel_clock_gettime

// int __kernel_gettimeofday(struct timeval *tv, struct timezone *tz)
//
// A timezone is the kernel's to fill (sys_gettimeofday), so asking for one
// takes the system call; nobody does outside a test.
    .balign 16
    .global __kernel_gettimeofday
    .type __kernel_gettimeofday, %function
__kernel_gettimeofday:
    .cfi_startproc
    cbnz    x1, 2f
    cbz     x0, 1f
    mov     x2, #0                          // CLOCK_REALTIME
    mrs     x2, AOK_VCLOCK
    tbnz    x2, #63, 2f
    mov     x3, #0xca00                     // 1000000000
    movk    x3, #0x3b9a, lsl #16
    udiv    x4, x2, x3
    msub    x5, x4, x3, x2
    mov     x3, #1000
    udiv    x5, x5, x3
    stp     x4, x5, [x0]
1:  mov     w0, #0
    ret
2:  mov     x8, #NR_gettimeofday
    svc     #0
    ret
    .cfi_endproc
    .size __kernel_gettimeofday, . - __kernel_gettimeofday

// int __kernel_clock_getres(clockid_t clock, struct timespec *res)
//
// The system call itself. The resolution is not something a program reads
// in a loop, and asking the kernel keeps its answer the only one.
    .balign 16
    .global __kernel_clock_getres
    .type __kernel_clock_getres, %function
__kernel_clock_getres:
    .cfi_startproc
    mov     x8, #NR_clock_getres
    svc     #0
    ret
    .cfi_endproc
    .size __kernel_clock_getres, . - __kernel_clock_getres

// Linux's arm64 vDSO has the signal return trampoline too, so it is here for
// anything that looks it up. The kernel itself returns handlers through the
// [sigpage] (kernel/exec.c map_sigpage), which holds these same two
// instructions -- the pair libgcc's unwinder recognises as a signal frame.
// The nop keeps an unwinder that backs up one instruction from landing in
// the function before.
    .balign 16
    nop
    .global __kernel_rt_sigreturn
    .type __kernel_rt_sigreturn, %function
__kernel_rt_sigreturn:
    mov     x8, #NR_rt_sigreturn
    svc     #0
    .size __kernel_rt_sigreturn, . - __kernel_rt_sigreturn

// The kernel version, as Linux's vDSO carries it: the release uname(2)
// reports (kernel/uname.c ISH_KERNEL_RELEASE), which is what an older glibc
// reads from here in preference to asking.
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
