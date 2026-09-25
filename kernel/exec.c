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
#include "emu/cpuid.h"
#include "emu/i386_sreg.h"
#include "kernel/personality.h"
#include "kernel/random.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/native.h"
#include "kernel/native_syscall.h"
#include "kernel/vdso.h"
#include "jit/jit.h"
#include "tools/ptraceomatic-config.h"
#include "util/sync.h"
#include "kernel/binfmt_misc.h"
#include "kernel/xattr.h"
#include "kernel/rseq.h"
#include "kernel/anonfd_ckpt.h"

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
static inline size_t args_strings_size(struct exec_args args);
static ssize_t user_read_exec_ptr(guest_addr_t addr, qword_t *ptr_out);
static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, ssize_t *envc_out, char **envp_out);
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

// Read the file being executed at `off`, leaving the description's position
// alone. The description can be the caller's own: open_exec shares it when no
// path reaches the file (a memfd, an unlinked file), and exec must leave the
// caller's offset where the caller put it. Every loader reads through here.
//
// Only a description of exec's own can lack pread (open_exec never shares one
// that does), so the lseek+read fallback moves nobody else's position. It is
// still needed: jumping through a NULL pread was a host EXC_BAD_ACCESS abort
// when execing a binary that lived on tmpfs, before tmpfs had one.
static ssize_t exec_read_at(struct fd *fd, void *buf, size_t size, off_t_ off) {
    if (fd->ops->pread != NULL)
        return fd->ops->pread(fd, buf, size, (off_t) off);
    if (fd->ops->lseek == NULL || fd->ops->read == NULL)
        return _EINVAL;
    off_t_ at = fd->ops->lseek(fd, off, LSEEK_SET);
    if (at < 0)
        return at;
    return fd->ops->read(fd, buf, size);
}

