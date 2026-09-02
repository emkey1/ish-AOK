#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "kernel/calls.h"
#include "kernel/mm.h"

#ifndef IOV_MAX
#define IOV_MAX 1024 // glibc only exposes IOV_MAX under _XOPEN_SOURCE
#endif

#define HTOP_RBX_FIELD_ABS_ADDR ((guest_addr_t) 0xf7f019e0u)
#define HTOP_RBX_FIELD_SIZE 8

static inline bool htop_watch_intersects(guest_addr_t addr, size_t count) {
    if (count == 0)
        return false;
    qword_t start = addr;
    qword_t end = start + count;
    qword_t watch_start = HTOP_RBX_FIELD_ABS_ADDR;
    qword_t watch_end = watch_start + HTOP_RBX_FIELD_SIZE;
    return start < watch_end && end > watch_start;
}

// Returns true iff it resolved a guest pointer of its own, which is the
// caller's signal that the mem read lock may have been dropped and retaken
// underneath it: mem_ptr upgrades to the write lock to materialise a lazy
// reservation, grow a stack down, or break COW (emu/memory.c, via
// read_to_write_lock), and a sibling munmap can free host backing in that
// window. __user_write_task_mem calls this with a host pointer already minted,
// so it has to re-mint it afterwards. Same trap as the multi-pointer walks
// below; see user_transform_two's comment for the full account.
static inline bool trace_htop_user_write(struct task *task, struct mem *mem,
        guest_addr_t addr, const void *buf, size_t count, bool ptrace) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_HTOP_USER_WRITE") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (task == NULL || strcmp(task->comm, "htop") != 0)
        return false;
    if (!htop_watch_intersects(addr, count))
        return false;

    uint8_t field[HTOP_RBX_FIELD_SIZE] = {};
    bool have_field = false;
    void *field_ptr = mem_ptr(mem, HTOP_RBX_FIELD_ABS_ADDR,
                              ptrace ? MEM_READ : MEM_READ);
    if (field_ptr != NULL) {
        memcpy(field, field_ptr, sizeof(field));
        have_field = true;
    }

    uint64_t observed = 0;
    size_t observed_size = count < sizeof(observed) ? count : sizeof(observed);
    memcpy(&observed, buf, observed_size);

    printk("htop user_write: addr=%#x count=%zu ptrace=%d value=%#llx\n",
           addr, count, ptrace, (unsigned long long) observed);
    if (have_field) {
        printk("htop user_write field: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               field[0], field[1], field[2], field[3],
               field[4], field[5], field[6], field[7]);
    }
    return true;
}

struct task_mem_read_handle {
    struct mm *mm;
    struct mem *mem;
};

static struct mem *task_mem_read_lock(struct task *task, struct task_mem_read_handle *handle) {
    struct mem *mem;
    handle->mm = NULL;
    handle->mem = NULL;
    if (task == current) {
        mem = task->mem;
        if (mem != NULL) {
            mem_read_lock_quiesce_aware(mem);
            handle->mem = mem;
        }
        return mem;
    }
    lock(&task->general_lock, 0);
    if (task->mm != NULL) {
        handle->mm = task->mm;
        mm_retain(handle->mm);
        mem = &handle->mm->mem;
        mem_read_lock_quiesce_aware(mem);
        handle->mem = mem;
    } else {
        mem = NULL;
    }
    unlock(&task->general_lock);
    return mem;
}

static void task_mem_read_unlock(struct task_mem_read_handle *handle) {
    if (handle->mem != NULL)
        mem_read_unlock_quiesce_aware(handle->mem);
    if (handle->mm != NULL)
        mm_release(handle->mm);
}

static bool user_range_valid_mem(struct task *task, struct mem *mem, guest_addr_t addr, size_t count) {
    if (!guest_abi_range_valid(task->abi, addr, count))
        return false;
    if (count == 0)
        return true;
    qword_t last = (qword_t) addr + count - 1;
    return PAGE(last) < mem->page_limit;
}

