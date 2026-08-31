#include "kernel/signal.h"
#include "task.h"
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "misc.h"
#include "kernel/calls.h"
#include "kernel/random.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/native.h"
#include "kernel/vdso.h"
#include "jit/jit.h"
#include "tools/ptraceomatic-config.h"
#include "util/sync.h"

#define ARGV_MAX 32 * PAGE_SIZE

struct exec_args {
    // number of arguments
    size_t count;
    // series of count null-terminated strings, plus an extra null for good measure
    const char *args;
};

struct elf_info {
    enum guest_abi abi;
    byte_t bitness;
    uint16_t type;
    uint16_t machine;
    qword_t entry_point;
    qword_t prghead_off;
    uint16_t phent_size;
    uint16_t phent_count;
};

struct elf_prg_info {
    uint32_t type;
    uint32_t flags;
    qword_t offset;
    qword_t vaddr;
    qword_t filesize;
    qword_t memsize;
    qword_t alignment;
};

static inline guest_addr_t align_stack(guest_addr_t sp);
static inline ssize_t user_strlen(guest_addr_t p);
static inline int user_memset(guest_addr_t start, byte_t val, dword_t len);
static inline guest_addr_t copy_string(guest_addr_t sp, const char *string);
static inline guest_addr_t args_copy(guest_addr_t sp, struct exec_args args);
static size_t args_size(struct exec_args args);
static ssize_t user_read_exec_ptr(guest_addr_t addr, qword_t *ptr_out);
static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, char **envp_out);
static int read_header(struct fd *fd, struct elf_info *header);
static int read_prg_headers(struct fd *fd, struct elf_info header, struct elf_prg_info **ph_out);
static int load_entry(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias, struct fd *fd);
static guest_addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph, pages_t headroom);
static int elf_load_addr_candidate(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias,
        guest_addr_t *addr_out);
static void amd64_trace_exec_attempt(const char *file, const char *argv);
static void amd64_trace_exec_loader_failure(const char *stage, const char *file, enum guest_abi abi,
        struct elf_prg_info *ph, guest_addr_t bias, struct fd *fd, int err, const char *interp_name);

// Guest arches can be compiled out with meson -Dguest_archs=... (the
// ISH_GUEST_* defines). This is the master gate: an ELF for a disabled arch
// is simply not recognized, so every exec of one fails with ENOEXEC and no
// downstream engine/syscall/signal path can ever see the ABI.
static bool elf_abi_detect(byte_t bitness, uint16_t machine, enum guest_abi *abi_out) {
    enum guest_abi abi;
    if (ISH_GUEST_AMD64 && bitness == ELF_64BIT && machine == ELF_X86_64) {
        abi = GUEST_ABI_AMD64;
    } else if (ISH_GUEST_ARM64 && bitness == ELF_64BIT && machine == ELF_AARCH64) {
        abi = GUEST_ABI_ARM64;
    } else if (ISH_GUEST_RISCV64 && bitness == ELF_64BIT && machine == ELF_RISCV) {
        abi = GUEST_ABI_RISCV64;
    } else if (ISH_GUEST_I386 && bitness == ELF_32BIT && machine == ELF_X86) {
        abi = GUEST_ABI_I386;
    } else {
        return false;
    }
    if (abi_out != NULL)
        *abi_out = abi;
    return true;
}

static bool elf_value_fits_addr(enum guest_abi abi, qword_t value) {
    return guest_abi_addr_valid(abi, value);
}

static int read_header(struct fd *fd, struct elf_info *header) {
    union {
        struct elf_header elf32;
        struct elf64_header elf64;
    } raw;

    ssize_t err;
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    if ((err = fd->ops->read(fd, &raw, sizeof(raw))) < (ssize_t) sizeof(struct elf_header)) {
        if (err < 0)
            return _EIO;
        return _ENOEXEC;
    }

    struct elf_header *ident = &raw.elf32;
    enum guest_abi elf_abi;
    if (memcmp(&ident->magic, ELF_MAGIC, sizeof(ident->magic)) != 0
            || (ident->type != ELF_EXECUTABLE && ident->type != ELF_DYNAMIC)
            || ident->endian != ELF_LITTLEENDIAN
            || ident->elfversion1 != 1
            || !elf_abi_detect(ident->bitness, ident->machine, &elf_abi))
        return _ENOEXEC;

    if (ident->bitness == ELF_32BIT) {
        *header = (struct elf_info) {
            .abi = elf_abi,
            .bitness = ident->bitness,
            .type = raw.elf32.type,
            .machine = raw.elf32.machine,
            .entry_point = raw.elf32.entry_point,
            .prghead_off = raw.elf32.prghead_off,
            .phent_size = raw.elf32.phent_size,
            .phent_count = raw.elf32.phent_count,
        };
    } else if (ident->bitness == ELF_64BIT) {
        if (err < (ssize_t) sizeof(struct elf64_header))
            return _ENOEXEC;
        *header = (struct elf_info) {
            .abi = elf_abi,
            .bitness = ident->bitness,
            .type = raw.elf64.type,
            .machine = raw.elf64.machine,
            .entry_point = raw.elf64.entry_point,
            .prghead_off = raw.elf64.prghead_off,
            .phent_size = raw.elf64.phent_size,
            .phent_count = raw.elf64.phent_count,
        };
    } else {
        return _ENOEXEC;
    }
    return 0;
}

static int read_prg_headers(struct fd *fd, struct elf_info header, struct elf_prg_info **ph_out) {
    size_t ph_size = sizeof(struct elf_prg_info) * header.phent_count;
    struct elf_prg_info *ph = malloc(ph_size);
    if (ph == NULL)
        return _ENOMEM;

    memset(ph, 0, ph_size);
    if (fd->ops->lseek(fd, header.prghead_off, SEEK_SET) < 0) {
        free(ph);
        return _EIO;
    }

    if (header.bitness == ELF_32BIT) {
        if (header.phent_size < sizeof(struct prg_header)) {
            free(ph);
            return _ENOEXEC;
        }
        for (uint16_t i = 0; i < header.phent_count; i++) {
            struct prg_header raw;
            if (fd->ops->read(fd, &raw, sizeof(raw)) != sizeof(raw)) {
                free(ph);
                if (errno != 0)
                    return _EIO;
                return _ENOEXEC;
            }
            if (header.phent_size > sizeof(raw) &&
                    fd->ops->lseek(fd, header.phent_size - sizeof(raw), SEEK_CUR) < 0) {
                free(ph);
                return _EIO;
            }
            ph[i] = (struct elf_prg_info) {
                .type = raw.type,
                .flags = raw.flags,
                .offset = raw.offset,
                .vaddr = raw.vaddr,
                .filesize = raw.filesize,
                .memsize = raw.memsize,
                .alignment = raw.alignment,
            };
        }
    } else if (header.bitness == ELF_64BIT) {
        if (header.phent_size < sizeof(struct prg_header64)) {
            free(ph);
            return _ENOEXEC;
        }
        for (uint16_t i = 0; i < header.phent_count; i++) {
            struct prg_header64 raw;
            if (fd->ops->read(fd, &raw, sizeof(raw)) != sizeof(raw)) {
                free(ph);
                if (errno != 0)
                    return _EIO;
                return _ENOEXEC;
            }
            if (header.phent_size > sizeof(raw) &&
                    fd->ops->lseek(fd, header.phent_size - sizeof(raw), SEEK_CUR) < 0) {
                free(ph);
                return _EIO;
            }
            ph[i] = (struct elf_prg_info) {
                .type = raw.type,
                .flags = raw.flags,
                .offset = raw.offset,
                .vaddr = raw.vaddr,
                .filesize = raw.filesize,
                .memsize = raw.memsize,
                .alignment = raw.alignment,
            };
        }
    } else {
        free(ph);
        return _ENOEXEC;
    }

    *ph_out = ph;
    return 0;
}