static int read_header(struct fd *fd, struct elf_info *header) {
    union {
        struct elf_header elf32;
        struct elf64_header elf64;
    } raw;

    ssize_t err;
    if ((err = exec_read_at(fd, &raw, sizeof(raw), 0)) < (ssize_t) sizeof(struct elf_header)) {
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
    // Each entry at its own offset (exec_read_at). A table the file is too
    // short to hold is EIO, as Linux's elf_read makes any short read; this
    // used to consult errno, which a short read does not set, so the answer
    // was whatever an earlier host call had left there.

    if (header.bitness == ELF_32BIT) {
        if (header.phent_size < sizeof(struct prg_header)) {
            free(ph);
            return _ENOEXEC;
        }
        for (uint16_t i = 0; i < header.phent_count; i++) {
            struct prg_header raw;
            off_t_ at = (off_t_) (header.prghead_off + (qword_t) i * header.phent_size);
            if (exec_read_at(fd, &raw, sizeof(raw), at) != sizeof(raw)) {
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
            off_t_ at = (off_t_) (header.prghead_off + (qword_t) i * header.phent_size);
            if (exec_read_at(fd, &raw, sizeof(raw), at) != sizeof(raw)) {
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

// ---- address space layout randomization --------------------------------------
//
// Every process was laid out the same way every time: the stack, the heap,
// the program, the loader and every library at the same addresses, run after
// run, so an exploit could use them as constants. exec now moves each by a
// random number of pages, as Linux does (arch_mmap_rnd, randomize_stack_top,
// arch_randomize_brk), within what the emulated address space holds:
//
//                    i386          amd64 / arm64       riscv64 (Sv39)
//   mmap base        8 bits        28 bits (1 TiB)     18 bits (1 GiB)
//   PIE base         8 bits        28 bits / mmap      mmap
//   heap start       32 MiB        1 GiB               1 GiB
//   stack top        8 MiB         1 GiB, below 4 GiB  1 GiB, below 4 GiB
//   stack in-page    up to 4 KiB, 16-byte aligned, everywhere
//
// The mmap base moves the loader, the libraries, the vDSO or sigpage, a
// dynamic arm64/riscv64 PIE and every mapping made without an address. The
// 64-bit stack stays below 4 GiB where exec has always put it (see
// guest_abi_vm_layout), so it moves less than Linux's 16 GiB.
//
// Off for a process with ADDR_NO_RANDOMIZE (setarch -R; gdb sets it by
// default), which a set-id exec clears first, and per randomize_va_space.
static _Atomic int randomize_va_space = -1;

int aslr_randomize_va_space(void) {
    int v = atomic_load(&randomize_va_space);
    if (v < 0) {
        const char *env = getenv("ISH_RANDOMIZE_VA_SPACE");
        v = env != NULL && env[0] >= '0' && env[0] <= '2' && env[1] == '\0' ? env[0] - '0' : 2;
        int expected = -1;
        if (!atomic_compare_exchange_strong(&randomize_va_space, &expected, v))
            v = expected;
    }
    return v;
}

void aslr_set_randomize_va_space(int value) {
    atomic_store(&randomize_va_space, value);
}

// A uniformly random page count below 2^bits.
static pages_t aslr_pages(unsigned bits) {
    uint64_t r = 0;
    get_random((char *) &r, sizeof(r));
    return (pages_t) (r & (((uint64_t) 1 << bits) - 1));
}

struct exec_aslr {
    pages_t mmap_shift;   // taken off the mmap ceiling
    pages_t pie_shift;    // added to a fixed PIE base (i386, amd64)
    pages_t brk_shift;    // added to the heap's start
    pages_t brk_room;     // how far that shift can reach, for the headroom
    pages_t stack_shift;  // taken off the stack's top
    unsigned stack_offset; // bytes below that, 16-byte aligned, < PAGE_SIZE
};

static void exec_aslr_plan(struct exec_aslr *a, enum guest_abi abi, dword_t personality) {
    *a = (struct exec_aslr) {};
    int va_space = aslr_randomize_va_space();
    if (va_space < 1 || (personality & ADDR_NO_RANDOMIZE_))
        return;
    bool is_64bit = guest_abi_is_64bit(abi);
    unsigned mmap_bits = !is_64bit ? 8 : abi == GUEST_ABI_RISCV64 ? 18 : 28;
    a->mmap_shift = aslr_pages(mmap_bits);
    a->pie_shift = aslr_pages(mmap_bits);
    a->stack_shift = aslr_pages(is_64bit ? 18 : 11);
    uint32_t off = 0;
    get_random((char *) &off, sizeof(off));
    a->stack_offset = off & (PAGE_SIZE - 1) & ~0xfu;
    if (va_space >= 2) {
        a->brk_room = is_64bit ? (0x40000000 >> PAGE_BITS) : (0x2000000 >> PAGE_BITS);
        a->brk_shift = aslr_pages(is_64bit ? 18 : 13);
        if (a->brk_shift >= a->brk_room)
            a->brk_shift = a->brk_room - 1;
    }
}

// The page a 64-bit guest's signal handler returns through: the kernel's
// rt_sigreturn trampoline, read-only and executable, as Linux keeps one in
// the vDSO (__kernel_rt_sigreturn). A handler returns to it when the program
// gave no SA_RESTORER -- musl on aarch64 never does, and riscv64 has no such
// flag -- and it used to return to a copy written onto the signal stack,
// which cannot run now that the stack is not executable. The instructions
// are the ones libgcc's unwinder looks for to recognise a signal frame.
//
// Recorded in mm->vdso, which a 64-bit guest otherwise leaves 0 and which a
// checkpoint already carries (i386 keeps its real vDSO there, and no 64-bit
// guest is ever handed it in an aux vector). Private anonymous memory, so a
// process that mprotects and rewrites its own copy changes nobody else's.
static int map_sigpage(struct task *task, enum guest_abi abi) {
    page_t page = pt_find_hole(task->mem, 1);
    if (page == BAD_PAGE)
        return _ENOMEM;
    int err = pt_map_nothing(task->mem, page, 1, P_READ | P_EXEC);
    if (err < 0)
        return err;
    struct pt_entry *entry = mem_pt(task->mem, page);
    if (entry == NULL || entry->data == NULL || entry->data->data == NULL)
        return _ENOMEM;
    uint8_t *code = (uint8_t *) entry->data->data + entry->offset;
    static const uint32_t arm64_code[] = {0xd2801168u, 0xd4000001u};  // movz x8, #139; svc #0
    static const uint32_t riscv64_code[] = {0x08b00893u, 0x00000073u}; // li a7, 139; ecall
    static const uint8_t amd64_code[] = {0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, // mov $15, %rax
                                         0x0f, 0x05};                            // syscall
    switch (abi) {
        case GUEST_ABI_ARM64: memcpy(code, arm64_code, sizeof(arm64_code)); break;
        case GUEST_ABI_RISCV64: memcpy(code, riscv64_code, sizeof(riscv64_code)); break;
        case GUEST_ABI_AMD64: memcpy(code, amd64_code, sizeof(amd64_code)); break;
        default: break;
    }
    entry->data->name = "[sigpage]";
    task->mm->vdso = (guest_addr_t) page << PAGE_BITS;
    return 0;
}

// Map a private copy of a vDSO image at [page, page + pages), read and
// executable, and name it [vdso].
//
// A COPY, per process, like the [sigpage]: a debugger planting a breakpoint in
// clock_gettime, or a program that mprotects the page and patches it, changes
// its own clock and nobody else's. The i386 vDSO used to be the one static
// array in kernel/vdso.c mapped straight into every 32-bit process, so any of
// them could mprotect it writable and rewrite the clock_gettime every other
// one ran -- root's included. tests/manual/vdso_clock.c has both routes.
static int map_vdso_copy(struct task *task, const void *image, size_t size,
                         page_t page, pages_t pages) {
    int err = pt_map_nothing(task->mem, page, pages, P_READ | P_EXEC);
    if (err < 0)
        return err;
    struct pt_entry *entry = mem_pt(task->mem, page);
    if (entry == NULL || entry->data == NULL || entry->data->data == NULL)
        return _ENOMEM;
    memcpy((char *) entry->data->data + entry->offset, image, size);
    entry->data->name = "[vdso]";
    return 0;
}

// The vDSO a 64-bit guest's C library reads the clock through, from
// vdso/amd64, arm64 or riscv64/vdso.S (the arm64 one says what it is for).
// Its address lives only in the aux vector, which is where the C library and
// gdb look for it; a checkpoint carries the pages and the aux vector like any
// others.
//
// ISH_VDSO=0 in the host environment leaves it out -- *base stays 0, no
// AT_SYSINFO_EHDR goes in the aux vector, and the C library makes the system
// calls the vDSO replaces -- to A/B a problem against them.
static int map_vdso64(struct task *task, enum guest_abi abi, guest_addr_t *base) {
    *base = 0;
    const char *image, *end;
    switch (abi) {
        case GUEST_ABI_AMD64: image = vdso_amd64_image; end = vdso_amd64_image_end; break;
        case GUEST_ABI_ARM64: image = vdso_arm64_image; end = vdso_arm64_image_end; break;
        case GUEST_ABI_RISCV64: image = vdso_riscv64_image; end = vdso_riscv64_image_end; break;
        default: return 0;
    }
    const char *knob = getenv("ISH_VDSO");
    if (knob != NULL && strcmp(knob, "0") == 0)
        return 0;
    size_t size = (size_t) (end - image);
    pages_t pages = (pages_t) ((size + PAGE_SIZE - 1) >> PAGE_BITS);
    page_t page = pt_find_hole(task->mem, pages);
    if (page == BAD_PAGE)
        return _ENOMEM;
    int err = map_vdso_copy(task, image, size, page, pages);
    if (err < 0)
        return err;
    *base = (guest_addr_t) page << PAGE_BITS;
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
    // Decided by elf_exec before the image is loaded; see READ_IMPLIES_EXEC_.
    if (current->group->personality & READ_IMPLIES_EXEC_)
        flags |= P_EXEC;

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
        // See fd_ops.mmap_prepare: the filesystem fetches what the mapping
        // will need, and ->mmap is entitled to assume it has run. This is the
        // path that loads a program off a FUSE mount, so it is the one that
        // most needs it.
        //
        // The address-space write lock IS held here (elf_exec takes it before
        // calling this), which for any other caller would be the exact thing
        // mmap_prepare exists to avoid. It is harmless here alone: exec_de_thread
        // has already reaped this process's other threads, and the mem being
        // locked is the freshly created one no other thread has ever seen, so
        // the quiesce has nothing to stop.
        if (fd->ops->mmap_prepare != NULL &&
                (err = fd->ops->mmap_prepare(fd, map_file_start,
                        (size_t) fb_pages << PAGE_BITS)) < 0) {
            amd64_trace_exec_loader_failure("segment-mmap-prepare", NULL, abi, &ph, bias, fd, err, NULL);
            return err;
        }
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
            ssize_t got = exec_read_at(fd, buf, copy_len, (off_t_) residual_file_start);
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
    complex_lockt(&pids_lock, 0);
    // Every exec, threaded or not, leaves the process a child that announces
    // its exit with SIGCHLD: Linux's de_thread ends at no_thread_group with
    // "we have changed execution domain" and exit_signal = SIGCHLD. A child
    // cloned with SIGUSR1, or with no exit signal, that then runs a program is
    // an ordinary child to its parent's plain wait. AOK kept the clone's, and
    // for a thread's exec copied the old leader's. Set here, before the leader
    // swap below makes this thread the process, so no wait sees the swap with
    // a thread's exit signal.
    current->exit_signal = SIGCHLD_;
    if (taking_over) {
        inherit_parent = leader->parent;
        if (inherit_parent != NULL)
            task_ref_cnt_mod(inherit_parent, 1);
    }
    unlock(&pids_lock);

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
        // Self-pointing, not NULL -- see the matching comment in
        // task_unlink_locked. list_for_each_entry has no NULL check and seven
        // sites walk alive_pids_list with it.
        list_remove(&own->alive);
        list_init(&own->alive);
    }
    // ...and take the leader's, which is this process's pid. Session and
    // process-group membership hang off struct pid, so they travel with it.
    struct pid *lead_pid = pid_get(leader->pid);
    if (lead_pid != NULL)
        lead_pid->task = current;
    current->pid = leader->pid;
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
        // Into the old leader's place among its siblings, as Linux's de_thread
        // puts it there (list_replace_init): the process is as old as it was,
        // so a wait that reaps its parent's zombies oldest first still finds
        // it where it was. The leader is on that list until just below. When
        // it is not -- the parent captured above has exited, and init takes
        // this one -- at the end, as any child handed on is.
        if (leader->parent == new_parent && !list_empty(&leader->siblings))
            list_add_before(&leader->siblings, &current->siblings);
        else
            list_add_tail(&new_parent->children, &current->siblings);
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

// The credentials the exec in progress on this thread will commit (struct
// exec_setid, below). Planned for the file the caller named, and planned
// again for each interpreter a #! line or binfmt_misc hands the exec to,
// because the one Linux applies is the file it finally loads
// (bprm_creds_from_file on bprm->file). exec_plan_error is that plan's refusal,
// which the loader that takes the file raises before anything is committed.
struct exec_setid;
static _Thread_local struct exec_setid *exec_plan_current;
static _Thread_local int exec_plan_error;
static void exec_setid_plan_file(struct exec_setid *plan, struct fd *fd,
        const struct statbuf *stat);

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
        size_t interp_size = ph[i].filesize;
        if (exec_read_at(fd, interp_name, interp_size, (off_t_) ph[i].offset) != (ssize_t) interp_size)
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

    // The credentials this exec planned may refuse it: the file's effective
    // capability bit asks for one the exec cannot give (EPERM), or its
    // capability attribute is no valid one (EINVAL). Linux's begin_new_exec
    // asks that first, before anything is torn down, so the caller gets the
    // error and carries on.
    if (exec_plan_error < 0) {
        err = exec_plan_error;
        goto out_free_interp;
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

    // Whether the new image may execute its stack, and whether it lives in
    // the pre-NX world where anything readable is executable. PT_GNU_STACK
    // with PF_X asks for an executable stack; an i386 binary with no
    // PT_GNU_STACK at all predates the header and gets READ_IMPLIES_EXEC, as
    // it does on Linux (elf_read_implies_exec); a 64-bit one never does. A
    // set-id exec first drops what its caller could have set to weaken it.
    // Decided here, where the exec can no longer fail back to the caller, and
    // before the address-space lock: group->lock nests outside it.
    bool exec_stack = false, has_gnu_stack = false;
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type == PT_GNU_STACK) {
            has_gnu_stack = true;
            exec_stack = (ph[i].flags & PH_X) != 0;
        }
    }
    lock(&save->group->lock, 0);
    if (save->exec_secure)
        save->group->personality &= ~PER_CLEAR_ON_SETID_;
    if (guest_abi_is_64bit(header.abi))
        save->group->personality &= ~READ_IMPLIES_EXEC_;
    else if (!has_gnu_stack)
        save->group->personality |= READ_IMPLIES_EXEC_;
    if (save->group->personality & READ_IMPLIES_EXEC_)
        exec_stack = true;
    dword_t personality = save->group->personality;
    unlock(&save->group->lock);
    struct exec_aslr aslr;
    exec_aslr_plan(&aslr, header.abi, personality);

    // free the process's memory.
    // from this point on, if any error occurs the process will have to be
    // killed before it even starts. please don't be too sad about it, it's
    // just a process.
    //
    // general_lock protects current->mm. otherwise procfs might read the
    // pointer before it's released and then try to lock it after it's
    // released.
    // The rseq registration belongs to the image being replaced.
    rseq_exec(save);
    lock(&save->general_lock, 0);
    mm_release(save->mm);
    save->abi = header.abi;
    task_set_mm(save, new_mm);
    new_mm = NULL;
    unlock(&save->general_lock);
    write_lock(&save->mem->lock);
    mem_locked = true;
    // The mmap base first: everything placed below without an address -- the
    // loader, a dynamic PIE, the vDSO or sigpage -- is placed from it.
    if (aslr.mmap_shift != 0)
        mem_set_mmap_window(save->mem, save->mem->mmap_floor,
                save->mem->mmap_ceiling - aslr.mmap_shift);

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
                bias = 0x56555000 + ((guest_addr_t) aslr.pie_shift << PAGE_BITS);
            else if (interp_name && header.abi == GUEST_ABI_AMD64)
                // Pin the amd64 PIE main executable at the conventional low
                // Linux base so the brk heap grows up into the large mmap
                // window. find_hole_for_elf() returns the *top* of the window
                // (just under mmap_ceiling), which pins start_brk there and
                // caps the heap at the ~32 MiB gap to the page limit (2^47);
                // brk-hungry programs like git then fail to expand the heap.
                bias = 0x555555554000 + ((guest_addr_t) aslr.pie_shift << PAGE_BITS);
            else {
                // arm64/riscv64 PIE binaries fall through to here
                // intentionally: dynamic placement, not a fixed low bias.
                // See the GUEST_ABI_ARM64 case in guest_abi_vm_layout()
                // (kernel/abi.h) for why — avoids the V8 CodeRange
                // collision that OpenMinis' ish-arm64 fork hit with a
                // fixed low bias.
                // 1 GiB of brk headroom above the image (see the helper).
                brk_headroom_pages = 0x40000000 >> PAGE_BITS;
                // ...plus room for the heap's random start, so the full
                // headroom is still there after it.
                bias = find_hole_for_elf(&header, ph, brk_headroom_pages + aslr.brk_room);
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

    // The heap's start, randomized (randomize_va_space 2). The pages between
    // the image and it are left unmapped, as on Linux.
    if (aslr.brk_shift != 0 && save->mm->start_brk != 0) {
        guest_addr_t shifted = save->mm->start_brk + ((guest_addr_t) aslr.brk_shift << PAGE_BITS);
        if (elf_value_fits_addr(header.abi, shifted) &&
                pt_is_hole(save->mem, PAGE(save->mm->start_brk), aslr.brk_shift))
            save->mm->start_brk = save->mm->brk = shifted;
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
        page_t mmap_ceiling = save->mem->mmap_ceiling;
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
    guest_addr_t vdso64_base = 0;
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
        if ((err = map_vdso_copy(save, vdso_data, sizeof(vdso_data), vdso_page, vdso_pages)) < 0)
            goto beyond_hope;
        save->mm->vdso = vdso_page << PAGE_BITS;
        vdso_entry = save->mm->vdso + ((struct elf_header *) vdso_data)->entry_point;

        page_t vvar_page = pt_find_hole(save->mem, VVAR_PAGES);
        if (vvar_page == BAD_PAGE)
            goto beyond_hope;
        if ((err = pt_map_nothing(save->mem, vvar_page, VVAR_PAGES, 0)) < 0)
            goto beyond_hope;
        mem_pt(save->mem, vvar_page)->data->name = "[vvar]";
    } else {
        if ((err = map_sigpage(save, header.abi)) < 0)
            goto beyond_hope;
        if ((err = map_vdso64(save, header.abi, &vdso64_base)) < 0)
            goto beyond_hope;
    }

    struct guest_vm_layout vm_layout = guest_abi_vm_layout(save->abi);
    // Readable and writable, and executable only when the binary asked for it
    // (exec_stack above): with instruction fetch checked, a stack mapped
    // executable by default would be the one place in memory an overflow could
    // still put code and run it. Growth takes these flags from the page above.
    // The stack's top, randomized (see exec_aslr_plan).
    page_t stack_page = vm_layout.stack_page - aslr.stack_shift;
    if ((err = pt_map_nothing(save->mem, stack_page, 1,
            P_READ | P_WRITE | P_GROWSDOWN | (exec_stack ? P_EXEC : 0))) < 0)
        goto beyond_hope;
    // Record where the stack starts and how far down it may grow. Linux bounds
    // stack expansion at RLIMIT_STACK measured from the stack's top; without
    // this the only thing stopping a runaway recursion is whatever it collides
    // with, which on a 64-bit guest is hundreds of megabytes away. See
    // mem_growsdown_allowed. RLIM_INFINITY is passed through as 0, meaning
    // "no rlimit bound" -- the guard gap still applies.
    rlim_t_ stack_limit = rlimit(RLIMIT_STACK_);
    mem_set_stack_bounds(save->mem, stack_page + 1,
                         stack_limit == RLIM_INFINITY_ ? 0 : (uint64_t) stack_limit);
    // prlimit64 from another process pushes a new limit into this space
    // (rlimit_set), and it may have landed between the read and the store
    // above, which would leave the older value. The new space is already
    // installed as current->mm, so any change made after this re-read is
    // pushed into it by its setter. Reading once more closes the gap.
    rlim_t_ stack_limit_now = rlimit(RLIMIT_STACK_);
    if (stack_limit_now != stack_limit)
        mem_set_stack_bounds(save->mem, 0,
                             stack_limit_now == RLIM_INFINITY_ ? 0 : (uint64_t) stack_limit_now);
    // RLIMIT_MEMLOCK too, which a locked stack may not grow past (struct mem's
    // memlock_limit_pages), read after the space is installed for the same
    // reason as the re-read above.
    rlim_t_ memlock_limit = rlimit(RLIMIT_MEMLOCK_);
    mem_set_memlock_limit(save->mem, (uint64_t) memlock_limit, memlock_limit == RLIM_INFINITY_);
    write_unlock(&save->mem->lock);
    mem_locked = false;

    guest_addr_t sp = vm_layout.stack_pointer - ((guest_addr_t) aslr.stack_shift << PAGE_BITS)
            - aslr.stack_offset;
    sp -= guest_word_size;

    err = _EFAULT;
    guest_addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto beyond_hope;
    // The strings back to back, as Linux's copy_strings() leaves them: the
    // environment's below the file name, the arguments' directly below those,
    // and no list terminator after either. So arg_end == env_start, and each
    // range ends at its last string's NUL -- exactly the bytes
    // /proc/<pid>/cmdline and environ return, and fs/proc/pid.c's setproctitle
    // rule reads on from one range into the other only because they touch.
    // The blocks' own terminators used to be copied and counted, so both files
    // ended in an extra NUL (`xargs -0` saw a trailing empty argument, and an
    // empty environment read back as one NUL instead of nothing).
    guest_addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto beyond_hope;
    save->mm->env_start = sp;
    save->mm->env_end = sp + args_strings_size(envp);
    guest_addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto beyond_hope;
    save->mm->argv_start = sp;
    save->mm->argv_end = sp + args_strings_size(argv);
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
            // Linux's i386 AT_HWCAP is CPUID leaf 1's EDX, and musl's i386
            // fenv code reads it: without the SSE bit, fesetround never
            // wrote MXCSR and fetestexcept never read it, so the SSE2 double
            // arithmetic Alpine's i386 gcc emits ignored the rounding mode
            // and raised no flags anyone could see.
            {AX_HWCAP, cpuid_leaf1_edx_features()},
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
            // First, where Linux's ARCH_DLINFO puts it, and left out entirely
            // when there is no vDSO rather than handed over as 0.
            {AX_SYSINFO_EHDR, vdso64_base},
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
        const struct aux64_ent *aux_from = vdso64_base != 0 ? aux : aux + 1;
        size_t aux_size = sizeof(aux) - (size_t) (aux_from - aux) * sizeof(aux[0]);
        sp -= vector_bytes;
        sp -= aux_size;
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
        if (user_write(p, aux_from, aux_size))
            goto beyond_hope;
        p += aux_size;
        save->mm->auxv_end = p;
    }

    save->mm->stack_start = sp;
    save->cpu.amd64_syscall = (struct amd64_syscall_state) {};
    save->cpu.fcw = 0x37f;
    save->cpu.mxcsr = 0x1f80;

    memset(save->cpu.amd64_regs, 0, sizeof(save->cpu.amd64_regs));
    // Linux's start_thread loads 0 into ES, DS, FS and GS.
    memset(save->cpu.amd64_sreg, 0, sizeof(save->cpu.amd64_sreg));
    // Or, for a 32-bit image, 0x2b into ES, DS and SS and 0 into FS and GS,
    // and flush_thread empties the TLS entries. This clears tls_ptr too,
    // which is amd64's FS base as well as i386's GS base.
    i386_sreg_exec_reset(&save->cpu);
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

// The block's strings, each with its NUL, without the list terminator after
// the last one: args_size() less that one byte.
static inline size_t args_strings_size(struct exec_args args) {
    return args_size(args) - 1;
}

// Copies the strings only (see args_strings_size); the stack layout in
// elf_exec depends on nothing separating one block from the next.
static inline guest_addr_t args_copy(guest_addr_t sp, struct exec_args args) {
    size_t size = args_strings_size(args);
    sp -= size;
    if (size != 0 && user_write(sp, args.args, size))
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

static struct fd *open_exec(struct fd *at, const char *name, int flags, struct statbuf *stat);
// path_inaccessible: `file` names the program through a close-on-exec
// descriptor -- "/dev/fd/<n>" -- so nothing the exec starts could open it,
// and an interpreter handed it would fail. Linux's
// BINPRM_FLAGS_PATH_INACCESSIBLE: a #! script or a binfmt_misc format is
// refused with ENOENT instead, once it is recognised. Only the file the
// caller named can be that; an interpreter is always named by path.
static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp,
        unsigned depth, bool path_inaccessible);
static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp,
        unsigned depth, bool path_inaccessible);

// How many times an exec may be handed on from one file to another before it
// is refused. `depth` is how many such rewrites it took to reach the file being
// loaded, so the top-level file is depth 0, the interpreter a #! line names is
// depth 1, and an interpreter THAT file names is depth 2.
//
// Linux runs the same thing as a loop in exec_binprm() -- "this allows 4 levels
// of binfmt rewrites before failing hard", `if (depth > 5) return -ELOOP;` --
// and the constant and the comparison here are that loop's. Measured on Linux
// 6.12 against a chain of scripts each naming the previous one: five rewrites
// resolve and run, the sixth is ELOOP.
#define EXEC_MAX_DEPTH 5

// Load one file that another file named as its interpreter, with every loader
// in turn. Declared here because both things that can name an interpreter --
// a #! line and a binfmt_misc registration -- are defined below it.
static int exec_interpreter(struct fd *fd, const struct statbuf *stat, const char *file,
        struct exec_args argv, struct exec_args envp, unsigned depth);

// Returned by native_dispatch_exec, and propagated by every loader path that
// can reach it, when the file turned out to be a program compiled into
// iSH-AOK. Distinct from a plain 0 because the two mean opposite things to the
// caller: 0 says an image was loaded and the rest of execve's process-state
// work is still to be done, while this says the exec is already COMMITTED --
// exec_apply_native_process_state has done all of it -- and doing it a second
// time is at best redundant and at worst a ptrace stop the direct native path
// never takes. __do_execve turns it back into 0 for the guest.
#define EXEC_NATIVE_DISPATCHED 1
static int native_dispatch_exec(struct fd *fd, struct exec_args argv, struct exec_args envp);

// binfmt_misc: hand the file to a registered interpreter.
//
// This is what makes /proc/sys/fs/binfmt_misc honest. That directory used to
// present `register` and `status` with nothing behind them, so update-binfmts
// registered a format, was told it worked, and execve never consulted it -- a
// guest looked configured and silently ran nothing. A registration that is
// visible there has to reach here, or the empty directory was the better lie.
//
// Modelled on shebang_exec above, and the argv it builds is Linux's
// (fs/binfmt_misc.c load_misc_binary):
//
//   without P: interpreter, file, original argv[1..]   -- argv[0] is DROPPED
//   with    P: interpreter, file, original argv[0..]   -- argv[0] preserved
static int binfmt_misc_exec(struct fd *fd, const char *file, struct exec_args argv,
                            struct exec_args envp, unsigned depth, bool path_inaccessible) {
    char header[128];
    ssize_t size = exec_read_at(fd, header, sizeof(header), 0);
    if (size < 0)
        return _EIO;

    char interpreter[MAX_PATH];
    bool preserve_argv0 = false;
    if (!binfmt_misc_match(file, header, (size_t) size, interpreter,
                           sizeof(interpreter), &preserve_argv0))
        return _ENOEXEC;
    // load_misc_binary: "Need to be able to load the file after exec".
    if (path_inaccessible)
        return _ENOENT;

    // Everything after argv[0]. With P the original argv[0] is kept as well,
    // so the interpreter can see how the program was invoked.
    struct exec_args argv_rest = {
        .count = argv.count > 0 ? argv.count - 1 : 0,
        .args = argv.count > 0 ? argv.args + strlen(argv.args) + 1 : argv.args,
    };
    const char *argv0 = argv.count > 0 ? argv.args : NULL;
    size_t args_rest_size = args_size(argv_rest);
    size_t interpreter_len = strlen(interpreter);
    size_t file_len = strlen(file);
    size_t argv0_len = (preserve_argv0 && argv0 != NULL) ? strlen(argv0) : 0;

    size_t extra = interpreter_len + 1 + file_len + 1;
    if (preserve_argv0 && argv0 != NULL)
        extra += argv0_len + 1;
    if (args_rest_size + extra >= ARGV_MAX)
        return _E2BIG;

    char *new_argv_buf = malloc(ARGV_MAX);
    if (new_argv_buf == NULL)
        return _ENOMEM;
    struct exec_args new_argv = {.args = new_argv_buf};
    size_t n = 0;
    memcpy(new_argv_buf, interpreter, interpreter_len + 1);
    new_argv.count++;
    n += interpreter_len + 1;
    memcpy(new_argv_buf + n, file, file_len + 1);
    new_argv.count++;
    n += file_len + 1;
    if (preserve_argv0 && argv0 != NULL) {
        memcpy(new_argv_buf + n, argv0, argv0_len + 1);
        new_argv.count++;
        n += argv0_len + 1;
    }
    memcpy(new_argv_buf + n, argv_rest.args, args_rest_size);
    new_argv.count += argv_rest.count;

    // The interpreter is executed, so it faces the same rules as any other
    // program -- execute permission, ordinary file, a mount that allows exec.
    struct statbuf interpreter_stat;
    struct fd *interpreter_fd = open_exec(AT_PWD, interpreter, 0, &interpreter_stat);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return (int) PTR_ERR(interpreter_fd);
    }
    // A registration's interpreter is a program chosen by this exec, so it gets
    // every loader -- native dispatch, ELF, another registration, a #! line --
    // exactly as the one on a #! line does.
    int err = exec_interpreter(interpreter_fd, &interpreter_stat, interpreter, new_argv, envp, depth + 1);
    free(new_argv_buf);
    // Unconditionally, as shebang_exec does with its own: a loader that takes
    // the file retains its own reference for mm->exefile (elf_exec), so the one
    // open_exec handed back is still ours either way. Closing it only on
    // failure leaked a descriptor on every successful binfmt_misc exec.
    fd_close(interpreter_fd);
    return err;
}

// depth is carried rather than used: only binfmt_misc_exec, which can hand the
// exec on to another file, needs it.
static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp,
        unsigned depth, bool path_inaccessible) {
    int err = (int)elf_exec(fd, file, argv, envp);
    if (err != _ENOEXEC)
        return err;
    // A registered binfmt_misc interpreter is consulted only after every
    // built-in format has declined, exactly as Linux orders its binfmt list.
    err = binfmt_misc_exec(fd, file, argv, envp, depth, path_inaccessible);
    if (err != _ENOEXEC)
        return err;
    return _ENOEXEC;
}

// execveat(fd, "", AT_EMPTY_PATH) -- fexecve -- runs the descriptor's own
// file. Linux opens a new description of it by its inode, and so does this
// where it can, by the descriptor's path (generic_reopen_by_path, which checks
// the path still names the file): the loaders keep it as mm->exefile, and
// the caller's description -- its flags, its locks, an O_PATH one that the
// guest cannot read -- is the caller's.
//
// A file no path reaches has nothing to open by: a memfd never had one, and
// an unlinked file's is the name it used to have. Both were ENOENT, where
// Linux runs them -- runc re-executes a sealed memfd copy of itself this way,
// and Python's os.fexecve is this call. And the name an unlinked file had can
// belong to another file by now, which is what this used to run.
//
// So such a file runs from the caller's description itself. The loaders read
// it only at offsets of their own (exec_read_at), so the caller's position is
// where it left it, as it is on Linux. One thing about the sharing shows: a
// flock or OFD lock taken through the caller's description is held for as
// long as the new image runs, where Linux's own description would let it go
// with the caller's last descriptor. An O_PATH description is readable underneath -- the guest
// may not read it, the kernel may -- but a write-only one is not, and that one
// Linux refuses anyway: a file open for writing is never executed, and this
// one is, by this very descriptor.
static struct fd *open_exec_descriptor(struct fd *at, int open_flags) {
    struct fd *fd = generic_reopen_by_path(at, open_flags);
    if (fd != NULL)
        return fd;
    if (at->ops == NULL || at->ops->pread == NULL)
        return ERR_PTR(_ENOENT);
    if ((fd_getflags(at) & O_ACCMODE_) == O_WRONLY_)
        return ERR_PTR(_ETXTBSY);
    return fd_retain(at);
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
//
// `at`, `name` and `flags` are execveat's (AT_EMPTY_PATH_, AT_SYMLINK_NOFOLLOW_),
// as Linux's do_open_execat takes them; execve is AT_PWD and no flags. The
// name is resolved from the descriptor, and with AT_EMPTY_PATH the file is
// the descriptor's own. That used to be done by taking the descriptor's PATH
// and resolving it again from the root, which is not the same file: in a
// chroot the path carries the chroot's prefix, which the root put on a second
// time, and in a mount `umount -l` has detached it is a staging point no walk
// from the root may enter (N_DETACHED_OK, fs/path.h). Both were ENOENT, and
// AT_SYMLINK_NOFOLLOW was dropped on the way.
static struct fd *open_exec(struct fd *at, const char *name, int flags, struct statbuf *stat) {
    int err = generic_statat(at, name, stat, flags);
    if (err < 0)
        return ERR_PTR(err);

    // may_open() refuses a symlink with ELOOP. Only a lookup that did not
    // follow one sees it: a final component under AT_SYMLINK_NOFOLLOW, or an
    // O_PATH|O_NOFOLLOW descriptor of a symlink under AT_EMPTY_PATH.
    if (S_ISLNK(stat->mode))
        return ERR_PTR(_ELOOP);
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
    int open_flags = O_RDONLY_ | O_NOACCESS_CHECK_;
    struct fd *fd;
    if (name[0] == '\0' && at != AT_PWD) {
        fd = open_exec_descriptor(at, open_flags);
    } else {
        if (flags & AT_SYMLINK_NOFOLLOW_)
            open_flags |= O_NOFOLLOW_;
        fd = generic_openat(at, name, open_flags, 0);
    }
    if (IS_ERR(fd))
        return fd;

    // fd->mount_flags, not fd->mount->flags: for a bind, fd->mount is the
    // origin the bind aliases, and noexec on the bind is not noexec on the
    // origin -- reading it off the mount asked about the wrong one, so
    // `mount -o remount,bind,noexec` executed anyway.
    if (fd->mount_flags & MS_NOEXEC_) {
        fd_close(fd);
        return ERR_PTR(_EACCES);
    }

    // MS_NOSUID: the set-id bits on a file here do not apply. Linux drops them
    // in bprm_fill_uid before anything reads them, so the binary still runs --
    // just without the privilege. Stripping them from the stat we return does
    // the same, because every decision downstream (the credential change, and
    // the AT_SECURE/AT_EUID aux vector) is made from these bits and nothing
    // else. Nosuid was recorded and printed and never enforced, which is worse
    // than not supporting it: a caller that mounts untrusted media nosuid,
    // reads /proc/mounts, and sees "nosuid" has been told a thing that is not
    // true about a decision it cannot re-check.
    if (fd->mount_flags & MS_NOSUID_)
        stat->mode &= ~(mode_t_) (S_ISUID | S_ISGID);
    return fd;
}

static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp,
        unsigned depth, bool path_inaccessible) {
    // read the first 128 bytes to get the shebang line out of
    char header[128];
    ssize_t size = exec_read_at(fd, header, sizeof(header) - 1, 0);
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

    // Where load_script asks it: after the #! line has been parsed, before
    // the interpreter is looked at.
    if (path_inaccessible)
        return _ENOENT;

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
    struct fd *interpreter_fd = open_exec(AT_PWD, interpreter, 0, &interpreter_stat);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return (int)PTR_ERR(interpreter_fd);
    }
    // ...and it faces every loader on the same terms, this one included: an
    // interpreter may itself be a #! script, which is how the placeholder at
    // /AOK/native/<name> gets to say out loud that this build does not carry
    // the program (fs/aok.c). new_argv_buf has to outlive the call because
    // new_argv points into it, so a chain holds one ARGV_MAX buffer per level;
    // EXEC_MAX_DEPTH is what bounds that.
    int err = exec_interpreter(interpreter_fd, &interpreter_stat, interpreter, new_argv, envp, depth + 1);
    fd_close(interpreter_fd);
    free(new_argv_buf);
    return err;
}

// One file that another named as its interpreter, offered to every loader in
// the order __do_execve offers them, and refused once the exec has been handed
// on too many times.
//
// The depth test is here, after the caller has opened the file, rather than
// before: Linux opens the interpreter inside the handler that named it and only
// then reaches the loop's `depth > 5` check, so a chain that ends in an
// interpreter that does not exist answers ENOENT rather than ELOOP however deep
// it is. Keeping the order keeps that answer.
static int exec_interpreter(struct fd *fd, const struct statbuf *stat, const char *file,
        struct exec_args argv, struct exec_args envp, unsigned depth) {
    if (depth > EXEC_MAX_DEPTH)
        return _ELOOP;
    // This file is what the exec now loads, so its credentials are the ones
    // it commits -- its set-id bits and capabilities, not the script's.
    if (exec_plan_current != NULL)
        exec_setid_plan_file(exec_plan_current, fd, stat);
    int err = native_dispatch_exec(fd, argv, envp);
    if (err != _ENOEXEC)
        return err;
    err = format_exec(fd, file, argv, envp, depth, false);
    if (err != _ENOEXEC)
        return err;
    return shebang_exec(fd, file, argv, envp, depth, false);
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
        // Its signal, if queued, stays -- pending signals survive an exec --
        // but is no longer the signal of any timer the new image makes here.
        signal_timer_disown(group, i);
        lock(&group->lock, 0);
    }
    unlock(&group->lock);
}