static int __user_read_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, void *buf, size_t count) {
    if (!user_range_valid_mem(task, mem, addr, count))
        return 1;
    char *cbuf = (char *) buf;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    while ((qword_t) p < end) {
        qword_t chunk_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > end)
            chunk_end = end;
  
        const char *ptr = mem_ptr(mem, p, MEM_READ);
        
        if (ptr == NULL)
            return 1;
        memcpy(&cbuf[p - addr], ptr, chunk_end - p);
        p = (guest_addr_t) chunk_end;
    }
    return 0;
}

static int __user_write_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count, bool ptrace) {
    if (!user_range_valid_mem(task, mem, addr, count))
        return 1;
    const char *cbuf = (const char *) buf;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    while ((qword_t) p < end) {
        qword_t chunk_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > end)
            chunk_end = end;
        char *ptr = mem_ptr(mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
        if (ptr == NULL)
            return 1;
        if (trace_htop_user_write(task, mem, p, &cbuf[p - addr], chunk_end - p, ptrace)) {
            // The tracer resolved a pointer of its own, so ptr may have been
            // freed while the read lock was briefly dropped. Re-mint it before
            // the memcpy rather than trusting the pre-trace value.
            ptr = mem_ptr(mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
            if (ptr == NULL)
                return 1;
        }
        memcpy(ptr, &cbuf[p - addr], chunk_end - p);
        p = (guest_addr_t) chunk_end;
    }
    return 0;
}

int user_read_task(struct task *task, guest_addr_t addr, void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_read_task_mem(task, mem, addr, buf, count);
    task_mem_read_unlock(&handle);
    return res;
}

int user_read_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, void *buf, size_t count) {
    if (mem == NULL)
        return 1;
    mem_read_lock_quiesce_aware(mem);
    int res = __user_read_task_mem(task, mem, addr, buf, count);
    mem_read_unlock_quiesce_aware(mem);
    return res;
}

int user_read(guest_addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

static int user_write_task_mem_internal(struct task *task, struct mem *mem, guest_addr_t addr,
                                        const void *buf, size_t count, bool ptrace) {
    if (mem == NULL)
        return 1;
    mem_read_lock_quiesce_aware(mem);
    int res = __user_write_task_mem(task, mem, addr, buf, count, ptrace);
    mem_read_unlock_quiesce_aware(mem);
    return res;
}

int user_write_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task_mem_internal(task, mem, addr, buf, count, false);
}

int user_write_task_ptrace_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task_mem_internal(task, mem, addr, buf, count, true);
}

int user_write_task(struct task *task, guest_addr_t addr, const void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_write_task_mem(task, mem, addr, buf, count, false);
    task_mem_read_unlock(&handle);
    return res;
}

// Zero a guest buffer directly (used to scrub an AEAD-open output buffer when
// authentication fails). Same locking discipline as user_transform_two.
int user_zero(guest_addr_t addr, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    if (!user_range_valid_mem(current, mem, addr, count))
        res = 1;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    while (res == 0 && (qword_t) p < end) {
        qword_t page_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (page_end > end) page_end = end;
        void *host = mem_ptr(mem, p, MEM_WRITE);
        if (host == NULL) { res = 1; break; }
        memset(host, 0, page_end - p);
        p = (guest_addr_t) page_end;
    }
    task_mem_read_unlock(&handle);
    return res;
}

// Read-only direct-pointer walk over one guest buffer: calls fn(host, span,
// ctx) for each page-span with a direct host pointer (no copy). Used by the
// crypto accelerator's tag/MAC path to Poly1305 guest ciphertext in place.
// Returns 0 on success, 1 on fault.
int user_read_walk(guest_addr_t addr, size_t count,
        void (*fn)(const void *host, size_t span, void *ctx), void *ctx) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    guest_addr_t p = addr;
    qword_t end = (qword_t) addr + count;
    if (!user_range_valid_mem(current, mem, addr, count))
        res = 1;
    while (res == 0 && (qword_t) p < end) {
        qword_t page_end = ((qword_t) PAGE(p) + 1) << PAGE_BITS;
        if (page_end > end) page_end = end;
        const void *host = mem_ptr(mem, p, MEM_READ);
        if (host == NULL) { res = 1; break; }
        fn(host, page_end - p, ctx);
        p = (guest_addr_t) page_end;
    }
    task_mem_read_unlock(&handle);
    return res;
}