static int load_entry(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias, struct fd *fd) {
    int err;

    if (!elf_value_fits_addr(abi, ph.vaddr) || !elf_value_fits_addr(abi, ph.offset) ||
            !elf_value_fits_addr(abi, ph.memsize) || !elf_value_fits_addr(abi, ph.filesize))
        return _EOVERFLOW;
    if (ph.vaddr > guest_abi_vm_layout(abi).user_addr_max - bias)
        return _EOVERFLOW;

    guest_addr_t addr = (guest_addr_t) ph.vaddr + bias;
    guest_addr_t offset = (guest_addr_t) ph.offset;
    guest_addr_t memsize = (guest_addr_t) ph.memsize;
    guest_addr_t filesize = (guest_addr_t) ph.filesize;

    int flags = P_READ;
    if (ph.flags & PH_W) flags |= P_WRITE;
    if (ph.flags & PH_X) flags |= P_EXEC;

    guest_addr_t file_end = addr + filesize;
    guest_addr_t mem_end = addr + memsize;
    guest_addr_t map_file_start = offset - PGOFFSET(addr);  // file offset of PAGE(addr)
    guest_addr_t content_file_end = offset + filesize;      // file offset where p_filesz ends

    // Guest pages spanned by the segment's file content, counted from the
    // segment's first page. This is the full extent mapped from the file by
    // default.
    pages_t fb_pages = PAGE_ROUND_UP(filesize + PGOFFSET(addr));

    // Guard against a host page that extends past the backing file's EOF.
    //
    // iOS host pages (16K) are larger than guest pages (4K). When a writable
    // PT_LOAD's file content ends partway through the final host page of its
    // private file mapping, the rest of that host page lies beyond the file's
    // EOF. The first write into that page -- the BSS zero-fill below, or any
    // guest store at run time -- triggers a copy-on-write fault, and the host
    // must page in the whole 16K cluster from the file to copy it. The part
    // past EOF makes APFS fail the pagein ("cluster_pagein past EOF") and the
    // guest dies with SIGBUS. (On a host whose page size equals the guest's,
    // the tail of the final page reads as zero and COW works, so we leave the
    // mapping alone there.)
    //
    // So when the host page is larger than the guest page, map only the whole
    // host pages that lie entirely within the file, and back the remainder --
    // the file tail that shares EOF's host page, plus the BSS -- with anonymous
    // memory, copying the residual file bytes into it. Read-only segments are
    // never written, so they never trigger the COW pagein and stay fully
    // file-backed (and shareable).
    bool split_tail = false;
    guest_addr_t residual_file_start = 0;  // file offset of bytes to copy into anon
    guest_addr_t split_file_size = 0;      // backing file size, for the copy clamp
    if (real_page_size > PAGE_SIZE && (flags & P_WRITE) && filesize != 0) {
        struct statbuf st;
        if (fd->mount->fs->fstat(fd, &st) >= 0) {
            guest_addr_t host_mask = (guest_addr_t) real_page_size - 1;
            guest_addr_t mapping_host_end = (content_file_end + host_mask) & ~host_mask;
            if ((qword_t) st.size < mapping_host_end) {
                // The final host page of the file mapping straddles EOF.
                split_tail = true;
                split_file_size = (guest_addr_t) st.size;
                guest_addr_t safe_file_end = (guest_addr_t) st.size & ~host_mask; // floor to host page
                guest_addr_t fb_file_end = content_file_end < safe_file_end ?
                        content_file_end : safe_file_end;
                fb_file_end &= ~host_mask;          // keep only whole host pages
                if (fb_file_end < map_file_start)
                    fb_file_end = map_file_start;   // nothing is safely file-backed
                fb_pages = (pages_t) ((fb_file_end - map_file_start) >> PAGE_BITS);
                residual_file_start = fb_file_end;
            }
        }
    }

    // Map the file-backed portion of the segment.
    if (fb_pages > 0) {
        if ((err = fd->ops->mmap(fd, current->mem, PAGE(addr), fb_pages,
                        map_file_start, flags, MMAP_PRIVATE)) < 0) {
            amd64_trace_exec_loader_failure("segment-mmap", NULL, abi, &ph, bias, fd, err, NULL);
            return err;
        }
        // TODO find a better place for these to avoid code duplication
        mem_pt(current->mem, PAGE(addr))->data->fd = fd_retain(fd);
        mem_pt(current->mem, PAGE(addr))->data->file_offset = map_file_start;
    }

    if (!split_tail) {
        // The file content's final page is mapped from the file. ELF requires the
        // remainder of that page (the BSS that shares the last file page) to read
        // as zero. When the host page size is larger than the guest page size,
        // the mmap above can otherwise expose later file bytes in that
        // guest-visible tail.
        dword_t tail_size = PAGE_SIZE - PGOFFSET(file_end);
        if (tail_size == PAGE_SIZE)
            tail_size = 0;

        if (tail_size != 0 && (flags & P_WRITE)) {
            // Unlock and lock the mem because the user functions must be
            // called without locking mem.
            struct mem *mem = current->mem;
            write_unlock(&mem->lock);

            int memset_err = user_memset(file_end, 0, tail_size);
            write_lock(&mem->lock);
            if (memset_err) {
                amd64_trace_exec_loader_failure("segment-bss-tail", NULL, abi, &ph, bias, fd, _EFAULT, NULL);
                return _EFAULT;
            }
        }

        if (memsize > filesize) {
            dword_t bss_size = memsize - filesize;
            if (tail_size > bss_size)
                tail_size = bss_size;
            dword_t extra_bss_size = bss_size - tail_size;
            if (extra_bss_size != 0) {
                if ((err = pt_map_nothing(current->mem, PAGE_ROUND_UP(file_end),
                                PAGE_ROUND_UP(extra_bss_size), flags)) < 0) {
                    amd64_trace_exec_loader_failure("segment-bss-map", NULL, abi, &ph, bias, fd, err, NULL);
                    return err;
                }
            }
        }
    } else {
        // Anonymous (zeroed) backing for the file tail sharing EOF's host page
        // plus the BSS, so neither the zero-fill below nor later guest stores
        // ever fault a file page past EOF.
        guest_addr_t anon_start = (guest_addr_t) (PAGE(addr) + fb_pages) << PAGE_BITS;
        pages_t anon_pages = mem_end > anon_start ? PAGE_ROUND_UP(mem_end - anon_start) : 0;
        if (anon_pages != 0) {
            if ((err = pt_map_nothing(current->mem, PAGE(anon_start), anon_pages, flags)) < 0) {
                amd64_trace_exec_loader_failure("segment-bss-map", NULL, abi, &ph, bias, fd, err, NULL);
                return err;
            }
        }

        // Copy the residual file bytes (the file content that fell into the anon
        // region) to the start of it; the rest stays zero (the BSS). Clamp to the
        // backing file's real size so an over-declared/truncated p_filesz never
        // makes us read past EOF or allocate an absurd buffer -- those bytes don't
        // exist and are already zero in the anon mapping.
        guest_addr_t copy_end = content_file_end < split_file_size ? content_file_end : split_file_size;
        dword_t copy_len = (dword_t) (copy_end - residual_file_start);
        if (copy_len != 0) {
            char *buf = malloc(copy_len);
            if (buf == NULL)
                return _ENOMEM;
            // Not every fs implements pread (jumping through a NULL pointer
            // here was a host EXC_BAD_ACCESS abort when execing a binary that
            // lived on tmpfs). Fall back to lseek+read: this fd is exec's own
            // private open, and every later loader read seeks first.
            ssize_t got;
            if (fd->ops->pread != NULL) {
                got = fd->ops->pread(fd, buf, copy_len, (off_t) residual_file_start);
            } else if (fd->ops->lseek != NULL) {
                got = fd->ops->lseek(fd, (off_t_) residual_file_start, LSEEK_SET);
                if (got >= 0)
                    got = fd->ops->read(fd, buf, copy_len);
            } else {
                got = _EINVAL;
            }
            if (got < 0) {
                free(buf);
                amd64_trace_exec_loader_failure("segment-tail-read", NULL, abi, &ph, bias, fd, _EIO, NULL);
                return _EIO;
            }
            // A short read leaves the remaining bytes zero, which is what ELF
            // wants for any content claimed past a truncated p_filesz.
            if ((dword_t) got < copy_len)
                memset(buf + got, 0, copy_len - (dword_t) got);

            // user functions must be called without holding the mem lock.
            struct mem *mem = current->mem;
            write_unlock(&mem->lock);
            int write_err = user_write(anon_start, buf, copy_len);
            write_lock(&mem->lock);
            free(buf);
            if (write_err) {
                amd64_trace_exec_loader_failure("segment-tail-copy", NULL, abi, &ph, bias, fd, _EFAULT, NULL);
                return _EFAULT;
            }
        }
    }

    return 0;
}

// headroom: extra free pages requested ABOVE the image (the hole is
// found for image+headroom and the image is placed at its bottom).
// Used for the arm64 main executable so start_brk — which sits directly
// after the image — has real room to grow: pt_find_hole hands back the
// top of the mmap window, and parking the image there capped the heap
// at (nearly) zero bytes. Same failure mode as the amd64 32-MiB-brk bug
// fixed by pinning that ABI's PIE low; arm64 keeps dynamic placement
// (see the V8 CodeRange note at the call site) and reserves instead.
static guest_addr_t find_hole_for_elf(struct elf_info *header, struct elf_prg_info *ph, pages_t headroom) {
    bool found = false;
    page_t first_page = 0;
    page_t last_page = 0;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        qword_t end_vaddr = ph[i].vaddr + ph[i].memsize;
        if (end_vaddr < ph[i].vaddr)
            return 0;
        if (!elf_value_fits_addr(header->abi, end_vaddr) || !elf_value_fits_addr(header->abi, ph[i].vaddr))
            return 0;

        page_t seg_first = PAGE(ph[i].vaddr);
        page_t seg_last = PAGE_ROUND_UP(end_vaddr);
        if (!found) {
            first_page = seg_first;
            last_page = seg_last;
            found = true;
            continue;
        }
        if (seg_first < first_page)
            first_page = seg_first;
        if (seg_last > last_page)
            last_page = seg_last;
    }
    pages_t size = 0;
    if (found) {
        if (last_page < first_page)
            return 0;
        size = last_page - first_page;
    }
    page_t hole = pt_find_hole(current->mem, size + headroom);
    if (hole == BAD_PAGE)
        return 0;
    guest_addr_t base = ((guest_addr_t) hole - first_page) << PAGE_BITS;
    return base;
}

static int elf_load_addr_candidate(enum guest_abi abi, struct elf_prg_info ph, guest_addr_t bias,
        guest_addr_t *addr_out) {
    qword_t mapped_load_addr = (qword_t) bias + ph.vaddr;
    if (ph.offset > mapped_load_addr)
        return _EOVERFLOW;
    mapped_load_addr -= ph.offset;
    if (!elf_value_fits_addr(abi, mapped_load_addr))
        return _EOVERFLOW;
    *addr_out = (guest_addr_t) mapped_load_addr;
    return 0;
}