// Everything execve does to the PROCESS once a native program is committed to.
// The image half has no work -- there is no ELF to load -- but a process is
// more than its image, and each of these was missing for native programs until
// something noticed out loud.
//
// new_mm is the address space the program will run in, already built by the
// caller so that an out-of-memory failure can still be reported as one.
static void exec_apply_native_process_state(struct mm *new_mm) {
    // Every other thread of the process dies here, as it does for an ELF exec
    // (exec_de_thread, and Linux's de_thread). Skipping it left siblings
    // running the old image -- which was survivable only for as long as they
    // went on sharing the exec'ing thread's address space. They do not any
    // more: the swap below would leave one thread group straddling two address
    // spaces, which is not a state a thread group can be in.
    exec_de_thread();

    // The new image gets a new address space, exactly as an ELF one does.
    //
    // This was missing entirely, and the effect was that a native program ran
    // in whatever space it inherited: `/AOK/native/zsh` kept /bin/busybox and
    // the musl loader mapped for its whole run, holding the memory of a program
    // that had already been replaced. That is the ordinary read of the bug. The
    // sharper one is vfork: the parent resumes at vfork_notify below, and until
    // the swap it resumed into an address space its child was still writing
    // guest scratch into (kernel/native_syscall.h).
    //
    // Empty rather than absent. A native program is host code, but it reaches
    // the kernel through the same syscalls a guest does, and those take guest
    // addresses -- so it needs somewhere in the guest to marshal through. What
    // it does not need is anything that was there before.
    //
    // This thread's own scratch goes back to the OLD space first, while it is
    // still the space that address means something in.
    native_arena_release();
    rseq_exec(current);
    // general_lock protects current->mm against a concurrent procfs read, the
    // same way elf_exec's swap does.
    lock(&current->general_lock, 0);
    struct mm *old_mm = current->mm;
    task_set_mm(current, new_mm);
    unlock(&current->general_lock);
    if (old_mm != NULL)
        mm_release(old_mm);

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
    current->exec_gen++;
    current->keepcaps = false;
    // A vfork parent is released by its child's exec, not by its exit. Without
    // this it stayed blocked for the native program's whole run -- which is
    // how glibc's posix_spawn waits, so it is not an exotic path.
    vfork_notify(current);
}

