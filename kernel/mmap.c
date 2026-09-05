#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "emu/memory.h"
#include "platform/platform.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/swap.h"
#include "util/sync.h"

extern struct timespec lock_pause;

extern _Atomic long quiesce_barriers;
extern _Atomic long quiesce_writer_naps;

// Structural-writer serialization, held as the outer lock by the full evicting
// barrier below and by the growth fast path. It serializes growth-vs-growth and
// growth-vs-(unmap/protect/COW barrier) without evicting readers.
static void mem_struct_lock(struct mem *mem) {
    pthread_mutex_lock(&mem->pt_alloc_lock);
}
static void mem_struct_unlock(struct mem *mem) {
    pthread_mutex_unlock(&mem->pt_alloc_lock);
}

// Lock set for the pure-growth mmap fast path. pt_alloc_lock serializes against
// other structural writers that take it (other growth mmaps, and the evicting
// barrier used by unmap/mprotect/COW). The read lock additionally serializes
// against mem_ptr_fault(), which mutates the page table under write_lock WITHOUT
// taking pt_alloc_lock (stack-growth and COW faults) -- its write_lock excludes
// our read_lock, so the two never mutate the structure concurrently. Crucially,
// taking the *read* lock (not write) means we run concurrently with the sibling
// reader threads instead of evicting them: that is the whole point of the fast
// path. Growth only publishes new chunks/entries atomically and frees nothing,
// so concurrent readers are safe.
static void mem_growth_lock(struct mem *mem) {
    pthread_mutex_lock(&mem->pt_alloc_lock);
    read_lock(&mem->lock);
}
static void mem_growth_unlock(struct mem *mem) {
    read_unlock(&mem->lock);
    pthread_mutex_unlock(&mem->pt_alloc_lock);
}

static void mem_write_lock_with_pokes(struct mem *mem) {
    mem_struct_lock(mem);
    atomic_fetch_add_explicit(&quiesce_barriers, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&mem->quiesce_requested, 1, memory_order_acq_rel);
    // quiesce_requested holds off the quiesce-aware readers -- the syscall
    // side's mem_read_lock_quiesce_aware (emu/memory.h) and the guest-execution
    // side's task_wait_for_mem_quiesce (kernel/task.c), the latter being the
    // one this barrier is really racing and the reason a poke-and-drain scheme
    // works at all. It is NOT a blanket "no new reader can enter" guarantee,
    // and reading it as one is wrong three times over.
    //
    // First, the two helpers do not give the same thing.
    // mem_read_lock_quiesce_aware re-checks quiesce_requested after acquiring
    // and backs out if it went up, so it can never be inside the lock while we
    // are here. task_wait_for_mem_quiesce re-checks nothing: it waits for the
    // count to reach zero and its caller (task_run_current's loop) then takes a
    // plain read_lock, so a sibling that cleared that wait microseconds before
    // the bump just above is already inside. That is exactly the race the
    // re-pokes in the loop below cover.
    //
    // Second, four sites on production paths take mem->lock raw and never
    // consult quiesce_requested at all: kernel/calls.c's amd64 fault-state dump
    // (dump_fault_pt_state) and its two i386 GPF decoders
    // (i386_gpf_addr_needs_page_fault and handle_i386_stack_store_gpf), and
    // jit_x86_gpf_addr_accessible in jit/jit.c, which uses trylockr. Two more
    // families of raw acquire exist and are excluded for their own reasons, not
    // by this protocol: the amd64 trace probes in emu/amd64_interp.c, seven
    // more trylockr acquisitions of the same lock, every one of them reached
    // only when an ISH_TRACE_AMD64_* env knob is set (and one of them,
    // amd64_as_scan_template_probe, memcmps the ENTIRE page table while holding
    // it, so they must stay debug-only); and mem_growth_lock just above in this
    // file, which the pt_alloc_lock taken at the top of this function already
    // excludes.
    //
    // Third, writer preference does not cover for those four either, because
    // trylockw (util/rw_locks.h) never touches writers_waiting -- for the whole
    // spin below this writer is invisible to both reader predicates, so those
    // four can and do acquire mid-spin. They still do not livelock us. Three of
    // them are bounded page-table peeks with no blocking in them. The fourth,
    // handle_i386_stack_store_gpf's `mov [esp], r32` decoder, is bounded for a
    // different reason and must not be mistaken for a peek: it calls
    // mem_ptr(.., MEM_WRITE), which on a P_COW or growsdown page upgrades via
    // read_to_write_lock, blocks for the other readers to drain, and mutates
    // the page table (an mmap plus a page memcpy for a COW break). That upgrade
    // does bump writers_waiting, so unlike the trylockr sites it is not
    // invisible once it commits, and it completes in one page copy. The
    // fall-through past the loop then uses the blocking write_lock, which
    // finally registers write intent and shuts all of them out.
    //
    // cpu_poke sets a sticky flag the quiesce-aware readers check at the next
    // block boundary, so a poke up front + periodic re-pokes (covering a
    // sibling that raced in just before the bump) suffice -- no need to
    // re-signal every spin. sched_yield() hands the core to those readers so
    // they release in microseconds; only a stubborn hold falls through to
    // nanosleep, then to a blocking write_lock.
    for (int attempts = 0; attempts < 1024; attempts++) {
        if ((attempts & 63) == 0)
            task_poke_shared_mem(current, mem);
        if (trylockw(&mem->lock) == 0)
            return;
        if (attempts < 256) {
            sched_yield();
        } else {
            atomic_fetch_add_explicit(&quiesce_writer_naps, 1, memory_order_relaxed);
            nanosleep(&lock_pause, NULL);
        }
    }

    task_poke_shared_mem(current, mem);
    write_lock(&mem->lock);
}

static void mem_write_unlock_with_pokes(struct mem *mem) {
    write_unlock(&mem->lock);
    atomic_fetch_sub_explicit(&mem->quiesce_requested, 1, memory_order_acq_rel);
    mem_quiesce_wake_parked(mem); // release the condvar-parked waiters (memory.h)
    mem_struct_unlock(mem);
    // Anything the unmap under that lock decided to close, closed now that it
    // is gone. Doing it here rather than at each unmap site is what makes it
    // cover mremap, MAP_FIXED's overwrite and brk's shrink as well as munmap.
    // See struct mem's deferred_fds.
    mem_close_deferred_fds(mem);
}

// The pager's evictor needs this barrier from emu/memory.c, which cannot see a
// static in this file.
//
// A TASK-LESS CALLER IS SUPPORTED, which section 1.10 of
// docs/simulated_swap_plan.md requires and kswapd relies on: task_poke_shared_
// mem treats a NULL `task` as "no self to skip" rather than as "do nothing", so
// the poke round still evicts the guest threads holding the read lock. Before
// that, a barrier taken with `current == NULL` poked nobody and fell through to
// a blocking write_lock behind threads that only release when they leave guest
// code.
void mem_write_lock_pokes_external(struct mem *mem) {
    mem_write_lock_with_pokes(mem);
}
void mem_write_unlock_pokes_external(struct mem *mem) {
    mem_write_unlock_with_pokes(mem);
}