static void amd64_trace_exec_attempt(const char *file, const char *argv) {
    (void) file;
    (void) argv;
}

static void amd64_trace_exec_loader_failure(const char *stage, const char *file, enum guest_abi abi,
        struct elf_prg_info *ph, guest_addr_t bias, struct fd *fd, int err, const char *interp_name) {
    (void) stage;
    (void) file;
    (void) abi;
    (void) ph;
    (void) bias;
    (void) fd;
    (void) err;
    (void) interp_name;
}

static bool i386_force_safe_exec_comm(const char *comm) {
    return comm != NULL &&
        strcmp(comm, "pkcsslotd") == 0;
}

// Linux's de_thread: an execve leaves exactly one thread standing, and the
// thread that called it becomes the group leader.
//
// AOK used to do neither. Every other thread kept running -- three tasks where
// Linux has one, each still executing the OLD program, since exec here swaps
// only the calling task's mm and the siblings hold the previous address space
// alive by reference. And a non-leader exec left the process with
// getpid() != gettid() forever, a state Linux only ever shows for a thread
// that is not the leader, and the standard way a program asks "am I the main
// thread". The new image is single-threaded, so the answer has to be yes.
//
// Two AOK specifics shape this:
//
//   - SIGKILL cannot express "just this thread": receive_signal routes
//     SIGNAL_KILL to do_exit_group, which would kill the exec'ing thread too.
//     So the signal still does the waking and reaching -- that machinery is
//     subtle and worth reusing -- and task->exit_requested changes only what
//     it does on arrival.
//
//   - A thread here is a child of its CREATOR, not of the leader's parent as
//     in Linux (kernel/fork.c re-links only for CLONE_PARENT). So when the old
//     leader exits, find_new_parent hands its children to the first live
//     thread in the group -- which is us -- and the exec'ing thread ends up
//     its own parent. The real parent then has no such child at all and its
//     wait() returns ECHILD. Hence the family-tree fixup below, which has no
//     counterpart in Linux's de_thread.
static void exec_de_thread(void) {
    struct tgroup *group = current->group;
    struct task *task;

    // Captured before anything is torn down: the old leader's identity is what
    // this thread is about to inherit, and its parent must be read while the
    // process tree is still intact.
    struct task *leader = group->leader;
    bool taking_over = leader != NULL && leader != current;
    struct task *inherit_parent = NULL;
    int inherit_exit_signal = 0;
    if (taking_over) {
        complex_lockt(&pids_lock, 0);
        inherit_parent = leader->parent;
        inherit_exit_signal = leader->exit_signal;
        if (inherit_parent != NULL)
            task_ref_cnt_mod(inherit_parent, 1);
        unlock(&pids_lock);
    }

    struct zap_target {
        struct task *task;
        struct sighand *sighand;
    };
    struct zap_target stack_targets[32];
    struct zap_target *targets = stack_targets;
    size_t target_cap = sizeof(stack_targets) / sizeof(stack_targets[0]);
    size_t target_count = 0;
    bool zapped_any = false;

    while (true) {
        complex_lockt(&pids_lock, 0);
        lock(&group->lock, 0);

        size_t needed = 0;
        list_for_each_entry(&group->threads, task, group_links) {
            if (task != current)
                needed++;
        }
        if (needed == 0) {
            unlock(&group->lock);
            unlock(&pids_lock);
            break;
        }
        if (needed > target_cap) {
            unlock(&group->lock);
            unlock(&pids_lock);
            if (targets != stack_targets)
                free(targets);
            targets = malloc(sizeof(*targets) * needed);
            if (targets == NULL)
                die("out of memory collecting exec zap targets");
            target_cap = needed;
            continue;
        }

        target_count = 0;
        list_for_each_entry(&group->threads, task, group_links) {
            if (task == current)
                continue;
            task_ref_cnt_mod(task, 1);
            __atomic_store_n(&task->exit_requested, true, __ATOMIC_RELEASE);
            targets[target_count].task = task;
            targets[target_count].sighand = task->sighand;
            if (targets[target_count].sighand != NULL)
                sighand_retain(targets[target_count].sighand);
            target_count++;
        }
        // A group-stopped sibling is parked in the job-control wait with
        // nothing left to wake it; clear the stop so they can all run to their
        // exits.
        group->stopped = false;
        unlock(&group->lock);
        unlock(&pids_lock);
        zapped_any = true;
        break;
    }

    if (zapped_any) {
        notify(&group->stopped_cond);
        for (size_t i = 0; i < target_count; i++) {
            if (targets[i].sighand != NULL) {
                deliver_signal_with_sighand(targets[i].task, targets[i].sighand,
                        SIGKILL_, SIGINFO_NIL);
                sighand_release(targets[i].sighand);
            }
            task_ref_cnt_mod(targets[i].task, -1);
        }
    }
    if (targets != stack_targets)
        free(targets);

    // Wait for them to leave the group. do_exit unlinks a thread from
    // group->threads partway through, so this is the honest "am I alone yet"
    // test; the ceiling keeps a sibling wedged somewhere a signal cannot reach
    // from hanging the exec forever.
    struct timespec zap_pause = { .tv_sec = 0, .tv_nsec = 200000 };  // 200us
    bool alone = false;
    for (int i = 0; i < 50000 && !alone; i++) {                      // ~10s
        complex_lockt(&pids_lock, 0);
        lock(&group->lock, 0);
        size_t others = 0;
        list_for_each_entry(&group->threads, task, group_links) {
            if (task != current)
                others++;
        }
        unlock(&group->lock);
        unlock(&pids_lock);
        if (others == 0)
            alone = true;
        else
            nanosleep(&zap_pause, NULL);
    }
    if (!alone)
        printk("WARNING: execve gave up waiting for sibling threads to exit "
               "(pid=%d comm=%s); continuing anyway\n", current->pid, current->comm);

    if (!taking_over || !alone) {
        if (inherit_parent != NULL)
            task_ref_cnt_mod(inherit_parent, -1);
        return;
    }

    // do_exit drops out of group->threads partway through and keeps working on
    // its own struct afterwards; releasing it before it is finished would be a
    // use-after-free. Wait for the marker it sets last.
    for (int i = 0; i < 50000; i++) {
        if (atomic_load_explicit(&leader->exit_finished, memory_order_acquire))
            break;
        nanosleep(&zap_pause, NULL);
    }
    if (!atomic_load_explicit(&leader->exit_finished, memory_order_acquire)) {
        printk("WARNING: execve could not retire the old thread-group leader "
               "(pid=%d comm=%s); keeping pid %d\n", current->pid, current->comm, current->pid);
        if (inherit_parent != NULL)
            task_ref_cnt_mod(inherit_parent, -1);
        return;
    }

    complex_lockt(&pids_lock, 0);
    // Give up the tid we were allocated as a thread...
    struct pid *own = pid_get(current->pid);
    if (own != NULL && own->task == current) {
        own->task = NULL;
        list_remove(&own->alive);
    }
    // ...and take the leader's, which is this process's pid. Session and
    // process-group membership hang off struct pid, so they travel with it.
    struct pid *lead_pid = pid_get(leader->pid);
    if (lead_pid != NULL)
        lead_pid->task = current;
    current->pid = leader->pid;
    // A thread has no exit signal; the process it now is does.
    current->exit_signal = inherit_exit_signal;
    // Before the release below, so task_free_final does not mistake the old
    // leader for the current one and free the tgroup out from under us.
    group->leader = current;

    // Take the leader's place in the process tree. Without this the exec'ing
    // thread stays parented to itself (see the comment above) and its real
    // parent's wait() reports ECHILD.
    struct task *new_parent = inherit_parent;
    if (new_parent == NULL || new_parent == current || new_parent->exiting)
        new_parent = pid_get_task(1);
    if (new_parent != NULL && new_parent != current) {
        list_remove(&current->siblings);
        list_add(&new_parent->children, &current->siblings);
        current->parent = new_parent;
    }

    // The old leader is nobody's child now, and owns no pid.
    list_remove(&leader->siblings);
    list_remove_safe(&leader->ptrace_siblings);
    leader->pid = 0;
    unlock(&pids_lock);

    if (inherit_parent != NULL)
        task_ref_cnt_mod(inherit_parent, -1);

    // Defers by itself if anything still holds a reference.
    task_destroy_unlinked(leader, 2);
}

