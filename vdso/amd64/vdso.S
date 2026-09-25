// The x86_64 vDSO: vdso/arm64/vdso.S's functions for an amd64 guest, under
// the names and version (LINUX_2.6) its C libraries and Go look up, with the
// unprefixed weak aliases Linux's own carries. The arm64 file says why a
// vDSO, and why it asks the JIT for the time instead of computing it from a
// counter and a page of numbers.
//
// AOK_VCLOCK here is VMCALL (0f 01 c1), the instruction a guest uses to call
// its hypervisor, and a user-mode one raises #UD on real hardware. The JIT
// (jit/gen.c) and the interpreter (emu/amd64_interp.c amd64_vmcall) take it
// as AOK's call: with the clock a Linux clockid names in RAX (its upper half
// zero -- anything else is reserved and faults), it leaves in RAX that clock's
// reading in nanoseconds, from kernel/time.c vdso_clock_ns -- the computation
// clock_gettime(2) itself makes -- or a value with bit 63 set when the system
// call has to answer instead, which the fallbacks below then make. Nothing
// else changes, flags included.
//
// Leaf functions, no stack, caller-saved registers only (the fallback's
// syscall clobbers rcx and r11, which the ABI allows).

#define NR_gettimeofday 96
#define NR_time 201
#define NR_clock_gettime 228
#define NR_clock_getres 229

    .text

// int __vdso_clock_gettime(clockid_t clock, struct timespec *ts)
    .balign 16
    .global __vdso_clock_gettime
    .type __vdso_clock_gettime, @function
__vdso_clock_gettime:
    .cfi_startproc
    mov     %edi, %eax
    vmcall
    test    %rax, %rax
    js      1f
    mov     $1000000000, %ecx
    xor     %edx, %edx
    div     %rcx                            // rax = seconds, rdx = nanoseconds
    mov     %rax, (%rsi)
    mov     %rdx, 8(%rsi)
    xor     %eax, %eax
    ret
1:  mov     $NR_clock_gettime, %eax
    syscall
    ret
    .cfi_endproc
    .size __vdso_clock_gettime, . - __vdso_clock_gettime
    .weak clock_gettime
    clock_gettime = __vdso_clock_gettime

// int __vdso_gettimeofday(struct timeval *tv, struct timezone *tz)
//
// A timezone is the kernel's to fill, so asking for one takes the system call.
    .balign 16
    .global __vdso_gettimeofday
    .type __vdso_gettimeofday, @function
__vdso_gettimeofday:
    .cfi_startproc
    test    %rsi, %rsi
    jnz     2f
    test    %rdi, %rdi
    jz      1f
    xor     %eax, %eax                      // CLOCK_REALTIME
    vmcall
    test    %rax, %rax
    js      2f
    mov     $1000000000, %ecx
    xor     %edx, %edx
    div     %rcx                            // rax = seconds, rdx = nanoseconds
    mov     %rax, (%rdi)
    mov     %rdx, %rax
    mov     $1000, %ecx
    xor     %edx, %edx
    div     %rcx                            // rax = microseconds
    mov     %rax, 8(%rdi)
1:  xor     %eax, %eax
    ret
2:  mov     $NR_gettimeofday, %eax
    syscall
    ret
    .cfi_endproc
    .size __vdso_gettimeofday, . - __vdso_gettimeofday
    .weak gettimeofday
    gettimeofday = __vdso_gettimeofday

// time_t __vdso_time(time_t *t) -- glibc resolves time() straight to this.
    .balign 16
    .global __vdso_time
    .type __vdso_time, @function
__vdso_time:
    .cfi_startproc
    xor     %eax, %eax                      // CLOCK_REALTIME
    vmcall
    test    %rax, %rax
    js      2f
    mov     $1000000000, %ecx
    xor     %edx, %edx
    div     %rcx                            // rax = seconds
    test    %rdi, %rdi
    jz      1f
    mov     %rax, (%rdi)
1:  ret
2:  mov     $NR_time, %eax
    syscall
    ret
    .cfi_endproc
    .size __vdso_time, . - __vdso_time
    .weak time
    time = __vdso_time

// int __vdso_clock_getres(clockid_t clock, struct timespec *res): the system
// call itself, as on arm64.
    .balign 16
    .global __vdso_clock_getres
    .type __vdso_clock_getres, @function
__vdso_clock_getres:
    .cfi_startproc
    mov     $NR_clock_getres, %eax
    syscall
    ret
    .cfi_endproc
    .size __vdso_clock_getres, . - __vdso_clock_getres
    .weak clock_getres
    clock_getres = __vdso_clock_getres

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