static bool amd64_vm_failure_trace_enabled(void) {
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

static void amd64_vm_failure_trace(const char *syscall, qword_t result,
        qword_t a0, qword_t a1, qword_t a2, qword_t a3, qword_t a4, qword_t a5) {
    enum { AMD64_VM_FAILURE_TRACE_BUDGET = 32 };
    static unsigned amd64_vm_failure_trace_count;
    if (!amd64_vm_failure_trace_enabled())
        return;
    if (amd64_vm_failure_trace_count >= AMD64_VM_FAILURE_TRACE_BUDGET)
        return;
    amd64_vm_failure_trace_count++;
    printk("amd64 vm fail: pid=%d comm=%s %s(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) = %#llx floor=%#llx ceiling=%#llx brk=%#llx start_brk=%#llx\n",
           current->pid, current->comm, syscall,
           (unsigned long long) a0, (unsigned long long) a1,
           (unsigned long long) a2, (unsigned long long) a3,
           (unsigned long long) a4, (unsigned long long) a5,
           (unsigned long long) result,
           (unsigned long long) ((guest_addr_t) current->mem->mmap_floor << PAGE_BITS),
           (unsigned long long) ((guest_addr_t) current->mem->mmap_ceiling << PAGE_BITS),
           (unsigned long long) current->mm->brk,
           (unsigned long long) current->mm->start_brk);
    if (strcmp(syscall, "mmap") == 0 && (sqword_t) a4 >= 0) {
        struct fd *fd = f_get_retain((fd_t) a4);
        if (fd != NULL) {
            char path[MAX_PATH];
            int path_err = generic_getpath(fd, path);
            if (path_err == 0) {
                printk("amd64 vm fail mmap fd: pid=%d fd=%lld path=%s\n",
                       current->pid, (long long) (fd_t) a4, path);
            } else {
                printk("amd64 vm fail mmap fd: pid=%d fd=%lld path_err=%d\n",
                       current->pid, (long long) (fd_t) a4, path_err);
            }
            fd_close(fd);
        }
    }
}

// Never 0 and never repeated, for the life of the process. See struct mm's id.
static uint64_t mm_next_id(void) {
    static _Atomic uint64_t next;
    return atomic_fetch_add_explicit(&next, 1, memory_order_relaxed) + 1;
}

static void mm_apply_abi_layout(struct mm *mm, enum guest_abi abi) {
    struct guest_vm_layout layout = guest_abi_vm_layout(abi);
    mem_set_page_limit(&mm->mem, layout.page_limit);
    mem_set_mmap_window(&mm->mem, layout.mmap_floor, layout.mmap_ceiling);
}

struct mm *mm_new(enum guest_abi abi) {
    // Zeroed, not merely allocated. Only some of struct mm is assigned below;
    // the rest was whatever malloc handed back -- vdso, the argv/env/auxv/stack
    // bounds procfs reports, and rss_pages_hwm.
    //
    // rss_pages_hwm is the one that cannot be left to luck, because it is a
    // HIGH-WATER MARK: task_maxrss_kb only ever raises it and then reports it,
    // so an initial value larger than anything real is never corrected by a
    // later honest sample -- it is simply what getrusage answers for the life
    // of the address space. Nothing on any path has ever initialized it. It has
    // stayed quiet because malloc mostly hands back fresh, already-zero pages,
    // which is not a guarantee and not something to keep relying on.
    //
    // The procfs bounds are overwritten by elf_exec while it loads an image; a
    // native exec (kernel/exec.c) loads none and overwrites none of them.
    struct mm *mm = calloc(1, sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mem_init(&mm->mem);
    // mem_init deliberately leaves these alone (so mm_copy's shallow struct
    // copy can hand them down to a fork()'d child), and it runs after the
    // calloc above, so a genuinely new mm still has to clear them itself.
    mm->mem.brk_reserve_start = mm->mem.brk_reserve_end = 0;
    mm_apply_abi_layout(mm, abi);
    ipc_mm_init(mm);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->mlockall_flags = 0;
    mm->exefile = NULL;
    mm->refcount = 1;
    mm->id = mm_next_id();
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    *new_mm = *mm;
    // A copy is a different address space, whatever it holds. The struct copy
    // above would otherwise hand it the original's identity.
    new_mm->id = mm_next_id();
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    mem_init(&new_mm->mem);
    mem_set_page_limit(&new_mm->mem, mm->mem.page_limit);
    mem_set_mmap_window(&new_mm->mem, mm->mem.mmap_floor, mm->mem.mmap_ceiling);
    // mem_init cleared these out of the wholesale struct copy above; a fork's
    // child runs the same image on the same stack under the same limit.
    mem_set_stack_bounds(&new_mm->mem, mm->mem.stack_top,
                         (uint64_t) atomic_load(&mm->mem.stack_limit_pages) << PAGE_BITS);
    ipc_mm_init(new_mm);
    // NULL for a task that has not exec'd anything yet, and mm_release already
    // tolerates that. A native exec is no longer one of those cases: it takes
    // the /AOK/native/<name> descriptor so /proc/<pid>/exe names the program
    // (kernel/exec.c), where it used to leave whatever the parent had.
    if (new_mm->exefile != NULL)
        fd_retain(new_mm->exefile);
    // Use the quiesce/poke protocol: sibling threads sharing this mm hold the
    // read lock for as long as they execute guest code, and only a poke makes
    // them exit at the next block boundary. A plain write_lock can stall fork
    // behind a guest busy-loop indefinitely.
    mem_write_lock_with_pokes(&mm->mem);
    int copy_err = pt_copy_on_write(&mm->mem, &new_mm->mem, 0, mm->mem.page_limit);
    if (copy_err == 0)
        ipc_mm_copy(new_mm, mm);
    mem_write_unlock_with_pokes(&mm->mem);

    // pt_copy_on_write stops at the first page-table allocation it cannot make
    // -- mem_pt_new returning NULL out of a failed calloc for a leaf or a
    // chunk (emu/memory.c) -- and says so. That return used to be dropped, and
    // the half-copied child was handed back as a success: kernel/fork.c only
    // checks for NULL, so fork() returned a pid for a child whose address
    // space was a PREFIX of the parent's. On i386 the stack sits at the top of
    // the address space, so the pages that got dropped were the stack and the
    // high libraries essentially every time, and the child died on its first
    // instruction with nothing said about why. ENOMEM out of fork() is a
    // failure a guest knows how to handle; a truncated child is not.
    //
    // mm_release is the correct teardown here and not a double free: the
    // refcount is still the 1 set above, no task has ever pointed at this mm,
    // and it owns exactly what mm_release drops -- the exefile reference
    // retained above (or NULL, which mm_release tolerates), an shm_regions
    // list that ipc_mm_init left empty because ipc_mm_copy is skipped on this
    // path, and the mem whose partially built page table mem_destroy unmaps,
    // which is also what returns the per-page data->refcount that
    // pt_copy_on_write took on each page it did manage to copy.
    //
    // Deliberately after the parent's lock is released: the teardown walks the
    // whole child page table and can close deferred descriptors, and none of
    // that wants the parent's sibling threads held at the quiesce barrier.
    // What it does NOT undo is the P_COW bit pt_copy_on_write set on the
    // parent's own entries before it broke off. Once the child's references are
    // gone, the parent's next write to such a page takes one extra copy and
    // clears the bit -- but that copy is itself an mmap(PAGE_SIZE), in the COW
    // break shared by mem_ptr and mem_ptr_fault (emu/memory.c), made under the
    // very memory pressure that just failed the child's page-table allocation.
    // Both answer a MAP_FAILED with NULL, and NULL out of mem_ptr_fault is what
    // handle_page_fault_interrupt turns into a guest SIGSEGV. So the residue can
    // turn a survivable ENOMEM-from-fork into faults in the PARENT, which is the
    // process that was supposed to be fine. It is left alone anyway: it is
    // bit-for-bit the state the pre-change code returned in too, and clearing
    // the bits would mean retaking the barrier for a second full walk of the
    // address space on the one path where memory is already gone.
    if (copy_err < 0) {
        // Rate-limited like host_mem_headroom_low's warning in do_mmap below: a
        // guest that keeps retrying fork() under memory pressure must not turn
        // the one diagnostic into a flood.
        static _Atomic unsigned fork_oom_log_count;
        if (atomic_fetch_add_explicit(&fork_oom_log_count, 1, memory_order_relaxed) < 8)
            printk("WARNING: %d(%s) fork failed, out of memory copying the address space\n",
                   current != NULL ? current->pid : 0,
                   current != NULL ? current->comm : "?");
        mm_release(new_mm);
        return NULL;
    }
    return new_mm;
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

void mm_release(struct mm *mm) {
    if (--mm->refcount == 0) {
        while (mem_ref_cnt_get(&mm->mem) != 0)
            nanosleep(&lock_pause, NULL);
        if (mm->exefile != NULL)
            fd_close(mm->exefile);

        ipc_mm_release(mm);
        mem_destroy(&mm->mem);
        free(mm);
    }
}

static guest_addr_t do_mmap(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    int err;
    pages_t pages = PAGE_ROUND_UP(len);
    if (!pages) return _EINVAL;
    page_t page = 0;
    bool fixed = flags & (MMAP_FIXED | MMAP_FIXED_NOREPLACE);
    // MAP_FIXED means "here or nowhere". Address 0 fell straight past the
    // block below into the pick-any-hole path, so a request to map at 0 was
    // silently answered with a mapping somewhere else and reported as
    // success -- the one thing MAP_FIXED exists to rule out. A caller that
    // asks for 0 is either probing (and needs the refusal) or is about to
    // write through a pointer it believes is at 0. Linux refuses it on any
    // default system: mmap_min_addr keeps the first 64K unmappable, and the
    // answer is EPERM.
    if (fixed && addr == 0)
        return _EPERM;
    if (addr != 0) {
        // A non-FIXED address is only a hint: Linux rounds an unaligned value
        // down to a page boundary rather than rejecting it. MAP_FIXED and
        // MAP_FIXED_NOREPLACE demand an exactly page-aligned address.
        if (PGOFFSET(addr) != 0) {
            if (fixed)
                return _EINVAL;
            addr -= PGOFFSET(addr);
        }
        if (!guest_abi_range_valid(current->abi, addr, len))
            return _ENOMEM;
        page = PAGE(addr);
        if (!fixed && !pt_is_hole(current->mem, page, pages)) {
            // hint region is occupied -> let the kernel place it anywhere
            addr = 0;
        } else if ((flags & MMAP_FIXED_NOREPLACE) && !pt_is_hole(current->mem, page, pages)) {
            // MAP_FIXED_NOREPLACE must fail rather than clobber or relocate
            return _EEXIST;
        }
    }
    if (addr == 0) {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    }
    qword_t mapped_addr = (qword_t) page << PAGE_BITS;
    if (!guest_abi_range_valid(current->abi, mapped_addr, len))
        return _ENOMEM;
    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
        // Large anonymous mappings are RESERVED, not materialised: one
        // struct pt_entry per page is ~65 bytes of host memory, so an
        // untouched reservation used to cost ~16.6 MB per GiB the instant it
        // was asked for. mem_lazy_reserve declines below the size threshold
        // and when the table is full, leaving the eager path unchanged.
        if (!mem_lazy_reserve(current->mem, page, pages, prot)) {
            if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
                return err;
        }
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags)) < 0)
            return err;
        // ...but not every ->mmap produces a file-backed mapping. A PRIVATE
        // mapping of /dev/zero hands back plain anonymous memory, which is what
        // Linux does with it too -- mmap_zero in drivers/char/mem.c calls
        // vma_set_anonymous for exactly this case, so the mapping has no file
        // and /proc/<pid>/maps shows none. Stamping the descriptor on anyway
        // left pages that are anonymous by every internal test (P_ANONYMOUS is
        // what fork, madvise and mprotect look at) while presenting as
        // /dev/zero to everything that reads data->fd, and pinned the
        // descriptor for the life of the mapping with no file to fault back in.
        //
        // A SHARED one keeps its descriptor: Linux backs that with shmem and
        // leaves vm_file pointing at /dev/zero, so it does name a file there.
        struct pt_entry *pt = mem_pt(current->mem, page);
        bool private_anon = pt != NULL &&
                (pt->flags & P_ANONYMOUS) && !(pt->flags & P_SHARED);
        if (pt != NULL && pt->data != NULL && !private_anon) {
            pt->data->fd = fd_retain(fd);
            pt->data->file_offset = offset;
        }
    }
    return mapped_addr;
}

extern _Atomic long quiesce_growth_fast;

static bool mmap_growth_fast_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *off = getenv("ISH_NO_MMAP_GROWTH_FAST");
        enabled = (off != NULL && off[0] != '\0' && off[0] != '0') ? 0 : 1;
    }
    return enabled == 1;
}