static intptr_t elf_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    intptr_t err = 0;
    struct task *save = current;
    bool mem_locked = false;
    struct mm *new_mm = NULL;

    // read the headers
    struct elf_info header;
    if ((err = read_header(fd, &header)) < 0)
        return err;
    // The patch-1 ENOEXEC guard here (rejecting GUEST_ABI_ARM64) is removed
    // as of aarch64_guest_plan.md patch 5: the register file (patch 2),
    // interpreter (patch 3), syscall table (patch 4), and the
    // cpu_run_to_interrupt() dispatch wiring below now exist, so an aarch64
    // ELF has somewhere real to go instead of falling through to the i386
    // JIT and having its instruction bytes misdecoded as x86.
    size_t guest_word_size = guest_abi_desc(header.abi).pointer_size;
    bool is_64bit = guest_abi_is_64bit(header.abi);
    struct elf_prg_info *ph;
    if ((err = read_prg_headers(fd, header, &ph)) < 0)
        return err;

    // look for an interpreter
    char *interp_name = NULL;
    struct fd *interp_fd = NULL;
    struct elf_info interp_header;
    struct elf_prg_info *interp_ph = NULL;
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_INTERP)
            continue;
        if (interp_name) {
            err = _EINVAL;
            goto out_free_interp;
        }

        interp_name = malloc(ph[i].filesize);
        err = _ENOMEM;
        if (interp_name == NULL)
            goto out_free_ph;

        err = _EIO;
        if (fd->ops->lseek(fd, ph[i].offset, SEEK_SET) < 0)
            goto out_free_interp;
        size_t interp_size = ph[i].filesize;
        if (fd->ops->read(fd, interp_name, interp_size) != (ssize_t) interp_size)
            goto out_free_interp;

        interp_fd = generic_open(interp_name, O_RDONLY, 0);
        if (IS_ERR(interp_fd)) {
            err = PTR_ERR(interp_fd);
            goto out_free_interp;
        }
        if ((err = read_header(interp_fd, &interp_header)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
        if (interp_header.abi != header.abi) {
            err = _ELIBBAD;
            goto out_free_interp;
        }
        if ((err = read_prg_headers(interp_fd, interp_header, &interp_ph)) < 0) {
            if (err == _ENOEXEC)
                err = _ELIBBAD;
            goto out_free_interp;
        }
    }

    new_mm = mm_new(header.abi);
    if (new_mm == NULL) {
        err = _ENOMEM;
        goto out_free_interp;
    }

    // Every other thread in the group dies here and this thread takes over
    // the leader's identity, before anything becomes irreversible -- the
    // same place Linux runs de_thread.
    exec_de_thread();

    // free the process's memory.
    // from this point on, if any error occurs the process will have to be
    // killed before it even starts. please don't be too sad about it, it's
    // just a process.
    //
    // general_lock protects current->mm. otherwise procfs might read the
    // pointer before it's released and then try to lock it after it's
    // released.
    lock(&save->general_lock, 0);
    mm_release(save->mm);
    save->abi = header.abi;
    task_set_mm(save, new_mm);
    new_mm = NULL;
    unlock(&save->general_lock);
    write_lock(&save->mem->lock);
    mem_locked = true;

    save->mm->exefile = fd_retain(fd);

    guest_addr_t load_addr = 0;
    bool load_addr_set = false;
    guest_addr_t bias = 0;
    // Set alongside the arm64/riscv64 dynamic-placement bias below; used
    // after the loop to actually reserve that headroom (see the comment
    // there for why the reservation, not just the address gap, matters).
    pages_t brk_headroom_pages = 0;

    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (!load_addr_set && header.type == ELF_DYNAMIC) {
            if (interp_name && header.abi == GUEST_ABI_I386)
                bias = 0x56555000;
            else if (interp_name && header.abi == GUEST_ABI_AMD64)
                // Pin the amd64 PIE main executable at the conventional low
                // Linux base so the brk heap grows up into the large mmap
                // window. find_hole_for_elf() returns the *top* of the window
                // (just under mmap_ceiling), which pins start_brk there and
                // caps the heap at the ~32 MiB gap to the page limit (2^47);
                // brk-hungry programs like git then fail to expand the heap.
                bias = 0x555555554000;
            else {
                // arm64/riscv64 PIE binaries fall through to here
                // intentionally: dynamic placement, not a fixed low bias.
                // See the GUEST_ABI_ARM64 case in guest_abi_vm_layout()
                // (kernel/abi.h) for why — avoids the V8 CodeRange
                // collision that OpenMinis' ish-arm64 fork hit with a
                // fixed low bias.
                // 1 GiB of brk headroom above the image (see the helper).
                brk_headroom_pages = 0x40000000 >> PAGE_BITS;
                bias = find_hole_for_elf(&header, ph, brk_headroom_pages);
            }
        }

        if ((err = load_entry(header.abi, ph[i], bias, fd)) < 0)
            goto beyond_hope;

        guest_addr_t candidate_load_addr;
        if ((err = elf_load_addr_candidate(header.abi, ph[i], bias, &candidate_load_addr)) < 0)
            goto beyond_hope;
        if (!load_addr_set || candidate_load_addr < load_addr) {
            load_addr = candidate_load_addr;
            load_addr_set = true;
        }

        qword_t brk_q = (qword_t) bias + ph[i].vaddr + ph[i].memsize;
        if (!elf_value_fits_addr(header.abi, brk_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        guest_addr_t brk = (guest_addr_t) brk_q;
        if (brk > save->mm->start_brk)
            save->mm->start_brk = save->mm->brk = BYTES_ROUND_UP(brk);
    }

    if (brk_headroom_pages > 0 && save->mm->start_brk != 0) {
        // find_hole_for_elf() above only computed an address gap; nothing
        // stops a later mmap() (ld.so, thread stacks, a GC's own segment
        // allocation, ...) from landing in it and colliding with a
        // subsequent brk() once the heap grows that far — sys_brk_guest
        // requires pt_is_hole() over the new range and silently refuses to
        // grow otherwise. Record the headroom as a plain [start, end) range
        // on mem (brk_reserve_start/end) so pt_is_hole()/pt_find_hole() treat
        // it as occupied, and sys_brk_guest claims prefixes of it for real as
        // the heap grows into it. Deliberately NOT materialized as real
        // page-table entries: those would have to be walked and
        // copy-on-write'd by every future fork() of this process, which for
        // a 1 GiB headroom made every fork() of a dynamic-PIE arm64/riscv64
        // binary ruinously slow (~65x measured). Best-effort: if the range
        // isn't actually free (shouldn't happen, find_hole_for_elf sized the
        // hole to include it) just skip the reservation rather than failing
        // exec.
        page_t reserve_start = PAGE(BYTES_ROUND_UP(save->mm->start_brk));
        page_t mmap_ceiling = guest_abi_vm_layout(header.abi).mmap_ceiling;
        pages_t reserve_pages = brk_headroom_pages;
        if (reserve_start >= mmap_ceiling)
            reserve_pages = 0;
        else if (reserve_start + reserve_pages > mmap_ceiling)
            reserve_pages = mmap_ceiling - reserve_start;
        if (reserve_pages > 0 && pt_is_hole(save->mem, reserve_start, reserve_pages)) {
            save->mem->brk_reserve_start = reserve_start;
            save->mem->brk_reserve_end = reserve_start + reserve_pages;
        }
    }

    qword_t entry_q = (qword_t) bias + header.entry_point;
    if (!elf_value_fits_addr(header.abi, entry_q)) {
        err = _EOVERFLOW;
        goto beyond_hope;
    }
    guest_addr_t entry = (guest_addr_t) entry_q;
    guest_addr_t interp_base = 0;

    if (interp_name) {
        interp_base = find_hole_for_elf(&interp_header, interp_ph, 0);
        for (int i = interp_header.phent_count - 1; i >= 0; i--) {
            if (interp_ph[i].type != PT_LOAD)
                continue;
            if ((err = load_entry(interp_header.abi, interp_ph[i], interp_base, interp_fd)) < 0)
                goto beyond_hope;
        }
        entry_q = (qword_t) interp_base + interp_header.entry_point;
        if (!elf_value_fits_addr(interp_header.abi, entry_q)) {
            err = _EOVERFLOW;
            goto beyond_hope;
        }
        entry = (guest_addr_t) entry_q;
    }

    guest_addr_t vdso_entry = 0;
    if (!is_64bit) {
        err = _ENOMEM;
        pages_t vdso_pages = sizeof(vdso_data) >> PAGE_BITS;
        page_t vdso_page = pt_find_hole(save->mem, vdso_pages + 1);
        if (vdso_page == BAD_PAGE)
            goto beyond_hope;
        vdso_page += 1;
        // The vDSO is read and executed by the guest (the loader parses its ELF
        // header; libc calls into it), so it must carry read+exec permission --
        // r-xp on real Linux. It was mapped with no permission bits, which only
        // worked while reads went unchecked; mem_ptr_nofault now faults a
        // PROT_NONE page on read, as Linux does.
        if ((err = pt_map(save->mem, vdso_page, vdso_pages, (void *) vdso_data, 0, P_READ | P_EXEC)) < 0)
            goto beyond_hope;
        mem_pt(save->mem, vdso_page)->data->name = "[vdso]";
        save->mm->vdso = vdso_page << PAGE_BITS;
        vdso_entry = save->mm->vdso + ((struct elf_header *) vdso_data)->entry_point;

        page_t vvar_page = pt_find_hole(save->mem, VVAR_PAGES);
        if (vvar_page == BAD_PAGE)
            goto beyond_hope;
        if ((err = pt_map_nothing(save->mem, vvar_page, VVAR_PAGES, 0)) < 0)
            goto beyond_hope;
        mem_pt(save->mem, vvar_page)->data->name = "[vvar]";
    }

    struct guest_vm_layout vm_layout = guest_abi_vm_layout(save->abi);
    if ((err = pt_map_nothing(save->mem, vm_layout.stack_page, 1, P_WRITE | P_GROWSDOWN)) < 0)
        goto beyond_hope;
    write_unlock(&save->mem->lock);
    mem_locked = false;

    guest_addr_t sp = vm_layout.stack_pointer;
    sp -= guest_word_size;

    err = _EFAULT;
    guest_addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto beyond_hope;
    guest_addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto beyond_hope;
    save->mm->env_start = sp;
    save->mm->env_end = sp + args_size(envp);
    guest_addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto beyond_hope;
    save->mm->argv_start = sp;
    save->mm->argv_end = sp + args_size(argv);
    sp = align_stack(sp);

    guest_addr_t platform_addr = sp = copy_string(sp, task_abi_desc(save).elf_platform);
    if (sp == 0)
        goto beyond_hope;
    char random[16] = {};
    get_random(random, sizeof(random));
    guest_addr_t random_addr = sp -= sizeof(random);
    if (user_put(sp, random))
        goto beyond_hope;

    size_t vector_bytes = ((argv.count + 1) + (envp.count + 1) + 1) * guest_word_size;
    if (!is_64bit) {
        struct aux_ent aux[] = {
            {AX_SYSINFO, vdso_entry},
            {AX_SYSINFO_EHDR, save->mm->vdso},
            {AX_HWCAP, 0},
            {AX_PAGESZ, PAGE_SIZE},
            {AX_CLKTCK, 0x64},
            {AX_PHDR, load_addr + header.prghead_off},
            {AX_PHENT, header.phent_size},
            {AX_PHNUM, header.phent_count},
            {AX_BASE, interp_base},
            {AX_FLAGS, 0},
            {AX_ENTRY, bias + header.entry_point},
            {AX_UID, current->exec_auxv_uid},
            {AX_EUID, current->exec_auxv_euid},
            {AX_GID, current->exec_auxv_gid},
            {AX_EGID, current->exec_auxv_egid},
            {AX_SECURE, current->exec_secure ? 1 : 0},
            {AX_RANDOM, random_addr},
            {AX_HWCAP2, 0},
            {AX_EXECFN, file_addr},
            {AX_PLATFORM, platform_addr},
            {0, 0}
        };
        sp -= vector_bytes;
        sp -= sizeof(aux);
        sp = align_stack(sp);

        guest_addr_t p = sp;
        dword_t argc_word = (dword_t) argv.count;
        dword_t zero = 0;
        if (user_put(p, argc_word))
            goto beyond_hope;
        p += guest_word_size;

        size_t argc = argv.count;
        while (argc-- > 0) {
            dword_t argv_word = (dword_t) argv_addr;
            if (user_put(p, argv_word))
                goto beyond_hope;
            ssize_t arg_len = user_strlen(argv_addr);
            if (arg_len < 0)
                goto beyond_hope;
            argv_addr += arg_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        size_t envc = envp.count;
        while (envc-- > 0) {
            dword_t envp_word = (dword_t) envp_addr;
            if (user_put(p, envp_word))
                goto beyond_hope;
            ssize_t env_len = user_strlen(envp_addr);
            if (env_len < 0)
                goto beyond_hope;
            envp_addr += env_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        save->mm->auxv_start = p;
        if (user_put(p, aux))
            goto beyond_hope;
        p += sizeof(aux);
        save->mm->auxv_end = p;
    } else {
        // AT_HWCAP: on aarch64, advertise exactly the ISA features the JIT
        // implements, so libc/OpenSSL take their accelerated paths.
        //   FP(0) ASIMD(1) AES(3) PMULL(4) SHA1(5) SHA2(6) CRC32(7)
        //   ATOMICS(8) SHA3(17) SHA512(21), matching the ID registers
        // gen.c serves. These hold on EVERY host device: CRC32 and SHA512
        // run as soft fallbacks where the host CPU lacks the instruction
        // (pre-A10 / pre-A13 — see gen.c's arm64_probe_host_caps), the
        // SHA3 ops are baseline NEON, and the LSE atomics run through C
        // helpers. ASIMDDP is deliberately NOT set: nothing implements
        // SDOT/UDOT yet. amd64 keeps 0 (that path predates any x86 HWCAP
        // need). Kept in sync with the ID_AA64ISAR0 value.
        qword_t hwcap = 0;
        if (current->abi == GUEST_ABI_ARM64)
            hwcap = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) |
                    (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
                    (1u << 17) | (1u << 21);
        // riscv64 COMPAT_HWCAP_ISA_*: one bit per ISA letter (bit = c-'a').
        // rv64imafdc, matching the JIT and /proc/cpuinfo's isa line.
        if (current->abi == GUEST_ABI_RISCV64)
            hwcap = (1u << ('i' - 'a')) | (1u << ('m' - 'a')) |
                    (1u << ('a' - 'a')) | (1u << ('f' - 'a')) |
                    (1u << ('d' - 'a')) | (1u << ('c' - 'a'));
        struct aux64_ent aux[] = {
            {AX_HWCAP, hwcap},
            {AX_PAGESZ, PAGE_SIZE},
            {AX_CLKTCK, 0x64},
            {AX_PHDR, load_addr + header.prghead_off},
            {AX_PHENT, header.phent_size},
            {AX_PHNUM, header.phent_count},
            {AX_BASE, interp_base},
            {AX_FLAGS, 0},
            {AX_ENTRY, bias + header.entry_point},
            {AX_UID, current->exec_auxv_uid},
            {AX_EUID, current->exec_auxv_euid},
            {AX_GID, current->exec_auxv_gid},
            {AX_EGID, current->exec_auxv_egid},
            {AX_SECURE, current->exec_secure ? 1 : 0},
            {AX_RANDOM, random_addr},
            {AX_HWCAP2, 0},
            {AX_EXECFN, file_addr},
            {AX_PLATFORM, platform_addr},
            {0, 0}
        };
        sp -= vector_bytes;
        sp -= sizeof(aux);
        sp = align_stack(sp);

        guest_addr_t p = sp;
        qword_t argc_word = (qword_t) argv.count;
        qword_t zero = 0;
        if (user_put(p, argc_word))
            goto beyond_hope;
        p += guest_word_size;

        size_t argc = argv.count;
        while (argc-- > 0) {
            qword_t argv_word = (qword_t) argv_addr;
            if (user_put(p, argv_word))
                goto beyond_hope;
            ssize_t arg_len = user_strlen(argv_addr);
            if (arg_len < 0)
                goto beyond_hope;
            argv_addr += arg_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        size_t envc = envp.count;
        while (envc-- > 0) {
            qword_t envp_word = (qword_t) envp_addr;
            if (user_put(p, envp_word))
                goto beyond_hope;
            ssize_t env_len = user_strlen(envp_addr);
            if (env_len < 0)
                goto beyond_hope;
            envp_addr += env_len + 1;
            p += guest_word_size;
        }
        if (user_put(p, zero))
            goto beyond_hope;
        p += guest_word_size;

        save->mm->auxv_start = p;
        if (user_put(p, aux))
            goto beyond_hope;
        p += sizeof(aux);
        save->mm->auxv_end = p;
    }

    save->mm->stack_start = sp;
    save->cpu.amd64_syscall = (struct amd64_syscall_state) {};
    save->cpu.fcw = 0x37f;
    save->cpu.mxcsr = 0x1f80;

    memset(save->cpu.amd64_regs, 0, sizeof(save->cpu.amd64_regs));
    save->cpu.amd64_rip = entry;
    save->cpu.amd64_regs[amd64_rsp] = sp;
    memset(save->cpu.amd64_store_trace, 0, sizeof(save->cpu.amd64_store_trace));
    save->cpu.amd64_store_trace_next = 0;

    save->cpu.esp = (addr_t) sp;
    save->cpu.eip = (addr_t) entry;
    save->cpu.eax = 0;
    save->cpu.ebx = 0;
    save->cpu.ecx = 0;
    save->cpu.edx = 0;
    save->cpu.esi = 0;
    save->cpu.edi = 0;
    save->cpu.ebp = 0;
    collapse_flags(&save->cpu);
    save->cpu.eflags = 0;

    // Unconditional like the i386/amd64 blocks above — struct cpu_state's
    // arm64 fields are always-present siblings (aarch64_guest_plan.md
    // patch 2), so there's no harm initializing them for a non-arm64 task;
    // only the abi-matched engine ever reads them.
    memset(save->cpu.arm64_regs, 0, sizeof(save->cpu.arm64_regs));
    save->cpu.arm64_pc = entry;
    save->cpu.arm64_sp = sp;
    save->cpu.arm64_nzcv = 0;
    save->cpu.arm64_excl_addr = UINT64_MAX;
    save->cpu.arm64_excl_val = 0;
    save->cpu.arm64_fpsr = 0;
    save->cpu.arm64_fpcr = 0;
    memset(save->cpu.arm64_v, 0, sizeof(save->cpu.arm64_v));

    // riscv64, same unconditional-sibling rationale as the arm64 block.
    // regs[0] is the hardwired-zero x0 and must stay 0; sp is x2.
    memset(save->cpu.riscv64_regs, 0, sizeof(save->cpu.riscv64_regs));
    save->cpu.riscv64_zero_sink = 0;
    save->cpu.riscv64_pc = entry;
    save->cpu.riscv64_regs[riscv64_sp] = sp;
    save->cpu.riscv64_res_addr = UINT64_MAX;
    save->cpu.riscv64_res_val = 0;
    memset(save->cpu.riscv64_f, 0, sizeof(save->cpu.riscv64_f));
    save->cpu.riscv64_fcsr = 0;

    err = 0;
out_free_interp:
    if (new_mm != NULL)
        mm_release(new_mm);
    if (interp_name != NULL)
        free(interp_name);
    if (interp_fd != NULL && !IS_ERR(interp_fd))
        fd_close(interp_fd);
    if (interp_ph != NULL)
        free(interp_ph);
out_free_ph:
    free(ph);
    return err;

beyond_hope:
    amd64_trace_exec_loader_failure("elf-exec", file, header.abi, NULL, bias, fd, err, interp_name);
    if (mem_locked)
        write_unlock(&save->mem->lock);
    goto out_free_interp;
}

// exec_args packs its strings back to back; natively-implemented programs
// (kernel/native.h) are ordinary C and want the argv/envp shape. The pointers
// alias the caller's block rather than copying it, so the vector is only valid
// for as long as that block is.
static char **exec_args_to_vector(struct exec_args args) {
    char **vec = malloc((args.count + 1) * sizeof(*vec));
    if (vec == NULL)
        return NULL;
    const char *p = args.args;
    for (size_t i = 0; i < args.count; i++) {
        vec[i] = (char *) p;
        p += strlen(p) + 1;
    }
    vec[args.count] = NULL;
    return vec;
}

static size_t args_size(struct exec_args args) {
    const char *args_end = args.args;
    for (size_t i = 0; i < args.count; i++) {
        args_end += strlen(args_end) + 1;
    }
    // don't forget the very last null terminator
    assert(args_end[0] == '\0');
    args_end++;
    return args_end - args.args;
}

static inline guest_addr_t align_stack(guest_addr_t sp) {
    return sp &~ 0xf;
}

static inline guest_addr_t copy_string(guest_addr_t sp, const char *string) {
    sp -= strlen(string) + 1;
    if (user_write_string(sp, string))
        return 0;
    return sp;
}

static inline guest_addr_t args_copy(guest_addr_t sp, struct exec_args args) {
    size_t size = args_size(args);
    sp -= size;
    if (user_write(sp, args.args, size))
        return 0;
    return sp;
}

static inline ssize_t user_strlen(guest_addr_t p) {
    size_t i = 0;
    char c;
    do {
        if (user_get(p + i, c))
            return -1;
        i++;
    } while (c != '\0');
    return i - 1;
}

static inline int user_memset(guest_addr_t start, byte_t val, dword_t len) {
    while (len--)
        if (user_put(start++, val))
            return 1;
    return 0;
}

static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    int err = (int)elf_exec(fd, file, argv, envp);
    if (err != _ENOEXEC)
        return err;
    // other formats would go here
    return _ENOEXEC;
}

// Open a file for execution, the way Linux's open_exec does: resolve the
// caller's execute permission BEFORE opening, and refuse anything that is not
// an ordinary file on a mount that allows execution. Fills *stat with the file
// it decided on, because the caller needs the set-id bits from the same stat
// the decision was made on. Returns an ERR_PTR on refusal.
//
// It used to be an ordinary O_RDONLY open with no permission question asked at
// all, which got three separate things wrong. The read check is a different
// question from the execute check, so a 0644 file the caller could read was
// executed and a 0711 file -- executable but not readable -- was refused. The
// type was never checked, so a directory reached the ELF loader and came back
// EIO while a FIFO reached open(2) and BLOCKED, hanging the task forever with
// no way to tell it from a slow program. And MS_NOEXEC was recorded on the
// mount, reported in /proc/mounts, and then never consulted, so `mount -o
// noexec` was purely decorative -- worse than not supporting it, because the
// whole point is that somebody is relying on it to hold.
//
// The extra stat costs one path resolution per exec. That is the honest price
// of asking the questions in the right order; exec is not a hot path next to
// open and stat.
static struct fd *open_exec(const char *file, struct statbuf *stat) {
    int err = generic_statat(AT_PWD, file, stat, 0);
    if (err < 0)
        return ERR_PTR(err);

    // Only a regular file is ever executable. Linux reports EACCES for a
    // directory, a fifo or a device alike.
    if (!S_ISREG(stat->mode))
        return ERR_PTR(_EACCES);

    // The CALLER's execute permission, not anybody's. access_check keeps
    // Linux's rule that even root needs at least one execute bit on a
    // non-directory, which is what the old test got right by accident.
    err = access_check(stat, AC_X);
    if (err < 0)
        return ERR_PTR(err);

    // O_NOACCESS_CHECK_ because the execute check above is the one that
    // governs: an execute-only file has to load despite being unreadable,
    // which is why Linux opens it with FMODE_EXEC rather than for reading.
    struct fd *fd = generic_open(file, O_RDONLY | O_NOACCESS_CHECK_, 0);
    if (IS_ERR(fd))
        return fd;

    if (fd->mount != NULL && (fd->mount->flags & MS_NOEXEC_)) {
        fd_close(fd);
        return ERR_PTR(_EACCES);
    }
    return fd;
}

static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    // read the first 128 bytes to get the shebang line out of
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    char header[128];
    ssize_t size = fd->ops->read(fd, header, sizeof(header) - 1);
    if (size < 0)
        return _EIO;
    header[size] = '\0';

    // only look at the first line
    char *newline = strchr(header, '\n');
    if (newline == NULL)
        return _ENOEXEC;
    *newline = '\0';

    // format: #![spaces]interpreter[spaces]argument[spaces]
    char *p = header;
    if (p[0] != '#' || p[1] != '!')
        return _ENOEXEC;
    p += 2;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return _ENOEXEC;

    char *interpreter = p;
    while (*p != ' ' && *p != '\0')
        p++;
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ')
            p++;
    }

    char *argument = p;
    // strip trailing whitespace
    p = strchr(p, '\0') - 1;
    while (*p == ' ')
        *p-- = '\0';
    if (*argument == '\0')
        argument = NULL;

    struct exec_args argv_rest = {
        .count = argv.count - 1,
        .args = argv.args + strlen(argv.args) + 1,
    };
    size_t args_rest_size = args_size(argv_rest);

    // Bolt: Cache lengths to avoid redundant O(N) traversals
    size_t interpreter_len = strlen(interpreter);
    size_t file_len = strlen(file);
    size_t argument_len = argument ? strlen(argument) : 0;

    size_t extra_args_size = interpreter_len + 1 + file_len + 1;
    if (argument)
        extra_args_size += argument_len + 1;
    if (args_rest_size + extra_args_size >= ARGV_MAX)
        return _E2BIG;

    char *new_argv_buf = malloc(ARGV_MAX);
    if (new_argv_buf == NULL)
        return _ENOMEM;
    struct exec_args new_argv = {.args = new_argv_buf};
    size_t n = 0;

    // Bolt: Use memcpy with cached lengths instead of strcpy + strlen
    memcpy(new_argv_buf, interpreter, interpreter_len + 1);
    new_argv.count++;
    n += interpreter_len + 1;
    if (argument) {
        memcpy(new_argv_buf + n, argument, argument_len + 1);
        new_argv.count++;
        n += argument_len + 1;
    }
    memcpy(new_argv_buf + n, file, file_len + 1);
    n += file_len + 1;
    new_argv.count++;
    memcpy(new_argv_buf + n, argv_rest.args, args_rest_size);
    new_argv.count += argv_rest.count;

    // The interpreter is executed, so it faces the same rules as any other
    // program: the caller must have execute permission on it, it must be an
    // ordinary file, and its mount must allow execution. This was a plain
    // O_RDONLY open, so a script could run an interpreter the caller was not
    // allowed to execute -- Linux answers EACCES.
    struct statbuf interpreter_stat;
    struct fd *interpreter_fd = open_exec(interpreter, &interpreter_stat);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return (int)PTR_ERR(interpreter_fd);
    }
    int err = format_exec(interpreter_fd, interpreter, new_argv, envp);
    fd_close(interpreter_fd);
    free(new_argv_buf);
    return err;
}