// What an exec does to the effective ids, and to the capabilities that go with
// them: decided before the image is loaded, because the aux vector elf_exec
// builds has to describe it, and applied only once the exec can no longer
// fail. Applied any earlier, a FAILED exec would leave the caller holding the
// privilege of a program that never ran.
//
// The capability half is Linux's cap_bprm_creds_from_file, whole:
//
//   pP' = (fP & bounding) | (fI & pI) | pA'     pA' = 0 if the file is set-id
//   pE' = fE ? pP' : pA'                              or has capabilities
//
// where fP, fI and fE are the file's capabilities (security.capability) --
// with root's shortcut on top: an exec that leaves the real or the effective
// uid 0 gets pP' = bounding | pI, and pE' = pP' when the effective one is 0
// (handle_privileged_root). The only exception is a setuid-root file that has
// capabilities of its own run by someone else: it gets only those, as Linux
// does (with its "has both setuid-root and effective capabilities" warning).
// Before file capabilities existed here this was a special case -- the
// setuid-root grant of everything, and a collapse to the ambient set for
// everyone else -- which the formula above reduces to when a file has none.
struct exec_setid {
    uid_t_ euid;
    uid_t_ egid;
    // AT_SECURE: Linux's secureexec. Set when the exec leaves the effective
    // ids other than the real ones (its is_setid) -- whether a set-id bit did
    // that or the process was already running that way -- and, for a caller
    // whose real uid is not root, when the file's effective bit is set or the
    // permitted set comes out wider than the ambient one: the exec gained
    // capabilities.
    //
    // It used to be "the file has a set-id bit". That marked a root running a
    // setuid-root program, or anyone running a setuid program of their own,
    // as gaining privilege they already had. And it marked as ordinary a
    // process whose effective uid was not its real one -- a setuid program's
    // child, a daemon between seteuid calls -- so its loader honoured
    // LD_PRELOAD from an environment its real user controls. Measured on
    // Linux 6.12 (camd, root): after setresuid(-1, 1000, -1), or
    // setresgid(-1, 1234, -1), a plain exec has AT_SECURE 1; a root exec of a
    // setuid-root binary has 0.
    //
    // Decided BEFORE the downgrade below, as Linux decides it, so an exec that
    // was refused its privilege is still a secure one: measured on 6.12, a
    // traced exec of a set-group-ID binary runs with the real gid and
    // AT_SECURE 1, AT_EGID showing the gid it really got. Likewise an exec of
    // a file with capabilities under no_new_privs: nothing is gained, and it
    // is still secure (tests/manual/file_caps_exec.c).
    bool secure;
    // The permitted, effective and ambient sets the new image starts with.
    // The inheritable and bounding sets pass through unchanged.
    dword_t prm[2], eff[2], amb[2];
    // A refusal: the file's effective bit asks for a capability this exec
    // cannot grant -- one outside the bounding set -- which is EPERM rather
    // than a program run without it; or its capability attribute is not a
    // valid one of any revision (EINVAL). Raised by the loader that takes the
    // file (elf_exec), so a #! script's own attribute never refuses anything.
    int error;
};