// A pure-growth mmap only adds never-before-mapped pages. Anonymous (no fd
// backing to wire up) and not MAP_FIXED* means do_mmap always lands on a hole --
// a non-fixed hint that collides is relocated to a fresh hole, so the whole
// target range is unmapped and do_mmap unmaps nothing. Adding hole pages needs
// no reader eviction: those pages were never mapped, so no sibling holds a TLB
// entry for them, and the page-table chunk/leaf publication is already atomic
// (acquire/release). Only writer-vs-writer exclusion is required.
static bool mmap_is_pure_growth(dword_t flags) {
    return (flags & MMAP_ANONYMOUS) &&
           !(flags & (MMAP_FIXED | MMAP_FIXED_NOREPLACE));
}

static guest_addr_t mmap_common_guest(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap(%#llx, %#llx, 0x%x, 0x%x, %d, %#llx)",
           (unsigned long long) addr, (unsigned long long) len, prot, flags, fd_no,
           (unsigned long long) offset);
    if (len == 0)
        return _EINVAL;
    // Accepted and ignored, as Linux does. See PROT_SEM_ in kernel/calls.h
    // for why it is stripped rather than passed through.
    prot &= ~(dword_t) PROT_SEM_;
    if (prot & ~P_RWX)
        return _EINVAL;
    // Refuse guest memory growth when the app is close to its jetsam budget:
    // a runaway guest (e.g. a 10k-thread storm mapping a stack per thread)
    // must get clean ENOMEMs here rather than starving UIKit/libobjc into a
    // NULL-deref crash. No-op outside iOS. See host_mem_headroom_low().
    // Before refusing, try to make room. Direct reclaim pages some of this
    // process's cold anonymous memory out to the swap area the user asked for,
    // which is the whole point of having one: without it the guard is the only
    // answer and a large mostly-idle heap simply cannot exist.
    //
    // Here rather than beside the guard's own barrier, and before it, because
    // reclaim takes the address-space barrier itself and pt_alloc_lock is not
    // recursive. A no-op returning 0 when swap is off, which is the default.
    if (host_mem_headroom_low())
        swap_direct_reclaim(current->mem, len);
    if (host_mem_headroom_low()) {
        // This guard fires silently otherwise -- from the guest's point of
        // view every mmap() in the whole app just starts failing with a
        // clean ENOMEM, with nothing to explain why (e.g. a Wayland
        // compositor's *next* window failing to open, with no obvious cause,
        // because its wl_shm buffer's mmap got refused here). Rate-limited
        // so a runaway guest hammering mmap under low headroom can't flood
        // the log.
        static _Atomic unsigned headroom_log_count;
        if (atomic_fetch_add_explicit(&headroom_log_count, 1, memory_order_relaxed) < 8)
            printk("WARNING: %d(%s) mmap refused, low iOS memory headroom (len=%#llx)\n",
                   current->pid, current->comm, (unsigned long long) len);
        return _ENOMEM;
    }
    // MAP_SHARED_VALIDATE maps onto plain MAP_SHARED: the strict flag
    // checking it asks for is what this function already does. Rejecting it
    // with EINVAL denied the mapping entirely to callers using the safer of
    // the two spellings.
    if ((flags & MMAP_SHARED_VALIDATE) == MMAP_SHARED_VALIDATE)
        flags &= ~(dword_t) MMAP_PRIVATE;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;
    // exactly one of MAP_PRIVATE / MAP_SHARED is required
    if (!(flags & (MMAP_PRIVATE | MMAP_SHARED)))
        return _EINVAL;

    // Fast path: a pure-growth mmap (the musl mallocng hot path) skips the
    // stop-the-world poke barrier entirely -- it serializes only against other
    // structural writers and never evicts the running sibling threads.
    if (mmap_growth_fast_enabled() && mmap_is_pure_growth(flags)) {
        mem_growth_lock(current->mem);
        guest_addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
        mem_growth_unlock(current->mem);
        if ((sqword_t) res == _ENOMEM)
            amd64_vm_failure_trace("mmap", res, addr, len, prot, flags, (qword_t) fd_no, offset);
        else
            atomic_fetch_add_explicit(&quiesce_growth_fast, 1, memory_order_relaxed);
        return res;
    }

    // Anything the filesystem has to fetch from outside the kernel happens
    // here, with no address-space lock held -- see fd_ops.mmap_prepare. Doing
    // it below, inside do_mmap, would freeze this process's other threads for
    // the fetch and deadlock when one of them is the one being waited on.
    if (!(flags & MMAP_ANONYMOUS)) {
        // Retained, not borrowed: mmap_prepare can block for as long as a
        // guest daemon takes, with no lock held, and close(2) no longer waits
        // on the fd table while it runs its own ->close (fs/fd.c). A borrowed
        // pointer here would be a use-after-free the moment a sibling thread
        // closed this descriptor mid-fetch.
        struct fd *pfd = f_get_retain(fd_no);
        if (pfd != NULL) {
            int perr = 0;
            if (pfd->ops->mmap_prepare != NULL)
                perr = pfd->ops->mmap_prepare(pfd, (off_t) offset, (size_t) len);
            fd_close(pfd);
            if (perr < 0)
                return (guest_addr_t) perr;
        }
    }

    mem_write_lock_with_pokes(current->mem);
    guest_addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    mem_write_unlock_with_pokes(current->mem);
    if ((sqword_t) res == _ENOMEM)
        amd64_vm_failure_trace("mmap", res, addr, len, prot, flags, (qword_t) fd_no, offset);
    return res;
}

guest_addr_t sys_mmap_guest(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    return mmap_common_guest(addr, len, prot, flags, fd_no, offset);
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return (addr_t) mmap_common_guest(addr, len, prot, flags, fd_no, offset << PAGE_BITS);
}

enum membarrier_cmd {
    MEMBARRIER_CMD_QUERY = 0,
    MEMBARRIER_CMD_GLOBAL = 1 << 0,
    MEMBARRIER_CMD_GLOBAL_EXPEDITED = 1 << 1,
    MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED = 1 << 2,
    MEMBARRIER_CMD_PRIVATE_EXPEDITED = 1 << 3,
    MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED = 1 << 4,
};
// Exactly what is implemented below -- no more. The SYNC_CORE and RSEQ
// commands Linux also offers are deliberately absent rather than claimed.
#define MEMBARRIER_SUPPORTED \
    (MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED | \
     MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)