// A native program (kernel/native.h) replaces this process image exactly as an
// ELF would, and everything below format_exec that is NOT about loading an
// image applies to it just the same. That half used to be skipped altogether,
// because the native branch returns before reaching any of it.
//
// The descriptor half of the omission was a deadlock. A parent that wants to
// know whether its child's exec worked hands the child a close-on-exec pipe
// and reads it: EOF means the exec happened, four bytes of errno mean it did
// not. apt's pager handshake is exactly that, and `apt search maria` wedged
// the whole app whenever the pager it found resolved to SmallCLUE's native
// less -- the write end survived an exec that never closed it, so apt sat on a
// four-byte read while the pager sat on the stdin apt had not begun writing.
// Neither could move. See docs/TODO.md.
// POSIX timers (timer_create) do not survive execve on Linux: the new image
// gets none. AOK kept them armed on the tgroup, which outlives the exec, so a
// timer set before the exec fired into a program that never created it -- with
// the old image's signal number and, for SIGALRM's default action, killing it
// outright. fork() already clears them (kernel/fork.c); this is the other half.
//
// Freed rather than merely forgotten, since the tgroup lives on. Clearing
// tgroup before timer_free mirrors kernel/exit.c: posix_timer_callback bails on
// a NULL tgroup, and timer_free does not wait for a callback already in flight.
static void exec_discard_posix_timers(void) {
    struct tgroup *group = current->group;
    if (group == NULL)
        return;
    lock(&group->lock, 0);
    for (int i = 0; i < TIMERS_MAX; i++) {
        struct posix_timer *pt = &group->posix_timers[i];
        if (pt->timer == NULL)
            continue;
        struct timer *timer = pt->timer;
        pt->tgroup = NULL;
        pt->timer = NULL;
        pt->timer_id = 0;
        unlock(&group->lock);
        timer_free(timer);
        lock(&group->lock, 0);
    }
    unlock(&group->lock);
}

