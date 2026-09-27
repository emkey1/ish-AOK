#ifndef KERNEL_ABI_H
#define KERNEL_ABI_H

#include "misc.h"
#include "emu/mmu.h"
#include "kernel/abi/i386.h"
#include "kernel/abi/amd64.h"
#include "kernel/abi/arm64.h"
#include "kernel/abi/riscv64.h"

enum guest_abi {
    GUEST_ABI_I386 = 0,
    GUEST_ABI_AMD64 = 1,
    GUEST_ABI_ARM64 = 2,
    GUEST_ABI_RISCV64 = 3,
};

struct guest_abi_desc {
    enum guest_abi abi;
    const char *name;
    const char *uname_machine;
    const char *elf_platform;
    size_t pointer_size;
    size_t word_size;
    size_t reg_size;
};

static inline struct guest_abi_desc guest_abi_desc(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return (struct guest_abi_desc) {
            .abi = abi,
            .name = "amd64",
            .uname_machine = "x86_64",
            .elf_platform = "x86_64",
            .pointer_size = sizeof(amd64_guest_addr_t),
            .word_size = sizeof(amd64_guest_word_t),
            .reg_size = sizeof(amd64_guest_reg_t),
        };
    case GUEST_ABI_ARM64:
        return (struct guest_abi_desc) {
            .abi = abi,
            .name = "arm64",
            .uname_machine = "aarch64",
            .elf_platform = "aarch64",
            .pointer_size = sizeof(arm64_guest_addr_t),
            .word_size = sizeof(arm64_guest_word_t),
            .reg_size = sizeof(arm64_guest_reg_t),
        };
    case GUEST_ABI_RISCV64:
        return (struct guest_abi_desc) {
            .abi = abi,
            .name = "riscv64",
            .uname_machine = "riscv64",
            .elf_platform = "riscv64",
            .pointer_size = sizeof(riscv64_guest_addr_t),
            .word_size = sizeof(riscv64_guest_word_t),
            .reg_size = sizeof(riscv64_guest_reg_t),
        };
    case GUEST_ABI_I386:
    default:
        return (struct guest_abi_desc) {
            .abi = GUEST_ABI_I386,
            .name = "i386",
            .uname_machine = "i686",
            .elf_platform = "i686",
            .pointer_size = sizeof(i386_guest_addr_t),
            .word_size = sizeof(i386_guest_word_t),
            .reg_size = sizeof(i386_guest_reg_t),
        };
    }
}

struct guest_vm_layout {
    enum guest_abi abi;
    page_t page_limit;
    page_t mmap_floor;
    page_t mmap_ceiling;
    qword_t user_addr_max;
    page_t stack_page;
    guest_addr_t stack_pointer;
};

// Scalar accessor for the one field the hot address-validation path needs.
// guest_abi_vm_layout() below returns a ~40-byte struct by value, which the
// compiler will not fold away — materializing the whole thing just to read
// user_addr_max showed up as a large slice of the amd64 interpreter's time, since
// guest_abi_range_valid() runs on every instruction fetch and memory access.
// This is the single source of truth for the value (the struct uses it too).
static inline qword_t guest_abi_user_addr_max(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return (qword_t) 1 << 47;
    case GUEST_ABI_ARM64:
        return (qword_t) 1 << 48;
    case GUEST_ABI_RISCV64:
        // Sv39, the Linux riscv64 default (Alpine/Debian userland is built
        // for Sv39-sized VA). Revisit (Sv48 = 1<<47) only if a workload
        // needs more; see docs/historical/riscv64_guest_plan.md.
        return (qword_t) 1 << 38;
    case GUEST_ABI_I386:
    default:
        return (qword_t) 1 << 32;
    }
}