// Whether this exec must not gain privilege: the conditions Linux's
// check_unsafe_exec records, as cap_bprm_creds_from_file weighs them.
//
// A traced process, unless whoever made the ptrace link could have traced
// anything anyway (ptracer_capable, which asks for CAP_SYS_PTRACE). Otherwise a
// debugger any user can run would be a way to run a setuid-root program with
// its memory and registers in that user's hands: strace or gdb on sudo, by
// its own unprivileged user, ran sudo as root. And no_new_privs, which is what
// the flag is for.
//
// Not modelled: Linux's third condition, a filesystem context shared with a
// process outside this one (clone with CLONE_FS but not CLONE_THREAD).
static bool exec_gain_unsafe(void) {
    if (current->no_new_privs)
        return true;
    lock(&current->ptrace.lock, 0);
    bool unsafe = current->ptrace.traced && !current->ptrace_link_capable;
    unlock(&current->ptrace.lock);
    return unsafe;
}

// Linux's bprm_fill_uid and cap_bprm_creds_from_file, for a file whose set-id
// bits (already stripped for a nosuid mount) are `setuid` with owner `owner`
// and `setgid` with group `group`, and whose capabilities are `caps` -- NULL
// for none. `caps_error` is a refusal from reading them (EINVAL for a
// malformed attribute), which this plan then carries.
static void exec_setid_plan(struct exec_setid *plan, bool setuid, uid_t_ owner,
        bool setgid, uid_t_ group, const struct file_caps *caps, int caps_error) {
    *plan = (struct exec_setid) {.euid = current->euid, .egid = current->egid,
            .error = caps_error};
    // no_new_privs: the set-id bits are not even looked at.
    if (!current->no_new_privs) {
        if (setuid)
            plan->euid = owner;
        if (setgid)
            plan->egid = group;
    }
    // Against the REAL ids: Linux's __is_setuid compares the new effective
    // uid with the old real one.
    bool is_setid = plan->euid != current->uid || plan->egid != current->gid;

    // pP' = (fP & bounding) | (fI & pI), for the file's capabilities.
    dword_t prm[2] = {0, 0};
    bool effective = false;
    if (caps != NULL) {
        effective = caps->effective;
        for (int i = 0; i < 2; i++)
            prm[i] = (caps->permitted[i] & current->cap_bounding[i]) |
                    (caps->inheritable[i] & current->cap_inheritable[i]);
        // A program that says it needs these to run (fE) is refused rather
        // than started without them. Measured on 6.12: CAP_NET_BIND_SERVICE
        // dropped from the bounding set, a +ep file of it fails with EPERM,
        // and a +p one runs without it.
        if (effective && ((caps->permitted[0] & ~prm[0]) || (caps->permitted[1] & ~prm[1])))
            plan->error = _EPERM;
    }

    // Root's capabilities, whatever the file says -- except for a setuid-root
    // file with capabilities of its own, run by someone else, which gets only
    // those (Linux's handle_privileged_root).
    if (!(caps != NULL && current->uid != 0 && plan->euid == 0)) {
        if (plan->euid == 0 || current->uid == 0) {
            prm[0] = current->cap_bounding[0] | current->cap_inheritable[0];
            prm[1] = current->cap_bounding[1] | current->cap_inheritable[1];
        }
        if (plan->euid == 0)
            effective = true;
    }

    // The downgrade, which Linux applies to an exec that would change the
    // effective ids or give capabilities the caller does not have (its
    // __cap_gained). The ids go back to the real ones -- unless the caller
    // could have set any ids it liked anyway (CAP_SETUID), which
    // no_new_privs does not allow for -- and the capabilities stay within what
    // the caller already had.
    //
    // Nothing looked at any of this, so a traced exec took everything the
    // file offered. Measured on Linux 6.12, as uid 1000 with a setgid binary
    // of one of its supplementary groups: untraced it runs with that egid;
    // traced -- PTRACE_TRACEME, a parent's SEIZE or ATTACH, or a fork the
    // tracer followed -- with egid 1000; traced and detached again before the
    // exec, with the group's. CAP_SYS_PTRACE and CAP_SETUID are asked as
    // current_capable asks everything, so root counts as holding both.
    //
    // "Gained" is against the caller's own permitted set: under no_new_privs a
    // caller already holding a file's capabilities keeps them (measured).
    bool gains_caps = (prm[0] & ~current->cap_permitted[0]) != 0 ||
            (prm[1] & ~current->cap_permitted[1]) != 0;
    if ((is_setid || gains_caps) && exec_gain_unsafe()) {
        if (current->no_new_privs || !current_capable(CAP_SETUID_)) {
            plan->euid = current->uid;
            plan->egid = current->gid;
        }
        prm[0] &= current->cap_permitted[0];
        prm[1] &= current->cap_permitted[1];
    }

    // The ambient set survives an ordinary exec -- that is what it is for --
    // but not one that is set-id or brings capabilities of its own.
    for (int i = 0; i < 2; i++) {
        plan->amb[i] = (caps != NULL || is_setid) ? 0 : current->cap_ambient[i];
        plan->prm[i] = prm[i] | plan->amb[i];
        plan->eff[i] = effective ? plan->prm[i] : plan->amb[i];
    }

    plan->secure = is_setid || (current->uid != 0 &&
            (effective || (plan->prm[0] & ~plan->amb[0]) != 0 ||
             (plan->prm[1] & ~plan->amb[1]) != 0));
}