static void exec_apply_native_process_state(void) {
    // cloexec: the trigger above, and the reason this function exists.
    fdtable_do_cloexec(current->files);

    // Caught signals go back to default across an exec; ignored ones stay
    // ignored. Skipping this left a native program running with the previous
    // program's handler addresses -- which, since no image was loaded over it,
    // still pointed into code that is no longer what is executing.
    lock(&current->sighand->lock, 0);
    for (int sig = 0; sig < NUM_SIGS; sig++) {
        struct sigaction_ *action = &current->sighand->action[sig];
        if (action->handler != SIG_IGN_)
            action->handler = SIG_DFL_;
    }
    current->altstack = 0;
    current->altstack_size = 0;
    unlock(&current->sighand->lock);
    // The shim keeps native code's own view of the dispositions beside the
    // guest table (kernel/native_libc.c). Resetting one and not the other
    // would leave the two disagreeing.
    native_sigtable_discard(current);
    exec_discard_posix_timers();

    // Linux clears the membarrier registration on exec (membarrier_exec_mmap):
    // the new image has not asked for expedited barriers and must find out it
    // needs to register, via the EPERM, exactly as a fresh process would.
    lock(&current->group->lock, 0);
    current->group->membarrier_private_expedited = false;
    unlock(&current->group->lock);

    current->did_exec = true;
    current->keepcaps = false;
    // A vfork parent is released by its child's exec, not by its exit. Without
    // this it stayed blocked for the native program's whole run -- which is
    // how glibc's posix_spawn waits, so it is not an exotic path.
    vfork_notify(current);
}