// membarrier's whole contract is that when it returns, every OTHER thread has
// executed a full memory barrier. What was here was a fence in the calling
// thread -- which provides none of that, and the comment beside the advertised
// mask said "lies" outright.
//
// It matters because the libraries that use this query the mask and skip their
// own fallback when the kernel claims support: crossbeam-epoch (transitively
// under much of the Rust ecosystem), liburcu, and .NET's
// FlushProcessWriteBuffers all do exactly that. Claiming the feature and not
// providing it is worse than not claiming it, because it disables the
// fallback that would have worked.
//
// The barrier is imposed here the way those same libraries impose it when
// membarrier is unavailable: an mprotect round trip on a private page.
// Changing a mapping's protection makes the host kernel shoot down that entry
// on every core currently running one of our threads, and the interrupt that
// does it is a full barrier on that core; a thread not currently running
// executed one when it was descheduled. AOK's guest threads are host threads
// sharing one address space, so that covers exactly the set membarrier is
// defined over.
static void membarrier_impose(void) {
    static lock_t page_lock = LOCK_INITIALIZER;
    static void *page;
    lock(&page_lock, 0);
    if (page == NULL) {
        void *p = mmap(NULL, real_page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            *(volatile char *) p = 0;      // fault it in, so there is an entry to shoot down
            page = p;
        }
    }
    if (page != NULL) {
        mprotect(page, real_page_size, PROT_READ);
        mprotect(page, real_page_size, PROT_READ | PROT_WRITE);
    }
    unlock(&page_lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

dword_t sys_membarrier(dword_t cmd, dword_t flags, dword_t cpuid) {
    STRACE("membarrier(0x%x, 0x%x, 0x%x)", cmd, flags, cpuid);
    // Only MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ takes a flag, and that is not
    // implemented, so any nonzero flags word is EINVAL -- as on Linux, which
    // is how a caller probes for the commands it can use.
    if (flags != 0)
        return _EINVAL;
    switch (cmd) {
        case MEMBARRIER_CMD_QUERY:
            return MEMBARRIER_SUPPORTED;

        case MEMBARRIER_CMD_PRIVATE_EXPEDITED: {
            // Refused until the process has registered, which is how a runtime
            // learns that it must.
            lock(&current->group->lock, 0);
            bool registered = current->group->membarrier_private_expedited;
            unlock(&current->group->lock);
            if (!registered)
                return _EPERM;
            membarrier_impose();
            return 0;
        }

        case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
            lock(&current->group->lock, 0);
            current->group->membarrier_private_expedited = true;
            unlock(&current->group->lock);
            return 0;

        case MEMBARRIER_CMD_GLOBAL:
        case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
            membarrier_impose();
            return 0;

        case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
            // Linux needs no per-process state for the global variant either.
            return 0;

        default:
            // An unknown or unimplemented command is EINVAL. Returning success
            // told a caller that a future command it does not have a fallback
            // for had been honoured.
            return _EINVAL;
    }
}

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    STRACE("sys_mmap(0x%x)", args_addr);
    if (user_get(args_addr, args))
        return _EFAULT;
    return (addr_t) mmap_common_guest(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

addr_t sys_mmap_amd64(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %d) [amd64]", addr, len, prot, flags, fd_no, offset);
    return (addr_t) mmap_common_guest(addr, len, prot, flags, fd_no, offset);
}

// Distinct fds seen during a writeback walk, retained so they can be used
// after the address-space lock is dropped.
struct msync_fds {
    struct fd **fds;
    unsigned n, cap;
    bool oom;
};

static void msync_fds_add(struct msync_fds *list, struct fd *fd) {
    if (fd == NULL)
        return;
    for (unsigned i = 0; i < list->n; i++)
        if (list->fds[i] == fd)
            return;
    if (list->n == list->cap) {
        unsigned cap = list->cap == 0 ? 4 : list->cap * 2;
        struct fd **grown = realloc(list->fds, cap * sizeof(*grown));
        if (grown == NULL) {
            list->oom = true;
            return;
        }
        list->fds = grown;
        list->cap = cap;
    }
    list->fds[list->n++] = fd_retain(fd);
}

// Push a walk's noted descriptors at their filesystems, and release them.
// Call with NO address-space lock held: this is a round trip to a guest
// process. Returns the first error, which msync is required to report --
// "the data did not reach the filesystem" is exactly what a caller of
// msync(MS_SYNC) is asking about.
static int flush_backing_fds(struct msync_fds *seen) {
    int err = 0;
    for (unsigned i = 0; i < seen->n; i++) {
        int one = fuse_fd_msync_writeback(seen->fds[i]);
        if (one < 0 && err == 0)
            err = one;
        fd_close(seen->fds[i]);
    }
    free(seen->fds);
    seen->fds = NULL;
    seen->n = seen->cap = 0;
    return err;
}

int_t sys_munmap_guest(guest_addr_t addr, qword_t len) {
    STRACE("munmap(%#llx, %#llx)", (unsigned long long) addr, (unsigned long long) len);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    
    // Dropping the last reference to a mapped file's descriptor runs its
    // ->close, which may block on a guest process. That does not happen here:
    // pt_unmap parks such closes and mem_write_unlock_with_pokes runs them
    // once the lock is gone. See struct mem's deferred_fds.
    mem_write_lock_with_pokes(current->mem);
    int err = pt_unmap_always(current->mem, PAGE(addr), PAGE_ROUND_UP(len));
    mem_write_unlock_with_pokes(current->mem);

    if (err < 0)
        return _EINVAL;
    return 0;
}

int_t sys_munmap(addr_t addr, uint_t len) {
    return sys_munmap_guest(addr, len);
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

// Map the freshly-grown tail of a file-backed mapping during an mremap grow. The extra
// pages continue the same file at file_offset; the existing pages are left untouched (the
// caller pt_moves them when relocating), which preserves both MAP_SHARED contents and any
// MAP_PRIVATE/COW data already written. Mirrors the fd path in do_mmap().
static int mremap_map_file_extra(struct mem *mem, page_t start, pages_t pages,
        struct fd *fd, qword_t file_offset, unsigned pt_flags) {
    if (fd == NULL || fd->ops->mmap == NULL)
        return _EFAULT;
    int prot = pt_flags & (P_READ | P_WRITE | P_EXEC | P_SHARED);
    int mmap_flags = (pt_flags & P_SHARED) ? MMAP_SHARED : MMAP_PRIVATE;
    int err = fd->ops->mmap(fd, mem, start, pages, (off_t) file_offset, prot, mmap_flags);
    if (err < 0)
        return err;
    struct pt_entry *e = mem_pt(mem, start);
    if (e != NULL && e->data != NULL) {
        e->data->fd = fd_retain(fd);
        e->data->file_offset = file_offset;
    }
    return 0;
}

guest_addr_t sys_mremap_guest(guest_addr_t addr, qword_t old_len, qword_t new_len, dword_t flags,
        guest_addr_t new_addr) {
    STRACE("mremap(%#llx, %#llx, %#llx, %d, %#llx)", (unsigned long long) addr,
           (unsigned long long) old_len, (unsigned long long) new_len, flags,
           (unsigned long long) new_addr);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    // Per mremap(2), MREMAP_FIXED can only be used together with MREMAP_MAYMOVE.
    if ((flags & MREMAP_FIXED_) && !(flags & MREMAP_MAYMOVE_))
        return _EINVAL;
    if ((flags & MREMAP_FIXED_) && PGOFFSET(new_addr) != 0)
        return _EINVAL;
    pages_t old_pages = PAGE_ROUND_UP(old_len);
    pages_t new_pages = PAGE_ROUND_UP(new_len);
    // A zero new_len is EINVAL, and the original mapping is left alone. Without
    // this the shrink path below unmapped every page of the mapping and then
    // returned `addr` -- a success value, not in the errno window, so the
    // caller could not even tell. Any size computation that rounds or
    // underflows to zero silently destroyed the mapping it meant to resize.
    if (new_pages == 0)
        return _EINVAL;
    // Same jetsam-headroom backpressure as mmap_common_guest (grow only), with
    // the same attempt to make room first. Before the barrier below, because
    // reclaim takes it.
    if (new_pages > old_pages && host_mem_headroom_low()) {
        swap_direct_reclaim(current->mem,
                (uint64_t) (new_pages - old_pages) << PAGE_BITS);
        if (host_mem_headroom_low())
            return _ENOMEM;
    }
    guest_addr_t res = _ENOMEM;

    mem_write_lock_with_pokes(current->mem);

    // old_len == 0 is not a resize at all: it asks for a second mapping of the
    // same pages. Linux permits it only for a SHARED mapping, because aliasing
    // a private one would quietly break its privacy, and returns EINVAL
    // otherwise. AOK checked nothing, so the private case produced a stray
    // detached mapping (64 calls, 64 leaked regions), and the legitimate
    // shared case "succeeded" with a fresh zero page instead of an alias --
    // the caller lost coherence with the original and was told it had not.
    if (old_pages == 0) {
        struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
        if (entry == NULL) {
            res = _EFAULT;
            goto out;
        }
        if (!(entry->flags & P_SHARED)) {
            res = _EINVAL;
            goto out;
        }
        page_t dest;
        if (flags & MREMAP_FIXED_) {
            dest = PAGE(new_addr);
            if (pt_unmap(current->mem, dest, new_pages) < 0) {
                res = _ENOMEM;
                goto out;
            }
        } else {
            dest = pt_find_hole(current->mem, new_pages);
            if (dest == BAD_PAGE) {
                res = _ENOMEM;
                goto out;
            }
        }
        int dup_err = pt_dup(current->mem, PAGE(addr), dest, new_pages);
        res = dup_err < 0 ? (guest_addr_t) dup_err : (guest_addr_t) (dest << PAGE_BITS);
        goto out;
    }

    if (flags & MREMAP_FIXED_) {
        page_t src_page = PAGE(addr);
        page_t dest_page = PAGE(new_addr);
        // Real Linux allows some overlapping source/destination cases; getting
        // that right risks corrupting the very pages being relocated (the
        // implicit unmap of the destination could clobber source pages that
        // haven't been moved yet), so reject any overlap -- including the
        // source and destination being identical -- rather than risk a wrong,
        // silent result. This is a deliberate, documented simplification.
        if (dest_page < src_page + old_pages && src_page < dest_page + new_pages) {
            res = _EINVAL;
            goto out;
        }

        struct pt_entry *entry = mem_pt(current->mem, src_page);
        if (entry == NULL) {
            res = _EFAULT;
            goto out;
        }
        dword_t pt_flags = entry->flags;
        struct data *backing_data = entry->data;
        for (page_t page = src_page; page < src_page + old_pages; page++) {
            entry = mem_pt(current->mem, page);
            if (entry == NULL || (entry->flags & ~P_COW) != (pt_flags & ~P_COW)) {
                res = _EFAULT;
                goto out;
            }
        }

        // Clear whatever's currently mapped at the destination -- MREMAP_FIXED
        // implicitly unmaps the destination range first, like a combined
        // munmap(new_addr, new_len) + the move below.
        int err = pt_unmap_always(current->mem, dest_page, new_pages);
        if (err < 0) {
            res = _EFAULT;
            goto out;
        }

        if (new_pages > old_pages) {
            bool is_file = !(pt_flags & P_ANONYMOUS);
            struct fd *backing_fd = is_file && backing_data != NULL ? backing_data->fd : NULL;
            if (is_file && (backing_fd == NULL || backing_fd->ops->mmap == NULL)) {
                FIXME("mremap grow on a mapping with no growable backing fd");
                res = _EFAULT;
                goto out;
            }
            qword_t extra_file_offset = backing_data != NULL
                    ? backing_data->file_offset + ((qword_t) old_pages << PAGE_BITS) : 0;
            pages_t extra_pages = new_pages - old_pages;
            err = is_file
                    ? mremap_map_file_extra(current->mem, dest_page + old_pages, extra_pages, backing_fd, extra_file_offset, pt_flags)
                    : pt_map_nothing(current->mem, dest_page + old_pages, extra_pages, pt_flags & ~P_COW);
            if (err < 0) {
                res = err;
                goto out;
            }
        }

        // Shrinking-while-moving keeps only the first new_pages of the
        // original mapping (matching plain shrink's tail-truncation
        // semantics); the rest is simply dropped from the source below.
        pages_t move_pages = new_pages < old_pages ? new_pages : old_pages;
        err = pt_move(current->mem, src_page, dest_page, move_pages);
        if (err < 0) {
            res = err;
            goto out;
        }
        if (new_pages < old_pages) {
            err = pt_unmap(current->mem, src_page + move_pages, old_pages - move_pages);
            if (err < 0) {
                res = _EFAULT;
                goto out;
            }
        }
        res = (guest_addr_t) dest_page << PAGE_BITS;
        goto out;
    }

    // shrinking always works
    if (new_pages <= old_pages) {
        // No task_ref_cnt spin needed here (unlike sys_munmap_guest just above,
        // which has never had one): every reader of another task's memory
        // (process_vm_readv's user_read_task, /proc/<pid>/mem's
        // user_read_task_mem) takes mem->lock in read mode -- the same lock
        // mem_write_lock_with_pokes above already holds in write mode -- so
        // they're already excluded regardless of task_ref_cnt.
        int err = pt_unmap(current->mem, PAGE(addr) + new_pages, old_pages - new_pages);
        res = err < 0 ? _EFAULT : addr;
        goto out;
    }

    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry == NULL) {
        res = _EFAULT;
        goto out;
    }
    dword_t pt_flags = entry->flags;
    struct data *backing_data = entry->data; // capture before the loop reassigns `entry`
    // P_COW is internal per-page copy-on-write state: it legitimately differs across a
    // single mapping after fork() + partial writes -- exactly how apt's anonymous
    // DynamicMMap looks when it tries to grow. Require only the mapping's type and
    // permission bits to match, not COW state.
    for (page_t page = PAGE(addr); page < PAGE(addr) + old_pages; page++) {
        entry = mem_pt(current->mem, page);
        if (entry == NULL || (entry->flags & ~P_COW) != (pt_flags & ~P_COW)) {
            res = _EFAULT;
            goto out;
        }
    }
    // File-backed grow: map the extra tail pages from the same fd. Only fd-backed
    // mappings whose fd we retained (data->fd) are growable this way.
    bool is_file = !(pt_flags & P_ANONYMOUS);
    struct fd *backing_fd = is_file && backing_data != NULL ? backing_data->fd : NULL;
    qword_t extra_file_offset = backing_data != NULL
            ? backing_data->file_offset + ((qword_t) old_pages << PAGE_BITS) : 0;
    if (is_file && (backing_fd == NULL || backing_fd->ops->mmap == NULL)) {
        FIXME("mremap grow on a mapping with no growable backing fd");
        res = _EFAULT;
        goto out;
    }

    page_t extra_start = PAGE(addr) + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    bool extra_is_hole = pt_is_hole(current->mem, extra_start, extra_pages);
    if (extra_is_hole) {
        int err = is_file
                ? mremap_map_file_extra(current->mem, extra_start, extra_pages, backing_fd, extra_file_offset, pt_flags)
                : pt_map_nothing(current->mem, extra_start, extra_pages, pt_flags & ~P_COW);
        res = err < 0 ? err : addr;
        goto out;
    }

    if (!(flags & MREMAP_MAYMOVE_)) {
        amd64_vm_failure_trace("mremap", _ENOMEM, addr, old_len, new_len, flags, 0, 0);
        res = _ENOMEM;
        goto out;
    }

    page_t new_page = pt_find_hole(current->mem, new_pages);
    if (new_page == BAD_PAGE) {
        amd64_vm_failure_trace("mremap", _ENOMEM, addr, old_len, new_len, flags, 0, 0);
        res = _ENOMEM;
        goto out;
    }
    int err = is_file
            ? mremap_map_file_extra(current->mem, new_page + old_pages, extra_pages, backing_fd, extra_file_offset, pt_flags)
            : pt_map_nothing(current->mem, new_page + old_pages, extra_pages, pt_flags & ~P_COW);
    if (err == 0) {
        err = pt_move(current->mem, PAGE(addr), new_page, old_pages);
        if (err < 0)
            pt_unmap_always(current->mem, new_page + old_pages, extra_pages);
    }
    if (err < 0) {
        if (err == _ENOMEM)
            amd64_vm_failure_trace("mremap", err, addr, old_len, new_len, flags, 0, 0);
        res = err;
        goto out;
    }
    res = (guest_addr_t) new_page << PAGE_BITS;

out:
    mem_write_unlock_with_pokes(current->mem);
    return res;
}

int_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags, addr_t new_addr) {
    return (int_t) sys_mremap_guest(addr, old_len, new_len, flags, new_addr);
}

// Adding PROT_WRITE to a shared mapping of a file that was not opened for
// writing is EACCES. Linux decides this from VM_MAYWRITE, fixed at mmap time:
// a private mapping always has it (the write is a copy), a shared one only if
// the file itself is writable.
//
// AOK recorded nothing of the sort, so mprotect returned 0 and the store that
// followed hit a read-only host mapping and killed the process with SIGBUS.
// A program that checks the mprotect return -- as it should -- had no way to
// see that coming. mmap already refused the same thing at map time; this is
// the door it left open.
//
// Caller holds the memory write lock.
static bool mprotect_write_forbidden(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *e = mem_pt(mem, page);
        if (e == NULL || !(e->flags & P_SHARED) || e->data == NULL)
            continue;
        // Shared ANONYMOUS memory has no file behind it and is always writable.
        struct fd *fd = e->data->fd;
        if (fd != NULL && (fd->flags & O_ACCMODE_) == O_RDONLY_)
            return true;
    }
    return false;
}