// The address-space change counter, sampled by the multi-pointer walks below
// around the window in which they hold more than one live host pointer. Every
// structural page-table mutation bumps it (pt_map, pt_unmap_always, pt_dup,
// pt_move, pt_set_flags, pt_copy_on_write, all via mem_changed) before it
// releases the lock it mutated under, so a walk that reads the same value
// before and after a mem_ptr call knows no writer ran in between. The one
// bumper that is not a write-lock holder is the growth-mmap fast path
// (kernel/mmap.c mem_growth_lock, which takes pt_alloc_lock and the READ
// lock); it only ever adds pages and frees nothing, so its bumps can cost this
// walk a retry but can never be the thing the walk needs to catch. Relaxed is
// enough: the lock the writer released and we re-acquired supplies the
// ordering, and this value is only ever compared against itself.
static inline uint64_t mem_change_id(struct mem *mem) {
    return atomic_load_explicit(&mem->mmu.changes, memory_order_relaxed);
}

// Walk two guest buffers ([in, count) read, [out, count) write) in lockstep
// spans -- each bounded by BOTH buffers' page boundaries -- and hand the
// caller DIRECT host pointers for each span, so a bulk transform (e.g. the
// crypto accelerator) runs straight over guest memory with no bounce buffer.
//
// Safe against mem_ptr's internal lock upgrades, and that is the subtle part
// of this function. mem_ptr drops the mem read lock and retakes it as a writer
// whenever it has to change the page table -- its lazy-reservation, growsdown
// and COW branches (emu/memory.c) all go through read_to_write_lock
// (util/rw_locks.h), which drops our reader and then waits for the remaining
// readers to drain. It does that under its own mutex, but the condvar sleep
// releases that mutex, so another writer can take the lock ahead of us -- in
// particular mem_write_lock_with_pokes' trylockw (util/rw_locks.h), which
// never registers write intent and succeeds the instant the reader count hits
// zero. In that window a sibling thread's munmap can take the barrier and free
// the host backing, so any pointer already minted for this span is only
// trustworthy if no writer ran while it was live.
//
// This used to be handled by ordering alone: resolve the write pointer first,
// on the theory that it is the only one that can upgrade, then the read
// pointer, "which never upgrades for an already-resident buffer". The
// already-resident premise is false, and that was the bug. The lazy and
// growsdown upgrades fire for MEM_READ too, keyed only on the entry being
// absent, so resolving the read pointer could drop the lock with the write
// pointer live and then hand fn() a dangling out_host.
// tests/manual/pread_stack_thread_race.c documents that crash shape (a host
// EXC_BAD_ACCESS inside a memcpy over a pointer mem_ptr had just validated)
// and already suspected the upgrade.
//
// What holds now: the write pointer is still resolved first (COW ordering, and
// its upgrade is free because no other pointer is live yet), and
// mem->mmu.changes is sampled around every resolve made after it. If the
// counter moved, some structural writer ran while out_host was live, so both
// pointers are dropped and the span is resolved again. fn() is never called on
// a span whose pointers straddle a page-table change and never runs twice on
// the same span, which matters because the accelerator's callbacks carry
// streaming cipher state across spans. In the steady state (both buffers
// resident) mem_ptr performs no upgrade at all, so the window between the two
// loads holds a page-table lookup and, on the first touch of a host page under
// protection mirroring, one mprotect from mem_ensure_host_writable
// (emu/memory.c -- mem_ptr's done_write_fault tail runs it for MEM_READ too,
// on any resident page carrying P_WRITE), after which the host_page_prot cache
// makes it a compare. The check itself costs two relaxed loads per span --
// this runs over megabytes of AES and pixel work, so the fast path had to stay
// a fast path.
//
// A retry costs one more resolve of each pointer, on pages that are resident
// by then, and only happens when the counter really moved: an upgrade here, or
// an unrelated sibling, because the growth-mmap fast path (kernel/mmap.c
// mem_growth_lock) bumps the counter while holding only the read lock and so
// can run alongside us. Either way the event that forced the retry is far more
// expensive than the retry, so this is a seqlock read side rather than a spin.
// No resolved pointer is ever held across the next span's resolve. Returns 0
// on success, 1 on fault (a partial prefix of out may have been written -- the
// caller treats that as EFAULT, same as a torn user_write).
int user_transform_two(guest_addr_t in, guest_addr_t out, size_t count,
        void (*fn)(const void *in_host, void *out_host, size_t span, void *ctx),
        void *ctx) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    guest_addr_t ip = in, op = out;
    size_t left = count;
    if (!user_range_valid_mem(current, mem, in, count) ||
            !user_range_valid_mem(current, mem, out, count))
        res = 1;
    while (res == 0 && left > 0) {
        qword_t in_page_end  = ((qword_t) PAGE(ip) + 1) << PAGE_BITS;
        qword_t out_page_end = ((qword_t) PAGE(op) + 1) << PAGE_BITS;
        size_t span = left;
        if (span > in_page_end - ip)   span = in_page_end - ip;
        if (span > out_page_end - op)  span = out_page_end - op;
        void *out_host = NULL;
        const void *in_host = NULL;
        for (;;) {
            in_host = NULL;
            out_host = mem_ptr(mem, op, MEM_WRITE); // resolve write first (COW ordering)
            if (out_host == NULL)
                break;
            // out_host is live from here on, so any lock drop inside the
            // resolve below can invalidate it.
            uint64_t gen = mem_change_id(mem);
            in_host = mem_ptr(mem, ip, MEM_READ);
            if (in_host == NULL)
                break;
            if (mem_change_id(mem) == gen)
                break; // nothing moved while both pointers were live
        }
        if (out_host == NULL || in_host == NULL) { res = 1; break; }
        fn(in_host, out_host, span, ctx);
        ip += span; op += span; left -= span;
    }
    task_mem_read_unlock(&handle);
    return res;
}