// Hands the planned ids to the aux vector elf_exec is about to build. musl and
// glibc both decide a process is secure-execution from AT_SECURE, and musl
// additionally from AT_UID == AT_EUID && AT_GID == AT_EGID -- all four were
// hardcoded 0, so a setuid-root binary looked like an ordinary one and
// honoured LD_PRELOAD, giving any local user root.
static void exec_setid_stage(const struct exec_setid *plan) {
    current->exec_auxv_uid = current->uid;
    current->exec_auxv_gid = current->gid;
    current->exec_auxv_euid = plan->euid;
    current->exec_auxv_egid = plan->egid;
    current->exec_secure = plan->secure;
    exec_plan_error = plan->error;
}

// Plan for the file an exec is about to load, from its stat and its
// capabilities -- which a nosuid mount's files do not have, as they have no
// set-id bits (open_exec strips those from the stat already).
static void exec_setid_plan_file(struct exec_setid *plan, struct fd *fd,
        const struct statbuf *stat) {
    struct file_caps caps;
    int caps_err = _ENODATA;
    if (!(fd->mount_flags & MS_NOSUID_))
        caps_err = xattr_exec_file_caps(fd, &caps);
    // A set-group-ID bit counts only with group execute beside it: without
    // S_IXGRP it is the old mandatory-locking marker, and Linux's bprm_fill_uid
    // ignores it. Measured on 6.12: a mode-2745 file of another group ran
    // with the caller's egid and AT_SECURE 0; AOK gave it the file's group.
    bool setgid = (stat->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP);
    exec_setid_plan(plan, stat->mode & S_ISUID, stat->uid, setgid, stat->gid,
            caps_err == 0 ? &caps : NULL, caps_err == _ENODATA || caps_err == 0 ? 0 : caps_err);
    exec_setid_stage(plan);
}

// What every exec does to the saved and filesystem ids, set-id or not, once
// the effective ones are settled: they take the effective ones. POSIX's "the
// effective user ID of the new process image shall be saved (as the saved
// set-user-ID)", and Linux's cap_bprm_creds_from_file, "new->suid =
// new->fsuid = new->euid" with the same for the gids.
//
// Only a set-id exec did it here. So a program that lowered its effective uid
// and kept root as its saved one -- a setuid-root program's
// seteuid(getuid()) before it runs a helper -- handed the saved root to what it
// exec'd, which could take it back with setuid(0); and a filesystem id set
// with setfsuid outlived the program that set it. Measured on Linux 6.12: a
// plain exec after setresuid(-1, -1, 1000) leaves the saved uid 0, one after
// setfsuid(1000) leaves the filesystem uid 0, and the same for the gids.
static void exec_reset_saved_ids(void) {
    current->suid = current->fsuid = current->euid;
    current->sgid = current->fsgid = current->egid;
}

// The credential change exec_setid_plan decided on, made once the exec is
// committed.
static void exec_setid_apply(const struct exec_setid *plan) {
    current->euid = plan->euid;
    current->egid = plan->egid;
    for (int i = 0; i < 2; i++) {
        current->cap_permitted[i] = plan->prm[i];
        current->cap_effective[i] = plan->eff[i];
        current->cap_ambient[i] = plan->amb[i];
    }
    exec_reset_saved_ids();
}

// The parent-death signal (PR_SET_PDEATHSIG) does not outlive an exec that
// changes who the process is. Linux forgets it twice over: begin_new_exec for
// a secure exec ("Make sure parent cannot signal privileged process") --
// struct exec_setid's `secure`, handed in here -- and commit_creds for a
// change of the effective or filesystem ids, the reset above included, or
// growth in the permitted capabilities (cred_change_commit). Any other exec
// keeps it. Measured on Linux 6.12: a set-group-ID binary of another group, a
// set-user-ID binary of another user, and a plain exec after setfsuid or
// setresuid(-1, 1000, -1) forget it; a root's exec of a setuid-root binary, a
// set-user-ID binary of the caller's own, and a plain exec after
// setresuid(-1, -1, 1000) keep it. A traced exec of a set-group-ID binary of
// another group forgets it too: it is secure although the downgrade left the
// ids as they were.
static void exec_forget_pdeath(const struct cred_change *before, bool secure) {
    if (secure)
        current->pdeath_signal = 0;
    cred_change_commit(before);
}

// Linux's begin_new_exec: the new image is dumpable -- inspectable by its own
// user through ptrace and /proc (task_ptrace_may_access) -- unless the caller
// was already running with euid != uid or egid != gid, or could not read the
// file it ran (would_dump: an execute-only binary must not be read back out
// of memory). Asked of the credentials before the exec changes them, as Linux
// asks, and followed by cred_change_commit, which makes it undumpable as well
// when the exec changes the effective or filesystem ids -- a setuid program.
static void exec_set_dumpable(bool unreadable) {
    bool undumpable = unreadable ||
            current->euid != current->uid || current->egid != current->gid;
    atomic_store(&current->group->undumpable, undumpable);
}

// Natively-implemented programs (/AOK/native/*, kernel/native.h) are dispatched
// here: after the caller's existence and permission checks, so they behave like
// any other executable, but before any ELF parsing, since there is no guest
// image to load. Keyed off the resolved fd rather than a path, so a symlink
// from anywhere in the guest lands here while argv[0] stays whatever the caller
// passed.
//
// Answers _ENOEXEC for anything that is not a native program this build
// carries, which is the same "not my format" every other loader answers, so a
// caller can try it alongside the rest. It never closes `fd`: the caller keeps
// the reference it opened with, exactly as it does across format_exec.
//
// A caller, not just __do_execve. An interpreter named on a #! line is
// executed, and it is executed by the same rules as anything else -- so this
// has to be asked there too. It was not, and the answer was silent: the file
// served at /AOK/native/<name> is a `#!/bin/sh` placeholder (fs/aok.c), so a
// script saying `#!/AOK/native/bash` reached that placeholder, found no loader
// that would take a shell script as an interpreter, and came back ENOEXEC --
// which every shell answers by re-running the script under /bin/sh. Writing
// `#!/AOK/native/bash` and being given dash is the worst available outcome:
// not the program asked for, not the placeholder's diagnostic, no error at all.
static int native_dispatch_exec(struct fd *fd, struct exec_args argv, struct exec_args envp) {
    const char *native_name = aokfs_native_program_name(fd);
    if (native_name == NULL)
        return _ENOEXEC;
    const struct native_program *prog = native_program_lookup(native_name);
    // No match means this build does not carry that program; the caller falls
    // through and runs the /AOK/native stub, which says so out loud.
    if (prog == NULL)
        return _ENOEXEC;

    char **native_argv = exec_args_to_vector(argv);
    char **native_envp = exec_args_to_vector(envp);
    if (native_argv == NULL || native_envp == NULL) {
        free(native_argv);
        free(native_envp);
        return _ENOMEM;
    }
    // Built here, before anything is committed, for the same reason elf_exec
    // builds its new_mm before exec_de_thread: past the commit point a failure
    // has nowhere to go but a dead process.
    struct mm *native_mm = mm_new(current->abi);
    if (native_mm == NULL) {
        free(native_argv);
        free(native_envp);
        return _ENOMEM;
    }
    // /proc/<pid>/exe should name what was exec'd. Inheriting the mm meant it
    // named the PARENT's binary -- /bin/busybox for anything a shell started --
    // which is worse than either the truth or nothing. For a #! script this is
    // the interpreter, which is what Linux records there too.
    native_mm->exefile = fd_retain(fd);
    // Recorded rather than run here. Running a native program never returns, so
    // doing it at this point would strand every buffer the execve syscall still
    // means to free -- including the argv/envp blocks themselves. Instead take
    // a private copy, report success, and let each entry point run it once its
    // own frees are done (see native_exec_run_pending).
    int perr = native_exec_set_pending(prog, (int) argv.count,
            native_argv, native_envp);
    free(native_argv);
    free(native_envp);
    if (perr < 0) {
        mm_release(native_mm);
        return perr;
    }
    // Only once the record is safely taken: everything below commits the exec,
    // and there is no undoing a closed descriptor.
    exec_apply_native_process_state(native_mm);

    // The setuid transition, on the far side of the commit -- which is the
    // only safe side. Applied any earlier, an exec that then FAILED would
    // leave the task holding root, which is the trap the ELF path below
    // spells out at its own set-id block. This is that same transition: uid 0
    // because /AOK is root-owned, so the bit can only ever mean setuid-root.
    //
    // A native program is NOT reached through elf_exec, so there is no aux
    // vector and AT_SECURE has nobody to tell -- the LD_PRELOAD hole that
    // motivated exec_secure cannot exist for compiled-in host code. What DOES
    // carry over is the caller's environment, and sanitising that is the
    // program's own job (kernel/native.h says so where the flag is declared).
    //
    // Planned as the ELF path plans it, so the same rules refuse the
    // privilege: a native sudo run under a tracer that could not have traced
    // root, or with no_new_privs set, runs as its caller. It was root either
    // way. The grant itself is the ELF path's for setuid-root, and for the
    // same reason: a sudo that means to drop to a target uid needs CAP_SETGID
    // and CAP_SETUID still in hand to do it.
    struct exec_setid setid;
    exec_setid_plan(&setid, prog->setuid_root, 0, false, 0, NULL, 0);
    exec_set_dumpable(false);
    struct cred_change creds;
    cred_change_begin(&creds);
    exec_setid_apply(&setid);
    exec_forget_pdeath(&creds, setid.secure);
    return EXEC_NATIVE_DISPATCHED;
}