int_t sys_mprotect_guest(guest_addr_t addr, qword_t len, int_t prot) {
    STRACE("mprotect(%#llx, %#llx, 0x%x)", (unsigned long long) addr,
           (unsigned long long) len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    prot &= ~(int_t) PROT_SEM_;    // see mmap_common_guest
    if (prot & ~P_RWX)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    mem_write_lock_with_pokes(current->mem);
    if ((prot & P_WRITE) && mprotect_write_forbidden(current->mem, PAGE(addr), pages)) {
        mem_write_unlock_with_pokes(current->mem);
        return _EACCES;
    }
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    mem_write_unlock_with_pokes(current->mem);
    if (err == _ENOMEM)
        amd64_vm_failure_trace("mprotect", err, addr, len, prot, 0, 0, 0);
    return err;
}

int_t sys_mprotect(addr_t addr, uint_t len, int_t prot) {
    return sys_mprotect_guest(addr, len, prot);
}

#define MADV_DONTNEED_ 4
#define MADV_FREE_ 8
#define MADV_REMOVE_ 9
#define MADV_WIPEONFORK_ 18
#define MADV_KEEPONFORK_ 19

// Advices Linux's madvise() accepts. Linux validates the argument up front and
// returns EINVAL for anything it does not recognize, independent of whether the
// advice has any effect. iSH acts on MADV_DONTNEED, MADV_FREE, MADV_REMOVE and
// the FORK pair; the rest are accepted as no-op hints so software that probes
// them does not see a spurious error.
static bool madvise_advice_valid(dword_t advice) {
    switch (advice) {
        case 0:   // MADV_NORMAL
        case 1:   // MADV_RANDOM
        case 2:   // MADV_SEQUENTIAL
        case 3:   // MADV_WILLNEED
        case 4:   // MADV_DONTNEED
        case 8:   // MADV_FREE
        case 9:   // MADV_REMOVE
        case 10:  // MADV_DONTFORK
        case 11:  // MADV_DOFORK
        case 12:  // MADV_MERGEABLE
        case 13:  // MADV_UNMERGEABLE
        case 14:  // MADV_HUGEPAGE
        case 15:  // MADV_NOHUGEPAGE
        case 16:  // MADV_DONTDUMP
        case 17:  // MADV_DODUMP
        case 18:  // MADV_WIPEONFORK
        case 19:  // MADV_KEEPONFORK
        case 20:  // MADV_COLD
        case 21:  // MADV_PAGEOUT
        case 25:  // MADV_COLLAPSE
            return true;
        default:
            return false;
    }
}

dword_t sys_madvise_guest(guest_addr_t addr, qword_t len, dword_t advice) {
    STRACE("madvise(%#llx, %#llx, %d)", (unsigned long long) addr,
           (unsigned long long) len, advice);
    // Linux validates the advice and the address up front for every advice.
    if (!madvise_advice_valid(advice))
        return _EINVAL;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    if (pages == 0)
        return 0;

    // MADV_DONTNEED and MADV_FREE are the two advices with destructive
    // semantics that software actually depends on -- notably jemalloc, which
    // probes DONTNEED at startup and, finding it does nothing here, permanently
    // falls back to memset ("<jemalloc>: MADV_DONTNEED does not work"). Honor
    // both for private anonymous pages by discarding them so the next access
    // reads back zero, matching Linux. Other advices, and file-backed or shared
    // mappings, stay hints / no-ops (re-faulting a file mapping would mean
    // re-reading the file, which MADV_DONTNEED does not require us to do here).
    struct mem *mem = current->mem;
    int err = 0;
    bool saw_hole = false;
    mem_write_lock_with_pokes(mem);
    page_t start = PAGE(addr);
    page_t end = start + pages;
    if (end < start || end > mem->page_limit) {
        saw_hole = true;       // part of the range is outside the address space
        end = mem->page_limit; // clamp the loop bound / guard overflow
    }
    for (page_t page = start; page < end; ) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL) {
            // A gap anywhere in the range makes the whole call ENOMEM on Linux,
            // after the advice is still applied to the mapped portion.
            saw_hole = true;
            page++;
            continue;
        }
        // MADV_WIPEONFORK marks the range so a child of fork() gets fresh zero
        // pages instead of the parent's data. The point is a page holding
        // something that must not cross a fork -- a PRNG state, a key -- so
        // accepting the advice and inheriting anyway, which is what happened,
        // is exactly the failure the caller asked to be protected from.
        //
        // Private anonymous memory only; anything else is EINVAL, as on Linux.
        if (advice == MADV_WIPEONFORK_ || advice == MADV_KEEPONFORK_) {
            if (!(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED)) {
                err = _EINVAL;
                break;
            }
            if (advice == MADV_WIPEONFORK_)
                pt->flags |= P_WIPEONFORK;
            else
                pt->flags &= ~P_WIPEONFORK;
            page++;
            continue;
        }

        // MADV_REMOVE punches a hole: the range must read back as zero for
        // every mapper, and through read() for a file. It was a pure no-op
        // that still returned 0, so the data stayed. Linux allows it only
        // where there is something shared to punch -- a private mapping has
        // no backing to remove from and is EINVAL.
        if (advice == MADV_REMOVE_) {
            if (!(pt->flags & P_SHARED)) {
                err = _EINVAL;
                break;
            }
            // Zero it in place rather than remapping: these pages are shared,
            // and replacing them would break the sharing that makes the
            // zeroing visible to the other mappers in the first place. For a
            // file mapping the zeroes travel to the file through the same
            // shared host mapping, which is the observable half of punching a
            // hole; the blocks are not deallocated, which is the half nothing
            // can see.
            if (pt->data != NULL && pt->data->data != NULL &&
                    pt->offset + PAGE_SIZE <= pt->data->size)
                memset((char *) pt->data->data + pt->offset, 0, PAGE_SIZE);
            page++;
            continue;
        }

        // MADV_FREE says the caller no longer needs the CONTENTS: the kernel
        // may drop the pages whenever it likes, and a later read gets either
        // the old data or zeroes. Both are conforming answers, which is why
        // taking it as a no-op looked defensible -- a Linux with no memory
        // pressure keeps the data too, and a probe that writes a megabyte,
        // advises it away and reads it back gets its megabyte on both systems.
        //
        // What is not conforming is that the memory never comes back. This
        // advice exists so an allocator can return pages to the system without
        // giving up the address range, and it is how modern glibc, jemalloc and
        // Go's runtime do exactly that. Accepting it and freeing nothing means
        // every allocator that prefers it over MADV_DONTNEED silently stops
        // being able to release memory at all -- and iSH has no reclaim path,
        // so "later, under pressure" never arrives here. Freeing immediately is
        // inside what the contract permits, and the only reading of it that
        // does anything at all on this kernel.
        //
        // Private anonymous only, as on Linux (madvise_free_single_vma refuses
        // anything else outright rather than ignoring it).
        if (advice == MADV_FREE_ && (!(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED))) {
            err = _EINVAL;
            break;
        }

        if ((advice != MADV_DONTNEED_ && advice != MADV_FREE_) ||
                !(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED)) {
            page++; // file-backed / shared / other advice: nothing to discard
            continue;
        }
        // Coalesce a run of same-flag private anonymous pages and replace them
        // with fresh zero-filled anonymous memory in one shot. pt_map_nothing
        // unmaps the old mapping internally (and only after its own allocation
        // succeeds), so there is no unmapped window on failure.
        unsigned flags = pt->flags & ~P_COW;
        page_t run = page;
        do {
            page++;
        } while (page < end &&
                 (pt = mem_pt(mem, page)) != NULL &&
                 (pt->flags & P_ANONYMOUS) && !(pt->flags & P_SHARED) &&
                 (pt->flags & ~P_COW) == flags);
        err = pt_map_nothing(mem, run, page - run, flags);
        if (err < 0)
            break;
    }
    mem_write_unlock_with_pokes(mem);
    if (err < 0)
        return err;
    if (saw_hole)
        return _ENOMEM;        // some address in the range was not mapped
    return 0;
}

dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice) {
    return sys_madvise_guest(addr, len, advice);
}

dword_t sys_mincore_guest(guest_addr_t addr, qword_t len, guest_addr_t vec_addr) {
    STRACE("mincore(%#llx, %#llx, %#llx)",
           (unsigned long long) addr,
           (unsigned long long) len,
           (unsigned long long) vec_addr);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    if (pages == 0)
        return 0;       // Linux: mincore of a zero-length range is a no-op success
    page_t start = PAGE(addr);

    // Collect the result before touching guest memory: user_write re-enters
    // mem_read_lock_quiesce_aware, and taking a nested read lock here
    // deadlocks against a pending writer's quiesce request (the inner lock
    // waits for quiesce_requested to drop while the writer waits for the
    // outer read lock to release).
    byte_t *vec = malloc(pages);
    if (vec == NULL)
        return _ENOMEM;
    mem_read_lock_quiesce_aware(current->mem);
    for (pages_t i = 0; i < pages; i++) {
        page_t pg = start + i;
        struct pt_entry *entry = mem_pt(current->mem, pg);
        if (entry == NULL) {
            // A lazy anonymous reservation IS mapped as far as the guest is
            // concerned -- it just has no page table entry yet, which is
            // exactly what "not resident" means. Only a genuine hole is
            // ENOMEM. (emu/memory.h, struct mem_lazy_map.)
            if (mem_lazy_find(current->mem, pg) != NULL) {
                vec[i] = 0;
                continue;
            }
            mem_read_unlock_quiesce_aware(current->mem);
            free(vec);
            return _ENOMEM;
        }
        // Every mapped page reported 1, so mincore said the whole address
        // space was resident. Programs that use it to decide what to touch --
        // an allocator sizing a madvise, a GC choosing pages to scan, a
        // prefaulting loader -- were told there was nothing to bring in.
        //
        // The residency answer belongs to the host: the guest page is backed
        // by host memory, and the host knows whether that memory is in RAM.
        // A page with no backing at all (a PROT_NONE reservation) is not
        // resident by definition.
        vec[i] = 0;
        if (entry->data != NULL && entry->data->data != NULL) {
            char host_vec = 0;
            void *host = (char *) entry->data->data + entry->offset;
            if (mincore(host, PAGE_SIZE, &host_vec) == 0)
                vec[i] = host_vec & 1;
            else
                vec[i] = 1;   // host cannot say; it is mapped, assume resident
        }
    }
    mem_read_unlock_quiesce_aware(current->mem);
    int err = user_write(vec_addr, vec, pages) ? _EFAULT : 0;
    free(vec);
    return err;
}