// One-image rectangular direct-pointer walk (kernel/ish_accel_pix.c's FILL
// kernel): calls fn(host, pixels, ctx) for each contiguous host span within
// a [x, x+width) x [y, y+height) sub-rectangle of a linear image with the
// given byte stride and bpp. Generalizes user_transform_two/user_read_walk
// from a single linear buffer to a 2D strided one -- each row is walked
// left to right, re-resolving a host pointer at every page boundary the row
// crosses (a row can span several host pages; consecutive guest pages are
// not guaranteed host-contiguous). bpp must evenly divide the host page
// size's relationship to the row's byte alignment -- callers only ever pass
// bpp values (4 for a8r8g8b8/x8r8g8b8) that keep every pixel's byte range
// inside a single page as long as `base` and `stride` are bpp-aligned
// (always true for real wl_shm/cairo surfaces); span==0 below is the decline
// signal for a caller that violated that assumption, treated as EFAULT
// rather than ever emitting a torn pixel.
int user_transform_rect(guest_addr_t base, uint32_t stride, uint32_t bpp,
        int32_t x, int32_t y, uint32_t width, uint32_t height, int prot,
        void (*fn)(void *host, uint32_t pixels, void *ctx), void *ctx) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    // Conservative single bounds check covering the whole rectangle's real
    // backing (every row of the rect lies within this linear span, since
    // width*bpp <= stride for any real sub-rect of an image) -- mirrors
    // user_transform_two's up-front user_range_valid_mem call. Individual
    // mem_ptr resolves below are the actual per-page enforcement.
    if (!user_range_valid_mem(current, mem,
            base + (qword_t) y * stride + (qword_t) x * bpp, (size_t) height * stride))
        res = 1;
    for (uint32_t row = 0; res == 0 && row < height; row++) {
        uint32_t remaining = width;
        int32_t cx = x;
        while (remaining > 0) {
            guest_addr_t addr = (guest_addr_t) (base + (qword_t) (y + row) * stride + (qword_t) cx * bpp);
            qword_t page_end = ((qword_t) PAGE(addr) + 1) << PAGE_BITS;
            uint32_t max_pixels = (uint32_t) ((page_end - addr) / bpp);
            uint32_t span = remaining < max_pixels ? remaining : max_pixels;
            if (span == 0) { res = 1; break; }
            void *host = mem_ptr(mem, addr, prot);
            if (host == NULL) { res = 1; break; }
            fn(host, span, ctx);
            cx += (int32_t) span;
            remaining -= span;
        }
    }
    task_mem_read_unlock(&handle);
    return res;
}