int __do_execve(const char *file, struct exec_args argv, struct exec_args envp) {
    // open_exec decides what the file IS and whether this caller may execute
    // it before opening it, which is Linux's do_open_execat order. This used
    // to open first and then ask only whether ANY execute bit was set, so a
    // root-owned 0744 binary was executable by every user on the system.
    struct statbuf stat;
    struct fd *fd = open_exec(file, &stat);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    int err;

    // Natively-implemented programs (/AOK/native/*, kernel/native.h) are
    // dispatched here: after the existence and permission checks above, so
    // they behave like any other executable, but before any ELF parsing, since
    // there is no guest image to load. Keyed off the resolved fd rather than
    // `file`, so a symlink from anywhere in the guest lands here while argv[0]
    // stays whatever the caller passed.
    const char *native_name = aokfs_native_program_name(fd);
    if (native_name != NULL) {
        const struct native_program *prog = native_program_lookup(native_name);
        // No match means this build does not carry that program; fall through
        // and run the /AOK/native stub, which says so out loud.
        if (prog != NULL) {
            char **native_argv = exec_args_to_vector(argv);
            char **native_envp = exec_args_to_vector(envp);
            if (native_argv == NULL || native_envp == NULL) {
                free(native_argv);
                free(native_envp);
                fd_close(fd);
                return _ENOMEM;
            }
            fd_close(fd);
            // Recorded rather than run here. Running a native program never
            // returns, so doing it at this point would strand every buffer the
            // execve syscall still means to free -- including the argv/envp
            // blocks themselves. Instead take a private copy, report success,
            // and let each entry point run it once its own frees are done (see
            // native_exec_run_pending).
            int perr = native_exec_set_pending(prog, (int) argv.count,
                    native_argv, native_envp);
            free(native_argv);
            free(native_envp);
            if (perr < 0)
                return perr;
            // Only once the record is safely taken: everything below commits
            // the exec, and there is no undoing a closed descriptor.
            exec_apply_native_process_state();
            return 0;
        }
    }

    // Stage what the credentials will be once this exec commits, for the aux
    // vector elf_exec is about to build. The real change stays below, after
    // the image is loaded: doing it here would leave a FAILED exec holding
    // elevated privilege. musl and glibc both decide a process is
    // secure-execution from AT_SECURE, and musl additionally from
    // AT_UID == AT_EUID && AT_GID == AT_EGID -- all four were hardcoded 0, so
    // a setuid-root binary looked like an ordinary one and honoured
    // LD_PRELOAD, giving any local user root.
    current->exec_secure = (stat.mode & (S_ISUID | S_ISGID)) != 0;
    current->exec_auxv_uid  = current->uid;
    current->exec_auxv_gid  = current->gid;
    current->exec_auxv_euid = (stat.mode & S_ISUID) ? stat.uid : current->euid;
    current->exec_auxv_egid = (stat.mode & S_ISGID) ? stat.gid : current->egid;

    err = format_exec(fd, file, argv, envp);
    if (err == _ENOEXEC) {
        // Linux ignores set-id bits on a #! script -- the interpreter runs
        // with the caller's credentials. We were applying the SCRIPT's bits in
        // the credential change below, so a root-owned mode-4755 script with a
        // cooperative interpreter handed any local user a root shell.
        //
        // Clear them before shebang_exec, which builds the interpreter's aux
        // vector from the staged values above, so the interpreter is neither
        // marked secure-execution nor given the script's owner as its euid.
        stat.mode &= ~(mode_t_) (S_ISUID | S_ISGID);
        current->exec_secure = false;
        current->exec_auxv_euid = current->euid;
        current->exec_auxv_egid = current->egid;
        err = shebang_exec(fd, file, argv, envp);
    }
    fd_close(fd);
    if (err < 0) {
        amd64_trace_exec_loader_failure("do-execve", file, current->abi, NULL, 0, NULL, err, NULL);
        return err;
    }

    // setuid/setgid
    if (stat.mode & S_ISUID) {
        current->euid = stat.uid;
        current->suid = stat.uid;  // saved-set-uid = new euid, not old
        current->fsuid = current->euid;
        if (stat.uid == 0) {
            // Legacy setuid-root: grant full permitted and effective caps so
            // helpers like sudo can use keepcaps+setresuid to drop uid while
            // retaining CAP_SETGID for a subsequent setresgid call.
            current->cap_effective[0] = current->cap_effective[1] = UINT32_MAX;
            current->cap_permitted[0] = current->cap_permitted[1] = UINT32_MAX;
        }
    }
    if (stat.mode & S_ISGID) {
        current->egid = stat.gid;
        current->sgid = stat.gid;  // saved-set-gid = new egid, not old
        current->fsgid = current->egid;
    }

    // Capabilities do not survive an ordinary exec. Linux recomputes them from
    // the file's own capabilities and the ambient set; with no file
    // capabilities and a caller that is not root, permitted and effective
    // collapse to the ambient set, which is normally empty.
    //
    // Nothing dropped them here, so a process that had lowered its uid while
    // holding capabilities -- exactly what prctl(PR_SET_KEEPCAPS) plus
    // setresuid is for -- handed the full set to whatever it exec'd next. That
    // is the escalation the recomputation exists to prevent.
    //
    // The AMBIENT set is preserved, which is the supported way to carry a
    // capability across an exec deliberately, and the root path is left
    // exactly as it was: Linux re-grants there too (handle_privileged_root),
    // and the setuid-root branch above depends on it.
    if (current->euid != 0 && current->uid != 0) {
        current->cap_permitted[0] = current->cap_ambient[0];
        current->cap_permitted[1] = current->cap_ambient[1];
        current->cap_effective[0] = current->cap_ambient[0];
        current->cap_effective[1] = current->cap_ambient[1];
    }

    // save current->comm
    char old_comm[sizeof(current->comm)];
    lock(&current->general_lock, 0);
    strncpy(old_comm, current->comm, sizeof(old_comm));
    old_comm[sizeof(old_comm) - 1] = '\0';
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    strncpy(current->comm, basename, sizeof(current->comm));
    current->comm[sizeof(current->comm) - 1] = '\0';
    unlock(&current->general_lock);

    bool force_safe_i386 = current->abi == GUEST_ABI_I386 &&
            i386_force_safe_exec_comm(current->comm);
    current->force_single_step = (current->abi == GUEST_ABI_I386 &&
            i386_single_step_comm_matches(current->comm)) || force_safe_i386;
    current->force_no_jit_cache = (current->abi == GUEST_ABI_I386 &&
            i386_no_cache_comm_matches(current->comm)) || force_safe_i386;
    if (current->force_no_jit_cache) {
        i386_special_trace_reset(current->tgid, current->comm);
    }

    {
        enum { AMD64_EXEC_TRACE_BUDGET = 64 };
        static unsigned amd64_exec_trace_count;
        lock(&current->group->lock, 0);
        struct tty *tty = current->group->tty;
        unlock(&current->group->lock);
        bool trace_exec = current->abi == GUEST_ABI_AMD64 &&
                tty != NULL &&
                (tty->type == TTY_CONSOLE_MAJOR || tty->type == TTY_PSEUDO_SLAVE_MAJOR);
        bool tracked_exec = strstr(file, "rustc") != NULL || strstr(file, "cargo") != NULL;
        bool tracked_lineage = amd64_trace_is_lineage_tgid(current->tgid);
        if ((trace_exec || tracked_exec || tracked_lineage) &&
                amd64_exec_trace_count < AMD64_EXEC_TRACE_BUDGET)
            amd64_exec_trace_count++;
        if (tracked_exec || tracked_lineage)
            amd64_trace_track_exec(current->pid, current->tgid, file);
    }

    update_thread_name();

    // cloexec
    // consider putting this in fd.c?
    fdtable_do_cloexec(current->files);

    // reset signal handlers
    lock(&current->sighand->lock, 0);
    for (int sig = 0; sig < NUM_SIGS; sig++) {
        struct sigaction_ *action = &current->sighand->action[sig];
        if (action->handler != SIG_IGN_)
            action->handler = SIG_DFL_;
    }
    current->altstack = 0;
    current->altstack_size = 0;
    unlock(&current->sighand->lock);
    // And the shim's own copy of the dispositions, for a task whose previous
    // image was a native program. A no-op for every other task, which never
    // allocates one.
    native_sigtable_discard(current);
    exec_discard_posix_timers();

    // Linux clears the membarrier registration on exec (membarrier_exec_mmap):
    // the new image has not asked for expedited barriers and must find out it
    // needs to register, via the EPERM, exactly as a fresh process would.
    lock(&current->group->lock, 0);
    current->group->membarrier_private_expedited = false;
    unlock(&current->group->lock);

    current->did_exec = true;
    current->keepcaps = false;
    vfork_notify(current);

    if (current->ptrace.traced) {
        current->ptrace.syscall = current->cpu.eax;
        current->cpu.eax = 0;
        struct siginfo_ info = {
            .sig = SIGTRAP_,
            .code = SI_USER_,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        };
        if (current->ptrace.options & PTRACE_O_TRACEEXEC_)
            ptrace_event_stop(SIGTRAP_, &info, PTRACE_EVENT_EXEC_, current->pid);
        else
            ptrace_signal_stop(SIGTRAP_, &info);
    }

    return 0;
}

