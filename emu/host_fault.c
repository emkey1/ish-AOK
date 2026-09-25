// Host faults on guest memory taken by kernel C code; see emu/host_fault.h.
#include <assert.h>
#include <string.h>
#include "emu/host_fault.h"
#include "emu/memory.h"

__thread struct host_fault_guard *host_fault_guard_armed = NULL;

void host_fault_guard_init(struct host_fault_guard *guard) {
    guard->prev = NULL;
    guard->fault_addr = NULL;
    guard->ranges = 0;
    // A guest task's thread installs these when it first enters the JIT,
    // which is before its first syscall. Any other thread that copies guest
    // memory has not, and on device a thread without the Mach port would
    // still take the fault as a crash.
    jit_host_fault_thread_init();
}

void host_fault_guard_cover(struct host_fault_guard *guard, const void *host, size_t len) {
    if (len == 0)
        return;
    assert(guard->ranges < HOST_FAULT_GUARD_RANGES);
    uintptr_t mask = (uintptr_t) real_page_size - 1;
    guard->lo[guard->ranges] = (uintptr_t) host & ~mask;
    guard->hi[guard->ranges] = ((uintptr_t) host + len + mask) & ~mask;
    guard->ranges++;
}

void host_fault_guard_recover(void *host_addr) {
    struct host_fault_guard *guard = host_fault_guard_armed;
    if (guard == NULL)
        return;
    uintptr_t addr = (uintptr_t) host_addr;
    for (unsigned i = 0; i < guard->ranges; i++) {
        if (addr >= guard->lo[i] && addr < guard->hi[i]) {
            guard->fault_addr = host_addr;
            host_fault_guard_armed = guard->prev;
            siglongjmp(guard->env, 1);
        }
    }
}

// Each helper is its own frame, which is the frame the recovery returns to,
// and noinline keeps it one: a function that calls sigsetjmp must still be
// running when its buffer is jumped to.
__attribute__((noinline))
bool host_copy_from_guest(void *dst, const void *guest_src, size_t n) {
    struct host_fault_guard guard;
    host_fault_guard_init(&guard);
    host_fault_guard_cover(&guard, guest_src, n);
    if (sigsetjmp(guard.env, 0) != 0)
        return false;
    host_fault_guard_arm(&guard);
    memcpy(dst, guest_src, n);
    host_fault_guard_disarm(&guard);
    return true;
}

__attribute__((noinline))
bool host_copy_to_guest(void *guest_dst, const void *src, size_t n) {
    struct host_fault_guard guard;
    host_fault_guard_init(&guard);
    host_fault_guard_cover(&guard, guest_dst, n);
    if (sigsetjmp(guard.env, 0) != 0)
        return false;
    host_fault_guard_arm(&guard);
    memcpy(guest_dst, src, n);
    host_fault_guard_disarm(&guard);
    return true;
}

__attribute__((noinline))
bool host_copy_guarded(void *dst, const void *src, size_t n) {
    struct host_fault_guard guard;
    host_fault_guard_init(&guard);
    host_fault_guard_cover(&guard, src, n);
    host_fault_guard_cover(&guard, dst, n);
    if (sigsetjmp(guard.env, 0) != 0)
        return false;
    host_fault_guard_arm(&guard);
    memcpy(dst, src, n);
    host_fault_guard_disarm(&guard);
    return true;
}

__attribute__((noinline))
bool host_zero_guest(void *guest_dst, size_t n) {
    struct host_fault_guard guard;
    host_fault_guard_init(&guard);
    host_fault_guard_cover(&guard, guest_dst, n);
    if (sigsetjmp(guard.env, 0) != 0)
        return false;
    host_fault_guard_arm(&guard);
    memset(guest_dst, 0, n);
    host_fault_guard_disarm(&guard);
    return true;
}

__attribute__((noinline))
bool host_call_guarded(void (*fn)(void *arg), void *arg,
        const void *r0, size_t n0, const void *r1, size_t n1, const void *r2, size_t n2) {
    struct host_fault_guard guard;
    host_fault_guard_init(&guard);
    host_fault_guard_cover(&guard, r0, n0);
    host_fault_guard_cover(&guard, r1, n1);
    host_fault_guard_cover(&guard, r2, n2);
    if (sigsetjmp(guard.env, 0) != 0)
        return false;
    host_fault_guard_arm(&guard);
    fn(arg);
    host_fault_guard_disarm(&guard);
    return true;
}