// Two-image rectangular direct-pointer walk (kernel/ish_accel_pix.c's COPY/
// OVER kernels): same per-row/per-page-span discipline as
// user_transform_rect, but resolves a DESTINATION and SOURCE sub-rectangle
// in lockstep, each span bounded by BOTH images' independent page grids (a
// dst page boundary and a src page boundary at different guest addresses
// don't line up in general). Per span, the write pointer is resolved before
// the read pointer and the resolves are then re-run if mem->mmu.changes moved
// while both were live -- same lock-upgrade safety user_transform_two
// documents at length, and the same bug before this was added: resolving the
// src pointer can materialise a lazy or growsdown page, which drops the read
// lock with dst_host already minted and lets a sibling munmap free it under
// us. Neither pointer is held across the next span's resolve. The
// caller must have already declined self-overlapping src==dst regions
// (this walk has no memmove-direction logic); it only ever does the
// requested op forward, left-to-right, top-to-bottom.
int user_transform_rect_two(
        guest_addr_t dst_base, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        guest_addr_t src_base, uint32_t src_stride, int32_t src_x, int32_t src_y,
        uint32_t bpp, uint32_t width, uint32_t height,
        void (*fn)(const void *src_host, void *dst_host, uint32_t pixels, void *ctx),
        void *ctx) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    if (!user_range_valid_mem(current, mem,
                dst_base + (qword_t) dst_y * dst_stride + (qword_t) dst_x * bpp, (size_t) height * dst_stride) ||
            !user_range_valid_mem(current, mem,
                src_base + (qword_t) src_y * src_stride + (qword_t) src_x * bpp, (size_t) height * src_stride))
        res = 1;
    for (uint32_t row = 0; res == 0 && row < height; row++) {
        uint32_t remaining = width;
        int32_t dcx = dst_x, scx = src_x;
        while (remaining > 0) {
            guest_addr_t daddr = (guest_addr_t) (dst_base + (qword_t) (dst_y + row) * dst_stride + (qword_t) dcx * bpp);
            guest_addr_t saddr = (guest_addr_t) (src_base + (qword_t) (src_y + row) * src_stride + (qword_t) scx * bpp);
            qword_t d_page_end = ((qword_t) PAGE(daddr) + 1) << PAGE_BITS;
            qword_t s_page_end = ((qword_t) PAGE(saddr) + 1) << PAGE_BITS;
            uint32_t d_max = (uint32_t) ((d_page_end - daddr) / bpp);
            uint32_t s_max = (uint32_t) ((s_page_end - saddr) / bpp);
            uint32_t span = remaining;
            if (span > d_max) span = d_max;
            if (span > s_max) span = s_max;
            if (span == 0) { res = 1; break; }
            void *dst_host = NULL;
            const void *src_host = NULL;
            for (;;) {
                src_host = NULL;
                dst_host = mem_ptr(mem, daddr, MEM_WRITE); // resolve write first (COW ordering)
                if (dst_host == NULL)
                    break;
                uint64_t gen = mem_change_id(mem); // dst_host is live from here on
                src_host = mem_ptr(mem, saddr, MEM_READ);
                if (src_host == NULL)
                    break;
                if (mem_change_id(mem) == gen)
                    break; // nothing moved while both pointers were live
            }
            if (dst_host == NULL || src_host == NULL) { res = 1; break; }
            fn(src_host, dst_host, span, ctx);
            dcx += (int32_t) span; scx += (int32_t) span;
            remaining -= span;
        }
    }
    task_mem_read_unlock(&handle);
    return res;
}