// What a traced exec tells its tracer once it has committed: a
// PTRACE_EVENT_EXEC stop if the tracer asked for them, and otherwise, for a
// tracer that did not seize, a SIGTRAP (Linux's ptrace_event). `old_pid` is the
// event's message.
//
// The SIGTRAP is QUEUED, as Linux's send_sig queues it, and not a stop taken
// here: it is delivered on the way out of execve like any other signal, so a
// PTRACE_SYSCALL tracer sees the syscall-exit stop first and the SIGTRAP's
// signal-delivery-stop after it. This used to stop at once, inside the call --
// between execve's entry and exit stops, where no Linux tracer sees it.
// Measured on 6.12: entry, exit, then 0x57f with si_code SI_USER and the
// tracee's own pid, which is what gdb counts one exec by.
//
// A native program is an exec too, and the tracer is told the same thing. It
// was told nothing, so gdb -- which starts a program as `$SHELL -c exec prog`
// and counts one trap per exec -- never saw the shell's when $SHELL was
// /AOK/native/zsh, and gave up with "During startup program exited".
static void exec_report_to_tracer(pid_t_ old_pid) {
    if (!current->ptrace.traced)
        return;
    struct siginfo_ info = {
        .sig = SIGTRAP_,
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    if (current->ptrace.options & PTRACE_O_TRACEEXEC_)
        ptrace_event_stop(SIGTRAP_, &info, PTRACE_EVENT_EXEC_, old_pid);
    else if (!current->ptrace.seized)
        send_signal(current, SIGTRAP_, info);
}

// What an exec runs: the file, found the way open_exec says, and the name the
// new image is given for it -- Linux's bprm->filename, which is AT_EXECFN and
// the name a #! or binfmt_misc interpreter is handed to open.
struct exec_file {
    struct fd *at;          // AT_PWD, or execveat's descriptor
    const char *name;       // as spelled; "" under AT_EMPTY_PATH
    int flags;              // AT_EMPTY_PATH_, AT_SYMLINK_NOFOLLOW_
    const char *filename;
    // The name is made up -- "/dev/fd/<n>" or "/dev/fd/<n>/<name>" -- because
    // the file was named through a descriptor; see sys_execveat.
    bool fdpath;
    // ...and that descriptor is close-on-exec. See format_exec.
    bool path_inaccessible;
};

// execve: a name, resolved from the cwd and root, and the same name given to
// the new image.
static struct exec_file exec_file_named(const char *file) {
    return (struct exec_file) {.at = AT_PWD, .name = file, .filename = file};
}

static int __do_execve(const struct exec_file *exe, struct exec_args argv, struct exec_args envp) {
    const char *file = exe->filename;
    // PTRACE_EVENT_EXEC's message is the pid this task had BEFORE the exec. A
    // thread that is not the leader takes the leader's pid in exec_de_thread,
    // and the tracer needs the old one to tell which of its tasks is gone.
    pid_t_ old_pid = current->pid;

    // An empty argv runs the program with one empty argument, as Linux has
    // since 5.18 (fs/exec.c: "When argv is empty, add an empty string ("") as
    // argv[0] to ensure confused userspace programs that start processing
    // from argv[1] won't end up walking envp"). Measured on 6.12: argc is 1
    // and /proc/<pid>/cmdline is a single NUL. It also keeps every loader
    // below from meeting argv.count == 0 -- the #! path's argv.count - 1
    // would wrap.
    static const char empty_argv[] = {'\0', '\0'};   // "", then the terminator
    if (argv.count == 0)
        argv = (struct exec_args) {.count = 1, .args = empty_argv};

    // open_exec decides what the file IS and whether this caller may execute
    // it before opening it, which is Linux's do_open_execat order. This used
    // to open first and then ask only whether ANY execute bit was set, so a
    // root-owned 0744 binary was executable by every user on the system.
    struct statbuf stat;
    struct fd *fd = open_exec(exe->at, exe->name, exe->flags, &stat);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    int err;

    // comm is the last component of bprm->filename. For a made-up
    // "/dev/fd/<n>" that is a number, which Linux 6.14 stopped using
    // ("exec: fix up /proc/pid/comm in the execveat(AT_EMPTY_PATH) case") in
    // favour of the name of the file itself; so does this.
    char comm_path[MAX_PATH];
    const char *comm_from = file;
    if (exe->fdpath && generic_getpath(fd, comm_path) == 0) {
        // A memfd's path is the way /proc shows it, "/memfd:<name> (deleted)",
        // and the file's own name is "memfd:<name>".
        static const char deleted[] = " (deleted)";
        size_t len = strlen(comm_path);
        if (memfd_fd_is(fd) && len >= sizeof(deleted) - 1 &&
                strcmp(comm_path + len - (sizeof(deleted) - 1), deleted) == 0)
            comm_path[len - (sizeof(deleted) - 1)] = '\0';
        comm_from = comm_path;
    }
    // would_dump(), with the caller's credentials as they are now.
    bool unreadable = access_check(&stat, AC_R) < 0;

    // A native program replaces this image with compiled-in host code, so it
    // is asked about before any loader gets the file. Anything else comes back
    // _ENOEXEC and carries on below.
    err = native_dispatch_exec(fd, argv, envp);
    if (err != _ENOEXEC) {
        fd_close(fd);
        if (err != EXEC_NATIVE_DISPATCHED)
            return err;
        exec_report_to_tracer(old_pid);
        return 0;
    }

    // Decide what the credentials will be once this exec commits, and stage
    // them for the aux vector elf_exec is about to build. The real change
    // stays below, after the image is loaded.
    //
    // For THIS file. If it turns out to be a #! script (or a binfmt_misc
    // format), exec_interpreter plans again for the interpreter, because the
    // credentials Linux applies are the ones of the file it finally loads: a
    // script's own set-id bits and capabilities count for nothing -- we were
    // applying the SCRIPT's bits once, so a root-owned mode-4755 script with a
    // cooperative interpreter handed any local user a root shell -- and its
    // interpreter's count in full (measured: an interpreter with
    // cap_net_bind_service+ep gives it to every script it runs).
    struct exec_setid setid;
    exec_setid_plan_file(&setid, fd, &stat);
    exec_plan_current = &setid;

    err = format_exec(fd, file, argv, envp, 0, exe->path_inaccessible);
    if (err == _ENOEXEC)
        err = shebang_exec(fd, file, argv, envp, 0, exe->path_inaccessible);
    exec_plan_current = NULL;
    exec_plan_error = 0;
    fd_close(fd);
    if (err < 0) {
        amd64_trace_exec_loader_failure("do-execve", file, current->abi, NULL, 0, NULL, err, NULL);
        return err;
    }
    // The interpreter was a native program, so the exec is already committed
    // and everything below has already happened once
    // (exec_apply_native_process_state). Returning here is what the direct
    // native path does a hundred lines up, and this is the same exec -- the
    // tracer's report included.
    if (err == EXEC_NATIVE_DISPATCHED) {
        exec_report_to_tracer(old_pid);
        return 0;
    }

    // The credentials, as planned: the ids, and the capabilities recomputed
    // from the file's own and the ambient set (exec_setid_plan). Nothing
    // survives an ordinary exec but the ambient set -- a process that had
    // lowered its uid while holding capabilities, which is exactly what
    // prctl(PR_SET_KEEPCAPS) plus setresuid is for, does not hand them to
    // whatever it runs next. Measured on 6.12: root exec'ing a set-user-ID
    // binary of uid 1000, traced or not, ran with CapEff 0 and a full CapPrm.
    exec_set_dumpable(unreadable);
    struct cred_change creds;
    cred_change_begin(&creds);
    exec_setid_apply(&setid);
    exec_forget_pdeath(&creds, setid.secure);

    // save current->comm
    char old_comm[sizeof(current->comm)];
    lock(&current->general_lock, 0);
    strncpy(old_comm, current->comm, sizeof(old_comm));
    old_comm[sizeof(old_comm) - 1] = '\0';
    const char *basename = strrchr(comm_from, '/');
    if (basename == NULL)
        basename = comm_from;
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
    current->exec_gen++;
    current->keepcaps = false;
    vfork_notify(current);

    if (current->ptrace.traced) {
        current->ptrace.syscall = current->cpu.eax;
        current->cpu.eax = 0;
        // Without PTRACE_O_TRACEEXEC, the legacy post-exec SIGTRAP goes only
        // to a tracee that was not seized (Linux's ptrace_event). A seized one
        // got it too, and a tracer that injects what it does not expect
        // killed the program it had just spawned.
        exec_report_to_tracer(old_pid);
    }

    return 0;
}

// Some getty/inittab setups hard-code TERM=vt102 on the boot console and apply it
// with setenv() before exec'ing login, so it can't be corrected through the
// environment we hand the boot command — only here, at exec time. vt102 advertises
// no color at all, so rewrite that single bogus value to screen-256color, matching
// the TERM the app hands its interactive sessions (TerminalViewController.m). Returns
// a malloc'd replacement buffer (caller frees) or NULL when no rewrite is needed,
// keeping the common path allocation-free.
//
// The block is walked by envp.count. It cannot be walked by scanning for the
// terminator: an empty string is a legal entry and looks exactly like the end
// of the list, so a scan stopped at the first one.
static char *exec_fixup_term(struct exec_args envp) {
    const char bogus[] = "TERM=vt102";
    const char fixed[] = "TERM=screen-256color";
    const char *match = NULL;
    const char *e = envp.args;
    for (size_t i = 0; i < envp.count; i++, e += strlen(e) + 1) {
        if (strncmp(e, "TERM=", 5) == 0) {
            // Only the first TERM entry takes effect; stop at it whatever its value.
            if (strcmp(e, bogus) == 0)
                match = e;
            break;
        }
    }
    if (match == NULL)
        return NULL;

    // Every byte of the block, the trailing terminator included.
    size_t envp_len = args_size(envp);
    char *buf = malloc(envp_len + (sizeof(fixed) - sizeof(bogus)));
    if (buf == NULL)
        return NULL; // out of memory: leave the env unchanged rather than fail exec
    size_t prefix = (size_t) (match - envp.args);
    const char *rest = match + sizeof(bogus); // next entry (sizeof includes the NUL)
    size_t rest_len = envp_len - (size_t) (rest - envp.args);
    char *w = buf;
    memcpy(w, envp.args, prefix); w += prefix;
    memcpy(w, fixed, sizeof(fixed)); w += sizeof(fixed);
    memcpy(w, rest, rest_len);
    return buf;
}

// Every exec funnels through here with BOTH counts already known. The packed
// block format ("s1\0s2\0...\0\0") cannot say how many strings it holds when one
// of them is empty, so the count travels beside it -- which is why argc has
// always been a parameter, and why envc has to be one too.
static int do_execve_args(const struct exec_file *exe, struct exec_args argv, struct exec_args envp) {
    char *fixed_env = exec_fixup_term(envp);
    if (fixed_env != NULL)
        envp.args = fixed_env;
    int err = __do_execve(exe, argv, envp);
    free(fixed_env); // NULL-safe: no-op when no rewrite happened
    return err;
}

// For the host's own callers -- app/*.m, kernel/init.c, kernel/native_io.c --
// whose blocks hold no empty string, so recounting one by scanning is safe
// here. A guest can pass one, which is why the execve syscalls below carry
// envc rather than coming through this. native_io.c is the one to watch: it
// packs a char *const[] a native program handed it, so if a native program
// ever passes an empty variable it belongs on the counted path instead --
// native_pack_args already computes the count it would need.
int do_execve(const char *file, size_t argc, const char *argv_p, const char *envp_p) {
    struct exec_args envp = {.args = envp_p};
    for (const char *e = envp_p; *e != '\0'; e += strlen(e) + 1)
        envp.count++;
    struct exec_file exe = exec_file_named(file);
    return do_execve_args(&exe, (struct exec_args) {.count = argc, .args = argv_p}, envp);
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
    ssize_t envc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envc, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    for (ssize_t i = 0; i < argc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("}, {");
    args = envp;
    for (ssize_t i = 0; i < envc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    struct exec_file exe = exec_file_named(filename);
    err = do_execve_args(&exe, (struct exec_args) {.count = (size_t) argc, .args = argv},
            (struct exec_args) {.count = (size_t) envc, .args = envp});

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
    ssize_t envc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envc, &envp);
    if (err < 0)
        return err;

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    for (ssize_t i = 0; i < argc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("}, {");
    args = envp;
    for (ssize_t i = 0; i < envc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("})");

    amd64_trace_exec_attempt(filename, argv);
    struct exec_file exe = exec_file_named(filename);
    err = do_execve_args(&exe, (struct exec_args) {.count = (size_t) argc, .args = argv},
            (struct exec_args) {.count = (size_t) envc, .args = envp});

    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

// execveat's file, and its name for it, as Linux's alloc_bprm chooses: the
// name itself for AT_FDCWD or an absolute name, and otherwise "/dev/fd/<n>"
// or "/dev/fd/<n>/<name>". That made-up name is what AT_EXECFN says and what
// a #! interpreter is handed to open, and it reaches the file for as long as
// the descriptor is open -- a path of the descriptor would not, from a
// chroot, where it carries the chroot's prefix, or from anywhere when the
// file is in a mount `umount -l` detached, where it is a staging point no
// process may walk into or be shown. A close-on-exec descriptor is gone by
// the time an interpreter would open it: path_inaccessible.
//
// Holds a reference to the descriptor, dropped by exec_file_release, so a
// sibling thread's close cannot free it under the exec.
static int exec_file_at(struct exec_file *exe, fd_t dirfd, const char *name, int flags,
        char *fdpath, size_t fdpath_size) {
    // getname_flags(): an empty name is ENOENT unless AT_EMPTY_PATH says it
    // means the descriptor.
    if (name[0] == '\0' && !(flags & AT_EMPTY_PATH_))
        return _ENOENT;
    *exe = (struct exec_file) {.at = AT_PWD, .name = name, .flags = flags, .filename = name};
    if (dirfd == AT_FDCWD_ || name[0] == '/')
        return 0;
    struct fdtable *table = current->files;
    lock(&table->lock, 0);
    struct fd *at = fdtable_get(table, dirfd);
    if (at != NULL) {
        fd_retain(at);
        exe->path_inaccessible = bit_test(dirfd, table->cloexec);
    }
    unlock(&table->lock);
    if (at == NULL)
        return _EBADF;
    exe->at = at;
    exe->fdpath = true;
    if (name[0] == '\0')
        snprintf(fdpath, fdpath_size, "/dev/fd/%d", dirfd);
    else
        snprintf(fdpath, fdpath_size, "/dev/fd/%d/%s", dirfd, name);
    exe->filename = fdpath;
    return 0;
}

static void exec_file_release(struct exec_file *exe) {
    if (exe->at != AT_PWD)
        fd_close(exe->at);
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
    ssize_t envc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envc, &envp);
    if (err < 0)
        return err;

    char fdpath[MAX_PATH + 32];
    struct exec_file exe;
    err = exec_file_at(&exe, dirfd, filename, flags, fdpath, sizeof(fdpath));
    if (err < 0)
        goto out_free_args;

    STRACE("execveat(%d, \"%s\", ..., %#x)", dirfd, filename, flags);
    amd64_trace_exec_attempt(exe.filename, argv);
    err = do_execve_args(&exe, (struct exec_args) {.count = (size_t) argc, .args = argv},
            (struct exec_args) {.count = (size_t) envc, .args = envp});
    exec_file_release(&exe);

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
    ssize_t envc;
    char *argv = NULL;
    char *envp = NULL;
    ssize_t err = read_execve_user_args(argv_addr, envp_addr, &argc, &argv, &envc, &envp);
    if (err < 0)
        return err;

    char fdpath[MAX_PATH + 32];
    struct exec_file exe;
    err = exec_file_at(&exe, dirfd, filename, flags, fdpath, sizeof(fdpath));
    if (err < 0)
        goto out_free_args;

    STRACE("execveat(%d, \"%.1000s\", {", dirfd, filename);
    const char *args = argv;
    for (ssize_t i = 0; i < argc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("}, {");
    args = envp;
    for (ssize_t i = 0; i < envc; i++, args += strlen(args) + 1)
        STRACE("\"%.1000s\", ", args);
    STRACE("}, %d)", flags);

    amd64_trace_exec_attempt(exe.filename, argv);
    err = do_execve_args(&exe, (struct exec_args) {.count = (size_t) argc, .args = argv},
            (struct exec_args) {.count = (size_t) envc, .args = envp});
    exec_file_release(&exe);

out_free_args:
    free(envp);
    free(argv);
    // After the frees: a native program recorded by __do_execve runs here and
    // does not return (kernel/native.h).
    native_exec_run_pending();
    return err;
}

static ssize_t read_execve_user_args(guest_addr_t argv_addr, guest_addr_t envp_addr, ssize_t *argc_out,
        char **argv_out, ssize_t *envc_out, char **envp_out) {
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
    ssize_t envc = 0;
    if (envp_addr != 0) {
        envc = user_read_string_array(envp_addr, envp, ARGV_MAX);
        if (envc < 0) {
            free(envp);
            free(argv);
            return envc;
        }
    } else {
        // Do not take advantage of this nonstandard and nonportable misfeature!
        // - Michael Kerrisk, execve(2)
        envp[0] = envp[1] = '\0';
    }

    *argc_out = argc;
    *argv_out = argv;
    *envc_out = envc;
    *envp_out = envp;
    return 0;
}
