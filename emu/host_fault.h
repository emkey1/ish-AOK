#ifndef EMU_HOST_FAULT_H
#define EMU_HOST_FAULT_H

#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Host faults on guest memory, taken by the kernel's own C code.
//
// A guest page is host memory, and for a file mapping that memory is a host
// mmap of the host file, which is only as long as the file. A host page that
// holds no byte of the file cannot be paged in, and touching it raises SIGBUS
// (EXC_BAD_ACCESS, KERN_MEMORY_ERROR on Darwin). Such pages are ordinary. A
// MAP_PRIVATE mapping may be longer than its file, and musl's dynamic linker
// makes one of every arm64 library: it maps the library's whole span from the
// file, so the gap between text and data is file offsets past EOF. A file
// truncated under a MAP_SHARED mapping makes more.
//
// The JIT turns a GUEST access to such a page into a guest SIGBUS
// (jit_translate_host_fault). A kernel access is a syscall copying to or from
// the page, and until this existed the fault killed the whole app (host exit
// 138, EXC_BAD_ACCESS in _platform_memmove) where Linux fails the syscall:
// EFAULT from write(2) and read(2), EIO from /proc/<pid>/mem. So such a copy
// arms a guard around itself, and the host fault handlers -- the POSIX SIGBUS
// handler on the CLI and the Mach EXC_BAD_ACCESS handler on device, both in
// jit/jit.c -- resume at the guard instead, which reports the fault to its
// caller. This is Linux's exception table, with sigsetjmp for the table.
//
// The rules:
// - Only a fault inside the ranges a guard names is absorbed. A wild pointer
//   anywhere else, the other operand of the copy included, still crashes as
//   loudly as before.
// - Nothing between arming and disarming may take a lock, allocate or sleep.
//   The recovery is a siglongjmp, and anything acquired after the arm would
//   be abandoned, still held (the lesson of sigunwind_start, util/sync.h).
//   What was acquired BEFORE the arm is still held at the recovery point and
//   released by the caller's normal path, which is why a copy made under the
//   address-space lock can be guarded at all.
// - Guard only memory that can fault. Host-anonymous memory cannot, and it is
//   the hot case: mem_ptr_may_fault() says which a page is, from the entry
//   mem_ptr resolved anyway, and an anonymous page gets a plain memcpy.
//   Arming costs one sigsetjmp -- a couple of dozen instructions and no
//   syscall, since it is savemask 0. sigsetjmp(buf, 1) would restore the mask
//   with sigprocmask on the way back, and on Darwin that sets EVERY thread's
//   mask.
//
// The shape of a guarded access, when one of the helpers below does not fit:
//
//     struct host_fault_guard guard;
//     host_fault_guard_init(&guard);
//     host_fault_guard_cover(&guard, page, len);
//     if (sigsetjmp(guard.env, 0) != 0)
//         return fault;           // recovered, and already disarmed
//     host_fault_guard_arm(&guard);
//     ... the access ...
//     host_fault_guard_disarm(&guard);
//
// The sigsetjmp must be in the function that makes the access (or one that
// outlives it): the recovery jumps back into that frame.

#define HOST_FAULT_GUARD_RANGES 3

struct host_fault_guard {
    sigjmp_buf env;
    struct host_fault_guard *prev;
    // Where the fault this guard absorbed was. For diagnostics only.
    void *fault_addr;
    unsigned ranges;
    uintptr_t lo[HOST_FAULT_GUARD_RANGES];
    uintptr_t hi[HOST_FAULT_GUARD_RANGES];
};

// The innermost armed guard of the calling thread, or NULL.
extern __thread struct host_fault_guard *host_fault_guard_armed;

// A guard with no ranges yet. Also makes sure this thread's host fault
// handlers are installed, since a thread that has never run the JIT has none.
void host_fault_guard_init(struct host_fault_guard *guard);
// Absorb faults in [host, host + len), widened to whole host pages: the host
// reports a failed page-in at host-page granularity.
void host_fault_guard_cover(struct host_fault_guard *guard, const void *host, size_t len);

// The fences keep the compiler from moving the access to either side of the
// store that arms or disarms: the handler reads this variable on this thread.
static inline void host_fault_guard_arm(struct host_fault_guard *guard) {
    guard->prev = host_fault_guard_armed;
    host_fault_guard_armed = guard;
    atomic_signal_fence(memory_order_seq_cst);
}

static inline void host_fault_guard_disarm(struct host_fault_guard *guard) {
    atomic_signal_fence(memory_order_seq_cst);
    host_fault_guard_armed = guard->prev;
}

// For the host fault handlers, on the faulting thread: if a guard of this
// thread covers host_addr, disarm it and resume at its sigsetjmp, which then
// returns nonzero. Returns only when no guard covers host_addr.
void host_fault_guard_recover(void *host_addr);

// memcpy with guest memory on one side, guarded. False, with the destination
// partly written, when touching the guest side faulted.
bool host_copy_from_guest(void *dst, const void *guest_src, size_t n);
bool host_copy_to_guest(void *guest_dst, const void *src, size_t n);
// memset(guest_dst, 0, n), guarded the same way.
bool host_zero_guest(void *guest_dst, size_t n);
// memcpy guarded on BOTH sides, for a copy between two pages that may either
// fault: a copy-on-write break of a file page, mem_host_copy.
bool host_copy_guarded(void *dst, const void *src, size_t n);
// fn(arg) under a guard covering up to three host ranges, for an access that
// is not a copy -- a callback over guest pages, an atomic on a guest word. A
// range with a zero length is not covered. False if touching a covered range
// faulted, in which case fn stopped where it was.
bool host_call_guarded(void (*fn)(void *arg), void *arg,
        const void *r0, size_t n0, const void *r1, size_t n1, const void *r2, size_t n2);

// Installs the calling thread's host fault handlers, once per thread: the Mach
// exception port on device, the POSIX SIGBUS handler and its altstack on the
// CLI. Defined in jit/jit.c.
void jit_host_fault_thread_init(void);

#endif
