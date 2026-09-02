#include <string.h>
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
    // Once quiesce_requested is up, no new reader can enter (they wait in
    // mem_*_quiesce_aware); we only need the readers already mid-block to drain.
    // cpu_poke sets a sticky flag they check at the next block boundary, so a
    // poke up front + periodic re-pokes (covering a sibling that raced in just
    // before the bump) suffice -- no need to re-signal every spin. sched_yield()
    // hands the core to those readers so they release in microseconds; only a
    // stubborn hold falls through to nanosleep, then to a blocking write_lock.
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
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, mm->mem.page_limit);
    ipc_mm_copy(new_mm, mm);
    mem_write_unlock_with_pokes(&mm->mem);
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
        mem_pt(current->mem, page)->data->fd = fd_retain(fd);
        mem_pt(current->mem, page)->data->file_offset = offset;
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
    // Same jetsam-headroom backpressure as mmap_common_guest (grow only).
    if (new_pages > old_pages && host_mem_headroom_low())
        return _ENOMEM;
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
#define MADV_REMOVE_ 9
#define MADV_WIPEONFORK_ 18
#define MADV_KEEPONFORK_ 19

// Advices Linux's madvise() accepts. Linux validates the argument up front and
// returns EINVAL for anything it does not recognize, independent of whether the
// advice has any effect. iSH only acts on MADV_DONTNEED; the rest are accepted
// as no-op hints so software that probes them does not see a spurious error.
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

    // MADV_DONTNEED is the one advice with destructive (zeroing) semantics that
    // software actually depends on -- notably jemalloc, which probes it at
    // startup and, finding it does nothing here, permanently falls back to
    // memset ("<jemalloc>: MADV_DONTNEED does not work"). Honor it for private
    // anonymous pages by discarding them so the next access reads back zero,
    // matching Linux. Other advices, and file-backed or shared mappings, stay
    // hints / no-ops (re-faulting a file mapping would mean re-reading the file,
    // which MADV_DONTNEED does not require us to do here).
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

        if (advice != MADV_DONTNEED_ ||
                !(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED)) {
            page++; // file-backed / shared / non-DONTNEED: nothing to discard
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
// The locking itself stays a no-op: iSH cannot pin host pages, and a lock is
// advisory against swap, which iOS manages itself. Claiming a range EXISTS
// when it does not is a different kind of answer.
static int_t mlock_range_check(guest_addr_t addr, qword_t len) {
    if (len == 0)
        return 0;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    page_t start = PAGE(addr);
    int_t err = 0;
    mem_read_lock_quiesce_aware(current->mem);
    for (pages_t i = 0; i < pages; i++) {
        if (mem_pt(current->mem, start + i) == NULL) {
            err = _ENOMEM;
            break;
        }
    }
    mem_read_unlock_quiesce_aware(current->mem);
    return err;
}

int_t sys_mlock_guest(guest_addr_t addr, qword_t len) {
    return mlock_range_check(addr, len);
}

int_t sys_mlock(addr_t addr, dword_t len) {
    return sys_mlock_guest(addr, len);
}

int_t sys_munlock_guest(guest_addr_t addr, qword_t len) {
    return mlock_range_check(addr, len);
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
    return 0;
}

int_t sys_mlockall(dword_t flags) {
    return sys_mlockall_guest(flags);
}

int_t sys_munlockall_guest(void) {
    current->mm->mlockall_flags = 0;
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