static inline struct guest_vm_layout guest_abi_vm_layout(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return (struct guest_vm_layout) {
            .abi = abi,
            // 128 TiB canonical user range for 4 KiB guest pages.
            .page_limit = (page_t) 1 << 35,
            // Let amd64 mappings use the full internal 47-bit guest window.
            // Keep the initial exec stack low for now because most syscall
            // pointer marshalling is still 32-bit.
            .mmap_floor = (page_t) 0x1000,
            .mmap_ceiling = ((page_t) 1 << 35) - 0x2000,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffe,
            .stack_pointer = 0xfffff000u,
        };
    case GUEST_ABI_ARM64:
        return (struct guest_vm_layout) {
            .abi = abi,
            // 256 TiB canonical user range (48-bit VA) for 4 KiB guest pages.
            .page_limit = (page_t) 1 << 36,
            // REVERSED from this field's original patch-2 value (top of the
            // 48-bit space) once patch 4 (syscall table) made the conflict
            // concrete: kernel/calls.c's syscall_t is `dword_t(*)(dword_t x5)`
            // — a 64-bit guest pointer argument gets silently truncated to 32
            // bits unless that specific syscall number has a hand-written
            // qword_t-safe dispatch case (the amd64 path builds these
            // one-by-one; see handle_syscall_interrupt's special-casing
            // around sys_open_guest/sys_write_guest/etc). amd64's own
            // mmap_floor/stack comment already states this exact rule ("keep
            // the initial exec stack low... because most syscall pointer
            // marshalling is still 32-bit") — arm64 needs the same discipline
            // for the same reason, not an exemption from it. The V8
            // CodeRange-collision rationale from patch 2 is deferred along
            // with V8/Node support generally (see aarch64_guest_plan.md's
            // Non-Goals) until the qword_t-safe dispatch exists for whatever
            // syscalls a real V8 needs; PIE binaries already get dynamic
            // placement via find_hole_for_elf() regardless of this bias
            // (kernel/exec.c), so this doesn't reopen that collision.
            .mmap_floor = (page_t) 0x1000,
            // Same mmap window scale as amd64 (128 TiB), not i386's much
            // smaller one — arm64 is a 64-bit ABI, it's only the *stack*
            // placement that's deliberately kept low here, matching amd64's
            // own rationale, not the whole address space.
            .mmap_ceiling = ((page_t) 1 << 35) - 0x2000,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffe,
            .stack_pointer = 0xfffff000u,
        };
    case GUEST_ABI_RISCV64:
        return (struct guest_vm_layout) {
            .abi = abi,
            // 256 GiB user range (Sv39, 38-bit user VA) in 4 KiB guest pages.
            .page_limit = (page_t) 1 << 26,
            // Same discipline as amd64/arm64 above: full mmap window inside
            // the ABI's VA limit, but the initial exec stack kept low
            // because most syscall pointer marshalling is still 32-bit.
            .mmap_floor = (page_t) 0x1000,
            .mmap_ceiling = ((page_t) 1 << 26) - 0x2000,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffe,
            .stack_pointer = 0xfffff000u,
        };
    case GUEST_ABI_I386:
    default:
        return (struct guest_vm_layout) {
            .abi = GUEST_ABI_I386,
            .page_limit = (page_t) 1 << 20,
            .mmap_floor = (page_t) 0x40000,
            .mmap_ceiling = (page_t) 0xf7ffe,
            .user_addr_max = guest_abi_user_addr_max(abi),
            .stack_page = (page_t) 0xffffd,
            .stack_pointer = 0xffffe000u,
        };
    }
}

static inline const char *guest_abi_name(enum guest_abi abi) {
    return guest_abi_desc(abi).name;
}

static inline bool guest_abi_is_64bit(enum guest_abi abi) {
    return guest_abi_desc(abi).pointer_size == sizeof(amd64_guest_addr_t);
}

static inline bool guest_abi_addr_valid(enum guest_abi abi, qword_t addr) {
    return addr < guest_abi_user_addr_max(abi);
}

static inline bool guest_abi_range_valid(enum guest_abi abi, qword_t addr, qword_t size) {
    qword_t max = guest_abi_user_addr_max(abi);
    if (addr >= max)
        return false;
    if (size == 0)
        return true;
    return size <= max - addr;
}

#endif
