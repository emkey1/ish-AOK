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
// GENERATED, from kernel/calls.c's own arm64 table -- see
// tools/gen-native-syscalls.py and the custom_target in meson.build. Every
// syscall AOK genuinely implements is here, so a native program reaches all of
// the kernel rather than the part somebody happened to need first.
//
// It was a hand-written list of "only what the shim uses", and that went stale
// the way such lists do: 77 numbers against 217 implemented syscalls, which
// left the second native program unable to reach 140 of them for no reason but
// that nobody had asked yet. Generating it means adding a syscall to AOK makes
// it reachable from native code the same day, with nothing to remember.
//
// Entries marked "// ABI" have a handler whose struct layout follows
// current->abi rather than the asm-generic shape -- getrusage and sysinfo are
// the long-standing examples. A native caller always speaks asm-generic, so
// those need their argument marshalled to the shape the calling TASK expects.
#include "native_syscall_nums.h"


#endif