dword_t sys_mincore(addr_t addr, dword_t len, addr_t vec_addr) {
    return sys_mincore_guest(addr, len, vec_addr);
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

dword_t sys_mbind_guest(guest_addr_t UNUSED(addr), qword_t UNUSED(len), int_t UNUSED(mode),
        guest_addr_t UNUSED(nodemask), qword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

long sys_get_mempolicy(int *UNUSED(mode), unsigned long *UNUSED(nodemask), unsigned long UNUSED(maxnode), void *UNUSED(addr), unsigned long UNUSED(flags)) {
    return 0;
}

long sys_get_mempolicy_guest(guest_addr_t UNUSED(mode_addr), guest_addr_t UNUSED(nodemask_addr),
        qword_t UNUSED(maxnode), guest_addr_t UNUSED(addr), qword_t UNUSED(flags)) {
    return 0;
}

long sys_set_mempolicy(int UNUSED(mode), const unsigned long *UNUSED(nodemask), unsigned long UNUSED(maxnode)) {
    return 0;
}

long sys_set_mempolicy_guest(int UNUSED(mode), guest_addr_t UNUSED(nodemask_addr), qword_t UNUSED(maxnode)) {
    return 0;
}

// Linux checks the range before doing anything: a range with an unmapped
// page in it is ENOMEM, for mlock and munlock alike. AOK returned 0 for any
// range at all, so a caller that locked a region it had got wrong -- the
// usual reason to call mlock is keeping a secret out of swap -- was told it
// had succeeded.
//
// THE LOCK IS REAL NOW, and it had to become real the moment the pager did.
// This comment used to say "a lock is advisory against swap, which iOS manages
// itself", which was true right up until AOK started writing guest pages to a
// file of its own. mlock's entire purpose is keeping something out of swap -- a
// key, a passphrase, a decrypted buffer -- so answering 0 and then paging it
// out is a lie about the one thing the caller asked for. Eviction now refuses
// any frame with a locked page in it (emu/memory.c, swap_frame_eligible).
//
// It is still not a HOST pin. AOK cannot stop iOS paging its own memory and
// never could, so a guest that locks a page has AOK's promise not to write it
// out, not the operating system's. That is the honest scope of it and it is
// what opt/AOK/docs says.
static int_t mlock_apply(guest_addr_t addr, qword_t len, bool locked) {
    if (len == 0)
        return 0;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    page_t start = PAGE(addr);

    if (locked) {
        // RLIMIT_MEMLOCK, charged against what is ALREADY locked plus what this
        // call would newly lock. Enforced before the change rather than after,
        // because a lock that has to be undone was never granted.
        //
        // superuser() is exempt, as on Linux: CAP_IPC_LOCK bypasses the limit.
        // The AOK CLI runs as uid 0, so this exemption is the whole story
        // there and the limit is only really visible to an unprivileged guest
        // process -- worth knowing when testing it.
        rlim_t_ limit = rlimit(RLIMIT_MEMLOCK_);
        if (!superuser() && limit != RLIM_INFINITY_) {
            size_t already = mem_locked_page_count(current->mem);
            // A page already locked costs nothing to lock again, but counting
            // the overlap exactly would mean walking the range twice; charging
            // the whole request is the conservative direction and matches what
            // Linux does for a range it has not yet examined.
            uint64_t want_bytes = ((uint64_t) already + pages) * PAGE_SIZE;
            // ENOMEM, not EPERM. EPERM is what Linux returned before 2.6.9;
            // since then exceeding a RLIMIT_MEMLOCK you are not privileged to
            // exceed is ENOMEM (mm/mlock.c: `if (locked > lock_limit &&
            // !capable(CAP_IPC_LOCK)) error = -ENOMEM`), including when the
            // limit is 0. A program that distinguishes the two -- and one that
            // retries on ENOMEM with a smaller range is the obvious case --
            // would take the wrong branch on EPERM.
            if (want_bytes > (uint64_t) limit)
                return _ENOMEM;
        }
    }

    long changed = pt_set_locked(current->mem, start, pages, locked);
    return changed < 0 ? (int_t) changed : 0;
}

int_t sys_mlock_guest(guest_addr_t addr, qword_t len) {
    return mlock_apply(addr, len, true);
}

int_t sys_mlock(addr_t addr, dword_t len) {
    return sys_mlock_guest(addr, len);
}

int_t sys_munlock_guest(guest_addr_t addr, qword_t len) {
    return mlock_apply(addr, len, false);
}

int_t sys_munlock(addr_t addr, dword_t len) {
    return sys_munlock_guest(addr, len);
}

#define MCL_CURRENT_ 0x1
#define MCL_FUTURE_ 0x2
#define MCL_ONFAULT_ 0x4

int_t sys_mlockall_guest(qword_t flags) {
    qword_t supported = MCL_CURRENT_ | MCL_FUTURE_ | MCL_ONFAULT_;
    if ((flags & ~supported) != 0)
        return _EINVAL;
    if ((flags & (MCL_CURRENT_ | MCL_FUTURE_)) == 0)
        return _EINVAL;
    if ((flags & MCL_ONFAULT_) != 0 && (flags & (MCL_CURRENT_ | MCL_FUTURE_)) == 0)
        return _EINVAL;
    current->mm->mlockall_flags = (dword_t) flags;
    // MCL_CURRENT locks what is mapped NOW, which is the half that can be done
    // here. MCL_FUTURE is recorded and not yet acted on: it would have to reach
    // pt_map, which is in emu/ and knows nothing about struct mm, and a flag
    // that silently did nothing would be worse than one that is written down.
    // Recorded rather than refused because the flag combination is legal and
    // programs pass MCL_CURRENT|MCL_FUTURE together as a matter of course.
    if ((flags & MCL_CURRENT_) != 0)
        pt_set_locked_all(current->mem, true);
    return 0;
}

int_t sys_mlockall(dword_t flags) {
    return sys_mlockall_guest(flags);
}

int_t sys_munlockall_guest(void) {
    current->mm->mlockall_flags = 0;
    pt_set_locked_all(current->mem, false);
    return 0;
}

int_t sys_munlockall(void) {
    return sys_munlockall_guest();
}

#define MS_ASYNC_      1
#define MS_INVALIDATE_ 2
#define MS_SYNC_       4

// Write the file-backed shared pages of a range back to the host file.
//
// Only those: a private mapping has nothing to write back, and an anonymous
// shared one has no file behind it. Contiguous runs of the same backing are
// coalesced so a large msync is a handful of host calls rather than one per
// guest page, and each run is widened to the host's page granularity, which on
// this platform is larger than the guest's.
//
// Done with the memory read lock still held. A concurrent munmap would need
// the write lock, so the host mappings cannot be pulled out from under us;
// the cost is that a slow MS_SYNC delays a writer, which is the right trade
// for an explicit, rare durability request.
//
// Caller holds mem_read_lock_quiesce_aware and has already validated the range.
static void msync_writeback(struct mem *mem, page_t start, page_t end, int_t flags,
        struct msync_fds *seen) {
    int host_flags = (flags & MS_SYNC_) ? MS_SYNC : MS_ASYNC;
    page_t page = start;
    while (page < end) {
        struct pt_entry *e = mem_pt(mem, page);
        if (e == NULL || e->data == NULL || e->data->data == NULL ||
                !(e->flags & P_SHARED) || e->data->fd == NULL) {
            page++;
            continue;
        }
        // Extend over the pages that continue this same host region.
        struct data *data = e->data;
        size_t first_offset = e->offset;
        page_t run_end = page + 1;
        while (run_end < end) {
            struct pt_entry *next = mem_pt(mem, run_end);
            if (next == NULL || next->data != data ||
                    next->offset != first_offset + (size_t) (run_end - page) * PAGE_SIZE)
                break;
            run_end++;
        }

        // Widen to host pages: the host refuses an unaligned address, and the
        // guest's page size is the smaller of the two here.
        char *base = (char *) data->data + first_offset;
        size_t length = (size_t) (run_end - page) * PAGE_SIZE;
        uintptr_t aligned = (uintptr_t) base & ~(uintptr_t) (real_page_size - 1);
        size_t head = (uintptr_t) base - aligned;
        size_t span = (length + head + real_page_size - 1) & ~(size_t) (real_page_size - 1);
        // ...but never past the end of the host region backing it.
        if (aligned + span > (uintptr_t) data->data + data->size)
            span = ((uintptr_t) data->data + data->size) - aligned;
        // Return value deliberately dropped: Linux reports msync failures from
        // the filesystem, and there is no filesystem error here to report --
        // a host EINVAL would mean this walk computed a bad range, which is a
        // bug to fix rather than an errno to hand the guest.
        (void) msync((void *) aligned, span, host_flags);
        // A FUSE mapping is backed by a host stand-in for the page cache, so
        // the host msync above only got the stores as far as that stand-in --
        // Linux writes a shared mapping's dirty pages back to the filesystem
        // from msync, so the daemon has to hear about them too. That is a
        // round trip to a guest process, which must not happen under this
        // lock: note the fd and do it once the walk is done.
        msync_fds_add(seen, data->fd);
        page = run_end;
    }
}

int_t sys_msync_guest(guest_addr_t addr, qword_t len, int_t flags) {
    STRACE("msync(%#llx, %#llx, 0x%x)", (unsigned long long) addr,
           (unsigned long long) len, flags);
    // Coherence between the guest's mappings needs nothing here -- anonymous
    // and shared mappings share their host pages, so a store is already
    // visible everywhere. DURABILITY is what msync is for, and that was
    // missing: the guest's stores reached the host page cache and no writeback
    // was ever issued, so a program that msync'd and then lost power (or, more
    // often, was killed) had nothing on disk. It was observable from an
    // ordinary guest program -- the file's mtime never moved, at any point.
    //
    // Linux still validates the arguments too, and software checks those:
    //   - an unknown flag bit -> EINVAL;
    //   - MS_SYNC and MS_ASYNC together -> EINVAL (mutually exclusive);
    //   - an unaligned address -> EINVAL;
    //   - a range that is not fully mapped -> ENOMEM.
    if (flags & ~(MS_ASYNC_ | MS_INVALIDATE_ | MS_SYNC_))
        return _EINVAL;
    if ((flags & MS_ASYNC_) && (flags & MS_SYNC_))
        return _EINVAL;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    if (pages == 0)
        return 0;

    struct mem *mem = current->mem;
    page_t start = PAGE(addr);
    page_t end = start + pages;
    int err = 0;
    mem_read_lock_quiesce_aware(mem);
    if (end < start || end > mem->page_limit) {
        err = _ENOMEM;     // range extends outside the address space
    } else {
        for (page_t page = start; page < end; page++) {
            if (mem_pt(mem, page) == NULL) {
                err = _ENOMEM;   // a gap in the range -> ENOMEM
                break;
            }
        }
    }
    struct msync_fds seen = {0};
    if (err == 0 && (flags & (MS_SYNC_ | MS_ASYNC_)))
        msync_writeback(mem, start, end, flags, &seen);
    mem_read_unlock_quiesce_aware(mem);

    // Now that no address-space lock is held, let any filesystem that keeps
    // the real copy of these bytes somewhere else go and update it. Only
    // fusefs does today, and for anything else this is a call that returns 0.
    int flush_err = flush_backing_fds(&seen);
    // Linux's msync reports the filesystem's error. Reporting success when
    // the daemon rejected every write would tell a caller its data is safe
    // when it is not, which is the whole question msync(MS_SYNC) asks.
    if (err == 0)
        err = flush_err;
    if (seen.oom && err == 0)
        err = _ENOMEM;
    return err;
}

int_t sys_msync(addr_t addr, dword_t len, int_t flags) {
    return sys_msync_guest(addr, len, flags);
}

guest_addr_t sys_brk_guest(guest_addr_t new_brk) {
    STRACE("brk(%#llx)", (unsigned long long) new_brk);
    struct mm *mm = current->mm;
    bool expand_failed = false;

    // HOISTED ABOVE THE BARRIER, unlike the guard it serves, and that placement
    // is forced rather than chosen: swap_direct_reclaim takes
    // mem_write_lock_with_pokes itself, and the pt_alloc_lock inside it is not
    // recursive, so calling it from beside the guard below would deadlock
    // against this very line.
    //
    // The cost of being early is that the brk may not actually grow -- the
    // check below can still refuse, or new_brk may be a shrink. Reclaiming for
    // a request that turns out not to need it is wasted I/O but never wrong,
    // and it only happens at all when the app is already within its headroom
    // floor of the jetsam limit. A no-op when swap is off, which is the default.
    if (host_mem_headroom_low() && new_brk > mm->brk)
        swap_direct_reclaim(&mm->mem, new_brk - mm->brk);

    mem_write_lock_with_pokes(&mm->mem);
    if (new_brk < mm->start_brk)
        goto out;
    guest_addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        // Same jetsam-headroom backpressure as mmap_common_guest.
        if (host_mem_headroom_low()) {
            expand_failed = true;
            goto out;
        }
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        // brk only ever grows forward from start_brk, where the exec-time
        // reservation (kernel/exec.c) begins, so start is always at or past
        // brk_reserve_start once a reservation exists; it can never start
        // before it.
        page_t reserve_end = mm->mem.brk_reserve_start < mm->mem.brk_reserve_end
            ? mm->mem.brk_reserve_end : start;
        if (start < reserve_end) {
            // Claim (a prefix of) the exec-time brk headroom reservation:
            // map real pages and shrink the recorded range. Split at the
            // reservation boundary if this growth spans past it -- the
            // reserved part just needs real backing, the rest is fresh
            // territory handled like any other brk growth below.
            pages_t claim_size = size;
            if (start + size > reserve_end)
                claim_size = reserve_end - start;
            int err = pt_map_nothing(&mm->mem, start, claim_size, P_WRITE);
            if (err < 0) {
                expand_failed = true;
                goto out;
            }
            mm->mem.brk_reserve_start = start + claim_size;
            if (claim_size < size) {
                page_t rest_start = start + claim_size;
                pages_t rest_size = size - claim_size;
                if (!pt_is_hole(&mm->mem, rest_start, rest_size)) {
                    expand_failed = true;
                    goto out;
                }
                int err2 = pt_map_nothing(&mm->mem, rest_start, rest_size, P_WRITE);
                if (err2 < 0) {
                    expand_failed = true;
                    goto out;
                }
            }
        } else if (!pt_is_hole(&mm->mem, start, size)) {
            expand_failed = true;
            goto out;
        } else {
            int err = pt_map_nothing(&mm->mem, start, size, P_WRITE);
            if (err < 0) {
                expand_failed = true;
                goto out;
            }
        }
    } else if (new_brk < old_brk) {
        // shrink heap: unmap region from new_brk to old_brk
        // first page to unmap is PAGE(new_brk)
        // last page to unmap is PAGE(old_brk)
        pt_unmap_always(&mm->mem, PAGE(new_brk), PAGE(old_brk) - PAGE(new_brk));
        // If this shrink dips below the brk-headroom reservation's already-
        // claimed boundary (brk_reserve_start), roll that boundary back to
        // match: the pages just unmapped return to being "reserved but not
        // yet claimed" territory. Without this, pt_find_hole (which only
        // treats [brk_reserve_start, brk_reserve_end) as off-limits) sees
        // the freed-but-still-logically-claimed pages as ordinary free
        // space and can hand them to an unrelated mmap() -- which a later
        // brk regrowth then silently maps straight over, corrupting
        // whatever that mmap placed there. Real-world trigger: glibc malloc
        // trims the heap (brk shrink) between two unrelated mmap() calls
        // during dynamic linking, then grows the heap again.
        if (mm->mem.brk_reserve_start < mm->mem.brk_reserve_end &&
                PAGE(new_brk) < mm->mem.brk_reserve_start)
            mm->mem.brk_reserve_start = PAGE(new_brk);
    }

    mm->brk = new_brk;
out:;
    guest_addr_t brk = mm->brk;
    mem_write_unlock_with_pokes(&mm->mem);
    if (expand_failed)
        amd64_vm_failure_trace("brk", brk, new_brk, old_brk, 0, 0, 0, 0);
    return brk;
}

addr_t sys_brk(addr_t new_brk) {
    return (addr_t) sys_brk_guest(new_brk);
}

// ===========================================================================
// Backpressure for memory the growth guards never see.
//
// do_mmap, mremap and brk all check headroom and reclaim before letting the
// guest grow. None of that covers how a large heap is actually committed: the
// guest reserves the region ONCE, while headroom is fine, and the host commits
// its physical pages as the guest touches them. Every page after the mapping is
// invisible to those guards.
//
// Measured, which is why this exists. Under a 1024 MiB budget, 48 separate
// 32 MiB mmaps are refused at 832 MiB while ONE 1536 MiB mmap then filled
// commits the lot unrefused. On a 3 GB iPhone SE, one 2048 MiB mapping filled
// to 1728 MiB through touches and the app was jetsam-killed with swap on and
// verified working. kswapd cannot cover it: the guest commits at ~240 MiB/s
// against kswapd's 8 MiB/s, so it reclaimed 1.2% of what was committed and
// direct reclaim was never called at all.
//
// The sensor is in tlb_handle_miss; this is the actor. It runs from
// handle_timer_interrupt because task_run_current releases mem->lock at
// kernel/task.c:918 before calling handle_interrupt at :921, making this the
// only point in guest execution where the thread holds nothing. Blocking here
// is not novel: group_stop_wait parks a task at this same call site, and so
// does every blocking syscall.
//
// The throttle IS the reclaim, not a sleep. A thread made to page 8 MiB out
// before it may commit more is slowed by exactly the cost of the I/O it is
// causing, which is what Linux does to a process in direct reclaim.

// Asked for per reclaim, and it is the FLOOR swap_direct_reclaim will accept:
// SWAP_RECLAIM_MIN_BYTES (kernel/swap.c) clamps anything smaller back up to
// 4 MiB, because the barrier costs every sibling thread ~109 us whatever it is
// protecting, so a tiny reclaim would be nearly all overhead. Asking for less
// than this does not make the barrier shorter, it just makes the constant lie.
//
// The number that matters is therefore the DUTY CYCLE, not the size: this call
// holds the address-space barrier, and while it runs every sibling guest thread
// is stopped. Section 3.9 caps device swap-out near 100 MiB/s, so 4 MiB is
// ~40 ms of frozen guest per acquisition -- and the interval below is what
// keeps that to a small share of wall clock.
#define FAULT_BACKPRESSURE_WANT_BYTES (4u << 20)
// Consecutive throttles where the space was still growing, headroom was still
// low, and reclaim could free nothing, before this is terminal. Not one:
// reclaim legitimately returns 0 when the aging clock has not yet made anything
// a candidate, and returns 0 immediately when the user has left swap off --
// which is the DEFAULT, and must not by itself be a death sentence.
#define FAULT_BACKPRESSURE_OOM_STRIKES 8
// Brake applied when reclaim cannot help. With swap off there is no page to
// evict, so this is the only backpressure available: it does not free memory,
// it slows the writer so other guest processes, the guest's own allocator and
// the host all get a chance to react -- and it turns the OOM decision from "N
// megabytes of touches" into "N milliseconds of sustained low headroom", which
// is what makes the kill defensible rather than trigger-happy.
#define FAULT_BACKPRESSURE_BRAKE_MS 20
// Minimum wall-clock gap between two throttles that take the address-space
// BARRIER. Without this the throttle monopolises it: a probe fires every
// MEM_HEADROOM_PROBE_MISSES (~4 MiB of touches), and each one that reclaims
// holds the barrier -- quiescing every sibling guest thread -- for as long as
// 8 MiB of eviction I/O takes. On a Mac that is invisible. On a device it is
// tens of milliseconds of flash writes per 4 MiB committed, back to back, with
// every other guest thread stopped; the app stops responding and iOS kills it
// as hung. OBSERVED on a 3 GB iPhone SE: the UI froze with the window still on
// screen and the app was then terminated -- which is not what a jetsam kill
// looks like, and was the clue.
//
// 250 ms against a ~40 ms barrier is a 16% duty cycle -- the guest is running
// 5/6 of the time -- and still permits ~16 MiB/s of reclaim, which is an order
// of magnitude above the rate a device guest actually commits at (the SE
// measured ~2 MiB/s filling a 2 GiB mapping). Sized from the duty cycle rather
// than from how fast reclaim could go, because the failure this prevents is the
// app being killed as unresponsive, not the pager being slow.
#define FAULT_BACKPRESSURE_MIN_INTERVAL_MS 250

void mem_fault_backpressure(void) {
    if (current == NULL || current->mem == NULL || current->mm == NULL)
        return;
    if (!current->mem_throttle_wanted)
        return;
    current->mem_throttle_wanted = false;
    struct mm *mm = current->mm;

    // Re-read rather than trusting the sensor: the guest has run since, and
    // kswapd may already have fixed it.
    if (!host_mem_should_reclaim()) {
        atomic_store_explicit(&mm->fault_oom_strikes, 0, memory_order_relaxed);
        return;
    }

    // Is this address space actually GROWING? The headroom reading is
    // app-wide, and the sensor over-counts badly: the TLB is direct-mapped, so
    // conflict misses and every post-flush re-miss look exactly like a fresh
    // commit. Without this test a process merely re-walking a working set it
    // already owns accrues strikes -- and is eventually killed for committing
    // nothing at all -- because some OTHER part of the app is near the ceiling.
    // Residency is the honest question: only a space whose page count is rising
    // is the one making things worse.
    size_t resident = mem_resident_page_count(current->mem);
    size_t before = atomic_exchange_explicit(&mm->fault_last_resident_pages,
            resident, memory_order_relaxed);
    bool growing = before == 0 || resident > before;
    if (!growing) {
        atomic_store_explicit(&mm->fault_oom_strikes, 0, memory_order_relaxed);
        return;
    }

    // Pay for the memory being committed, on the thread committing it. This is
    // the throttle: a thread made to page 8 MiB out before it may commit more
    // is slowed by exactly the cost of the I/O it is causing, which is what
    // Linux does to a process in direct reclaim.
    //
    // But not more often than FAULT_BACKPRESSURE_MIN_INTERVAL_MS, because this
    // call takes the address-space barrier and every sibling guest thread stops
    // while it runs. Between reclaims the writer is still braked below, so
    // backpressure is continuous even though the barrier is not.
    static _Atomic uint64_t last_reclaim_ms;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    uint64_t now_ms = (uint64_t) now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
    uint64_t last = atomic_load_explicit(&last_reclaim_ms, memory_order_relaxed);
    long freed = 0;
    bool reclaimed = false;
    if (now_ms - last >= FAULT_BACKPRESSURE_MIN_INTERVAL_MS) {
        atomic_store_explicit(&last_reclaim_ms, now_ms, memory_order_relaxed);
        freed = swap_direct_reclaim(current->mem, FAULT_BACKPRESSURE_WANT_BYTES);
        reclaimed = true;
    }

    // Brake unconditionally while headroom is still low, whether or not reclaim
    // just freed something. Making the brake conditional on reclaim FAILING was
    // wrong in a way that only a slow device would have shown: the throttle's
    // strength then came entirely from how long the eviction I/O took, so on
    // this Mac -- where the swap file is page cache and 2 MiB costs
    // microseconds -- a guest committing 1536 MiB was not slowed at all
    // (297 MiB/s, measured), while the same code on a phone would brake hard.
    // A backpressure mechanism whose strength is set by the host's I/O speed is
    // not a mechanism, it is an accident.
    if (!host_mem_should_reclaim()) {
        atomic_store_explicit(&mm->fault_oom_strikes, 0, memory_order_relaxed);
        return;
    }
    struct timespec brake = { .tv_sec = 0,
                              .tv_nsec = FAULT_BACKPRESSURE_BRAKE_MS * 1000000L };
    nanosleep(&brake, NULL);

    // Reclaim that FREED something means the pager is coping; the brake above
    // is the whole cost. Only a reclaim that ran and freed nothing is evidence
    // the pager cannot help -- swap off, or nothing a candidate yet -- and only
    // that accrues a strike. A probe that skipped reclaim because it was too
    // soon for the barrier proves nothing either way.
    if (!reclaimed || freed > 0)
        return;

    if (!host_mem_headroom_low()) {
        atomic_store_explicit(&mm->fault_oom_strikes, 0, memory_order_relaxed);
        return;
    }
    unsigned strikes = atomic_fetch_add_explicit(&mm->fault_oom_strikes, 1,
            memory_order_relaxed) + 1;
    if (strikes < FAULT_BACKPRESSURE_OOM_STRIKES)
        return;
    atomic_store_explicit(&mm->fault_oom_strikes, 0, memory_order_relaxed);

    // Terminal: the space is still growing, the host has nothing left, the
    // pager can free none of it, and braking has not stopped it. Linux answers
    // this with the OOM killer, and so do we -- one guest process dies instead
    // of iOS killing the app, which takes every other guest process and the
    // user's session with it.
    //
    // The faulting process, not a badness heuristic: it is the one asking for
    // memory that is not there, its death certainly helps, and picking another
    // would mean walking the task table under pids_lock at the worst moment.
    printk("ERROR: %d(%s) out of memory: growing with no host headroom left and "
           "nothing reclaimable; killing it\n",
           current->pid, current->comm);
    struct siginfo_ info = { .code = SI_KERNEL_ };
    deliver_signal(current, SIGKILL_, info);
}