// Three-image rectangular direct-pointer walk (kernel/ish_accel_pix.c's
// OVER_MASK_A8 kernel): same discipline as user_transform_rect_two, but
// resolves a THIRD (mask) sub-rectangle in lockstep too, with its own bpp
// (the a8 mask is 1 byte/pixel, vs dst/src's 4) and its own independent
// page grid. Per span, dst (write) is resolved first, then src and mask
// (both read), and the whole span is re-resolved if mem->mmu.changes moved
// while any of them was live -- same lock-upgrade safety the two-image walk
// uses and user_transform_two explains. This one had the widest exposure of
// the three: the mask resolve is the third, so before this it could drop the
// read lock with two live pointers already minted. No resolved pointer is
// ever held across the next span's resolve.
int user_transform_rect_three(
        guest_addr_t dst_base, uint32_t dst_stride, int32_t dst_x, int32_t dst_y, uint32_t dst_bpp,
        guest_addr_t src_base, uint32_t src_stride, int32_t src_x, int32_t src_y, uint32_t src_bpp,
        guest_addr_t mask_base, uint32_t mask_stride, int32_t mask_x, int32_t mask_y, uint32_t mask_bpp,
        uint32_t width, uint32_t height,
        void (*fn)(const void *src_host, const void *mask_host, void *dst_host, uint32_t pixels, void *ctx),
        void *ctx) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    int res = 0;
    if (!user_range_valid_mem(current, mem,
                dst_base + (qword_t) dst_y * dst_stride + (qword_t) dst_x * dst_bpp, (size_t) height * dst_stride) ||
            !user_range_valid_mem(current, mem,
                src_base + (qword_t) src_y * src_stride + (qword_t) src_x * src_bpp, (size_t) height * src_stride) ||
            !user_range_valid_mem(current, mem,
                mask_base + (qword_t) mask_y * mask_stride + (qword_t) mask_x * mask_bpp, (size_t) height * mask_stride))
        res = 1;
    for (uint32_t row = 0; res == 0 && row < height; row++) {
        uint32_t remaining = width;
        int32_t dcx = dst_x, scx = src_x, mcx = mask_x;
        while (remaining > 0) {
            guest_addr_t daddr = (guest_addr_t) (dst_base + (qword_t) (dst_y + row) * dst_stride + (qword_t) dcx * dst_bpp);
            guest_addr_t saddr = (guest_addr_t) (src_base + (qword_t) (src_y + row) * src_stride + (qword_t) scx * src_bpp);
            guest_addr_t maddr = (guest_addr_t) (mask_base + (qword_t) (mask_y + row) * mask_stride + (qword_t) mcx * mask_bpp);
            qword_t d_page_end = ((qword_t) PAGE(daddr) + 1) << PAGE_BITS;
            qword_t s_page_end = ((qword_t) PAGE(saddr) + 1) << PAGE_BITS;
            qword_t m_page_end = ((qword_t) PAGE(maddr) + 1) << PAGE_BITS;
            uint32_t d_max = (uint32_t) ((d_page_end - daddr) / dst_bpp);
            uint32_t s_max = (uint32_t) ((s_page_end - saddr) / src_bpp);
            uint32_t m_max = (uint32_t) ((m_page_end - maddr) / mask_bpp);
            uint32_t span = remaining;
            if (span > d_max) span = d_max;
            if (span > s_max) span = s_max;
            if (span > m_max) span = m_max;
            if (span == 0) { res = 1; break; }
            void *dst_host = NULL;
            const void *src_host = NULL, *mask_host = NULL;
            for (;;) {
                src_host = mask_host = NULL;
                dst_host = mem_ptr(mem, daddr, MEM_WRITE); // resolve write first (COW ordering)
                if (dst_host == NULL)
                    break;
                uint64_t gen = mem_change_id(mem); // dst_host is live from here on
                src_host = mem_ptr(mem, saddr, MEM_READ);
                if (src_host == NULL)
                    break;
                mask_host = mem_ptr(mem, maddr, MEM_READ);
                if (mask_host == NULL)
                    break;
                if (mem_change_id(mem) == gen)
                    break; // nothing moved while all three pointers were live
            }
            if (dst_host == NULL || src_host == NULL || mask_host == NULL) { res = 1; break; }
            fn(src_host, mask_host, dst_host, span, ctx);
            dcx += (int32_t) span; scx += (int32_t) span; mcx += (int32_t) span;
            remaining -= span;
        }
    }
    task_mem_read_unlock(&handle);
    return res;
}

