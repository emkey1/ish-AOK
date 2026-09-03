// Guest syscalls issued from native (host) code, and the guest-memory scratch
// that makes them possible. See kernel/native_syscall.h for why this replaced
// a per-libc-function shim.

#include <stdlib.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/native_syscall.h"
#include "kernel/task.h"
#include "emu/memory.h"

// ---------------------------------------------------------------- the arena
//
// One region per THREAD, not per task: a native program may run on pthreads it
// created itself, which share the task (and so the mm) but must not share a
// bump pointer. Per-thread means no locking on the hot path at all.
//
// Anonymous guest pages are mapped lazily -- pt_map_nothing reserves address
// space and faults real memory in on touch -- so a generous arena costs
// nothing until it is used, and only the used part ever costs. That is what
// makes a megabyte the right default rather than an extravagance: it keeps
// every realistic buffer (paths at 4 KiB, stat structs, the 64 KiB-ish I/O
// buffers SmallCLUE uses) on the cheap path.
#define NATIVE_ARENA_SIZE (1u << 20)
#define NATIVE_SCRATCH_ALIGN 16

struct native_arena {
    guest_addr_t base;
    size_t size;
    size_t used;
    // Which address space `base' is an address in (struct mm's id, so a freed
    // and reallocated mm can never be mistaken for this one). A guest address
    // means nothing without one, and neither a task nor a thread keeps the same
    // address space for life:
    //
    //   - exec hands the task a fresh, empty mm (kernel/exec.c);
    //   - a host thread does not always act for its own task. native_spawn_opts
    //     (kernel/native_io.c) borrows the CALLING thread and points `current`
    //     at the child across its exec, the same impersonation kernel/init.c
    //     uses. While that lasts, this thread's arena belongs to a space that
    //     is not the one `current` names.
    //
    // So every use of the arena is guarded by this, and a mismatch means hands
    // off entirely: the bump pointer must not be used (the address means
    // something else over there) and must not be dropped either (its owner is
    // still using it).
    uint64_t mm_id;
};
static __thread struct native_arena arena;

// Anything too big for the arena gets a mapping of its own, released with the
// frame. Two extra syscalls, which is the right trade for a rare case.
struct native_scratch_big {
    struct native_scratch_big *next;
    guest_addr_t addr;
    size_t size;
};

static __thread struct native_frame *frame_top;

// The address space this thread is acting for right now, or 0 if there is none.
static uint64_t native_current_mm_id(void) {
    if (!native_have_task() || current->mm == NULL)
        return 0;
    return current->mm->id;
}

// Give the arena back to the address space it came from.
//
// It used to be per-thread and never released, so it was a permanent megabyte
// hole in whatever mm the thread first marshalled a syscall in. That is
// invisible when the mm dies with the program and a real leak when it does
// not. Measured: native bash running a 40-command script created and finished
// about thirty threads, each of which mapped an arena into the one address
// space the shell was running in and left it there. The same shape applies to
// any task sharing an address space with the one that made it (CLONE_VM, which
// is what vfork and posix_spawn use), where the arena outlives its mapper by
// the whole life of the other party.
//
// Strictly this thread's own arena, and only in the space it was mapped in --
// so a thread tearing its own down can never unmap memory another thread is
// still bump-allocating from, which is the one way this could go badly wrong.
void native_arena_release(void) {
    if (arena.base == 0)
        return;
    // Not the space the arena is in -- this thread is impersonating another
    // task (see struct native_arena). Leave it entirely alone: unmapping would
    // punch a hole in the wrong address space, and merely forgetting would
    // strand a megabyte in the right one. This is not hypothetical; it is what
    // every command a native shell runs goes through, and forgetting here cost
    // the shell a megabyte per child until the guard was added.
    if (arena.mm_id != native_current_mm_id())
        return;
    // Live frames still hold marks into it. No caller does this today (every
    // release point is outside any shim call), and the guard is here so that
    // stays true by construction rather than by everyone remembering.
    if (frame_top != NULL)
        return;
    guest_addr_t base = arena.base;
    size_t size = arena.size;
    arena.base = 0;
    arena.size = 0;
    arena.used = 0;
    arena.mm_id = 0;
    native_syscall(NATIVE_SYS_munmap, base, size);
}

void native_frame_push(struct native_frame *frame) {
    frame->prev = frame_top;
    frame->mark = arena.used;
    frame->big = NULL;
    frame_top = frame;
}

void native_frame_pop(struct native_frame *frame) {
    for (struct native_scratch_big *big = frame->big; big != NULL; ) {
        struct native_scratch_big *next = big->next;
        native_syscall(NATIVE_SYS_munmap, big->addr, big->size);
        free(big);
        big = next;
    }
    frame->big = NULL;
    arena.used = frame->mark;
    frame_top = frame->prev;
}