// Some getty/inittab setups hard-code TERM=vt102 on the boot console and apply it
// with setenv() before exec'ing login, so it can't be corrected through the
// environment we hand the boot command — only here, at exec time. vt102 advertises
// no color at all, so rewrite that single bogus value to screen-256color, matching
// the TERM the app hands its interactive sessions (TerminalViewController.m). Returns
// a malloc'd replacement buffer (caller frees) or NULL when no rewrite is needed,
// keeping the common path allocation-free. envp_len counts every byte of the block
// including the trailing terminator, matching args_size()'s view of it.
static char *exec_fixup_term(const char *envp, size_t envp_len) {
    const char bogus[] = "TERM=vt102";
    const char fixed[] = "TERM=screen-256color";
    const char *match = NULL;
    for (const char *e = envp; *e != '\0'; e += strlen(e) + 1) {
        if (strncmp(e, "TERM=", 5) == 0) {
            // Only the first TERM entry takes effect; stop at it whatever its value.
            if (strcmp(e, bogus) == 0)
                match = e;
            break;
        }
    }
    if (match == NULL)
        return NULL;

    char *buf = malloc(envp_len + (sizeof(fixed) - sizeof(bogus)));
    if (buf == NULL)
        return NULL; // out of memory: leave the env unchanged rather than fail exec
    size_t prefix = (size_t) (match - envp);
    const char *rest = match + sizeof(bogus); // next entry (sizeof includes the NUL)
    size_t rest_len = envp_len - (size_t) (rest - envp);
    char *w = buf;
    memcpy(w, envp, prefix); w += prefix;
    memcpy(w, fixed, sizeof(fixed)); w += sizeof(fixed);
    memcpy(w, rest, rest_len);
    return buf;
}

int do_execve(const char *file, size_t argc, const char *argv_p, const char *envp_p) {
    struct exec_args argv = {.count = argc, .args = argv_p};
    struct exec_args envp = {.args = envp_p};
    while (*envp_p != '\0') {
        envp_p += strlen(envp_p) + 1;
        envp.count++;
    }
    // envp_p now points at the trailing terminator; the block spans envp.args..envp_p.
    size_t envp_len = (size_t) (envp_p - envp.args) + 1;
    char *fixed_env = exec_fixup_term(envp.args, envp_len);
    if (fixed_env != NULL)
        envp.args = fixed_env;
    int err = __do_execve(file, argv, envp);
    free(fixed_env); // NULL-safe: no-op when no rewrite happened
    return err;
}

static ssize_t user_read_string_array(guest_addr_t addr, char *buf, size_t max) {
    size_t guest_ptr_size = task_abi_desc(current).pointer_size;
    size_t i = 0;
    size_t p = 0;
    for (;;) {
        qword_t str_addr_q;
        ssize_t err = user_read_exec_ptr(addr + i * guest_ptr_size, &str_addr_q);
        if (err < 0)
            return err;
        if (str_addr_q == 0)
            break;
        if (!guest_abi_addr_valid(current->abi, str_addr_q))
            return _EFAULT;
        guest_addr_t str_addr = str_addr_q;
        size_t str_p = 0;
        for (;;) {
            if (p >= max)
                return _E2BIG;
            if (user_get(str_addr + str_p, buf[p]))
                return _EFAULT;
            str_p++;
            p++;
            if (buf[p - 1] == '\0')
                break;
        }
        i++;
    }
    if (p >= max)
        return _E2BIG;
    buf[p] = '\0';
    return i;
}

static ssize_t user_read_exec_ptr(guest_addr_t addr, qword_t *ptr_out) {
    if (task_is_64bit(current)) {
        qword_t ptr;
        if (user_get(addr, ptr))
            return _EFAULT;
        *ptr_out = ptr;
    } else {
        dword_t ptr;
        if (user_get(addr, ptr))
            return _EFAULT;
        *ptr_out = ptr;
    }
    return 0;
}

ssize_t sys_execve(addr_t filename_addr, addr_t argv_addr, addr_t envp_addr) {
    char filename[MAX_PATH];
    int path_err = user_read_path(filename_addr, filename, sizeof(filename));
    if (path_err)
        return path_err;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    err = do_execve(filename, argc, argv, envp);

    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

ssize_t sys_execve_guest(guest_addr_t filename_addr, guest_addr_t argv_addr, guest_addr_t envp_addr) {
    char filename[MAX_PATH];
    int path_err = user_read_path(filename_addr, filename, sizeof(filename));
    if (path_err)
        return path_err;

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    err = do_execve(filename, argc, argv, envp);

    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

ssize_t sys_execveat(fd_t dirfd, addr_t filename_addr, addr_t argv_addr, addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_)) {
        if (current != NULL && current->abi == GUEST_ABI_AMD64 && amd64_trace_is_lineage_tgid(current->tgid))
            printk("amd64 execveat invalid flags: pid=%d tgid=%d comm=%s flags=%#x dirfd=%d guest=0\n",
                   current->pid, current->tgid, current->comm, flags, dirfd);
        return _EINVAL;
    }

    char filename[MAX_PATH] = "";
    if (filename_addr != 0) {
        int path_err = user_read_path(filename_addr, filename, sizeof(filename));
        if (path_err)
            return path_err;
    }

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    char resolved[MAX_PATH];
    if (filename[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_)) {
            err = _ENOENT;
            goto out_free_args;
        }
        struct fd *fd = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (fd == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = generic_getpath(fd, resolved);
        if (err < 0)
            goto out_free_args;
    } else if (filename[0] == '/') {
        strcpy(resolved, filename);
    } else {
        struct fd *at = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (at == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = path_normalize(at, filename, resolved,
                (flags & AT_SYMLINK_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
        if (err < 0)
            goto out_free_args;
    }

    STRACE("execveat(%d, \"%s\", ..., %#x)", dirfd, filename, flags);
    amd64_trace_exec_attempt(resolved, argv);
    err = do_execve(resolved, argc, argv, envp);

out_free_args:
    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

ssize_t sys_execveat_guest(fd_t dirfd, guest_addr_t filename_addr, guest_addr_t argv_addr, guest_addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_)) {
        if (current != NULL && current->abi == GUEST_ABI_AMD64 && amd64_trace_is_lineage_tgid(current->tgid))
            printk("amd64 execveat invalid flags: pid=%d tgid=%d comm=%s flags=%#x dirfd=%d guest=1\n",
                   current->pid, current->tgid, current->comm, flags, dirfd);
        return _EINVAL;
    }

    char filename[MAX_PATH] = "";
    if (filename_addr != 0) {
        int path_err = user_read_path(filename_addr, filename, sizeof(filename));
        if (path_err)
            return path_err;
    }

    ssize_t argc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envp);
    if (err < 0)
        return err;

    char resolved[MAX_PATH];
    if (filename[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_)) {
            err = _ENOENT;
            goto out_free_args;
        }
        struct fd *fd = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (fd == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = generic_getpath(fd, resolved);
        if (err < 0)
            goto out_free_args;
    } else if (filename[0] == '/') {
        strcpy(resolved, filename);
    } else {
        struct fd *at = (dirfd == AT_FDCWD_) ? AT_PWD : f_get(dirfd);
        if (at == NULL) {
            err = _EBADF;
            goto out_free_args;
        }
        err = path_normalize(at, filename, resolved,
                (flags & AT_SYMLINK_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
        if (err < 0)
            goto out_free_args;
    }

    STRACE("execveat(%d, \"%.1000s\", {", dirfd, resolved);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, %d)", flags);

    amd64_trace_exec_attempt(resolved, argv);
    err = do_execve(resolved, argc, argv, envp);

out_free_args:
    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, char **envp_out) {
    char *argv = malloc(ARGV_MAX);
    if (argv == NULL)
        return _ENOMEM;
    ssize_t argc = user_read_string_array(argv_addr, argv, ARGV_MAX);
    if (argc < 0) {
        free(argv);
        return argc;
    }

    char *envp = malloc(ARGV_MAX);
    if (envp == NULL) {
        free(argv);
        return _ENOMEM;
    }
    if (envp_addr != 0) {
        ssize_t err = user_read_string_array(envp_addr, envp, ARGV_MAX);
        if (err < 0) {
            free(envp);
            free(argv);
            return err;
        }
    } else {
        // Do not take advantage of this nonstandard and nonportable misfeature!
        // - Michael Kerrisk, execve(2)
        envp[0] = envp[1] = '\0';
    }

    *argc_out = argc;
    *argv_out = argv;
    *envp_out = envp;
    return 0;
}
