#ifndef KERNEL_NATIVE_SYSCALL_H
#define KERNEL_NATIVE_SYSCALL_H

#include <stddef.h>
#include "misc.h"

/* Guest syscalls issued from a natively-compiled program (kernel/native.h).
 *
 * Why this exists
 * ---------------
 * A native program is host code, so its libc binds to the HOST libc and
 * resolves against iOS. kernel/native_libc.h redirects that surface onto AOK,
 * and the first implementation of the redirect reimplemented each call against
 * AOK's internal VFS helpers -- a hand-written list, one entry per libc
 * function.
 *
 * A list only covers what someone thought of, and the misses were found by
 * people running the thing rather than by tooling: uname reported Darwin, df
 * listed the Mac's volumes, whoami answered "mobile" (the iOS account), ls -l
 * showed group "wheel". Each fix was another entry. The libc surface is
 * unbounded; the syscall surface is not.
 *
 * So the shim calls syscalls now, through the same dispatcher a JIT'd guest
 * reaches. Three things follow:
 *
 *  1. A syscall added or fixed for the guest works for native programs the
 *    same day, with no shim to update. This is the main reason.
 *  2. strace covers native programs for free -- the trace comes from inside
 *    the dispatcher, so an applet's calls appear alongside every other task's
 *    (AOK's printk goes to fd 555:
 *    `bash -c 'exec 555>trace.log; ./build/ish ...'`).
 *  3. Semantics match translated code by construction rather than by a shim
 *    staying in sync with it.
 *
 * The blocker, and the scratch region
 * -----------------------------------
 * sys_* handlers take GUEST addresses and go through user_read/user_write. A
 * native program's buffers are host memory, which those cannot reach. The task
 * still has an mm -- exec intercepts before the image would have been replaced
 * (kernel/exec.c), so the address space it inherited from its parent is intact
 * -- so this reserves a region in it and marshals through:
 *
 *     host buffer -> guest scratch -> syscall -> guest scratch -> host buffer
 *
 * The cost is a copy per buffered call: a 4 KiB write becomes ~8 KiB of
 * memcpy. Against JIT-executing the same program that is nothing, and it buys
 * a shim that cannot silently diverge from the kernel it sits on.
 *
 * The numbering is asm-generic (the arm64/riscv64 one), whatever ABI the
 * calling task's guest image had -- see syscall_dispatch_native in
 * kernel/calls.c for why. Struct layouts follow from that: stat is the arm64
 * one, and NATIVE_SYS_* below names only calls that have been checked against
 * the arm64 dispatch.
 */

// -------------------------------------------------------------- issuing one
//
// Returns what the guest would see: a result, or a negative Linux errno.
// Negative Linux errnos are NOT the host's -- kernel/native_libc.c translates
// before touching `errno`, because Linux ENOSYS (38) is macOS ENOTSOCK and
// "Socket operation on non-socket" from df is how that was found.
sqword_t native_syscall_args(unsigned num, const qword_t args[6]);

// native_syscall(NATIVE_SYS_write, fd, buf, len) -- trailing arguments default
// to 0, so each call passes only the ones the syscall takes.
#define native_syscall(...) \
    native_syscall_pad_(__VA_ARGS__, 0, 0, 0, 0, 0, 0)
#define native_syscall_pad_(num, a, b, c, d, e, f, ...) \
    native_syscall_args((num), (const qword_t[6]) { (qword_t) (a), (qword_t) (b), \
            (qword_t) (c), (qword_t) (d), (qword_t) (e), (qword_t) (f) })

// ------------------------------------------------------------ the marshalling
//
// A frame bounds the lifetime of everything marshalled inside one shim call.
// Declare NATIVE_FRAME at the top of the function and the scratch is released
// however the function returns -- with ~90 shim entry points, most of them
// full of early returns, an explicit release would be leaked somewhere.
//
// Frames nest: nlibc_fopen calling nlibc_open is one frame inside another, and
// the inner one releases only what it allocated.
struct native_scratch_big;
struct native_frame {
    struct native_frame *prev;
    size_t mark;
    struct native_scratch_big *big;
};

void native_frame_push(struct native_frame *frame);
void native_frame_pop(struct native_frame *frame);

#define NATIVE_FRAME \
    struct native_frame native_frame_ __attribute__((cleanup(native_frame_pop))); \
    native_frame_push(&native_frame_)