int user_write_task_ptrace(struct task *task, guest_addr_t addr, const void *buf, size_t count) {
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(task, &handle);
    if (mem == NULL)
        return 1;
    int res = __user_write_task_mem(task, mem, addr, buf, count, true);
    task_mem_read_unlock(&handle);
    return res;
}

int user_write(guest_addr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(guest_addr_t addr, char *buf, size_t max) {
    if (addr == 0)
        return 1;
    if (max == 0)
        return 1;
    if (!guest_abi_addr_valid(current->abi, addr))
        return 1;
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    size_t i = 0;
    while (i < max) {
        if (!guest_abi_range_valid(current->abi, (qword_t) addr + i, 1)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (__user_read_task_mem(current, mem, addr + i, &buf[i], sizeof(buf[i]))) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    task_mem_read_unlock(&handle);
    if (i == max || buf[i] != '\0')
        return 1;
    return 0;
}

// Like user_read_string, but for pathnames: distinguishes a real memory fault
// (returns _EFAULT) from a string that overruns the buffer without a NUL
// (returns _ENAMETOOLONG, matching Linux for paths longer than PATH_MAX).
// Returns 0 on success. Callers should propagate the return value directly.
int user_read_path(guest_addr_t addr, char *buf, size_t max) {
    if (addr == 0)
        return _EFAULT;
    if (max == 0)
        return _EFAULT;
    if (!guest_abi_addr_valid(current->abi, addr))
        return _EFAULT;
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return _EFAULT;
    size_t i = 0;
    while (i < max) {
        if (!guest_abi_range_valid(current->abi, (qword_t) addr + i, 1)) {
            task_mem_read_unlock(&handle);
            return _EFAULT;
        }
        if (__user_read_task_mem(current, mem, addr + i, &buf[i], sizeof(buf[i]))) {
            task_mem_read_unlock(&handle);
            return _EFAULT;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    task_mem_read_unlock(&handle);
    // Buffer filled before a terminating NUL: the path is too long.
    if (i == max)
        return _ENAMETOOLONG;
    return 0;
}

int user_write_string(guest_addr_t addr, const char *buf) {
    if (addr == 0) {
        return 1;
    }
    if (!guest_abi_addr_valid(current->abi, addr))
        return 1;
    struct task_mem_read_handle handle;
    struct mem *mem = task_mem_read_lock(current, &handle);
    if (mem == NULL)
        return 1;
    size_t i = 0;
    do {
        if (!guest_abi_range_valid(current->abi, (qword_t) addr + i, 1)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        if (__user_write_task_mem(current, mem, addr + i, &buf[i], sizeof(buf[i]), false)) {
            task_mem_read_unlock(&handle);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    task_mem_read_unlock(&handle);
    return 0;
}

struct guest_iovec_ *user_read_iovecs_abi(struct task *task, enum guest_abi abi, guest_addr_t iov_addr, dword_t iov_count) {
    if (iov_count == 0)
        return NULL;
    if (iov_count > IOV_MAX)
        return ERR_PTR(_EINVAL);

    // arm64/riscv64 are 64-bit ABIs with the same {ptr, size_t} iovec layout
    // as amd64 (both fields qword_t) -- only i386 uses the 32-bit layout.
    // This used to check `abi == GUEST_ABI_AMD64` specifically, so arm64 and
    // riscv64 fell into the i386 (8-byte) branch: user_read_iovecs_abi read
    // half as many bytes as the guest's real 16-byte struct iovec, splitting
    // the 64-bit iov_base into a bogus base/len pair (len ends up as the
    // pointer's upper 32 bits, typically 0 for small addresses). Every
    // process_vm_readv call on those guests then copied 0 bytes, which is
    // why a real strace tracing an arm64/riscv64 process showed every
    // path/struct/array argument as empty or zeroed while return values
    // stayed correct (ptrace(2) PEEKDATA/PEEKTEXT wasn't affected, only the
    // process_vm_readv fast path this function feeds).
    size_t guest_size;
    if (guest_abi_is_64bit(abi)) {
        guest_size = sizeof(struct amd64_iovec_);
    } else {
        guest_size = sizeof(struct i386_iovec_);
    }

    size_t raw_size = guest_size * iov_count;
    void *raw_iov = malloc(raw_size);
    if (raw_iov == NULL)
        return ERR_PTR(_ENOMEM);
    if (user_read_task(task, iov_addr, raw_iov, raw_size)) {
        free(raw_iov);
        return ERR_PTR(_EFAULT);
    }

    struct guest_iovec_ *iov = malloc(sizeof(*iov) * iov_count);
    if (iov == NULL) {
        free(raw_iov);
        return ERR_PTR(_ENOMEM);
    }

    for (dword_t i = 0; i < iov_count; i++) {
        qword_t base;
        qword_t len;
        if (guest_abi_is_64bit(abi)) {
            struct amd64_iovec_ *amd64_iov = raw_iov;
            base = amd64_iov[i].base;
            len = amd64_iov[i].len;
        } else {
            struct i386_iovec_ *i386_iov = raw_iov;
            base = i386_iov[i].base;
            len = i386_iov[i].len;
        }
        if (!guest_abi_addr_valid(abi, base) || len > SIZE_MAX) {
            free(raw_iov);
            free(iov);
            return ERR_PTR(_EINVAL);
        }
        iov[i] = (struct guest_iovec_) {
            .base = base,
            .len = (size_t) len,
        };
    }
    free(raw_iov);
    return iov;
}

dword_t sys_process_vm_readv_guest(pid_t_ pid, guest_addr_t local_iov_addr, dword_t liovcnt,
                             guest_addr_t remote_iov_addr, dword_t riovcnt, dword_t flags) {
    if (flags != 0)
        return _EINVAL;

    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        return _ESRCH;
    if (task != current && task->parent != current && current->parent != task) {
        task_ref_cnt_mod(task, -1);
        return _EPERM;
    }
    // Being a parent or child is a relationship, not permission. Without this
    // an unprivileged child read its privileged parent's entire address space
    // -- the relationship test above was the only gate.
    if (!current_may_access_task_mem(task)) {
        task_ref_cnt_mod(task, -1);
        return _EPERM;
    }

    struct guest_iovec_ *local_iov = user_read_iovecs_abi(current, current->abi, local_iov_addr, liovcnt);
    if (IS_ERR(local_iov)) {
        task_ref_cnt_mod(task, -1);
        return PTR_ERR(local_iov);
    }
    struct guest_iovec_ *remote_iov = user_read_iovecs_abi(current, current->abi, remote_iov_addr, riovcnt);
    if (IS_ERR(remote_iov)) {
        free(local_iov);
        task_ref_cnt_mod(task, -1);
        return PTR_ERR(remote_iov);
    }

    dword_t local_index = 0, remote_index = 0;
    size_t local_off = 0, remote_off = 0;
    dword_t total = 0;

    while (local_index < liovcnt && remote_index < riovcnt) {
        while (local_index < liovcnt && local_iov[local_index].len == local_off) {
            local_index++;
            local_off = 0;
        }
        while (remote_index < riovcnt && remote_iov[remote_index].len == remote_off) {
            remote_index++;
            remote_off = 0;
        }
        if (local_index >= liovcnt || remote_index >= riovcnt)
            break;

        size_t local_left = local_iov[local_index].len - local_off;
        size_t remote_left = remote_iov[remote_index].len - remote_off;
        size_t chunk = local_left < remote_left ? local_left : remote_left;
        if (chunk == 0)
            break;

        char buf[4096];
        size_t done = 0;
        while (done < chunk) {
            size_t step = chunk - done;
            if (step > sizeof(buf))
                step = sizeof(buf);
            if (user_read_task(task, remote_iov[remote_index].base + remote_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            if (user_write(local_iov[local_index].base + local_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            done += step;
            total += step;
        }

        local_off += chunk;
        remote_off += chunk;
    }

    free(local_iov);
    free(remote_iov);
    task_ref_cnt_mod(task, -1);
    return total;
}

dword_t sys_process_vm_readv(pid_t_ pid, addr_t local_iov_addr, dword_t liovcnt,
                             addr_t remote_iov_addr, dword_t riovcnt, dword_t flags) {
    return sys_process_vm_readv_guest(pid, local_iov_addr, liovcnt, remote_iov_addr, riovcnt, flags);
}