static guest_addr_t native_map_anon(size_t size) {
    sqword_t res = native_syscall(NATIVE_SYS_mmap, 0, size, P_READ | P_WRITE,
            MMAP_PRIVATE | MMAP_ANONYMOUS, -1, 0);
    // An error comes back as a small negative value; a real mapping never is.
    if (res < 0 && res > -4096)
        return 0;
    return (guest_addr_t) res;
}

guest_addr_t native_scratch_alloc(size_t size) {
    if (!native_have_task() || current->mem == NULL)
        return 0;
    if (size == 0)
        size = 1;
    size = (size + NATIVE_SCRATCH_ALIGN - 1) & ~(size_t) (NATIVE_SCRATCH_ALIGN - 1);

    uint64_t mm_id = native_current_mm_id();
    if (mm_id == 0)
        return 0;

    if (arena.base == 0) {
        arena.base = native_map_anon(NATIVE_ARENA_SIZE);
        if (arena.base == 0)
            return 0;
        arena.size = NATIVE_ARENA_SIZE;
        arena.used = 0;
        arena.mm_id = mm_id;
    }

    if (arena.mm_id == mm_id && size <= arena.size - arena.used) {
        guest_addr_t addr = arena.base + arena.used;
        arena.used += size;
        return addr;
    }

    // Oversized, the arena is full inside a deeply nested frame, or the arena
    // is in another address space entirely (see struct native_arena) -- in
    // which case a mapping the frame owns is not merely the fallback, it is the
    // only shape that is right: it is made here, in the space `current` names,
    // and released by the frame that asked for it.
    if (frame_top == NULL)
        return 0;   // nothing would ever release it
    struct native_scratch_big *big = malloc(sizeof(*big));
    if (big == NULL)
        return 0;
    big->size = (size + PAGE_SIZE - 1) & ~(size_t) (PAGE_SIZE - 1);
    big->addr = native_map_anon(big->size);
    if (big->addr == 0) {
        free(big);
        return 0;
    }
    big->next = frame_top->big;
    frame_top->big = big;
    return big->addr;
}

guest_addr_t native_scratch_put(const void *src, size_t size) {
    guest_addr_t addr = native_scratch_alloc(size);
    if (addr == 0)
        return 0;
    if (src != NULL && size > 0 && user_write(addr, src, size))
        return 0;
    return addr;
}

guest_addr_t native_scratch_str(const char *str) {
    if (str == NULL)
        return 0;
    return native_scratch_put(str, strlen(str) + 1);
}

int native_scratch_get(void *dst, guest_addr_t src, size_t size) {
    if (size == 0)
        return 0;
    if (dst == NULL || src == 0)
        return _EFAULT;
    return user_read(src, dst, size) ? _EFAULT : 0;
}

// ------------------------------------------------------------------ issuing
//
// syscall_dispatch_native (kernel/calls.c) is the same dispatch a translated
// guest gets. Everything specific to a native caller is here: the signal
// checkpoint, and restarting.

sqword_t native_syscall_args(unsigned num, const qword_t args[6]) {
    if (!native_have_task())
        return _EFAULT;

    for (;;) {
        // Every syscall is a yield point, which is what makes a native program
        // interruptible at all -- nothing else checks for signals the way the
        // instruction dispatcher does for translated code (kernel/native.h).
        // Doing it here rather than in individual shim calls covers anything
        // that talks to the kernel, which is what `top` needed for ^C.
        //
        // Not on the mmap that grows the scratch, though: that runs underneath
        // a shim call which has already checkpointed, and receive_signals may
        // not return -- exiting from inside the allocator would leave the
        // frame it was called from unreleased.
        if (num != NATIVE_SYS_mmap && num != NATIVE_SYS_munmap)
            native_checkpoint();

        sqword_t result = syscall_dispatch_native(num, args);
        // A syscall cut short by a signal: run the handler BEFORE the program
        // sees the EINTR, which is the order a real signal delivery has. Left
        // to the next syscall's checkpoint instead, readline saw an EINTR with
        // none of its interrupt flags set yet, retried the read, and only then
        // ran the handler -- eating the keystroke that had woken it.
        if (result == _EINTR)
            native_checkpoint();
        // _ERESTART is the kernel asking for the instruction to be re-executed
        // after a signal. A guest gets its PC rewound; a native caller just
        // issues the call again, which is the same thing one level up.
        // Both restart flavours mean "issue it again"; a native caller has no
        // PC to rewind, so the NOHAND cancellation has nothing to undo.
        if (result != _ERESTART && result != _ERESTART_NOHAND)
            return result;
        // ...unless the handler that justifies the restart cannot run yet.
        // Inside a host stdio callback the checkpoint above deliberately
        // delivers nothing (nlibc_stdio_defer_fatal), so re-issuing would meet
        // the very same pending signal and be cut short again -- a spin, not a
        // restart. The deferral's own contract is that the callback FAILS, so
        // stdio unwinds through its unlock and the signal is taken at the next
        // checkpoint outside it; hand the caller the EINTR that says so.
        if (nlibc_delivery_deferred())
            return _EINTR;
    }
}