// Guest space for `size` bytes, uninitialised. 0 if the scratch cannot be
// grown, which the caller should report as ENOMEM.
guest_addr_t native_scratch_alloc(size_t size);
// Guest space holding a copy of a host buffer.
guest_addr_t native_scratch_put(const void *src, size_t size);
// Guest space holding a copy of a C string, NUL included. A NULL string gives
// 0, which is the guest address a NULL argument should have -- so a call site
// that must distinguish "no string" from "out of scratch" checks its own
// pointer first.
guest_addr_t native_scratch_str(const char *str);
// Copies back out. 0, or a negative errno.
int native_scratch_get(void *dst, guest_addr_t src, size_t size);

// ------------------------------------------------------------------- numbers
//
// asm-generic syscall numbers, checked against arm64_syscall_table and
// handle_asm_generic_native_syscall in kernel/calls.c. Only what the shim
// uses; adding one means checking that AOK actually implements it there.
enum {
    NATIVE_SYS_getcwd = 17,
    NATIVE_SYS_dup = 23,
    NATIVE_SYS_dup3 = 24,
    NATIVE_SYS_fcntl = 25,
    NATIVE_SYS_ioctl = 29,
    NATIVE_SYS_mknodat = 33,
    NATIVE_SYS_mkdirat = 34,
    NATIVE_SYS_unlinkat = 35,
    NATIVE_SYS_symlinkat = 36,
    NATIVE_SYS_linkat = 37,
    NATIVE_SYS_renameat = 38,
    NATIVE_SYS_mount = 40,
    NATIVE_SYS_statfs = 43,
    NATIVE_SYS_fstatfs = 44,
    NATIVE_SYS_truncate = 45,
    NATIVE_SYS_ftruncate = 46,
    NATIVE_SYS_faccessat = 48,
    NATIVE_SYS_chdir = 49,
    NATIVE_SYS_chroot = 51,
    NATIVE_SYS_fchmodat = 53,
    NATIVE_SYS_fchownat = 54,
    NATIVE_SYS_openat = 56,
    NATIVE_SYS_close = 57,
    NATIVE_SYS_pipe2 = 59,
    NATIVE_SYS_getdents64 = 61,
    NATIVE_SYS_lseek = 62,
    NATIVE_SYS_read = 63,
    NATIVE_SYS_write = 64,
    NATIVE_SYS_pselect6 = 72,
    NATIVE_SYS_ppoll = 73,
    NATIVE_SYS_readlinkat = 78,
    NATIVE_SYS_newfstatat = 79,
    NATIVE_SYS_fstat = 80,
    NATIVE_SYS_utimensat = 88,
    NATIVE_SYS_exit_group = 94,
    NATIVE_SYS_nanosleep = 101,
    NATIVE_SYS_clock_gettime = 113,
    NATIVE_SYS_kill = 129,
    NATIVE_SYS_rt_sigaction = 134,
    NATIVE_SYS_rt_sigprocmask = 135,
    NATIVE_SYS_rt_sigpending = 136,
    NATIVE_SYS_rt_sigtimedwait = 137,
    NATIVE_SYS_setgid = 144,
    NATIVE_SYS_setuid = 146,
    NATIVE_SYS_setpgid = 154,
    NATIVE_SYS_getpgid = 155,
    NATIVE_SYS_getsid = 156,
    NATIVE_SYS_setsid = 157,
    NATIVE_SYS_getgroups = 158,
    NATIVE_SYS_setgroups = 159,
    NATIVE_SYS_uname = 160,
    NATIVE_SYS_sethostname = 161,
    NATIVE_SYS_getrusage = 165,
    NATIVE_SYS_umask = 166,
    NATIVE_SYS_getpid = 172,
    NATIVE_SYS_getppid = 173,
    NATIVE_SYS_getuid = 174,
    NATIVE_SYS_geteuid = 175,
    NATIVE_SYS_getgid = 176,
    NATIVE_SYS_getegid = 177,
    NATIVE_SYS_sysinfo = 179,
    NATIVE_SYS_socket = 198,
    NATIVE_SYS_socketpair = 199,
    NATIVE_SYS_bind = 200,
    NATIVE_SYS_listen = 201,
    NATIVE_SYS_accept = 202,
    NATIVE_SYS_connect = 203,
    NATIVE_SYS_getsockname = 204,
    NATIVE_SYS_getpeername = 205,
    NATIVE_SYS_sendto = 206,
    NATIVE_SYS_recvfrom = 207,
    NATIVE_SYS_setsockopt = 208,
    NATIVE_SYS_getsockopt = 209,
    NATIVE_SYS_shutdown = 210,
    NATIVE_SYS_munmap = 215,
    NATIVE_SYS_mmap = 222,
    NATIVE_SYS_wait4 = 260,
};

#endif
