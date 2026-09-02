#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu/cpuid.h"
#include "emu/cpu.h"
#include "emu/fpu.h"
#include "emu/fxsave.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "emu/avx.h"
#include "emu/vec.h"
#include "emu/interrupt.h"
#include "emu/modrm.h"
#include "kernel/task.h"
#include "kernel/elf.h"
#include "util/sync.h"

struct amd64_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_modrm {
    bool is_reg;
    bool rex_present;
    uint8_t reg;
    uint8_t rm;
    bool has_base;
    uint8_t base;
    bool has_index;
    uint8_t index;
    uint8_t scale;
    bool rip_relative;
    int32_t disp;
};

static struct tlb *volatile amd64_jit_bridge_tlb;

static inline bool amd64_ignored_segment_prefix(byte_t byte) {
    return byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e;
}

struct fpu_env32 {
    uint32_t control;
    uint32_t status;
    uint32_t tag;
    uint32_t ip;
    uint32_t ip_selector;
    uint32_t operand;
    uint32_t operand_selector;
};

struct fpu_state32 {
    struct fpu_env32 env;
    uint8_t regs[8][10];
};

// The FXSAVE area layout and its cpu_state conversions are shared with the
// i386 engine; see emu/fxsave.h. Long mode sees sixteen XMM registers.
#define AMD64_FXSAVE_XMM_COUNT 16

#define AMD64_BUSYBOX_INIT_SLOT 0x5661a6d8ull
#define AMD64_BUSYBOX_INIT_SLOT_SIZE 8
#define AMD64_BUSYBOX_INIT_LOAD_RIP 0x565e39fbull
#define AMD64_BUSYBOX_INIT_TEST_RIP 0x565e3a02ull
#define AMD64_BUSYBOX_INIT_JNE_RIP 0x565e3a05ull
#define AMD64_BUSYBOX_INIT_CMP_RIP 0x565e3a0bull
#define AMD64_BUSYBOX_INIT_CORRUPT_WRITE_RIP 0xfff929caull
#define AMD64_HTOP_RBX_LOAD_RIP 0x5656370eull
#define AMD64_HTOP_R13_CORRUPT_WRITE_RIP 0x5656375full
#define AMD64_HTOP_TRACE_WINDOW_START 0x56563700ull
#define AMD64_HTOP_TRACE_WINDOW_END 0x56563780ull
#define AMD64_HTOP_RBX_FIELD_OFFSET 0x170ull
#define AMD64_HTOP_RBX_FIELD_SIZE 8
#define AMD64_HTOP_RBX_FIELD_ABS_ADDR 0xf7f019e0ull
#define AMD64_HTOP_FIELD_FILL_RIP 0x56569e88ull
#define AMD64_HTOP_R13_CORRUPT_BLOCK_BASE 0x56587de0ull
#define AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE 32
#define AMD64_CARGO_R12_FAULT_RIP 0xf7f92174ull
#define AMD64_CARGO_ENTRY_RIP 0xf7f920d0ull
#define AMD64_CARGO_START_CALL_RIP 0xf7fbfd85ull
#define AMD64_CARGO_PFWIN_WINDOW_START 0xf7f920e0ull
#define AMD64_CARGO_PFWIN_WINDOW_END 0xf7f92190ull
#define AMD64_CARGO_R12_TRACE_WINDOW_START 0xf7f92000ull
#define AMD64_CARGO_R12_TRACE_WINDOW_END 0xf7f92220ull
#define AMD64_BUSYBOX_INIT_WATCH_COUNT 32
#define AMD64_BUSYBOX_INIT_WATCH_SPAN 16

static qword_t amd64_busybox_init_watch[AMD64_BUSYBOX_INIT_WATCH_COUNT];
static unsigned amd64_busybox_init_watch_next;
static qword_t amd64_htop_watch_field_addr = AMD64_HTOP_RBX_FIELD_ABS_ADDR;
static const bool amd64_htop_legacy_trace_enabled = false;
static const bool amd64_cargo_trace_enabled = false;
static unsigned amd64_cargo_r12_trace_count;
static unsigned amd64_cargo_rdx_trace_count;
static unsigned amd64_cargo_rdi_trace_count;
static unsigned amd64_cargo_xfer_trace_count;
static unsigned amd64_cargo_start_call_trace_count;
static unsigned amd64_cc1_slot_probe_count;
static unsigned amd64_cc1_cmp_probe_count;
static unsigned amd64_cc1_je_probe_count;
static unsigned amd64_cc1_va_list_branch_probe_count;
static unsigned amd64_cc1_slot_write_probe_count;
static unsigned amd64_cc1_va_list_init_probe_count;
static unsigned amd64_cc1_xfer_probe_count;
static unsigned amd64_bash_cond_probe_count;

#define AMD64_CC1_NULL_SLOT_ADDR 0x2e416f0ull
#define AMD64_CC1_CMP_GLOBAL_ADDR 0x2e403a0ull
#define AMD64_CC1_SYSV_SLOT_ADDR 0x2e416f8ull
#define AMD64_CC1_ABI_FLAG_ADDR 0x2e6cbf0ull
#define AMD64_CC1_MS_VARIANT_ADDR 0x2e6cbccull
#define AMD64_CC1_VA_LIST_HOOK_RIP 0x11b1610ull
#define AMD64_CC1_VA_LIST_HOOK_JNE_RIP 0x11b1617ull
#define AMD64_CC1_VA_LIST_HOOK_FALLBACK_JMP_RIP 0x11b1620ull
#define AMD64_CC1_VA_LIST_COMPLEX_RIP 0x11b1628ull
#define AMD64_CC1_VA_LIST_GETTER_RIP 0x11b10d0ull
#define AMD64_CC1_VA_LIST_INIT_ENTRY_RIP 0x11b1704ull
#define AMD64_CC1_VA_LIST_INIT_SYSV_STORE_RIP 0x11b17dbull
#define AMD64_CC1_VA_LIST_INIT_ATTR_CALL_RIP 0x11b1817ull
#define AMD64_CC1_VA_LIST_INIT_ATTR_RET_RIP 0x11b181cull
#define AMD64_CC1_VA_LIST_INIT_MS_STORE_RIP 0x11b1823ull

#define AMD64_BASH_COND_UNEXP_RIP 0x20f50ull
#define AMD64_BASH_COND_BINOP_RIP 0x23967ull
#define AMD64_BASH_SYNTAXTAB_ADDR 0xbf540ull
#define AMD64_BASH_TOKEN_A_ADDR 0xc6bccull
#define AMD64_BASH_TOKEN_B_ADDR 0xc6bd0ull
#define AMD64_BASH_LINE_NUMBER_ADDR 0xc6ab4ull
#define AMD64_BASH_PARSER_STATE_ADDR 0xc6b2cull
#define AMD64_BASH_EXTENDED_GLOB_ADDR 0xcea9cull

static inline bool amd64_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out);
static inline bool amd64_mem_read(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, void *out, unsigned size);

static bool amd64_trace_undefined_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_UNDEFINED") != NULL ? 1 : 0;
    return enabled;
}

#define AMD64_CC1_TRACE_COUNT 64
struct amd64_cc1_trace {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r8;
    qword_t r9;
    qword_t r12;
    uint8_t bytes[8];
    uint8_t byte_count;
};

static struct amd64_cc1_trace amd64_cc1_trace[AMD64_CC1_TRACE_COUNT];
static unsigned amd64_cc1_trace_next;
static pid_t_ amd64_cc1_trace_pid;

#define AMD64_AS_TRACE_COUNT 16384
struct amd64_as_trace {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r8;
    qword_t r9;
    qword_t r12;
    uint8_t bytes[8];
    uint8_t byte_count;
};

static struct amd64_as_trace amd64_as_trace[AMD64_AS_TRACE_COUNT];
static unsigned amd64_as_trace_next;
static pid_t_ amd64_as_trace_pid;

#define AMD64_AS_EVENT_COUNT 128
#define AMD64_AS_RESET_DONE_RIP 0x7ffffdf67be1ull
#define AMD64_AS_STATE31_WRITE1_DONE_RIP 0x7ffffdf6bbbfull
#define AMD64_AS_STATE31_WRITE2_DONE_RIP 0x7ffffdf76c74ull
#define AMD64_AS_STATE31_CHECK_DONE_RIP 0x7ffffdf6f25bull
#define AMD64_AS_STATE31_ADDR 0x7ffffdfff8b1ull

enum amd64_as_event_kind {
    amd64_as_event_reset_done = 1,
    amd64_as_event_state31_write,
    amd64_as_event_state31_check,
};

struct amd64_as_event {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t r12;
    uint8_t state31;
    uint8_t kind;
};

static struct amd64_as_event amd64_as_events[AMD64_AS_EVENT_COUNT];
static unsigned amd64_as_event_next;
static pid_t_ amd64_as_event_pid;

enum amd64_as_suspect_kind {
    amd64_as_suspect_bt = 1,
    amd64_as_suspect_stack,
};

enum amd64_as_stack_op {
    amd64_as_stack_push = 1,
    amd64_as_stack_pop,
    amd64_as_stack_leave,
};

#define AMD64_AS_SUSPECT_COUNT 256
struct amd64_as_suspect {
    qword_t rip;
    qword_t lhs;
    qword_t value;
    qword_t addr;
    qword_t bit_index;
    qword_t bit;
    qword_t old_rsp;
    qword_t new_rsp;
    uint8_t kind;
    uint8_t op;
    uint8_t size;
    uint8_t aux;
};

static struct amd64_as_suspect amd64_as_suspects[AMD64_AS_SUSPECT_COUNT];
static unsigned amd64_as_suspect_next;
static pid_t_ amd64_as_suspect_pid;

#define AMD64_AS_FOCUS_COUNT 128
struct amd64_as_focus {
    qword_t rip;
    qword_t rax;
    qword_t rbx;
    qword_t rcx;
    qword_t rdx;
    qword_t rsi;
    qword_t rdi;
    qword_t rsp;
    qword_t rbp;
    qword_t r12;
};

static struct amd64_as_focus amd64_as_focus[AMD64_AS_FOCUS_COUNT];
static unsigned amd64_as_focus_next;
static pid_t_ amd64_as_focus_pid;
static pid_t_ amd64_as_template_probe_pid;
static qword_t amd64_as_template_probe_entry;
static qword_t amd64_as_template_probe_cmp;

enum amd64_as_state_region_kind {
    amd64_as_state_region_block = 1,
    amd64_as_state_region_desc,
};

#define AMD64_AS_STATE_WRITE_COUNT 256
struct amd64_as_state_write {
    qword_t rip;
    qword_t addr;
    qword_t value;
    qword_t region_base;
    qword_t snapshot_addr;
    uint8_t size;
    uint8_t region_kind;
    uint8_t region_index;
    uint8_t byte_count;
    uint8_t bytes[16];
};

static struct amd64_as_state_write amd64_as_state_writes[AMD64_AS_STATE_WRITE_COUNT];
static unsigned amd64_as_state_write_next;
static pid_t_ amd64_as_state_write_pid;

#define AMD64_AS_FOCUS_DISPATCH_RIP 0x7ffffdee87f8ull
#define AMD64_AS_FOCUS_CASE0_RIP 0x7ffffdee88ffull
#define AMD64_AS_FOCUS_SOURCE_BRANCH_RIP 0x7ffffdea6758ull
#define AMD64_AS_FOCUS_SOURCE_SET_RIP 0x7ffffdea6956ull
#define AMD64_AS_FOCUS_SOURCE_CHECK_RIP 0x7ffffdea6abeull
#define AMD64_AS_FOCUS_BUILD_RIP 0x7ffffdea6b57ull
#define AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP 0x7ffffdee9a9eull
#define AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP 0x7ffffdee9aa3ull
#define AMD64_AS_ERROR_PRINTF_ENTRY_RIP 0x7ffffdea6600ull
#define AMD64_AS_ERROR_WRAPPER_RIP 0x7ffffdee4e2dull
#define AMD64_AS_ERROR_REPORT_RIP 0x7ffffdee89d4ull
#define AMD64_AS_ERROR_PRE_COUNT 192u
#define AMD64_AS_ERROR_POST_COUNT 16u

static inline bool amd64_trace_read_guest(qword_t addr, void *out, size_t size);
static inline bool amd64_as_is_template_moffs_probe(struct cpu_state *cpu);
static inline bool amd64_trace_try_read_lock(wrlock_t *lock);
static bool amd64_trace_read_task_guest_cstring(const struct task *task, qword_t addr,
        char *buf, size_t size);
static bool amd64_resolve_task_image_base(const struct task *task, qword_t *base);

static inline bool amd64_cc1_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_CC1") != NULL ? 1 : 0;
    return enabled == 1 &&
        current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        strcmp(current->comm, "cc1") == 0;
}

static inline bool amd64_cc1_trace_record_enabled(void) {
    // Runs on EVERY amd64 instruction (amd64_step_to_interrupt). Unlike the other
    // trace checks it has no getenv gate, so guard the strcmp with a one-byte
    // pre-filter: only "cc1" begins with 'c', so every other guest skips the
    // per-instruction strcmp call entirely. Behavior-identical (comm[0]=='c' is
    // implied by comm=="cc1"; a non-'c' comm can never equal "cc1").
    return current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        current->comm[0] == 'c' &&
        strcmp(current->comm, "cc1") == 0;
}

static inline bool amd64_as_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS") != NULL ? 1 : 0;
    return enabled == 1 &&
        current != NULL &&
        current->abi == GUEST_ABI_AMD64 &&
        strcmp(current->comm, "as") == 0;
}

static inline bool amd64_as_stderr_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS_STDERR") != NULL ? 1 : 0;
    return enabled == 1;
}

static inline bool amd64_as_alu_stderr_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_AS_ALU") != NULL ? 1 : 0;
    return enabled == 1;
}

static inline bool amd64_suspect_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_SUSPECT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (amd64_as_trace_enabled())
        return true;
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

static inline bool amd64_bash_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_BASH") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    return current != NULL && current->abi == GUEST_ABI_AMD64 && strcmp(current->comm, "bash") == 0;
}

static inline bool amd64_as_is_error_path_rip(qword_t rip) {
    qword_t image_base = 0;
    if (current != NULL && current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            amd64_resolve_task_image_base(current, &image_base)) {
        qword_t off = rip - image_base;
        if (off == 0x49a9e || off == 0x49aa3)
            return true;
    }
    switch (rip) {
    case AMD64_AS_ERROR_REPORT_RIP:
    case AMD64_AS_ERROR_WRAPPER_RIP:
    case AMD64_AS_ERROR_PRINTF_ENTRY_RIP:
    case AMD64_AS_FOCUS_DISPATCH_RIP:
    case AMD64_AS_FOCUS_CASE0_RIP:
    case AMD64_AS_FOCUS_SOURCE_BRANCH_RIP:
    case AMD64_AS_FOCUS_SOURCE_SET_RIP:
    case AMD64_AS_FOCUS_SOURCE_CHECK_RIP:
    case AMD64_AS_FOCUS_BUILD_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP:
        return true;
    default:
        return false;
    }
}

static inline bool amd64_as_is_template_moffs_probe(struct cpu_state *cpu) {
    uint8_t bytes[10] = {};
    static const uint8_t entry[] = {0x89, 0xd6, 0x83, 0xce, 0x01};
    static const uint8_t cmp[] = {0x66, 0x81, 0xfe, 0xa1, 0x00};
    if (cpu == NULL || current == NULL || current->abi != GUEST_ABI_AMD64 ||
            strcmp(current->comm, "as") != 0)
        return false;
    if (amd64_as_template_probe_pid == current->pid &&
            (cpu->amd64_current_insn_rip == amd64_as_template_probe_entry ||
             cpu->amd64_current_insn_rip == amd64_as_template_probe_cmp))
        return true;
    if (!amd64_trace_read_guest(cpu->amd64_current_insn_rip, bytes, sizeof(bytes)))
        return false;
    return memcmp(bytes, entry, sizeof(entry)) == 0 ||
        memcmp(bytes, cmp, sizeof(cmp)) == 0;
}

static void amd64_as_scan_template_probe(struct cpu_state *cpu) {
    static const uint8_t pattern[] = {
        0x89, 0xd6, 0x83, 0xce, 0x01, 0x66, 0x81, 0xfe, 0xa1, 0x00,
    };

    if (!amd64_as_trace_enabled() || current == NULL || current->mem == NULL ||
            amd64_as_template_probe_pid == current->pid)
        return;

    amd64_as_template_probe_pid = current->pid;
    amd64_as_template_probe_entry = 0;
    amd64_as_template_probe_cmp = 0;

    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return;
    for (page_t page = 0; page < current->mem->page_limit; mem_next_page(current->mem, &page)) {
        struct pt_entry *pt = mem_pt(current->mem, page);
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            continue;
        uint8_t *base = (uint8_t *) pt->data->data + pt->offset;
        for (size_t off = 0; off + sizeof(pattern) <= PAGE_SIZE; off++) {
            if (memcmp(base + off, pattern, sizeof(pattern)) != 0)
                continue;
            amd64_as_template_probe_entry = ((qword_t) page << PAGE_BITS) + off;
            amd64_as_template_probe_cmp = amd64_as_template_probe_entry + 5;
            break;
        }
        if (amd64_as_template_probe_entry != 0)
            break;
    }
    read_unlock(&current->mem->lock);

    if (amd64_as_template_probe_entry != 0 && amd64_as_stderr_enabled())
        fprintf(stderr, "amd64 as template probe: entry=%#llx cmp=%#llx current_rip=%#llx\n",
                (unsigned long long) amd64_as_template_probe_entry,
                (unsigned long long) amd64_as_template_probe_cmp,
                (unsigned long long) cpu->amd64_current_insn_rip);
}

static inline bool amd64_trace_copy_guest_locked(guest_addr_t guest_addr, void *out, size_t size) {
    uint8_t *dst = out;
    size_t copied = 0;

    while (copied < size) {
        struct pt_entry *pt = mem_pt(current->mem, PAGE(guest_addr + copied));
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            return false;
        size_t page_off = PGOFFSET(guest_addr + copied);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy(dst + copied, (uint8_t *) pt->data->data + pt->offset + page_off, chunk);
        copied += chunk;
    }

    return true;
}

static inline bool amd64_trace_try_read_lock(wrlock_t *lock) {
    return trylockr(lock) == 0;
}

static bool amd64_trace_read_current_guest(qword_t guest_addr, void *out, size_t size) {
    guest_addr_t addr;
    if (current == NULL || current->mem == NULL || out == NULL)
        return false;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr))
        return false;

    bool ok;
    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return false;
    ok = amd64_trace_copy_guest_locked(addr, out, size);
    read_unlock(&current->mem->lock);
    return ok;
}

static void amd64_trace_as_event(struct cpu_state *cpu, enum amd64_as_event_kind kind) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_event_pid != current->pid) {
        memset(amd64_as_events, 0, sizeof(amd64_as_events));
        amd64_as_event_next = 0;
        amd64_as_event_pid = current->pid;
    }

    struct amd64_as_event *event = &amd64_as_events[amd64_as_event_next++ % AMD64_AS_EVENT_COUNT];
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->rip = cpu->amd64_current_insn_rip;
    event->rax = cpu->amd64_regs[amd64_rax];
    event->rbx = cpu->amd64_regs[amd64_rbx];
    event->rcx = cpu->amd64_regs[amd64_rcx];
    event->rdx = cpu->amd64_regs[amd64_rdx];
    event->rsi = cpu->amd64_regs[amd64_rsi];
    event->rdi = cpu->amd64_regs[amd64_rdi];
    event->r12 = cpu->amd64_regs[amd64_r12];
    amd64_trace_read_current_guest(AMD64_AS_STATE31_ADDR, &event->state31, sizeof(event->state31));
}

static struct amd64_as_suspect *amd64_trace_as_suspect_reserve(void) {
    if (!amd64_suspect_trace_enabled())
        return NULL;

    if (amd64_as_suspect_pid != current->pid) {
        memset(amd64_as_suspects, 0, sizeof(amd64_as_suspects));
        amd64_as_suspect_next = 0;
        amd64_as_suspect_pid = current->pid;
    }

    struct amd64_as_suspect *suspect =
        &amd64_as_suspects[amd64_as_suspect_next++ % AMD64_AS_SUSPECT_COUNT];
    memset(suspect, 0, sizeof(*suspect));
    suspect->rip = current->cpu.amd64_current_insn_rip;
    return suspect;
}

static const char *amd64_as_stack_op_name(unsigned op) {
    switch (op) {
    case amd64_as_stack_push:
        return "push";
    case amd64_as_stack_pop:
        return "pop";
    case amd64_as_stack_leave:
        return "leave";
    default:
        return "unknown";
    }
}

static void amd64_dump_recent_suspects(pid_t_ pid, const char *tag) {
    if (amd64_as_suspect_pid != pid || amd64_as_suspect_next == 0)
        return;

    unsigned total = amd64_as_suspect_next;
    unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
    unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
    printk("[amd64-jit] %s recent-suspects pid=%d count=%u\n", tag, pid, count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_suspect *suspect =
            &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
        switch (suspect->kind) {
        case amd64_as_suspect_bt:
            printk("[amd64-jit]   suspect[%02u] kind=bt rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx\n",
                   i,
                   (unsigned long long) suspect->rip,
                   suspect->op,
                   suspect->size,
                   suspect->aux,
                   (unsigned long long) suspect->bit_index,
                   (unsigned long long) suspect->bit,
                   (unsigned long long) suspect->addr,
                   (unsigned long long) suspect->lhs,
                   (unsigned long long) suspect->value);
            break;
        case amd64_as_suspect_stack:
            printk("[amd64-jit]   suspect[%02u] kind=stack op=%s size=%u rip=%#llx old-rsp=%#llx new-rsp=%#llx value=%#llx\n",
                   i,
                   amd64_as_stack_op_name(suspect->op),
                   suspect->size,
                   (unsigned long long) suspect->rip,
                   (unsigned long long) suspect->old_rsp,
                   (unsigned long long) suspect->new_rsp,
                   (unsigned long long) suspect->value);
            break;
        default:
            break;
        }
    }
}

static void amd64_dump_recent_suspects_for_stack_slot(pid_t_ pid, qword_t slot_addr,
        const char *tag) {
    if (amd64_as_suspect_pid != pid || amd64_as_suspect_next == 0)
        return;

    unsigned total = amd64_as_suspect_next;
    unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
    unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
    unsigned matches = 0;

    printk("[amd64-jit] %s slot-suspects pid=%d slot=%#llx count=%u\n",
           tag, pid, (unsigned long long) slot_addr, count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_suspect *suspect =
            &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
        if (suspect->kind != amd64_as_suspect_stack)
            continue;
        if (suspect->old_rsp != slot_addr && suspect->new_rsp != slot_addr)
            continue;
        matches++;
        printk("[amd64-jit]   slot-suspect[%02u] op=%s size=%u rip=%#llx old-rsp=%#llx new-rsp=%#llx value=%#llx\n",
               i,
               amd64_as_stack_op_name(suspect->op),
               suspect->size,
               (unsigned long long) suspect->rip,
               (unsigned long long) suspect->old_rsp,
               (unsigned long long) suspect->new_rsp,
               (unsigned long long) suspect->value);
    }
    printk("[amd64-jit] %s slot-suspects-matches=%u\n", tag, matches);
}

static void amd64_trace_as_bt(struct cpu_state *cpu, uint8_t op, unsigned size,
        bool is_mem, qword_t bit_index, qword_t bit, qword_t addr,
        qword_t lhs, qword_t value) {
    struct amd64_as_suspect *suspect = amd64_trace_as_suspect_reserve();
    if (suspect == NULL)
        return;
    suspect->kind = amd64_as_suspect_bt;
    suspect->op = op;
    suspect->size = size;
    suspect->aux = is_mem ? 1 : 0;
    suspect->lhs = lhs;
    suspect->value = value;
    suspect->addr = addr;
    suspect->bit_index = bit_index;
    suspect->bit = bit;
    if (amd64_as_alu_stderr_enabled()) {
        uint8_t insn_bytes[8] = {};
        bool have_insn_bytes = amd64_trace_read_guest(cpu->amd64_current_insn_rip,
                insn_bytes, sizeof(insn_bytes));
        fprintf(stderr,
                "amd64 as bt: rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx cf=%u%s\n",
                (unsigned long long) cpu->amd64_current_insn_rip,
                op,
                size,
                is_mem ? 1u : 0u,
                (unsigned long long) bit_index,
                (unsigned long long) bit,
                (unsigned long long) addr,
                (unsigned long long) lhs,
                (unsigned long long) value,
                cpu->cf,
                have_insn_bytes ? "" : " bytes=?");
        if (have_insn_bytes) {
            fprintf(stderr,
                    "amd64 as bt: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                    insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
    }
}

static void amd64_trace_as_stack(unsigned op, unsigned size,
        qword_t old_rsp, qword_t new_rsp, qword_t value) {
    struct amd64_as_suspect *suspect = amd64_trace_as_suspect_reserve();
    if (suspect == NULL)
        return;
    suspect->kind = amd64_as_suspect_stack;
    suspect->op = op;
    suspect->size = size;
    suspect->old_rsp = old_rsp;
    suspect->new_rsp = new_rsp;
    suspect->value = value;
}

static void amd64_dump_stack_window(struct cpu_state *cpu, struct tlb *tlb,
        qword_t center_rsp, unsigned before, unsigned after, const char *tag) {
    qword_t start = center_rsp - (qword_t) before * 8;
    qword_t end = center_rsp + (qword_t) after * 8;
    printk("[amd64-jit] %s stack-window center=%#llx range=%#llx..%#llx\n",
           tag,
           (unsigned long long) center_rsp,
           (unsigned long long) start,
           (unsigned long long) end);
    for (unsigned i = 0; i <= before + after; i++) {
        qword_t addr = start + (qword_t) i * 8;
        qword_t value = 0;
        if (amd64_mem_read(cpu, tlb, addr, &value, sizeof(value))) {
            printk("[amd64-jit]   stack[%+lld] addr=%#llx value=%#llx%s\n",
                   (long long) i - (long long) before,
                   (unsigned long long) addr,
                   (unsigned long long) value,
                   addr == center_rsp ? " <== popped target slot" : "");
        } else {
            printk("[amd64-jit]   stack[%+lld] addr=%#llx unreadable%s\n",
                   (long long) i - (long long) before,
                   (unsigned long long) addr,
                   addr == center_rsp ? " <== popped target slot" : "");
        }
    }
}

static bool amd64_mem_read_direct(qword_t guest_addr, void *out, unsigned size) {
    guest_addr_t addr;
    unsigned copied = 0;
    if (current == NULL || current->mem == NULL)
        return false;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr))
        return false;
    while (copied < size) {
        guest_addr_t chunk_addr = addr + copied;
        void *ptr = mem_ptr(current->mem, chunk_addr, MEM_READ);
        unsigned chunk = PAGE_SIZE - PGOFFSET(chunk_addr);
        if (ptr == NULL)
            return false;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy((char *) out + copied, ptr, chunk);
        copied += chunk;
    }
    return true;
}

static void amd64_dump_tlb_slot(struct tlb *tlb, qword_t guest_addr, unsigned size,
        const char *tag) {
    guest_addr_t addr;
    struct tlb_entry entry;
    uint8_t cached_bytes[16] = {};
    bool have_cached_bytes = false;
    if (size > sizeof(cached_bytes))
        size = sizeof(cached_bytes);
    if (tlb == NULL || !amd64_guest_addr_ok(guest_addr, size, &addr)) {
        printk("[amd64-jit] %s tlb-slot addr=%#llx unavailable\n",
               tag,
               (unsigned long long) guest_addr);
        return;
    }
    entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *ptr = (void *) (entry.data_minus_addr + addr);
        if (ptr != NULL) {
            memcpy(cached_bytes, ptr, size);
            have_cached_bytes = true;
        }
    }
    printk("[amd64-jit] %s tlb-slot addr=%#llx index=%u page=%#llx want=%#llx writable=%#llx delta=%#llx changes=%llu/%llu bytes=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
           tag,
           (unsigned long long) guest_addr,
           TLB_INDEX(addr),
           (unsigned long long) entry.page,
           (unsigned long long) TLB_PAGE(addr),
           (unsigned long long) entry.page_if_writable,
           (unsigned long long) entry.data_minus_addr,
           (unsigned long long) tlb->mem_changes,
           (unsigned long long) (tlb->mmu != NULL ? tlb->mmu->changes : 0),
           have_cached_bytes ? "" : "unreadable ",
           cached_bytes[0], cached_bytes[1], cached_bytes[2], cached_bytes[3],
           cached_bytes[4], cached_bytes[5], cached_bytes[6], cached_bytes[7]);
}

static void amd64_dump_guest_bytes(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, const char *tag) {
    uint8_t bytes[16] = {};
    if (size > sizeof(bytes))
        size = sizeof(bytes);
    if (!amd64_mem_read(cpu, tlb, guest_addr, bytes, size)) {
        printk("[amd64-jit] %s addr=%#llx unreadable size=%u\n",
               tag,
               (unsigned long long) guest_addr,
               size);
        return;
    }
    printk("[amd64-jit] %s addr=%#llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x%s\n",
           tag,
           (unsigned long long) guest_addr,
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5], bytes[6], bytes[7],
           size > 8 ? " ..." : "");
}

static void amd64_trace_as_focus(struct cpu_state *cpu) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_is_template_moffs_probe(cpu))
        goto record_focus;
    return;

#if 0
    qword_t image_base = 0;
    bool have_image_base = current != NULL &&
        amd64_resolve_task_image_base(current, &image_base);
    if (have_image_base) {
        qword_t off = cpu->amd64_current_insn_rip - image_base;
        if (off == 0x49a9e || off == 0x49aa3)
            goto record_focus;
    }

    switch (cpu->amd64_current_insn_rip) {
    case AMD64_AS_FOCUS_DISPATCH_RIP:
    case AMD64_AS_FOCUS_CASE0_RIP:
    case AMD64_AS_FOCUS_SOURCE_BRANCH_RIP:
    case AMD64_AS_FOCUS_SOURCE_SET_RIP:
    case AMD64_AS_FOCUS_SOURCE_CHECK_RIP:
    case AMD64_AS_FOCUS_BUILD_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_RIP:
    case AMD64_AS_FOCUS_TEMPLATE_MOFFS_CMP_RIP:
        break;
    default:
        return;
    }

record_focus:
#endif
record_focus:
    if (amd64_as_focus_pid != current->pid) {
        memset(amd64_as_focus, 0, sizeof(amd64_as_focus));
        amd64_as_focus_next = 0;
        amd64_as_focus_pid = current->pid;
    }

    struct amd64_as_focus *focus = &amd64_as_focus[amd64_as_focus_next++ % AMD64_AS_FOCUS_COUNT];
    memset(focus, 0, sizeof(*focus));
    focus->rip = cpu->amd64_current_insn_rip;
    focus->rax = cpu->amd64_regs[amd64_rax];
    focus->rbx = cpu->amd64_regs[amd64_rbx];
    focus->rcx = cpu->amd64_regs[amd64_rcx];
    focus->rdx = cpu->amd64_regs[amd64_rdx];
    focus->rsi = cpu->amd64_regs[amd64_rsi];
    focus->rdi = cpu->amd64_regs[amd64_rdi];
    focus->rsp = cpu->amd64_regs[amd64_rsp];
    focus->rbp = cpu->amd64_regs[amd64_rbp];
    focus->r12 = cpu->amd64_regs[amd64_r12];
    if (amd64_as_stderr_enabled()) {
        static pid_t_ amd64_as_focus_image_base_pid;
        static qword_t amd64_as_focus_image_base;
        qword_t dword_at_rdx = 0;
        qword_t image_base = 0;
        uint8_t insn_bytes[8] = {};
        char rbx_text[64];
        char rsi_text[64];
        char rdi_text[64];
        if (amd64_as_focus_image_base_pid != current->pid) {
            amd64_as_focus_image_base = 0;
            if (!amd64_resolve_task_image_base(current, &amd64_as_focus_image_base))
                amd64_as_focus_image_base = 0;
            amd64_as_focus_image_base_pid = current->pid;
        }
        image_base = amd64_as_focus_image_base;
        bool have_dword_at_rdx = amd64_trace_read_guest(focus->rdx, &dword_at_rdx, sizeof(uint32_t));
        bool have_insn_bytes = amd64_trace_read_guest(focus->rip, insn_bytes, sizeof(insn_bytes));
        bool have_rbx_text = amd64_trace_read_task_guest_cstring(current, focus->rbx,
                rbx_text, sizeof(rbx_text));
        bool have_rsi_text = amd64_trace_read_task_guest_cstring(current, focus->rsi,
                rsi_text, sizeof(rsi_text));
        bool have_rdi_text = amd64_trace_read_task_guest_cstring(current, focus->rdi,
                rdi_text, sizeof(rdi_text));
        fprintf(stderr,
                "amd64 as focus: rip=%#llx off=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r12=%#llx%s%s%s%s%s\n",
                (unsigned long long) focus->rip,
                (unsigned long long) (image_base == 0 || focus->rip < image_base ? 0 : focus->rip - image_base),
                (unsigned long long) focus->rax,
                (unsigned long long) focus->rbx,
                (unsigned long long) focus->rcx,
                (unsigned long long) focus->rdx,
                (unsigned long long) focus->rsi,
                (unsigned long long) focus->rdi,
                (unsigned long long) focus->rsp,
                (unsigned long long) focus->rbp,
                (unsigned long long) focus->r12,
                have_insn_bytes ? "" : " bytes=?",
                have_dword_at_rdx ? "" : " [rdx]=?",
                have_rbx_text ? "" : " rbx_str=?",
                have_rsi_text ? "" : " rsi_str=?",
                have_rdi_text ? "" : " rdi_str=?");
        if (have_insn_bytes) {
            fprintf(stderr,
                    "amd64 as focus: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                    insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
        if (have_dword_at_rdx)
            fprintf(stderr, "amd64 as focus: [rdx]=%#llx\n", (unsigned long long) dword_at_rdx);
        if (have_rbx_text)
            fprintf(stderr, "amd64 as focus: rbx_str=\"%s\"\n", rbx_text);
        if (have_rsi_text)
            fprintf(stderr, "amd64 as focus: rsi_str=\"%s\"\n", rsi_text);
        if (have_rdi_text)
            fprintf(stderr, "amd64 as focus: rdi_str=\"%s\"\n", rdi_text);
    }
}

static inline bool amd64_trace_copy_task_guest_locked(const struct task *task,
        guest_addr_t guest_addr, void *out, size_t size) {
    uint8_t *dst = out;
    size_t copied = 0;

    if (task == NULL || task->mem == NULL)
        return false;

    while (copied < size) {
        struct pt_entry *pt = mem_pt(task->mem, PAGE(guest_addr + copied));
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            return false;
        size_t page_off = PGOFFSET(guest_addr + copied);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > size - copied)
            chunk = size - copied;
        memcpy(dst + copied, (uint8_t *) pt->data->data + pt->offset + page_off, chunk);
        copied += chunk;
    }

    return true;
}

static inline bool amd64_trace_read_guest(qword_t addr, void *out, size_t size) {
    if (current == NULL || current->mem == NULL)
        return false;
    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, size, &guest_addr))
        return false;
    bool ok = false;
    if (!amd64_trace_try_read_lock(&current->mem->lock))
        return false;
    ok = amd64_trace_copy_guest_locked(guest_addr, out, size);
    read_unlock(&current->mem->lock);
    return ok;
}

static inline bool amd64_trace_read_task_guest(const struct task *task, qword_t addr,
        void *out, size_t size) {
    if (task == NULL || task->mem == NULL)
        return false;
    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, size, &guest_addr))
        return false;
    bool ok = false;
    if (!amd64_trace_try_read_lock(&task->mem->lock))
        return false;
    ok = amd64_trace_copy_task_guest_locked(task, guest_addr, out, size);
    read_unlock(&task->mem->lock);
    return ok;
}

static bool amd64_trace_read_task_guest_cstring(const struct task *task, qword_t addr,
        char *buf, size_t size) {
    if (buf == NULL || size == 0) 
        return false;
    buf[0] = '\0';
    if (task == NULL || task->mem == NULL)
        return false;

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(addr, 1, &guest_addr))
        return false;

    size_t i;
    for (i = 0; i + 1 < size; i++) {
        char ch;
        if (!amd64_trace_read_task_guest(task, addr + i, &ch, sizeof(ch)))
            break;
        if (ch == '\0')
            break;
        if ((unsigned char) ch < 0x20 || (unsigned char) ch > 0x7e)
            buf[i] = '.';
        else
            buf[i] = ch;
    }
    buf[i] = '\0';
    return i != 0;
}

static inline bool amd64_trace_read_guest_cstring(qword_t addr, char *buf, size_t size) {
    if (size == 0)
        return false;
    buf[0] = '\0';
    if (addr == 0)
        return false;
    for (size_t i = 0; i + 1 < size; i++) {
        uint8_t ch = 0;
        if (!amd64_trace_read_guest(addr + i, &ch, sizeof(ch)))
            return false;
        buf[i] = ch;
        if (ch == '\0')
            return true;
    }
    buf[size - 1] = '\0';
    return true;
}

static inline void amd64_trace_bash_cond_probe(struct cpu_state *cpu) {
    if (!amd64_bash_trace_enabled() || amd64_bash_cond_probe_count >= 8)
        return;

    qword_t rip = cpu->amd64_current_insn_rip;
    if (rip != AMD64_BASH_COND_UNEXP_RIP && rip != AMD64_BASH_COND_BINOP_RIP)
        return;

    dword_t line = 0;
    dword_t token_a = 0;
    dword_t token_b = 0;
    dword_t parser_state = 0;
    dword_t extended_glob = 0;
    dword_t syn_dollar = 0;
    dword_t syn_star = 0;
    dword_t syn_dash = 0;
    dword_t syn_lbrack = 0;
    dword_t syn_rbrack = 0;
    dword_t syn_i = 0;
    char token_text[64];
    char format_text[96];

    bool have_line = amd64_trace_read_guest(AMD64_BASH_LINE_NUMBER_ADDR, &line, sizeof(line));
    bool have_token_a = amd64_trace_read_guest(AMD64_BASH_TOKEN_A_ADDR, &token_a, sizeof(token_a));
    bool have_token_b = amd64_trace_read_guest(AMD64_BASH_TOKEN_B_ADDR, &token_b, sizeof(token_b));
    bool have_parser_state = amd64_trace_read_guest(AMD64_BASH_PARSER_STATE_ADDR, &parser_state, sizeof(parser_state));
    bool have_extended_glob = amd64_trace_read_guest(AMD64_BASH_EXTENDED_GLOB_ADDR, &extended_glob, sizeof(extended_glob));
    bool have_syn_dollar = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x24u, &syn_dollar, sizeof(syn_dollar));
    bool have_syn_star = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x2au, &syn_star, sizeof(syn_star));
    bool have_syn_dash = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x2du, &syn_dash, sizeof(syn_dash));
    bool have_syn_lbrack = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x5bu, &syn_lbrack, sizeof(syn_lbrack));
    bool have_syn_rbrack = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x5du, &syn_rbrack, sizeof(syn_rbrack));
    bool have_syn_i = amd64_trace_read_guest(AMD64_BASH_SYNTAXTAB_ADDR + 4 * 0x69u, &syn_i, sizeof(syn_i));
    bool have_token_text = amd64_trace_read_guest_cstring(cpu->amd64_regs[amd64_rdx], token_text, sizeof(token_text));
    bool have_format_text = amd64_trace_read_guest_cstring(cpu->amd64_regs[amd64_rsi], format_text, sizeof(format_text));

    amd64_bash_cond_probe_count++;
    printk("amd64 bash cond probe: rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rbp=%#llx line=%u%s tok_a=%#x%s tok_b=%#x%s parser_state=%#x%s extglob=%#x%s\n",
           (unsigned long long) rip,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           line, have_line ? "" : "<?>",
           token_a, have_token_a ? "" : "<?>",
           token_b, have_token_b ? "" : "<?>",
           parser_state, have_parser_state ? "" : "<?>",
           extended_glob, have_extended_glob ? "" : "<?>");
    printk("amd64 bash cond probe: format=%s%s token=%s%s syn[$]=%#x%s syn[*]=%#x%s syn[-]=%#x%s syn[[]=%#x%s syn[]]=%#x%s syn[i]=%#x%s\n",
           have_format_text ? "\"" : "<unreadable>",
           have_format_text ? format_text : "",
           have_token_text ? "\"" : "<unreadable>",
           have_token_text ? token_text : "",
           syn_dollar, have_syn_dollar ? "" : "<?>",
           syn_star, have_syn_star ? "" : "<?>",
           syn_dash, have_syn_dash ? "" : "<?>",
           syn_lbrack, have_syn_lbrack ? "" : "<?>",
           syn_rbrack, have_syn_rbrack ? "" : "<?>",
           syn_i, have_syn_i ? "" : "<?>");
}

static inline void amd64_trace_cc1_slot_probe(struct cpu_state *cpu, qword_t rip, qword_t addr, qword_t value) {
    (void) cpu;
    if (!amd64_cc1_trace_enabled() || rip != 0x1288c6dull || amd64_cc1_slot_probe_count >= 4)
        return;

    amd64_cc1_slot_probe_count++;
    uint8_t bytes[32] = {};
    size_t have_bytes = 0;
    struct pt_entry *pt = NULL;
    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        guest_addr_t guest_addr;
        if (amd64_guest_addr_ok(addr, sizeof(bytes), &guest_addr)) {
            if (amd64_trace_copy_guest_locked(guest_addr, bytes, sizeof(bytes))) {
                have_bytes = sizeof(bytes);
            }
        }
        pt = mem_pt(current->mem, PAGE(addr));
        read_unlock(&current->mem->lock);
    }
    printk("amd64 cc1 slot probe: rip=%#llx addr=%#llx value=%#llx\n",
           (unsigned long long) rip,
           (unsigned long long) addr,
           (unsigned long long) value);
    if (pt == NULL) {
        printk("amd64 cc1 slot probe: page=%#llx unmapped\n",
               (unsigned long long) PAGE(addr));
    } else {
        const char *name = pt->data != NULL ? pt->data->name : "-";
        printk("amd64 cc1 slot probe: page=%#llx flags=%#x off=%#zx name=%s\n",
               (unsigned long long) PAGE(addr),
               pt->flags,
               pt->offset,
               name != NULL ? name : "-");
    }

    if (have_bytes != 0) {
        printk("amd64 cc1 slot probe bytes:");
        for (size_t i = 0; i < have_bytes; i++)
            printk(" %02x", bytes[i]);
        printk("\n");
    }
}

static inline void amd64_trace_cc1_cmp_probe(struct cpu_state *cpu, qword_t rip, qword_t addr,
        qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    if (!amd64_cc1_trace_enabled() || rip != 0x11257e0ull || amd64_cc1_cmp_probe_count >= 4)
        return;

    amd64_cc1_cmp_probe_count++;
    printk("amd64 cc1 cmp probe: rip=%#llx addr=%#llx lhs=%#llx rhs=%#llx result=%#llx size=%u zf=%d sf=%d of=%d cf=%d\n",
           (unsigned long long) rip,
           (unsigned long long) addr,
           (unsigned long long) lhs,
           (unsigned long long) rhs,
           (unsigned long long) result,
           size,
           cpu->zf,
           cpu->sf,
           cpu->of,
           cpu->cf);
}

static inline void amd64_trace_cc1_je_probe(struct cpu_state *cpu, qword_t rip, bool taken, qword_t target) {
    if (!amd64_cc1_trace_enabled() || rip != 0x11257efull || amd64_cc1_je_probe_count >= 4)
        return;

    amd64_cc1_je_probe_count++;
    printk("amd64 cc1 je probe: rip=%#llx zf=%d taken=%d target=%#llx rdi=%#llx rbp=%#llx\n",
           (unsigned long long) rip,
           cpu->zf,
           taken,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
}

static inline void amd64_trace_cc1_va_list_branch_probe(struct cpu_state *cpu, qword_t rip,
        bool taken, qword_t target, const char *kind) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_va_list_branch_probe_count >= 8)
        return;
    if (rip != AMD64_CC1_VA_LIST_HOOK_JNE_RIP && rip != AMD64_CC1_VA_LIST_HOOK_FALLBACK_JMP_RIP)
        return;

    qword_t abi_flags = 0;
    qword_t ms_slot = 0;
    qword_t sysv_slot = 0;
    bool have_abi_flags = false;
    bool have_ms_slot = false;
    bool have_sysv_slot = false;

    have_abi_flags = amd64_trace_read_guest(AMD64_CC1_ABI_FLAG_ADDR, &abi_flags, sizeof(abi_flags));
    have_ms_slot = amd64_trace_read_guest(AMD64_CC1_NULL_SLOT_ADDR, &ms_slot, sizeof(ms_slot));
    have_sysv_slot = amd64_trace_read_guest(AMD64_CC1_SYSV_SLOT_ADDR, &sysv_slot, sizeof(sysv_slot));

    amd64_cc1_va_list_branch_probe_count++;
    printk("amd64 cc1 va_list branch: rip=%#llx kind=%s taken=%d target=%#llx zf=%d cf=%d sf=%d of=%d rdi=%#llx rbp=%#llx flags=%#llx%s ms=%#llx%s sysv=%#llx%s\n",
           (unsigned long long) rip,
           kind,
           taken,
           (unsigned long long) target,
           cpu->zf,
           cpu->cf,
           cpu->sf,
           cpu->of,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) abi_flags,
           have_abi_flags ? "" : "<?>",
           (unsigned long long) ms_slot,
           have_ms_slot ? "" : "<?>",
           (unsigned long long) sysv_slot,
           have_sysv_slot ? "" : "<?>");
}

static inline void amd64_trace_cc1_xfer_probe(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, qword_t target, const char *kind) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (!amd64_cc1_trace_enabled() || amd64_cc1_xfer_probe_count >= 32)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cc1_xfer_probe_count++;
    printk("amd64 cc1 xfer: kind=%s from=%#llx to=%#llx rsp=%#llx rbp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
           kind,
           (unsigned long long) saved_rip,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cc1 xfer bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_range_intersects(qword_t base_a, unsigned size_a, qword_t base_b, unsigned size_b) {
    qword_t end_a = base_a + size_a;
    qword_t end_b = base_b + size_b;
    return base_a < end_b && base_b < end_a;
}

static inline void amd64_trace_cc1_slot_write_probe(struct cpu_state *cpu, qword_t guest_addr,
        const void *value, unsigned size) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_slot_write_probe_count >= 16)
        return;
    if (!amd64_trace_range_intersects(guest_addr, size, AMD64_CC1_NULL_SLOT_ADDR, 8) &&
            !amd64_trace_range_intersects(guest_addr, size, AMD64_CC1_CMP_GLOBAL_ADDR, 8))
        return;

    amd64_cc1_slot_write_probe_count++;
    qword_t observed = 0;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
    printk("amd64 cc1 slot write: rip=%#llx addr=%#llx size=%u value=%#llx rdi=%#llx rbp=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
}

static inline void amd64_trace_cc1_va_list_init_probe(struct cpu_state *cpu) {
    if (!amd64_cc1_trace_enabled() || amd64_cc1_va_list_init_probe_count >= 24)
        return;

    qword_t rip = cpu->amd64_current_insn_rip;
    if (rip != AMD64_CC1_VA_LIST_HOOK_RIP &&
            rip != AMD64_CC1_VA_LIST_COMPLEX_RIP &&
            rip != AMD64_CC1_VA_LIST_GETTER_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ENTRY_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_SYSV_STORE_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ATTR_CALL_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_ATTR_RET_RIP &&
            rip != AMD64_CC1_VA_LIST_INIT_MS_STORE_RIP)
        return;

    qword_t ms_slot = 0;
    qword_t sysv_slot = 0;
    qword_t abi_flags = 0;
    uint32_t option = 0;
    bool have_ms_slot = false;
    bool have_sysv_slot = false;
    bool have_abi_flags = false;
    bool have_option = false;

    have_ms_slot = amd64_trace_read_guest(AMD64_CC1_NULL_SLOT_ADDR, &ms_slot, sizeof(ms_slot));
    have_sysv_slot = amd64_trace_read_guest(AMD64_CC1_SYSV_SLOT_ADDR, &sysv_slot, sizeof(sysv_slot));
    have_abi_flags = amd64_trace_read_guest(AMD64_CC1_ABI_FLAG_ADDR, &abi_flags, sizeof(abi_flags));
    have_option = amd64_trace_read_guest(AMD64_CC1_MS_VARIANT_ADDR, &option, sizeof(option));

    amd64_cc1_va_list_init_probe_count++;
    printk("amd64 cc1 va_list init: rip=%#llx rax=%#llx rbx=%#llx rdi=%#llx rsi=%#llx rbp=%#llx flags=%#llx%s ms=%#llx%s sysv=%#llx%s opt=%#x%s\n",
           (unsigned long long) rip,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) abi_flags,
           have_abi_flags ? "" : "<?>",
           (unsigned long long) ms_slot,
           have_ms_slot ? "" : "<?>",
           (unsigned long long) sysv_slot,
           have_sysv_slot ? "" : "<?>",
           option,
           have_option ? "" : "<?>");
}

static inline void amd64_trace_cc1_step(struct cpu_state *cpu) {
    if (amd64_cc1_trace_pid != current->pid) {
        memset(amd64_cc1_trace, 0, sizeof(amd64_cc1_trace));
        amd64_cc1_trace_next = 0;
        amd64_cc1_trace_pid = current->pid;
    }

    struct amd64_cc1_trace *trace = &amd64_cc1_trace[amd64_cc1_trace_next++ % AMD64_CC1_TRACE_COUNT];
    memset(trace, 0, sizeof(*trace));
    trace->rip = cpu->amd64_current_insn_rip;
    trace->rax = cpu->amd64_regs[amd64_rax];
    trace->rbx = cpu->amd64_regs[amd64_rbx];
    trace->rcx = cpu->amd64_regs[amd64_rcx];
    trace->rdx = cpu->amd64_regs[amd64_rdx];
    trace->rsi = cpu->amd64_regs[amd64_rsi];
    trace->rdi = cpu->amd64_regs[amd64_rdi];
    trace->rsp = cpu->amd64_regs[amd64_rsp];
    trace->rbp = cpu->amd64_regs[amd64_rbp];
    trace->r8 = cpu->amd64_regs[amd64_r8];
    trace->r9 = cpu->amd64_regs[amd64_r9];
    trace->r12 = cpu->amd64_regs[amd64_r12];

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(trace->rip, sizeof(trace->bytes), &guest_addr) || current->mem == NULL)
        return;

    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        if (amd64_trace_copy_guest_locked(guest_addr, trace->bytes, sizeof(trace->bytes))) {
            trace->byte_count = sizeof(trace->bytes);
        }
        read_unlock(&current->mem->lock);
    }

    amd64_trace_cc1_va_list_init_probe(cpu);
}

static inline void amd64_trace_as_step(struct cpu_state *cpu) {
    if (!amd64_as_trace_enabled())
        return;

    if (amd64_as_trace_pid != current->pid) {
        memset(amd64_as_trace, 0, sizeof(amd64_as_trace));
        amd64_as_trace_next = 0;
        amd64_as_trace_pid = current->pid;
        memset(amd64_as_events, 0, sizeof(amd64_as_events));
        amd64_as_event_next = 0;
        amd64_as_event_pid = current->pid;
        memset(amd64_as_suspects, 0, sizeof(amd64_as_suspects));
        amd64_as_suspect_next = 0;
        amd64_as_suspect_pid = current->pid;
        memset(amd64_as_focus, 0, sizeof(amd64_as_focus));
        amd64_as_focus_next = 0;
        amd64_as_focus_pid = current->pid;
    }

    amd64_as_scan_template_probe(cpu);

    struct amd64_as_trace *trace = &amd64_as_trace[amd64_as_trace_next++ % AMD64_AS_TRACE_COUNT];
    memset(trace, 0, sizeof(*trace));
    trace->rip = cpu->amd64_current_insn_rip;
    trace->rax = cpu->amd64_regs[amd64_rax];
    trace->rbx = cpu->amd64_regs[amd64_rbx];
    trace->rcx = cpu->amd64_regs[amd64_rcx];
    trace->rdx = cpu->amd64_regs[amd64_rdx];
    trace->rsi = cpu->amd64_regs[amd64_rsi];
    trace->rdi = cpu->amd64_regs[amd64_rdi];
    trace->rsp = cpu->amd64_regs[amd64_rsp];
    trace->rbp = cpu->amd64_regs[amd64_rbp];
    trace->r8 = cpu->amd64_regs[amd64_r8];
    trace->r9 = cpu->amd64_regs[amd64_r9];
    trace->r12 = cpu->amd64_regs[amd64_r12];

    guest_addr_t guest_addr;
    if (!amd64_guest_addr_ok(trace->rip, sizeof(trace->bytes), &guest_addr) || current->mem == NULL)
        return;

    if (amd64_trace_try_read_lock(&current->mem->lock)) {
        if (amd64_trace_copy_guest_locked(guest_addr, trace->bytes, sizeof(trace->bytes)))
            trace->byte_count = sizeof(trace->bytes);
        read_unlock(&current->mem->lock);
    }

    amd64_trace_as_focus(cpu);

    switch (trace->rip) {
    case AMD64_AS_RESET_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_reset_done);
        break;
    case AMD64_AS_STATE31_WRITE1_DONE_RIP:
    case AMD64_AS_STATE31_WRITE2_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_state31_write);
        break;
    case AMD64_AS_STATE31_CHECK_DONE_RIP:
        amd64_trace_as_event(cpu, amd64_as_event_state31_check);
        break;
    default:
        break;
    }

}

#define AMD64_AS_STATE_BLOCK_OFFSET 0xd3880ull
#define AMD64_AS_STATE_DUMP_SIZE 0x40u
#define AMD64_AS_STATE_TRACE_WINDOW 0x140u
#define AMD64_AS_DESCRIPTOR_SIZE 16u

static bool amd64_trace_read_task_u32(const struct task *task, qword_t addr, uint32_t *value) {
    return amd64_trace_read_task_guest(task, addr, value, sizeof(*value));
}

static bool amd64_trace_read_task_u64(const struct task *task, qword_t addr, uint64_t *value) {
    return amd64_trace_read_task_guest(task, addr, value, sizeof(*value));
}

static void amd64_dump_as_descriptor_task(const struct task *task, unsigned index, qword_t ptr,
        uint32_t slot_1c, uint32_t slot_48, uint32_t slot_88, uint32_t slot_9c) {
    if (ptr == 0 && slot_1c == 0 && slot_48 == 0 && slot_88 == 0 && slot_9c == 0)
        return;

    printk("amd64 as desc[%u]: slot_1c=%#x slot_48=%#x slot_88=%#x slot_9c=%#x ptr=%#llx",
           index,
           slot_1c,
           slot_48,
           slot_88,
           slot_9c,
           (unsigned long long) ptr);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                "amd64 as desc[%u]: slot_1c=%#x slot_48=%#x slot_88=%#x slot_9c=%#x ptr=%#llx",
                index,
                slot_1c,
                slot_48,
                slot_88,
                slot_9c,
                (unsigned long long) ptr);
    }
    if (ptr == 0) {
        printk("\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "\n");
        return;
    }

    uint8_t desc[16] = {};
    if (!amd64_trace_read_task_guest(task, ptr, desc, sizeof(desc))) {
        printk(" unreadable\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, " unreadable\n");
        return;
    }

    printk(" flags=%#x kind=%#x raw="
           "%02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           desc[0xc],
           desc[0xd],
           desc[0], desc[1], desc[2], desc[3],
           desc[4], desc[5], desc[6], desc[7],
           desc[8], desc[9], desc[10], desc[11],
           desc[12], desc[13], desc[14], desc[15]);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                " flags=%#x kind=%#x raw="
                "%02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                desc[0xc],
                desc[0xd],
                desc[0], desc[1], desc[2], desc[3],
                desc[4], desc[5], desc[6], desc[7],
                desc[8], desc[9], desc[10], desc[11],
                desc[12], desc[13], desc[14], desc[15]);
    }
}

static bool amd64_read_task_auxv64(const struct task *task, qword_t type, qword_t *value) {
    if (task == NULL || task->mm == NULL || value == NULL)
        return false;
    guest_addr_t start = task->mm->auxv_start;
    guest_addr_t end = task->mm->auxv_end;
    if (start == 0 || end <= start)
        return false;

    struct aux64_ent ent;
    for (guest_addr_t addr = start; addr + sizeof(ent) <= end; addr += sizeof(ent)) {
        if (!amd64_trace_read_task_guest(task, addr, &ent, sizeof(ent)))
            return false;
        if (ent.type == 0)
            break;
        if (ent.type == type) {
            *value = ent.value;
            return true;
        }
    }
    return false;
}

static bool amd64_resolve_task_image_base(const struct task *task, qword_t *base) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || base == NULL)
        return false;

    qword_t phdr_addr = 0;
    qword_t phent_size = 0;
    qword_t phnum = 0;
    if (!amd64_read_task_auxv64(task, AX_PHDR, &phdr_addr) ||
            !amd64_read_task_auxv64(task, AX_PHENT, &phent_size) ||
            !amd64_read_task_auxv64(task, AX_PHNUM, &phnum))
        return false;
    if (phdr_addr == 0 || phent_size < sizeof(struct prg_header64) || phnum == 0 || phnum > 128)
        return false;

    for (qword_t i = 0; i < phnum; i++) {
        struct prg_header64 ph;
        qword_t addr = phdr_addr + i * phent_size;
        if (!amd64_trace_read_task_guest(task, addr, &ph, sizeof(ph)))
            return false;
        if (ph.type == PT_PHDR) {
            *base = phdr_addr - ph.vaddr;
            return true;
        }
    }
    return false;
}

static inline bool amd64_trace_intersects_guest_range(qword_t guest_addr, unsigned size,
        qword_t watch_addr, unsigned watch_size) {
    qword_t end = guest_addr + size;
    qword_t watch_end = watch_addr + watch_size;
    return guest_addr < watch_end && end > watch_addr;
}

static bool amd64_trace_as_state_region(qword_t guest_addr, unsigned size,
        qword_t *region_base, uint8_t *region_kind, uint8_t *region_index) {
    static pid_t_ amd64_as_image_base_pid;
    static qword_t amd64_as_image_base;

    if (!amd64_as_trace_enabled())
        return false;

    if (amd64_as_image_base_pid != current->pid) {
        amd64_as_image_base = 0;
        if (!amd64_resolve_task_image_base(current, &amd64_as_image_base))
            return false;
        amd64_as_image_base_pid = current->pid;
    }

    qword_t state_addr = amd64_as_image_base + AMD64_AS_STATE_BLOCK_OFFSET;
    if (amd64_trace_intersects_guest_range(guest_addr, size, state_addr, AMD64_AS_STATE_TRACE_WINDOW)) {
        *region_base = state_addr;
        *region_kind = amd64_as_state_region_block;
        *region_index = 0;
        return true;
    }

    for (unsigned i = 0; i <= 4; i++) {
        uint64_t ptr = 0;
        if (!amd64_trace_read_current_guest(state_addr + 0x60 + (qword_t) i * 8, &ptr, sizeof(ptr)) || ptr == 0)
            continue;
        if (!amd64_trace_intersects_guest_range(guest_addr, size, ptr, AMD64_AS_DESCRIPTOR_SIZE))
            continue;
        *region_base = ptr;
        *region_kind = amd64_as_state_region_desc;
        *region_index = i;
        return true;
    }

    return false;
}

static void amd64_trace_as_state_write(struct cpu_state *cpu, qword_t guest_addr,
        const void *value, unsigned size) {
    qword_t region_base = 0;
    uint8_t region_kind = 0;
    uint8_t region_index = 0;
    if (!amd64_trace_as_state_region(guest_addr, size, &region_base, &region_kind, &region_index))
        return;

    if (amd64_as_state_write_pid != current->pid) {
        memset(amd64_as_state_writes, 0, sizeof(amd64_as_state_writes));
        amd64_as_state_write_next = 0;
        amd64_as_state_write_pid = current->pid;
    }

    struct amd64_as_state_write *entry =
        &amd64_as_state_writes[amd64_as_state_write_next++ % AMD64_AS_STATE_WRITE_COUNT];
    memset(entry, 0, sizeof(*entry));
    entry->rip = cpu->amd64_current_insn_rip;
    entry->addr = guest_addr;
    entry->region_base = region_base;
    entry->region_kind = region_kind;
    entry->region_index = region_index;
    entry->size = size;
    memcpy(&entry->value, value, size < sizeof(entry->value) ? size : sizeof(entry->value));

    if (region_kind == amd64_as_state_region_desc) {
        entry->snapshot_addr = region_base;
    } else {
        qword_t offset = guest_addr - region_base;
        entry->snapshot_addr = region_base + (offset & ~0xfULL);
    }

    if (amd64_trace_read_current_guest(entry->snapshot_addr, entry->bytes, sizeof(entry->bytes)))
        entry->byte_count = sizeof(entry->bytes);
}

void dump_amd64_cc1_trace(const struct cpu_state *cpu) {
    (void) cpu;
    if (current == NULL || current->abi != GUEST_ABI_AMD64 || strcmp(current->comm, "cc1") != 0)
        return;
    if (amd64_cc1_trace_pid != current->pid)
        return;

    unsigned total = amd64_cc1_trace_next;
    if (total == 0)
        return;

    unsigned count = total < AMD64_CC1_TRACE_COUNT ? total : AMD64_CC1_TRACE_COUNT;
    unsigned start = total >= AMD64_CC1_TRACE_COUNT ? total - AMD64_CC1_TRACE_COUNT : 0;
    printk("amd64 cc1 trace (%u entries):\n", count);
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_cc1_trace *trace = &amd64_cc1_trace[(start + i) % AMD64_CC1_TRACE_COUNT];
        printk("cc1[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
               i,
               (unsigned long long) trace->rip,
               (unsigned long long) trace->rax,
               (unsigned long long) trace->rbx,
               (unsigned long long) trace->rcx,
               (unsigned long long) trace->rdx,
               (unsigned long long) trace->rsi,
               (unsigned long long) trace->rdi,
               (unsigned long long) trace->rsp,
               (unsigned long long) trace->rbp,
               (unsigned long long) trace->r8,
               (unsigned long long) trace->r9,
               (unsigned long long) trace->r12);
        for (unsigned j = 0; j < trace->byte_count; j++)
            printk("%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
        printk("\n");
    }
}

void dump_amd64_as_trace_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;
    if (amd64_as_trace_pid != task->pid)
        return;

    if (amd64_as_state_write_pid == task->pid && amd64_as_state_write_next != 0) {
        unsigned total = amd64_as_state_write_next;
        unsigned count = total < AMD64_AS_STATE_WRITE_COUNT ? total : AMD64_AS_STATE_WRITE_COUNT;
        unsigned start = total >= AMD64_AS_STATE_WRITE_COUNT ? total - AMD64_AS_STATE_WRITE_COUNT : 0;
        printk("amd64 as state writes pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_state_write *entry =
                &amd64_as_state_writes[(start + i) % AMD64_AS_STATE_WRITE_COUNT];
            const char *kind = entry->region_kind == amd64_as_state_region_desc ? "desc" : "state";
            printk("as_state_write[%02u] kind=%s index=%u rip=%#llx addr=%#llx size=%u value=%#llx region=%#llx snapshot=%#llx",
                   i,
                   kind,
                   entry->region_index,
                   (unsigned long long) entry->rip,
                   (unsigned long long) entry->addr,
                   entry->size,
                   (unsigned long long) entry->value,
                   (unsigned long long) entry->region_base,
                   (unsigned long long) entry->snapshot_addr);
            if (entry->byte_count == 0) {
                printk(" bytes=?\n");
                continue;
            }
            printk(" bytes=");
            for (unsigned j = 0; j < entry->byte_count; j++)
                printk("%02x%s", entry->bytes[j], j + 1 == entry->byte_count ? "" : " ");
            printk("\n");
        }
    }

    if (amd64_as_event_pid == task->pid && amd64_as_event_next != 0) {
        unsigned total = amd64_as_event_next;
        unsigned count = total < AMD64_AS_EVENT_COUNT ? total : AMD64_AS_EVENT_COUNT;
        unsigned start = total >= AMD64_AS_EVENT_COUNT ? total - AMD64_AS_EVENT_COUNT : 0;
        printk("amd64 as state31 events pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_event *event = &amd64_as_events[(start + i) % AMD64_AS_EVENT_COUNT];
            const char *kind = "unknown";
            switch (event->kind) {
            case amd64_as_event_reset_done:
                kind = "reset_done";
                break;
            case amd64_as_event_state31_write:
                kind = "state31_write";
                break;
            case amd64_as_event_state31_check:
                kind = "state31_check";
                break;
            default:
                break;
            }
            printk("as_event[%02u] kind=%s rip=%#llx state31=%#x rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx\n",
                   i,
                   kind,
                   (unsigned long long) event->rip,
                   event->state31,
                   (unsigned long long) event->rax,
                   (unsigned long long) event->rbx,
                   (unsigned long long) event->rcx,
                   (unsigned long long) event->rdx,
                   (unsigned long long) event->rsi,
                   (unsigned long long) event->rdi,
                   (unsigned long long) event->r12);
        }
    }

    if (amd64_as_suspect_pid == task->pid && amd64_as_suspect_next != 0) {
        unsigned total = amd64_as_suspect_next;
        unsigned count = total < AMD64_AS_SUSPECT_COUNT ? total : AMD64_AS_SUSPECT_COUNT;
        unsigned start = total >= AMD64_AS_SUSPECT_COUNT ? total - AMD64_AS_SUSPECT_COUNT : 0;
        printk("amd64 as suspect ops pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_suspect *suspect =
                &amd64_as_suspects[(start + i) % AMD64_AS_SUSPECT_COUNT];
            switch (suspect->kind) {
            case amd64_as_suspect_bt:
                printk("as_suspect[%02u] kind=bt rip=%#llx op=%#x size=%u mem=%u index=%#llx bit=%#llx addr=%#llx lhs=%#llx value=%#llx aux=%#x\n",
                       i,
                       (unsigned long long) suspect->rip,
                       suspect->op,
                       suspect->size,
                       suspect->aux,
                       (unsigned long long) suspect->bit_index,
                       (unsigned long long) suspect->bit,
                       (unsigned long long) suspect->addr,
                       (unsigned long long) suspect->lhs,
                       (unsigned long long) suspect->value,
                       suspect->aux);
                break;
            case amd64_as_suspect_stack:
                printk("as_suspect[%02u] kind=stack rip=%#llx op=%u size=%u old_rsp=%#llx new_rsp=%#llx value=%#llx\n",
                       i,
                       (unsigned long long) suspect->rip,
                       suspect->op,
                       suspect->size,
                       (unsigned long long) suspect->old_rsp,
                       (unsigned long long) suspect->new_rsp,
                       (unsigned long long) suspect->value);
                break;
            default:
                break;
            }
        }
    }

    if (amd64_as_focus_pid == task->pid && amd64_as_focus_next != 0) {
        unsigned total = amd64_as_focus_next;
        unsigned count = total < AMD64_AS_FOCUS_COUNT ? total : AMD64_AS_FOCUS_COUNT;
        unsigned start = total >= AMD64_AS_FOCUS_COUNT ? total - AMD64_AS_FOCUS_COUNT : 0;
        printk("amd64 as focus pid=%d (%u entries):\n", task->pid, count);
        for (unsigned i = 0; i < count; i++) {
            const struct amd64_as_focus *focus =
                &amd64_as_focus[(start + i) % AMD64_AS_FOCUS_COUNT];
            qword_t dword_at_rdx = 0;
            bool have_dword_at_rdx = amd64_trace_read_task_guest(task, focus->rdx,
                    &dword_at_rdx, sizeof(uint32_t));
            char rbx_text[64];
            char rsi_text[64];
            char rdi_text[64];
            bool have_rbx_text = amd64_trace_read_task_guest_cstring(task, focus->rbx,
                    rbx_text, sizeof(rbx_text));
            bool have_rsi_text = amd64_trace_read_task_guest_cstring(task, focus->rsi,
                    rsi_text, sizeof(rsi_text));
            bool have_rdi_text = amd64_trace_read_task_guest_cstring(task, focus->rdi,
                    rdi_text, sizeof(rdi_text));
            printk("as_focus[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r12=%#llx%s%s%s%s\n",
                   i,
                   (unsigned long long) focus->rip,
                   (unsigned long long) focus->rax,
                   (unsigned long long) focus->rbx,
                   (unsigned long long) focus->rcx,
                   (unsigned long long) focus->rdx,
                   (unsigned long long) focus->rsi,
                   (unsigned long long) focus->rdi,
                   (unsigned long long) focus->rsp,
                   (unsigned long long) focus->rbp,
                   (unsigned long long) focus->r12,
                   have_dword_at_rdx ? "" : " [rdx]=?",
                   have_rbx_text ? "" : " rbx_str=?",
                   have_rsi_text ? "" : " rsi_str=?",
                   have_rdi_text ? "" : " rdi_str=?");
            if (have_dword_at_rdx)
                printk("as_focus[%02u] [rdx]=%#llx\n", i, (unsigned long long) dword_at_rdx);
            if (have_rbx_text)
                printk("as_focus[%02u] rbx_str=\"%s\"\n", i, rbx_text);
            if (have_rsi_text)
                printk("as_focus[%02u] rsi_str=\"%s\"\n", i, rsi_text);
            if (have_rdi_text)
                printk("as_focus[%02u] rdi_str=\"%s\"\n", i, rdi_text);
        }
    }

    unsigned total = amd64_as_trace_next;
    if (total == 0)
        return;

    unsigned count = total < AMD64_AS_TRACE_COUNT ? total : AMD64_AS_TRACE_COUNT;
    unsigned start = total >= AMD64_AS_TRACE_COUNT ? total - AMD64_AS_TRACE_COUNT : 0;

    bool have_error_window = false;
    unsigned window_first = 0;
    unsigned window_count = count;
    unsigned trigger_index = 0;
    qword_t trigger_rip = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct amd64_as_trace *trace = &amd64_as_trace[(start + i) % AMD64_AS_TRACE_COUNT];
        if (!amd64_as_is_error_path_rip(trace->rip))
            continue;

        unsigned pre = i < AMD64_AS_ERROR_PRE_COUNT ? i : AMD64_AS_ERROR_PRE_COUNT;
        unsigned post = count - i;
        if (post > AMD64_AS_ERROR_POST_COUNT)
            post = AMD64_AS_ERROR_POST_COUNT;
        window_first = i - pre;
        window_count = pre + post;
        trigger_index = i;
        trigger_rip = trace->rip;
        have_error_window = true;
        break;
    }

    if (have_error_window) {
        printk("amd64 as trace pid=%d (%u entries, showing %u around error path rip=%#llx at trace index=%u):\n",
               task->pid,
               count,
               window_count,
               (unsigned long long) trigger_rip,
               trigger_index);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as trace pid=%d (%u entries, showing %u around error path rip=%#llx at trace index=%u):\n",
                    task->pid,
                    count,
                    window_count,
                    (unsigned long long) trigger_rip,
                    trigger_index);
        }
    } else {
        printk("amd64 as trace pid=%d (%u entries):\n", task->pid, count);
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "amd64 as trace pid=%d (%u entries):\n", task->pid, count);
    }

    for (unsigned i = 0; i < window_count; i++) {
        unsigned trace_index = window_first + i;
        const struct amd64_as_trace *trace =
            &amd64_as_trace[(start + trace_index) % AMD64_AS_TRACE_COUNT];
        printk("as[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
               trace_index,
               (unsigned long long) trace->rip,
               (unsigned long long) trace->rax,
               (unsigned long long) trace->rbx,
               (unsigned long long) trace->rcx,
               (unsigned long long) trace->rdx,
               (unsigned long long) trace->rsi,
               (unsigned long long) trace->rdi,
               (unsigned long long) trace->rsp,
               (unsigned long long) trace->rbp,
               (unsigned long long) trace->r8,
               (unsigned long long) trace->r9,
               (unsigned long long) trace->r12);
        for (unsigned j = 0; j < trace->byte_count; j++)
            printk("%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
        printk("\n");
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "as[%02u] rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rsp=%#llx rbp=%#llx r8=%#llx r9=%#llx r12=%#llx bytes=",
                    trace_index,
                    (unsigned long long) trace->rip,
                    (unsigned long long) trace->rax,
                    (unsigned long long) trace->rbx,
                    (unsigned long long) trace->rcx,
                    (unsigned long long) trace->rdx,
                    (unsigned long long) trace->rsi,
                    (unsigned long long) trace->rdi,
                    (unsigned long long) trace->rsp,
                    (unsigned long long) trace->rbp,
                    (unsigned long long) trace->r8,
                    (unsigned long long) trace->r9,
                    (unsigned long long) trace->r12);
            for (unsigned j = 0; j < trace->byte_count; j++)
                fprintf(stderr, "%02x%s", trace->bytes[j], j + 1 == trace->byte_count ? "" : " ");
            fprintf(stderr, "\n");
        }
    }
}

void dump_amd64_as_state_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;

    qword_t image_base = 0;
    if (!amd64_resolve_task_image_base(task, &image_base)) {
        printk("amd64 as state: failed to resolve image base\n");
        if (amd64_as_stderr_enabled())
            fprintf(stderr, "amd64 as state: failed to resolve image base\n");
        return;
    }

    qword_t state_addr = image_base + AMD64_AS_STATE_BLOCK_OFFSET;
    uint8_t state[AMD64_AS_STATE_DUMP_SIZE] = {};
    if (!amd64_trace_read_task_guest(task, state_addr, state, sizeof(state))) {
        printk("amd64 as state: image_base=%#llx state=%#llx unreadable\n",
               (unsigned long long) image_base,
               (unsigned long long) state_addr);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr, "amd64 as state: image_base=%#llx state=%#llx unreadable\n",
                    (unsigned long long) image_base,
                    (unsigned long long) state_addr);
        }
        return;
    }

    uint32_t slot_f8 = 0;
    uint8_t slot_fc = 0;
    uint8_t slot_100 = 0;
    uint32_t slot_124 = 0;
    uint32_t slot_104 = 0;
    uint32_t slot_108 = 0;
    uint32_t slot_10c = 0;
    uint32_t slot_110 = 0;
    uint32_t slot_114 = 0;
    uint32_t slot_118 = 0;
    uint64_t slot_b0 = 0;
    uint64_t slot_b8 = 0;
    uint64_t slot_c0 = 0;
    uint64_t slot_c8 = 0;
    uint64_t slot_128 = 0;
    bool have_f8 = amd64_trace_read_task_guest(task, state_addr + 0xf8, &slot_f8, sizeof(slot_f8));
    bool have_fc = amd64_trace_read_task_guest(task, state_addr + 0xfc, &slot_fc, sizeof(slot_fc));
    bool have_100 = amd64_trace_read_task_guest(task, state_addr + 0x100, &slot_100, sizeof(slot_100));
    bool have_104 = amd64_trace_read_task_u32(task, state_addr + 0x104, &slot_104);
    bool have_108 = amd64_trace_read_task_u32(task, state_addr + 0x108, &slot_108);
    bool have_10c = amd64_trace_read_task_u32(task, state_addr + 0x10c, &slot_10c);
    bool have_110 = amd64_trace_read_task_u32(task, state_addr + 0x110, &slot_110);
    bool have_114 = amd64_trace_read_task_u32(task, state_addr + 0x114, &slot_114);
    bool have_118 = amd64_trace_read_task_u32(task, state_addr + 0x118, &slot_118);
    bool have_124 = amd64_trace_read_task_guest(task, state_addr + 0x124, &slot_124, sizeof(slot_124));
    bool have_b0 = amd64_trace_read_task_u64(task, state_addr + 0xb0, &slot_b0);
    bool have_b8 = amd64_trace_read_task_u64(task, state_addr + 0xb8, &slot_b8);
    bool have_c0 = amd64_trace_read_task_u64(task, state_addr + 0xc0, &slot_c0);
    bool have_c8 = amd64_trace_read_task_u64(task, state_addr + 0xc8, &slot_c8);
    bool have_128 = amd64_trace_read_task_u64(task, state_addr + 0x128, &slot_128);

    printk("amd64 as state: image_base=%#llx state=%#llx op=%02x%02x mode=%#x flags=%#x extra=%#x state31=%#x%s%s%s%s\n",
           (unsigned long long) image_base,
           (unsigned long long) state_addr,
           state[5],
           state[4],
           state[6],
           state[8],
           state[0xb],
           state[0x31],
           have_f8 ? "" : " f8=?",
           have_fc ? "" : " fc=?",
           have_100 ? "" : " 100=?",
           have_124 ? "" : " 124=?");
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr,
                "amd64 as state: image_base=%#llx state=%#llx op=%02x%02x mode=%#x flags=%#x extra=%#x state31=%#x%s%s%s%s\n",
                (unsigned long long) image_base,
                (unsigned long long) state_addr,
                state[5],
                state[4],
                state[6],
                state[8],
                state[0xb],
                state[0x31],
                have_f8 ? "" : " f8=?",
                have_fc ? "" : " fc=?",
                have_100 ? "" : " 100=?",
                have_124 ? "" : " 124=?");
    }
    if (have_f8 || have_fc || have_100 || have_124) {
        printk("amd64 as state ext: slot_f8=%#x slot_fc=%#x slot_100=%#x slot_124=%#x\n",
               have_f8 ? slot_f8 : 0,
               have_fc ? slot_fc : 0,
               have_100 ? slot_100 : 0,
               have_124 ? slot_124 : 0);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as state ext: slot_f8=%#x slot_fc=%#x slot_100=%#x slot_124=%#x\n",
                    have_f8 ? slot_f8 : 0,
                    have_fc ? slot_fc : 0,
                    have_100 ? slot_100 : 0,
                    have_124 ? slot_124 : 0);
        }
    }
    if (have_104 || have_108 || have_10c || have_110 || have_114 || have_118) {
        printk("amd64 as state ext2: slot_104=%#x slot_108=%#x slot_10c=%#x slot_110=%#x slot_114=%#x slot_118=%#x\n",
               have_104 ? slot_104 : 0,
               have_108 ? slot_108 : 0,
               have_10c ? slot_10c : 0,
               have_110 ? slot_110 : 0,
               have_114 ? slot_114 : 0,
               have_118 ? slot_118 : 0);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as state ext2: slot_104=%#x slot_108=%#x slot_10c=%#x slot_110=%#x slot_114=%#x slot_118=%#x\n",
                    have_104 ? slot_104 : 0,
                    have_108 ? slot_108 : 0,
                    have_10c ? slot_10c : 0,
                    have_110 ? slot_110 : 0,
                    have_114 ? slot_114 : 0,
                    have_118 ? slot_118 : 0);
        }
    }
    if (have_b0 || have_b8 || have_c0 || have_c8 || have_128) {
        printk("amd64 as ptrs: slot_b0=%#llx slot_b8=%#llx slot_c0=%#llx slot_c8=%#llx slot_128=%#llx\n",
               (unsigned long long) (have_b0 ? slot_b0 : 0),
               (unsigned long long) (have_b8 ? slot_b8 : 0),
               (unsigned long long) (have_c0 ? slot_c0 : 0),
               (unsigned long long) (have_c8 ? slot_c8 : 0),
               (unsigned long long) (have_128 ? slot_128 : 0));
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr,
                    "amd64 as ptrs: slot_b0=%#llx slot_b8=%#llx slot_c0=%#llx slot_c8=%#llx slot_128=%#llx\n",
                    (unsigned long long) (have_b0 ? slot_b0 : 0),
                    (unsigned long long) (have_b8 ? slot_b8 : 0),
                    (unsigned long long) (have_c0 ? slot_c0 : 0),
                    (unsigned long long) (have_c8 ? slot_c8 : 0),
                    (unsigned long long) (have_128 ? slot_128 : 0));
        }
    }

    for (unsigned i = 0; i < sizeof(state); i += 16) {
        printk("amd64 as state[%02x]: "
               "%02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x\n",
               i,
               state[i + 0], state[i + 1], state[i + 2], state[i + 3],
               state[i + 4], state[i + 5], state[i + 6], state[i + 7],
               state[i + 8], state[i + 9], state[i + 10], state[i + 11],
               state[i + 12], state[i + 13], state[i + 14], state[i + 15]);
    }

    for (unsigned i = 0; i <= 4; i++) {
        uint32_t slot_1c = 0, slot_48 = 0, slot_88 = 0, slot_9c = 0;
        uint64_t ptr = 0;
        bool have_1c = amd64_trace_read_task_u32(task, state_addr + 0x1c + (qword_t) i * 4, &slot_1c);
        bool have_48 = amd64_trace_read_task_u32(task, state_addr + 0x48 + (qword_t) i * 4, &slot_48);
        bool have_88 = amd64_trace_read_task_u32(task, state_addr + 0x88 + (qword_t) i * 4, &slot_88);
        bool have_9c = amd64_trace_read_task_u32(task, state_addr + 0x9c + (qword_t) i * 4, &slot_9c);
        bool have_ptr = amd64_trace_read_task_u64(task, state_addr + 0x60 + (qword_t) i * 8, &ptr);
        amd64_dump_as_descriptor_task(task, i, have_ptr ? ptr : 0,
                                      have_1c ? slot_1c : 0,
                                      have_48 ? slot_48 : 0,
                                      have_88 ? slot_88 : 0,
                                      have_9c ? slot_9c : 0);
    }
}

void dump_amd64_as_stack_task(const struct task *task) {
    if (task == NULL || task->abi != GUEST_ABI_AMD64 || strcmp(task->comm, "as") != 0)
        return;

    const struct cpu_state *cpu = &task->cpu;
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    qword_t rbp = cpu->amd64_regs[amd64_rbp];

    printk("amd64 as stack pid=%d rip=%#llx rsp=%#llx rbp=%#llx\n",
           task->pid,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) rsp,
           (unsigned long long) rbp);
    if (amd64_as_stderr_enabled()) {
        fprintf(stderr, "amd64 as stack pid=%d rip=%#llx rsp=%#llx rbp=%#llx\n",
                task->pid,
                (unsigned long long) cpu->amd64_rip,
                (unsigned long long) rsp,
                (unsigned long long) rbp);
    }

    for (unsigned i = 0; i < 12; i++) {
        qword_t addr = rsp + (qword_t) i * 8;
        qword_t value = 0;
        if (amd64_trace_read_task_guest(task, addr, &value, sizeof(value))) {
            printk("as stack[%02u] addr=%#llx value=%#llx%s\n",
                   i,
                   (unsigned long long) addr,
                   (unsigned long long) value,
                   addr == rbp ? " <rbp>" : "");
            if (amd64_as_stderr_enabled()) {
                fprintf(stderr, "as stack[%02u] addr=%#llx value=%#llx%s\n",
                        i,
                        (unsigned long long) addr,
                        (unsigned long long) value,
                        addr == rbp ? " <rbp>" : "");
            }
        } else {
            printk("as stack[%02u] addr=%#llx unreadable%s\n",
                   i,
                   (unsigned long long) addr,
                   addr == rbp ? " <rbp>" : "");
            if (amd64_as_stderr_enabled()) {
                fprintf(stderr, "as stack[%02u] addr=%#llx unreadable%s\n",
                        i,
                        (unsigned long long) addr,
                        addr == rbp ? " <rbp>" : "");
            }
        }
    }

    qword_t frame = rbp;
    for (unsigned depth = 0; depth < 4; depth++) {
        qword_t next_rbp = 0;
        qword_t return_rip = 0;
        if (!amd64_trace_read_task_guest(task, frame, &next_rbp, sizeof(next_rbp)) ||
                !amd64_trace_read_task_guest(task, frame + 8, &return_rip, sizeof(return_rip))) {
            printk("as frame[%u] rbp=%#llx unreadable\n",
                   depth, (unsigned long long) frame);
            if (amd64_as_stderr_enabled())
                fprintf(stderr, "as frame[%u] rbp=%#llx unreadable\n",
                        depth, (unsigned long long) frame);
            break;
        }
        printk("as frame[%u] rbp=%#llx next=%#llx ret=%#llx\n",
               depth,
               (unsigned long long) frame,
               (unsigned long long) next_rbp,
               (unsigned long long) return_rip);
        if (amd64_as_stderr_enabled()) {
            fprintf(stderr, "as frame[%u] rbp=%#llx next=%#llx ret=%#llx\n",
                    depth,
                    (unsigned long long) frame,
                    (unsigned long long) next_rbp,
                    (unsigned long long) return_rip);
        }
        if (next_rbp <= frame || (next_rbp & 7) != 0)
            break;
        frame = next_rbp;
    }
}

// Legacy (non-VEX) encodings can only name xmm0-15 even with REX, so this
// stays at 16 even though cpu_state now carries 32 slots for EVEX.
#define AMD64_XMM_COUNT 16u

static inline qword_t amd64_cvtt_scalar_to_int(double value, bool wide) {
    if (isnan(value))
        return wide ? (qword_t) INT64_MIN : (qword_t) (uint32_t) INT32_MIN;
    if (wide) {
        if (value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return (qword_t) INT64_MIN;
        return (qword_t) (sqword_t) value;
    }
    if (value < (double) INT32_MIN || value >= 2147483648.0)
        return (qword_t) (uint32_t) INT32_MIN;
    return (qword_t) (uint32_t) (int32_t) value;
}

// CVTSD2SI/CVTSS2SI (0F 2D): like the truncating cvtt form above, but rounds to
// the nearest integer using the current rounding mode. iSH does not model the
// MXCSR rounding-control bits, so we use rint(), which honors the host FP
// rounding mode -- round-to-nearest-even, the x86/SSE default. Out-of-range and
// NaN inputs yield the "integer indefinite" value, exactly like the cvtt form.
static inline qword_t amd64_cvt_scalar_to_int(double value, bool wide) {
    if (isnan(value))
        return wide ? (qword_t) INT64_MIN : (qword_t) (uint32_t) INT32_MIN;
    double rounded = rint(value);
    if (wide) {
        if (rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0)
            return (qword_t) INT64_MIN;
        return (qword_t) (sqword_t) rounded;
    }
    if (rounded < (double) INT32_MIN || rounded >= 2147483648.0)
        return (qword_t) (uint32_t) INT32_MIN;
    return (qword_t) (uint32_t) (int32_t) rounded;
}

static inline void amd64_set_fp_compare_flags(struct cpu_state *cpu, int cmp_result, bool unordered) {
    cpu->of = 0;
    cpu->sf = 0;
    cpu->af = 0;
    cpu->af_ops = 0;
    if (unordered) {
        cpu->zf = 1;
        cpu->pf = 1;
        cpu->cf = 1;
    } else if (cmp_result < 0) {
        cpu->zf = 0;
        cpu->pf = 0;
        cpu->cf = 1;
    } else if (cmp_result == 0) {
        cpu->zf = 1;
        cpu->pf = 0;
        cpu->cf = 0;
    } else {
        cpu->zf = 0;
        cpu->pf = 0;
        cpu->cf = 0;
    }
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline bool amd64_trace_intersects_busybox_slot(qword_t guest_addr, unsigned size) {
    if (size == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t slot_start = AMD64_BUSYBOX_INIT_SLOT;
    qword_t slot_end = slot_start + AMD64_BUSYBOX_INIT_SLOT_SIZE;
    return start < slot_end && end > slot_start;
}

static inline void amd64_busybox_watch_addr(qword_t guest_addr) {
    if (guest_addr == 0)
        return;
    for (unsigned i = 0; i < AMD64_BUSYBOX_INIT_WATCH_COUNT; i++) {
        if (amd64_busybox_init_watch[i] == guest_addr)
            return;
    }
    amd64_busybox_init_watch[amd64_busybox_init_watch_next++ % AMD64_BUSYBOX_INIT_WATCH_COUNT] = guest_addr;
}

static inline bool amd64_trace_intersects_busybox_watch(qword_t guest_addr, unsigned size,
        qword_t *base_out, qword_t *offset_out) {
    if (size == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    for (unsigned i = 0; i < AMD64_BUSYBOX_INIT_WATCH_COUNT; i++) {
        qword_t base = amd64_busybox_init_watch[i];
        if (base == 0)
            continue;
        qword_t watch_end = base + AMD64_BUSYBOX_INIT_WATCH_SPAN;
        if (start < watch_end && end > base) {
            if (base_out != NULL)
                *base_out = base;
            if (offset_out != NULL)
                *offset_out = start - base;
            return true;
        }
    }
    return false;
}

static inline bool amd64_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out) {
    if (!guest_abi_range_valid(GUEST_ABI_AMD64, guest_addr, size))
        return false;
    *addr_out = guest_addr;
    return true;
}

static int amd64_bad_transfer_target(struct cpu_state *cpu, struct tlb *tlb,
        qword_t from, qword_t target, const char *kind) {
    (void) tlb;
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    printk("[amd64-jit] bad-%s-target from=%#llx target=%#llx rsp=%#llx\n",
           kind,
           (unsigned long long) from,
           (unsigned long long) target,
           (unsigned long long) rsp);
    if (getenv("ISH_TRACE_GUEST_FATAL") != NULL ||
            getenv("ISH_TRACE_AMD64_AS_STDERR") != NULL) {
        fprintf(stderr, "[amd64-jit] bad-%s-target from=%#llx target=%#llx rsp=%#llx\n",
                kind,
                (unsigned long long) from,
                (unsigned long long) target,
                (unsigned long long) rsp);
    }
    if (strcmp(kind, "ret") == 0 && rsp >= 8) {
        qword_t slot_addr = rsp - 8;
        uint8_t slot_bytes[8] = {};
        bool have_slot = amd64_mem_read_direct(slot_addr, slot_bytes, sizeof(slot_bytes));
        printk("[amd64-jit] bad-ret-target-generic slot-addr=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               (unsigned long long) slot_addr,
               have_slot ? "" : "unreadable ",
               slot_bytes[0], slot_bytes[1], slot_bytes[2], slot_bytes[3],
               slot_bytes[4], slot_bytes[5], slot_bytes[6], slot_bytes[7]);
        if (current != NULL) {
            amd64_dump_recent_suspects_for_stack_slot(current->pid, slot_addr,
                    "bad-ret-target-generic");
            amd64_dump_recent_suspects(current->pid, "bad-ret-target-generic");
        }
    }
    cpu->amd64_rip = target;
    return INT_GPF;
}

static inline int amd64_validate_transfer_target(struct cpu_state *cpu, struct tlb *tlb,
        qword_t from, qword_t target, const char *kind) {
    guest_addr_t checked_target;
    if (!amd64_guest_addr_ok(target, 1, &checked_target))
        return amd64_bad_transfer_target(cpu, tlb, from, target, kind);
    return INT_NONE;
}

static inline qword_t amd64_mask(unsigned size) {
    switch (size) {
    case 8: return 0xff;
    case 16: return 0xffff;
    case 32: return 0xffffffffu;
    case 64: return ~0ull;
    default: return 0;
    }
}

static inline qword_t amd64_sign_bit(unsigned size) {
    return 1ull << (size - 1);
}

static inline qword_t amd64_trunc(qword_t value, unsigned size) {
    return value & amd64_mask(size);
}

static inline qword_t amd64_rdtsc_value(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (qword_t) now.tv_sec * 1000000000ull + (qword_t) now.tv_nsec;
}

static inline sqword_t amd64_sign_extend(qword_t value, unsigned size) {
    qword_t masked = amd64_trunc(value, size);
    if ((masked & amd64_sign_bit(size)) == 0)
        return (sqword_t) masked;
    return (sqword_t) (masked | ~amd64_mask(size));
}

static inline void amd64_sync_legacy_regs(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static inline void amd64_trace_suspicious_rsp_write(struct cpu_state *cpu,
        qword_t old_rsp, qword_t new_rsp, unsigned size) {
    if (new_rsp >= 0x1000)
        return;
    printk("amd64 rsp write: rip=%#llx old=%#llx new=%#llx size=%u\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_rsp,
           (unsigned long long) new_rsp,
           size);
}

static inline void amd64_trace_cargo_r12_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_r12_trace_count >= 32)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_r12_trace_count++;
    printk("amd64 cargo r12 write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 cargo r12 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }
}

static inline void amd64_trace_cargo_rdx_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_rdx_trace_count >= 64)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_rdx_trace_count++;
    printk("amd64 cargo rdx write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 cargo rdx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }

}

static inline void amd64_trace_cargo_rdi_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    guest_addr_t guest_addr;
    bool current_ip_in_window;
    uint8_t insn_bytes[16] = {};
    bool have_bytes = false;

    if (amd64_cargo_rdi_trace_count >= 64)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;

    current_ip_in_window = cpu->amd64_current_insn_rip >= AMD64_CARGO_R12_TRACE_WINDOW_START &&
                           cpu->amd64_current_insn_rip < AMD64_CARGO_R12_TRACE_WINDOW_END;
    if (!current_ip_in_window)
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &guest_addr) &&
            current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, guest_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    amd64_cargo_rdi_trace_count++;
    printk("amd64 cargo rdi write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rsi=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    if (have_bytes) {
        printk("amd64 cargo rdi bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7],
               insn_bytes[8], insn_bytes[9], insn_bytes[10], insn_bytes[11],
               insn_bytes[12], insn_bytes[13], insn_bytes[14], insn_bytes[15]);
    }
}

static inline void amd64_trace_htop_r13_write(struct cpu_state *cpu,
        qword_t old_value, qword_t new_value, unsigned size, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_R13_CORRUPT_WRITE_RIP)
        return;

    uint8_t insn_bytes[8] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) cpu->amd64_current_insn_rip, MEM_READ);
        if (ptr != NULL) {
            memcpy(insn_bytes, ptr, sizeof(insn_bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop r13 write: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rbp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    printk("amd64 htop r13 regs: rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r14=%#llx r15=%#llx%s%s\n",
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r10],
           (unsigned long long) cpu->amd64_regs[amd64_r11],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15],
           have_bytes ? " bytes=" : "",
           have_bytes ? "" : "");
    if (have_bytes) {
        printk("amd64 htop r13 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
    }
}

static inline void amd64_trace_htop_r13_source(struct cpu_state *cpu, qword_t addr, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_R13_CORRUPT_WRITE_RIP)
        return;

    qword_t base = cpu->amd64_regs[amd64_rbx];
    uint8_t bytes[32] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) base, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop r13 src: rip=%#llx base=%#llx addr=%#llx value=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) base,
           (unsigned long long) addr,
           (unsigned long long) value);
    if (have_bytes) {
        printk("amd64 htop r13 mem: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        printk("amd64 htop r13 mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
               bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
    }
}

static inline bool amd64_trace_in_htop_window(qword_t rip) {
    if (!amd64_htop_legacy_trace_enabled)
        return false;
    return rip >= AMD64_HTOP_TRACE_WINDOW_START && rip < AMD64_HTOP_TRACE_WINDOW_END;
}

static inline bool amd64_trace_intersects_watch_addr(qword_t guest_addr, unsigned size,
        qword_t watch_addr, unsigned watch_size) {
    if (size == 0 || watch_size == 0 || watch_addr == 0)
        return false;
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t watch_end = watch_addr + watch_size;
    return start < watch_end && end > watch_addr;
}

static inline void amd64_trace_htop_window(struct cpu_state *cpu, struct tlb *tlb) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_in_htop_window(cpu->amd64_current_insn_rip))
        return;
    if (cpu->amd64_current_insn_rip == AMD64_HTOP_RBX_LOAD_RIP && cpu->amd64_regs[amd64_rdi] != 0)
        amd64_htop_watch_field_addr = cpu->amd64_regs[amd64_rdi] + AMD64_HTOP_RBX_FIELD_OFFSET;

    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;
    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    printk("amd64 htop win: rip=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx r12=%#llx r13=%#llx r15=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 htop win bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_in_cargo_pf_window(qword_t rip) {
    return rip >= AMD64_CARGO_PFWIN_WINDOW_START && rip < AMD64_CARGO_PFWIN_WINDOW_END;
}

static inline void amd64_trace_cargo_pf_window(struct cpu_state *cpu, struct tlb *tlb) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (current == NULL)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (!amd64_trace_in_cargo_pf_window(cpu->amd64_current_insn_rip))
        return;

    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    printk("amd64 cargo pfwin: rip=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx r8=%#llx r9=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo pfwin bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_transfer(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, qword_t target, const char *kind) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (amd64_cargo_xfer_trace_count >= 32)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (target != AMD64_CARGO_ENTRY_RIP)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cargo_xfer_trace_count++;
    printk("amd64 cargo xfer: kind=%s from=%#llx to=%#llx rsp=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r15=%#llx\n",
           kind,
           (unsigned long long) saved_rip,
           (unsigned long long) target,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo xfer bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_predecessor(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip) {
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    guest_addr_t insn_addr;

    if (amd64_cargo_xfer_trace_count >= 1)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (saved_rip == AMD64_CARGO_ENTRY_RIP || cpu->amd64_rip != AMD64_CARGO_ENTRY_RIP)
        return;

    if (amd64_guest_addr_ok(saved_rip, sizeof(bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, bytes, sizeof(bytes))) {
        have_bytes = true;
    }

    amd64_cargo_xfer_trace_count++;
    printk("amd64 cargo prev: from=%#llx to=%#llx rsp=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r15=%#llx\n",
           (unsigned long long) saved_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_r15]);
    if (have_bytes) {
        printk("amd64 cargo prev bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_cargo_start_call(struct cpu_state *cpu) {
    guest_addr_t stack_addr;
    guest_addr_t bytes_addr;
    uint8_t bytes[16] = {};
    bool have_bytes = false;
    qword_t popped = 0;
    qword_t next0 = 0;
    qword_t next1 = 0;

    if (amd64_cargo_start_call_trace_count >= 1)
        return;
    if (!amd64_cargo_trace_enabled)
        return;
    if (current == NULL)
        return;
    if (strcmp(current->comm, "cargo") != 0 && strcmp(current->comm, "gcc") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_CARGO_START_CALL_RIP)
        return;
    if (current->mem == NULL)
        return;

    if (cpu->amd64_current_insn_rip >= 5 &&
            amd64_guest_addr_ok(cpu->amd64_current_insn_rip - 5, sizeof(bytes), &bytes_addr)) {
        void *ptr = mem_ptr(current->mem, bytes_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp] - 8, sizeof(popped), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&popped, ptr, sizeof(popped));
    }
    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp], sizeof(next0), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&next0, ptr, sizeof(next0));
    }
    if (amd64_guest_addr_ok(cpu->amd64_regs[amd64_rsp] + 8, sizeof(next1), &stack_addr)) {
        void *ptr = mem_ptr(current->mem, stack_addr, MEM_READ);
        if (ptr != NULL)
            memcpy(&next1, ptr, sizeof(next1));
    }

    amd64_cargo_start_call_trace_count++;
    printk("amd64 cargo startcall: rip=%#llx r9=%#llx rdi=%#llx rsp=%#llx popped=%#llx next0=%#llx next1=%#llx rsi=%#llx rdx=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_rdi],
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) popped,
           (unsigned long long) next0,
           (unsigned long long) next1,
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdx]);
    if (have_bytes) {
        printk("amd64 cargo startcall bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3],
               bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11],
               bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline void amd64_trace_htop_store_history(struct cpu_state *cpu, qword_t watch_addr) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    unsigned total = cpu->amd64_store_trace_next;
    if (total > AMD64_STORE_TRACE_COUNT)
        total = AMD64_STORE_TRACE_COUNT;

    unsigned reported = 0;
    for (unsigned i = 0; i < total && reported < 6; i++) {
        unsigned seq = cpu->amd64_store_trace_next - 1 - i;
        struct amd64_store_trace entry =
                cpu->amd64_store_trace[seq % AMD64_STORE_TRACE_COUNT];
        if (entry.addr != watch_addr)
            continue;
        printk("amd64 htop rbx store%u: rip=%#llx opcode=%#x addr=%#llx value=%#llx\n",
               reported,
               (unsigned long long) entry.rip,
               entry.opcode,
               (unsigned long long) entry.addr,
               (unsigned long long) entry.value);
        reported++;
    }
}

static inline void amd64_trace_htop_rbx_source(struct cpu_state *cpu, qword_t base, qword_t addr, qword_t value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (cpu->amd64_current_insn_rip != AMD64_HTOP_RBX_LOAD_RIP)
        return;

    uint8_t bytes[64] = {};
    bool have_bytes = false;
    qword_t dump_addr = addr >= 0x10 ? addr - 0x10 : addr;
    uint8_t pointee[128] = {};
    bool have_pointee = false;
    qword_t pointee_addr = value >= 0x50 ? value - 0x50 : value;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) dump_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
        if (value != 0) {
            ptr = mem_ptr(current->mem, (addr_t) pointee_addr, MEM_READ);
            if (ptr != NULL) {
                memcpy(pointee, ptr, sizeof(pointee));
                have_pointee = true;
            }
        }
    }

    printk("amd64 htop rbx src: rip=%#llx base=%#llx addr=%#llx value=%#llx rsp=%#llx r12=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) base,
           (unsigned long long) addr,
           (unsigned long long) value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_r12]);
    amd64_trace_htop_store_history(cpu, addr);
    if (have_bytes) {
        printk("amd64 htop rbx obj0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        printk("amd64 htop rbx obj1: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
               bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
        printk("amd64 htop rbx obj2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
               bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
        printk("amd64 htop rbx obj3: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[48], bytes[49], bytes[50], bytes[51], bytes[52], bytes[53], bytes[54], bytes[55],
               bytes[56], bytes[57], bytes[58], bytes[59], bytes[60], bytes[61], bytes[62], bytes[63]);
    }
    if (have_pointee) {
        printk("amd64 htop rbx mem0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[0], pointee[1], pointee[2], pointee[3], pointee[4], pointee[5], pointee[6], pointee[7],
               pointee[8], pointee[9], pointee[10], pointee[11], pointee[12], pointee[13], pointee[14], pointee[15]);
        printk("amd64 htop rbx mem1: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[16], pointee[17], pointee[18], pointee[19], pointee[20], pointee[21], pointee[22], pointee[23],
               pointee[24], pointee[25], pointee[26], pointee[27], pointee[28], pointee[29], pointee[30], pointee[31]);
        printk("amd64 htop rbx mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[32], pointee[33], pointee[34], pointee[35], pointee[36], pointee[37], pointee[38], pointee[39],
               pointee[40], pointee[41], pointee[42], pointee[43], pointee[44], pointee[45], pointee[46], pointee[47]);
        printk("amd64 htop rbx mem3: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[48], pointee[49], pointee[50], pointee[51], pointee[52], pointee[53], pointee[54], pointee[55],
               pointee[56], pointee[57], pointee[58], pointee[59], pointee[60], pointee[61], pointee[62], pointee[63]);
        printk("amd64 htop rbx mem4: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[64], pointee[65], pointee[66], pointee[67], pointee[68], pointee[69], pointee[70], pointee[71],
               pointee[72], pointee[73], pointee[74], pointee[75], pointee[76], pointee[77], pointee[78], pointee[79]);
        printk("amd64 htop rbx mem5: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[80], pointee[81], pointee[82], pointee[83], pointee[84], pointee[85], pointee[86], pointee[87],
               pointee[88], pointee[89], pointee[90], pointee[91], pointee[92], pointee[93], pointee[94], pointee[95]);
        printk("amd64 htop rbx mem6: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[96], pointee[97], pointee[98], pointee[99], pointee[100], pointee[101], pointee[102], pointee[103],
               pointee[104], pointee[105], pointee[106], pointee[107], pointee[108], pointee[109], pointee[110], pointee[111]);
        printk("amd64 htop rbx mem7: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pointee[112], pointee[113], pointee[114], pointee[115], pointee[116], pointee[117], pointee[118], pointee[119],
               pointee[120], pointee[121], pointee[122], pointee[123], pointee[124], pointee[125], pointee[126], pointee[127]);
    }
}

static inline void amd64_trace_htop_rbx_base(struct cpu_state *cpu, qword_t old_value,
        qword_t new_value, unsigned size, qword_t raw_value) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (new_value != AMD64_HTOP_R13_CORRUPT_BLOCK_BASE)
        return;

    uint8_t bytes[16] = {};
    bool have_bytes = false;
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) cpu->amd64_current_insn_rip, MEM_READ);
        if (ptr != NULL) {
            memcpy(bytes, ptr, sizeof(bytes));
            have_bytes = true;
        }
    }

    printk("amd64 htop rbx base: rip=%#llx old=%#llx new=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) old_value,
           (unsigned long long) new_value,
           size,
           (unsigned long long) raw_value,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_bytes) {
        printk("amd64 htop rbx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
}

static inline bool amd64_trace_intersects_htop_r13_block(qword_t guest_addr, unsigned size) {
    qword_t start = guest_addr;
    qword_t end = guest_addr + size;
    qword_t watch_start = AMD64_HTOP_R13_CORRUPT_BLOCK_BASE;
    qword_t watch_end = watch_start + AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE;
    return start < watch_end && end > watch_start;
}

static inline void amd64_trace_htop_field_write(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, const void *value, unsigned size) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_intersects_watch_addr(guest_addr, size, amd64_htop_watch_field_addr, AMD64_HTOP_RBX_FIELD_SIZE))
        return;

    qword_t observed = 0;
    uint8_t field[AMD64_HTOP_RBX_FIELD_SIZE] = {};
    bool have_field = false;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));

    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) amd64_htop_watch_field_addr, MEM_READ);
        if (ptr != NULL) {
            memcpy(field, ptr, sizeof(field));
            have_field = true;
        }
    }

    if (cpu->amd64_current_insn_rip == AMD64_HTOP_FIELD_FILL_RIP) {
        uint8_t insn[16] = {};
        bool have_insn = false;
        guest_addr_t insn_addr;
        if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn), &insn_addr) &&
                tlb_read(tlb, insn_addr, insn, sizeof(insn))) {
            have_insn = true;
        }
        printk("amd64 htop fill: rip=%#llx next=%#llx addr=%#llx size=%u rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               (unsigned long long) cpu->amd64_regs[amd64_rax],
               (unsigned long long) cpu->amd64_regs[amd64_rbx],
               (unsigned long long) cpu->amd64_regs[amd64_rcx],
               (unsigned long long) cpu->amd64_regs[amd64_rdx],
               (unsigned long long) cpu->amd64_regs[amd64_rsi],
               (unsigned long long) cpu->amd64_regs[amd64_rdi],
               (unsigned long long) cpu->amd64_regs[amd64_r12],
               (unsigned long long) cpu->amd64_regs[amd64_r13],
               (unsigned long long) cpu->amd64_regs[amd64_r14],
               (unsigned long long) cpu->amd64_regs[amd64_r15]);
        if (have_insn) {
            printk("amd64 htop fill bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   insn[0], insn[1], insn[2], insn[3], insn[4], insn[5], insn[6], insn[7],
                   insn[8], insn[9], insn[10], insn[11], insn[12], insn[13], insn[14], insn[15]);
        }
    }

    printk("amd64 htop field write: rip=%#llx next=%#llx watch=%#llx addr=%#llx size=%u value=%#llx rsp=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) amd64_htop_watch_field_addr,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    if (have_field) {
        printk("amd64 htop field bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7]);
    }
}

static inline void amd64_trace_htop_r13_block_write(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, const void *value, unsigned size) {
    if (!amd64_htop_legacy_trace_enabled)
        return;
    if (current == NULL || strcmp(current->comm, "htop") != 0)
        return;
    if (!amd64_trace_intersects_htop_r13_block(guest_addr, size))
        return;

    qword_t observed = 0;
    uint8_t insn_bytes[8] = {};
    bool have_insn = false;
    uint8_t block[AMD64_HTOP_R13_CORRUPT_BLOCK_SIZE] = {};
    bool have_block = false;
    memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));

    guest_addr_t insn_addr;
    if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &insn_addr) &&
            tlb_read(tlb, insn_addr, insn_bytes, sizeof(insn_bytes))) {
        have_insn = true;
    }
    if (current->mem != NULL) {
        void *ptr = mem_ptr(current->mem, (addr_t) AMD64_HTOP_R13_CORRUPT_BLOCK_BASE, MEM_READ);
        if (ptr != NULL) {
            memcpy(block, ptr, sizeof(block));
            have_block = true;
        }
    }

    printk("amd64 htop block write: rip=%#llx next=%#llx addr=%#llx size=%u value=%#llx rsp=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx%s%s\n",
           (unsigned long long) cpu->amd64_current_insn_rip,
           (unsigned long long) cpu->amd64_rip,
           (unsigned long long) guest_addr,
           size,
           (unsigned long long) observed,
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           have_insn ? " bytes=" : "",
           have_insn ? "" : "");
    if (have_insn) {
        printk("amd64 htop block bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
               insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
    }
    if (have_block) {
        printk("amd64 htop block mem: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7],
               block[8], block[9], block[10], block[11], block[12], block[13], block[14], block[15]);
        printk("amd64 htop block mem2: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               block[16], block[17], block[18], block[19], block[20], block[21], block[22], block[23],
               block[24], block[25], block[26], block[27], block[28], block[29], block[30], block[31]);
    }
}

static inline qword_t amd64_reg_get(const struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t value = cpu->amd64_regs[reg & 0xf];
    switch (size) {
    case 8: return value & 0xff;
    case 16: return value & 0xffff;
    case 32: return (uint32_t) value;
    case 64: return value;
    default: return value;
    }
}

static inline qword_t amd64_reg_get_encoded8(const struct cpu_state *cpu, unsigned reg, bool rex_present) {
    reg &= 0xf;
    if (!rex_present && reg >= 4 && reg < 8)
        return (cpu->amd64_regs[reg - 4] >> 8) & 0xff;
    return amd64_reg_get(cpu, reg, 8);
}

static inline void amd64_reg_set_encoded8(struct cpu_state *cpu, unsigned reg, bool rex_present, qword_t value) {
    reg &= 0xf;
    if (!rex_present && reg >= 4 && reg < 8) {
        unsigned base = reg - 4;
        qword_t old_value = cpu->amd64_regs[base];
        cpu->amd64_regs[base] = (cpu->amd64_regs[base] & ~0xff00ull) | ((value & 0xff) << 8);
        if (base == amd64_r13)
            amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[base], 8, value & 0xff);
        return;
    }
    qword_t old_value = cpu->amd64_regs[reg];
    cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
    if (reg == amd64_r13)
        amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[reg], 8, value & 0xff);
    if (reg == amd64_rbx)
        amd64_trace_htop_rbx_base(cpu, old_value, cpu->amd64_regs[reg], 8, value & 0xff);
}

static inline void amd64_reg_set(struct cpu_state *cpu, unsigned reg, unsigned size, qword_t value) {
    reg &= 0xf;
    qword_t old_value = cpu->amd64_regs[reg];
    switch (size) {
    case 8:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
        break;
    case 16:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffffull) | (value & 0xffff);
        break;
    case 32:
        cpu->amd64_regs[reg] = (uint32_t) value;
        break;
    case 64:
        cpu->amd64_regs[reg] = value;
        break;
    default:
        break;
    }
    if (reg == amd64_r13)
        amd64_trace_htop_r13_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rbx)
        amd64_trace_htop_rbx_base(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rsp)
        amd64_trace_suspicious_rsp_write(cpu, old_value, cpu->amd64_regs[reg], size);
    if (reg == amd64_rdx)
        amd64_trace_cargo_rdx_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_rdi)
        amd64_trace_cargo_rdi_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
    if (reg == amd64_r12)
        amd64_trace_cargo_r12_write(cpu, old_value, cpu->amd64_regs[reg], size, value);
}

static inline void amd64_set_logic_flags(struct cpu_state *cpu, qword_t result, unsigned size) {
    qword_t masked = amd64_trunc(result, size);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = masked == 0;
    cpu->sf = (masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_add_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t mask = amd64_mask(size);
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    cpu->cf = size == 64 ? res_masked < lhs_masked : ((lhs_masked + rhs_masked) & ~mask) != 0;
    cpu->of = ((~(lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_sub_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    cpu->cf = lhs_masked < rhs_masked;
    cpu->of = (((lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_adc_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs,
        unsigned carry_in, qword_t result, unsigned size) {
    qword_t mask = amd64_mask(size);
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    __uint128_t full = (__uint128_t) lhs_masked + rhs_masked + carry_in;
    cpu->cf = size == 64 ? (full >> 64) != 0 : full > mask;
    // OF/AF must use the original rhs, not rhs+carry: pre-folding the carry lets
    // it ripple past bit 3 (e.g. 0x7f+1=0x80), corrupting the bit-4 XOR (AF) and
    // the signed-overflow test (OF). AF is the true carry out of bit 3.
    cpu->of = ((~(lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((((lhs_masked & 0xf) + (rhs_masked & 0xf) + carry_in) >> 4) & 1);
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_sbb_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs,
        unsigned carry_in, qword_t result, unsigned size) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t rhs_masked = amd64_trunc(rhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    __uint128_t subtrahend = (__uint128_t) rhs_masked + carry_in;
    cpu->cf = (__uint128_t) lhs_masked < subtrahend;
    // OF/AF use the original rhs, not rhs+carry (see amd64_set_adc_flags). AF is
    // the true borrow out of bit 3 including the incoming borrow.
    cpu->of = (((lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_sign_bit(size)) != 0;
    cpu->af = ((((lhs_masked & 0xf) - (rhs_masked & 0xf) - carry_in) >> 4) & 1);
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_mul_flags(struct cpu_state *cpu, bool overflow) {
    cpu->cf = overflow;
    cpu->of = overflow;
    collapse_flags(cpu);
}

static inline void amd64_set_shift_flags(struct cpu_state *cpu, qword_t lhs, qword_t result,
        unsigned size, unsigned count, unsigned subop) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    qword_t sign = amd64_sign_bit(size);
    cpu->cf = 0;
    cpu->of = 0;
    if (count != 0) {
        switch (subop) {
        case 4:
            if (count <= size)
                cpu->cf = (lhs_masked >> (size - count)) & 1;
            if (count == 1)
                cpu->of = ((res_masked & sign) != 0) ^ cpu->cf;
            break;
        case 5:
            if (count <= size)
                cpu->cf = (lhs_masked >> (count - 1)) & 1;
            if (count == 1)
                cpu->of = (lhs_masked & sign) != 0;
            break;
        case 7:
            // sar shifts in the sign bit, so once the count reaches the operand
            // width the last bit shifted out is the sign bit (not 0).
            cpu->cf = (count <= size)
                ? (lhs_masked >> (count - 1)) & 1
                : (lhs_masked >> (size - 1)) & 1;
            if (count == 1)
                cpu->of = 0;
            break;
        }
    }
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & sign) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline void amd64_set_double_shift_flags(struct cpu_state *cpu, qword_t lhs, qword_t result,
        unsigned size, unsigned count, bool left) {
    qword_t lhs_masked = amd64_trunc(lhs, size);
    qword_t res_masked = amd64_trunc(result, size);
    qword_t sign = amd64_sign_bit(size);
    cpu->cf = 0;
    cpu->of = 0;
    if (count != 0) {
        if (left) {
            cpu->cf = (lhs_masked >> (size - count)) & 1;
            if (count == 1)
                cpu->of = ((res_masked & sign) != 0) ^ cpu->cf;
        } else {
            cpu->cf = (lhs_masked >> (count - 1)) & 1;
            if (count == 1)
                cpu->of = (lhs_masked & sign) != 0;
        }
    }
    cpu->af = 0;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & sign) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static inline qword_t amd64_rotate_value(qword_t value, unsigned size, unsigned count, unsigned subop) {
    qword_t masked = amd64_trunc(value, size);
    unsigned effective = count % size;
    if (effective == 0)
        return masked;
    if (subop == 0) {
        return amd64_trunc((masked << effective) | (masked >> (size - effective)), size);
    } else {
        return amd64_trunc((masked >> effective) | (masked << (size - effective)), size);
    }
}

static inline void amd64_set_rotate_flags(struct cpu_state *cpu, qword_t result,
        unsigned size, unsigned count, unsigned subop) {
    // CF/OF are affected iff the count masked to 5 bits (6 for 64-bit) is
    // nonzero -- including when it is a nonzero multiple of the operand size (a
    // full rotation), where count % size is 0 but CF still takes the rotated bit.
    unsigned masked = count & (size == 64 ? 63 : 31);
    if (masked == 0)
        return;
    qword_t res = amd64_trunc(result, size);
    if (subop == 0) {
        cpu->cf = res & 1;
        if (masked == 1)
            cpu->of = cpu->cf ^ ((res >> (size - 1)) & 1);
    } else {
        cpu->cf = (res >> (size - 1)) & 1;
        // ROR OF (1-bit) = MSB ^ next-MSB of the result, not MSB ^ LSB.
        if (masked == 1)
            cpu->of = cpu->cf ^ ((res >> (size - 2)) & 1);
    }
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = cpu->of;
}

static inline unsigned amd64_rotate_carry_count(unsigned size, unsigned count) {
    if (size == 8 || size == 16)
        return count % (size + 1);
    return count;
}

static inline qword_t amd64_rotate_carry_value(struct cpu_state *cpu, qword_t value,
        unsigned size, unsigned count, unsigned subop) {
    qword_t result = amd64_trunc(value, size);
    qword_t sign = amd64_sign_bit(size);
    qword_t mask = size == 64 ? ~(qword_t) 0 : (((qword_t) 1 << size) - 1);
    unsigned effective = amd64_rotate_carry_count(size, count);
    bool old_cf = cpu->cf != 0;

    for (unsigned i = 0; i < effective; i++) {
        bool new_cf;
        if (subop == 2) {
            new_cf = (result & sign) != 0;
            result = ((result << 1) & mask) | (old_cf ? 1 : 0);
        } else {
            new_cf = (result & 1) != 0;
            result = (result >> 1) | (old_cf ? sign : 0);
        }
        old_cf = new_cf;
    }

    if (effective != 0) {
        cpu->cf = old_cf ? 1 : 0;
        cpu->cf_bit = cpu->cf;
        if (effective == 1) {
            if (subop == 2)
                cpu->of = (((result & sign) != 0) ^ (cpu->cf != 0)) ? 1 : 0;
            else
                cpu->of = (((result & sign) != 0) ^
                        ((result & (sign >> 1)) != 0)) ? 1 : 0;
            cpu->of_bit = cpu->of;
        }
    }
    return result;
}

static inline bool amd64_fetch(struct cpu_state *cpu, struct tlb *tlb, void *out, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(cpu->amd64_rip, size, &addr)) {
        cpu->segfault_addr = cpu->amd64_rip;
        cpu->segfault_was_write = false;
        return false;
    }
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = addr;
        cpu->segfault_was_write = false;
        return false;
    }
    cpu->amd64_rip += size;
    return true;
}

static inline bool amd64_fetch_u8(struct cpu_state *cpu, struct tlb *tlb, byte_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_u32(struct cpu_state *cpu, struct tlb *tlb, uint32_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_u64(struct cpu_state *cpu, struct tlb *tlb, uint64_t *out) {
    return amd64_fetch(cpu, tlb, out, sizeof(*out));
}

static inline bool amd64_fetch_moffs_addr(struct cpu_state *cpu, struct tlb *tlb, qword_t *addr_out) {
    if (cpu->amd64_address_size_prefix) {
        uint32_t addr32;
        if (!amd64_fetch_u32(cpu, tlb, &addr32))
            return false;
        *addr_out = addr32;
    } else {
        uint64_t addr64;
        if (!amd64_fetch_u64(cpu, tlb, &addr64))
            return false;
        *addr_out = addr64;
    }
    return true;
}

static inline bool amd64_fetch_accum_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned size, bool sign_extend_imm32, qword_t *value) {
    if (size == 8) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            return false;
        *value = imm8;
        return true;
    }
    if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            return false;
        *value = imm16;
        return true;
    }
    uint32_t imm32;
    if (!amd64_fetch_u32(cpu, tlb, &imm32))
        return false;
    *value = size == 64 && sign_extend_imm32 ? (qword_t) (sqword_t) (int32_t) imm32 : imm32;
    return true;
}

static inline bool amd64_verbose_boot_trace_enabled(void) {
    return false;
}

static inline bool amd64_mem_read(struct cpu_state *cpu, struct tlb *tlb, qword_t guest_addr, void *out, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = guest_addr;
        cpu->segfault_was_write = false;
        return false;
    }
    tlb->segfault_addr = 0;
    if (!tlb_read(tlb, addr, out, size)) {
        cpu->segfault_addr = tlb->segfault_addr != 0 ? tlb->segfault_addr : addr;
        cpu->segfault_was_write = false;
        return false;
    }
    if (amd64_verbose_boot_trace_enabled() && amd64_trace_intersects_busybox_slot(guest_addr, size)) {
        qword_t observed = 0;
        memcpy(&observed, out, size < sizeof(observed) ? size : sizeof(observed));
        printk("amd64 slot read: rip=%#llx addr=%#llx size=%u value=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) observed);
    }
    return true;
}

static inline bool amd64_mem_write(struct cpu_state *cpu, struct tlb *tlb, qword_t guest_addr, const void *value, unsigned size) {
    guest_addr_t addr;
    if (!amd64_guest_addr_ok(guest_addr, size, &addr)) {
        cpu->segfault_addr = guest_addr;
        cpu->segfault_was_write = true;
        return false;
    }
    tlb->segfault_addr = 0;
    if (!tlb_write(tlb, addr, value, size)) {
        // Cross-page accesses can fault on a later page than the starting
        // guest address. Preserve the TLB-reported failing page so the page
        // fault handler resolves the actual missing/COW page.
        cpu->segfault_addr = tlb->segfault_addr != 0 ? tlb->segfault_addr : addr;
        cpu->segfault_was_write = true;
        return false;
    }
    // These per-write debug probes are all disabled by default. Gate each on its
    // own (cached) enable check at the call site so the common path skips them
    // entirely -- amd64_trace_as_state_write in particular is a non-inlined,
    // two-call-deep chain that otherwise ran on every guest write (~8% of an
    // amd64 string-op/memset under profiling).
    if (unlikely(amd64_cc1_trace_enabled()))
        amd64_trace_cc1_slot_write_probe(cpu, guest_addr, value, size);
    if (amd64_htop_legacy_trace_enabled) {
        amd64_trace_htop_field_write(cpu, tlb, guest_addr, value, size);
        amd64_trace_htop_r13_block_write(cpu, tlb, guest_addr, value, size);
    }
    if (unlikely(amd64_as_trace_enabled()))
        amd64_trace_as_state_write(cpu, guest_addr, value, size);
    if (amd64_verbose_boot_trace_enabled() && amd64_trace_intersects_busybox_slot(guest_addr, size)) {
        qword_t observed = 0;
        memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
        printk("amd64 slot write: rip=%#llx addr=%#llx size=%u value=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) guest_addr,
               size,
               (unsigned long long) observed);
    }
    qword_t watch_base = 0, watch_offset = 0;
    if (amd64_verbose_boot_trace_enabled() &&
            amd64_trace_intersects_busybox_watch(guest_addr, size, &watch_base, &watch_offset)) {
        qword_t observed = 0;
        uint8_t insn_bytes[8] = {};
        bool have_bytes = false;
        memcpy(&observed, value, size < sizeof(observed) ? size : sizeof(observed));
        guest_addr_t insn_addr;
        if (amd64_guest_addr_ok(cpu->amd64_current_insn_rip, sizeof(insn_bytes), &insn_addr) &&
                tlb_read(tlb, insn_addr, insn_bytes, sizeof(insn_bytes))) {
            have_bytes = true;
        }
        printk("amd64 init write: rip=%#llx next=%#llx base=%#llx addr=%#llx off=%#llx size=%u value=%#llx%s%s\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) watch_base,
               (unsigned long long) guest_addr,
               (unsigned long long) watch_offset,
               size,
               (unsigned long long) observed,
               have_bytes ? " bytes=" : "",
               have_bytes ? "" : "");
        if (have_bytes) {
            printk("amd64 init write bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                   insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
        }
        if (cpu->amd64_current_insn_rip == AMD64_BUSYBOX_INIT_CORRUPT_WRITE_RIP) {
            printk("amd64 init write regs: rax=%#llx rsp=%#llx rbp=%#llx r8=%#llx rcx=%#llx rdx=%#llx rsi=%#llx\n",
                   (unsigned long long) cpu->amd64_regs[amd64_rax],
                   (unsigned long long) cpu->amd64_regs[amd64_rsp],
                   (unsigned long long) cpu->amd64_regs[amd64_rbp],
                   (unsigned long long) cpu->amd64_regs[amd64_r8],
                   (unsigned long long) cpu->amd64_regs[amd64_rcx],
                   (unsigned long long) cpu->amd64_regs[amd64_rdx],
                   (unsigned long long) cpu->amd64_regs[amd64_rsi]);
        }
    }
    return true;
}

static inline bool amd64_mem_read_value(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, qword_t *value) {
    switch (size) {
    case 8: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, guest_addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, guest_addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (!amd64_mem_read(cpu, tlb, guest_addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, guest_addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_push(struct cpu_state *cpu, struct tlb *tlb, qword_t value) {
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
    qword_t rsp = old_rsp - sizeof(value);
    if (!amd64_mem_write(cpu, tlb, rsp, &value, sizeof(value)))
        return false;
    cpu->amd64_regs[amd64_rsp] = rsp;
    amd64_trace_suspicious_rsp_write(cpu, old_rsp, rsp, 64);
    amd64_trace_as_stack(amd64_as_stack_push, 64, old_rsp, rsp, value);
    return true;
}

static inline bool amd64_push_size(struct cpu_state *cpu, struct tlb *tlb, unsigned size,
        qword_t value) {
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];

    switch (size) {
    case 16: {
        uint16_t tmp = (uint16_t) value;
        qword_t rsp = old_rsp - sizeof(tmp);
        if (!amd64_mem_write(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        cpu->amd64_regs[amd64_rsp] = rsp;
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, rsp, size);
        amd64_trace_as_stack(amd64_as_stack_push, size, old_rsp, rsp, value);
        return true;
    }
    case 64:
        return amd64_push(cpu, tlb, value);
    default:
        return false;
    }
}

static inline bool amd64_read_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t *value);
static inline bool amd64_write_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t value);

static inline int amd64_grp3_muldiv(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size) {
    qword_t src;
    if (!amd64_read_rm(cpu, tlb, modrm, fs_prefix, size, &src))
        return INT_PF;

    switch (modrm->reg) {
    case 2: {
        qword_t result = amd64_trunc(~src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_PF;
        return INT_NONE;
    }
    case 3: {
        qword_t result = amd64_trunc(0 - src, size);
        if (!amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, result))
            return INT_PF;
        amd64_set_sub_flags(cpu, 0, src, result, size);
        return INT_NONE;
    }
    case 4:
        switch (size) {
        case 8: {
            uint16_t product = (uint8_t) amd64_reg_get(cpu, amd64_rax, 8) * (uint8_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, product);
            amd64_set_mul_flags(cpu, (product >> 8) != 0);
            return INT_NONE;
        }
        case 16: {
            uint32_t product = (uint16_t) amd64_reg_get(cpu, amd64_rax, 16) * (uint16_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, product);
            amd64_reg_set(cpu, amd64_rdx, 16, product >> 16);
            amd64_set_mul_flags(cpu, (product >> 16) != 0);
            return INT_NONE;
        }
        case 32: {
            uint64_t product = (uint64_t) (uint32_t) amd64_reg_get(cpu, amd64_rax, 32) * (uint32_t) src;
            amd64_reg_set(cpu, amd64_rax, 32, product);
            amd64_reg_set(cpu, amd64_rdx, 32, product >> 32);
            amd64_set_mul_flags(cpu, (product >> 32) != 0);
            return INT_NONE;
        }
        case 64: {
            __uint128_t product = (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64) * (__uint128_t) src;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) product);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) (product >> 64));
            amd64_set_mul_flags(cpu, (product >> 64) != 0);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 5:
        switch (size) {
        case 8: {
            int16_t product = (int8_t) amd64_reg_get(cpu, amd64_rax, 8) * (int8_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) product);
            amd64_set_mul_flags(cpu, product != (int16_t) (int8_t) product);
            return INT_NONE;
        }
        case 16: {
            int32_t product = (int16_t) amd64_reg_get(cpu, amd64_rax, 16) * (int16_t) src;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) product);
            amd64_reg_set(cpu, amd64_rdx, 16, (uint16_t) ((uint32_t) product >> 16));
            amd64_set_mul_flags(cpu, product != (int32_t) (int16_t) product);
            return INT_NONE;
        }
        case 32: {
            int64_t product = (int64_t) (int32_t) amd64_reg_get(cpu, amd64_rax, 32) * (int32_t) src;
            amd64_reg_set(cpu, amd64_rax, 32, (uint32_t) product);
            amd64_reg_set(cpu, amd64_rdx, 32, (uint32_t) ((uint64_t) product >> 32));
            amd64_set_mul_flags(cpu, product != (int64_t) (int32_t) product);
            return INT_NONE;
        }
        case 64: {
            __int128_t product = (__int128_t) (sqword_t) amd64_reg_get(cpu, amd64_rax, 64) *
                    (__int128_t) (sqword_t) src;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) product);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) (((__uint128_t) product) >> 64));
            amd64_set_mul_flags(cpu, product != (__int128_t) (sqword_t) (uint64_t) product);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 6:
        switch (size) {
        case 8: {
            uint8_t divisor = (uint8_t) src;
            uint16_t dividend = (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            uint16_t quotient = dividend / divisor;
            uint16_t remainder = dividend % divisor;
            if (quotient > 0xff)
                return INT_DIV;
            amd64_reg_set_encoded8(cpu, 0, true, quotient);
            amd64_reg_set_encoded8(cpu, 4, false, remainder);
            return INT_NONE;
        }
        case 16: {
            uint16_t divisor = (uint16_t) src;
            uint32_t dividend = ((uint32_t) amd64_reg_get(cpu, amd64_rdx, 16) << 16) |
                    (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            uint32_t quotient = dividend / divisor;
            uint32_t remainder = dividend % divisor;
            if (quotient > 0xffff)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 16, quotient);
            amd64_reg_set(cpu, amd64_rdx, 16, remainder);
            return INT_NONE;
        }
        case 32: {
            uint32_t divisor = (uint32_t) src;
            uint64_t dividend = ((uint64_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (uint32_t) amd64_reg_get(cpu, amd64_rax, 32);
            if (divisor == 0)
                return INT_DIV;
            uint64_t quotient = dividend / divisor;
            uint64_t remainder = dividend % divisor;
            if (quotient > 0xffffffffu)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 32, quotient);
            amd64_reg_set(cpu, amd64_rdx, 32, remainder);
            return INT_NONE;
        }
        case 64: {
            uint64_t divisor = (uint64_t) src;
            __uint128_t dividend = ((__uint128_t) amd64_reg_get(cpu, amd64_rdx, 64) << 64) |
                    (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64);
            if (divisor == 0)
                return INT_DIV;
            __uint128_t quotient = dividend / divisor;
            __uint128_t remainder = dividend % divisor;
            if ((quotient >> 64) != 0)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) remainder);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    case 7:
        switch (size) {
        case 8: {
            int8_t divisor = (int8_t) src;
            int16_t dividend = (int16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            int quotient = (int) dividend / (int) divisor;
            int remainder = (int) dividend % (int) divisor;
            if (quotient < INT8_MIN || quotient > INT8_MAX)
                return INT_DIV;
            amd64_reg_set_encoded8(cpu, 0, true, (uint8_t) quotient);
            amd64_reg_set_encoded8(cpu, 4, false, (uint8_t) remainder);
            return INT_NONE;
        }
        case 16: {
            int16_t divisor = (int16_t) src;
            int32_t dividend = ((int32_t) (int16_t) amd64_reg_get(cpu, amd64_rdx, 16) << 16) |
                    (uint16_t) amd64_reg_get(cpu, amd64_rax, 16);
            if (divisor == 0)
                return INT_DIV;
            int64_t quotient = (int64_t) dividend / (int64_t) divisor;
            int64_t remainder = (int64_t) dividend % (int64_t) divisor;
            if (quotient < INT16_MIN || quotient > INT16_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 16, (uint16_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 16, (uint16_t) remainder);
            return INT_NONE;
        }
        case 32: {
            int32_t divisor = (int32_t) src;
            int64_t dividend = ((int64_t) (int32_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (uint32_t) amd64_reg_get(cpu, amd64_rax, 32);
            if (divisor == 0)
                return INT_DIV;
            __int128_t quotient = (__int128_t) dividend / (__int128_t) divisor;
            __int128_t remainder = (__int128_t) dividend % (__int128_t) divisor;
            if (quotient < INT32_MIN || quotient > INT32_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 32, (uint32_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 32, (uint32_t) remainder);
            return INT_NONE;
        }
        case 64: {
            int64_t divisor = (int64_t) src;
            __int128_t dividend = ((__int128_t) (int64_t) amd64_reg_get(cpu, amd64_rdx, 64) << 64) |
                    (__uint128_t) amd64_reg_get(cpu, amd64_rax, 64);
            if (divisor == 0)
                return INT_DIV;
            if (divisor == -1 && (__uint128_t) dividend == ((__uint128_t) 1 << 127))
                return INT_DIV;
            __int128_t quotient = dividend / divisor;
            __int128_t remainder = dividend % divisor;
            if (quotient < INT64_MIN || quotient > INT64_MAX)
                return INT_DIV;
            amd64_reg_set(cpu, amd64_rax, 64, (uint64_t) quotient);
            amd64_reg_set(cpu, amd64_rdx, 64, (uint64_t) remainder);
            return INT_NONE;
        }
        default:
            return INT_UNDEFINED;
        }
    default:
        return INT_UNDEFINED;
    }
}

static inline bool amd64_pop_size(struct cpu_state *cpu, struct tlb *tlb, unsigned size, qword_t *value) {
    qword_t rsp = cpu->amd64_regs[amd64_rsp];
    switch (size) {
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        cpu->amd64_regs[amd64_rsp] = rsp + sizeof(tmp);
        amd64_trace_suspicious_rsp_write(cpu, rsp, cpu->amd64_regs[amd64_rsp], size);
        amd64_trace_as_stack(amd64_as_stack_pop, size, rsp, cpu->amd64_regs[amd64_rsp], *value);
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, rsp, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        cpu->amd64_regs[amd64_rsp] = rsp + sizeof(tmp);
        amd64_trace_suspicious_rsp_write(cpu, rsp, cpu->amd64_regs[amd64_rsp], size);
        amd64_trace_as_stack(amd64_as_stack_pop, size, rsp, cpu->amd64_regs[amd64_rsp], *value);
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_pop(struct cpu_state *cpu, struct tlb *tlb, qword_t *value) {
    return amd64_pop_size(cpu, tlb, 64, value);
}

static inline qword_t amd64_effective_addr(struct cpu_state *cpu, const struct amd64_modrm *modrm,
        bool fs_prefix);

static inline qword_t amd64_bt_mem_addr(qword_t addr, unsigned size, qword_t bit_index,
        bool stride_memory, bool signed_index, qword_t *bit_out) {
    qword_t truncated_index = amd64_trunc(bit_index, size);
    *bit_out = truncated_index & (size - 1);
    if (!stride_memory)
        return addr;

    sqword_t scaled_index = signed_index
            ? amd64_sign_extend(bit_index, size)
            : (sqword_t) truncated_index;
    sqword_t element_index = scaled_index >> __builtin_ctz(size);
    return addr + (qword_t) (element_index * (sqword_t) (size / 8));
}

static inline bool amd64_read_bt_operand(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t bit_index,
        bool stride_memory, bool signed_index, qword_t *value, qword_t *addr_out, qword_t *bit_out) {
    if (modrm->is_reg) {
        *addr_out = 0;
        *bit_out = bit_index & (size - 1);
        return amd64_read_rm(cpu, tlb, modrm, fs_prefix, size, value);
    }

    qword_t addr = amd64_bt_mem_addr(amd64_effective_addr(cpu, modrm, fs_prefix),
            size, bit_index, stride_memory, signed_index, bit_out);
    *addr_out = addr;
    switch (size) {
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_write_bt_operand(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t addr,
        qword_t value) {
    if (modrm->is_reg)
        return amd64_write_rm(cpu, tlb, modrm, fs_prefix, size, value);

    switch (size) {
    case 16: {
        uint16_t tmp = (uint16_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 32: {
        uint32_t tmp = (uint32_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 64: {
        uint64_t tmp = (uint64_t) value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    default:
        return false;
    }
}

static inline bool amd64_decode_modrm(struct cpu_state *cpu, struct tlb *tlb,
        struct amd64_rex_prefix rex, struct amd64_modrm *modrm) {
    byte_t modrm_byte;
    if (!amd64_fetch_u8(cpu, tlb, &modrm_byte))
        return false;

    unsigned mod = MOD(modrm_byte);
    modrm->rex_present = rex.present;
    modrm->reg = REG(modrm_byte) | (rex.r ? 8 : 0);
    modrm->rm = RM(modrm_byte) | (rex.b ? 8 : 0);
    modrm->is_reg = mod == 3;
    modrm->has_base = false;
    modrm->has_index = false;
    modrm->rip_relative = false;
    modrm->disp = 0;
    modrm->scale = 0;

    if (modrm->is_reg)
        return true;

    if (cpu->amd64_address_size_prefix) {
        unsigned rm_low = RM(modrm_byte);
        if (rm_low == 4) {
            byte_t sib;
            if (!amd64_fetch_u8(cpu, tlb, &sib))
                return false;
            unsigned base_low = RM(sib);
            unsigned index_low = REG(sib);
            modrm->scale = MOD(sib);
            if (index_low != 4 || rex.x) {
                modrm->has_index = true;
                modrm->index = index_low | (rex.x ? 8 : 0);
            }
            // mod=00 with base=101 means no base + disp32, regardless of REX.B.
            if (mod == 0 && base_low == 5) {
                modrm->has_base = false;
            } else {
                modrm->has_base = true;
                modrm->base = base_low | (rex.b ? 8 : 0);
            }
        } else if (mod == 0 && rm_low == 5 && !rex.b) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = modrm->rm;
        }

        if (mod == 1) {
            int8_t disp8;
            if (!amd64_fetch(cpu, tlb, &disp8, sizeof(disp8)))
                return false;
            modrm->disp = disp8;
        } else if (mod == 2 || (mod == 0 && !modrm->has_base)) {
            int32_t disp32;
            if (!amd64_fetch(cpu, tlb, &disp32, sizeof(disp32)))
                return false;
            modrm->disp = disp32;
        }
        return true;
    }

    unsigned rm_low = RM(modrm_byte);
    if (rm_low == 4) {
        byte_t sib;
        if (!amd64_fetch_u8(cpu, tlb, &sib))
            return false;
        unsigned base_low = RM(sib);
        unsigned index_low = REG(sib);
        modrm->scale = MOD(sib);
        // In 64-bit mode, SIB index 100 means "no index" only when REX.X is clear.
        if (index_low != 4 || rex.x) {
            modrm->has_index = true;
            modrm->index = index_low | (rex.x ? 8 : 0);
        }
        // mod=00 with base=101 means no base + disp32, regardless of REX.B
        // (r13 as a base requires mod=01/10).
        if (mod == 0 && base_low == 5) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = base_low | (rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        modrm->rip_relative = true;
    } else {
        modrm->has_base = true;
        modrm->base = modrm->rm;
    }

    if (mod == 1) {
        int8_t disp8;
        if (!amd64_fetch(cpu, tlb, &disp8, sizeof(disp8)))
            return false;
        modrm->disp = disp8;
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 || (rm_low == 4 && !modrm->has_base)))) {
        int32_t disp32;
        if (!amd64_fetch(cpu, tlb, &disp32, sizeof(disp32)))
            return false;
        modrm->disp = disp32;
    }
    return true;
}

static inline qword_t amd64_effective_addr(struct cpu_state *cpu, const struct amd64_modrm *modrm, bool fs_prefix) {
    if (cpu->amd64_address_size_prefix) {
        uint32_t addr32 = (uint32_t) modrm->disp;
        if (modrm->has_base)
            addr32 += (uint32_t) cpu->amd64_regs[modrm->base];
        if (modrm->has_index)
            addr32 += (uint32_t) cpu->amd64_regs[modrm->index] << modrm->scale;
        qword_t addr = addr32;
        if (fs_prefix)
            addr += cpu->tls_ptr;
        return addr;
    }

    qword_t addr = (sqword_t) modrm->disp;
    if (modrm->rip_relative)
        addr += cpu->amd64_rip;
    if (modrm->has_base)
        addr += cpu->amd64_regs[modrm->base];
    if (modrm->has_index)
        addr += cpu->amd64_regs[modrm->index] << modrm->scale;
    if (fs_prefix)
        addr += cpu->tls_ptr;
    return addr;
}

static inline bool amd64_read_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t *value) {
    if (modrm->is_reg) {
        *value = size == 8 ? amd64_reg_get_encoded8(cpu, modrm->rm, modrm->rex_present) : amd64_reg_get(cpu, modrm->rm, size);
        return true;
    }

    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    switch (size) {
    case 8: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 16: {
        uint16_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp)))
            return false;
        *value = tmp;
        if (modrm->reg == amd64_rbx && modrm->has_base)
            amd64_trace_htop_rbx_source(cpu, cpu->amd64_regs[modrm->base], addr, tmp);
        if (modrm->reg == amd64_r13)
            amd64_trace_htop_r13_source(cpu, addr, tmp);
        return true;
    }
    default:
        return false;
    }
}

static inline bool amd64_read_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, union xmm_reg *value) {
    if (modrm->reg >= AMD64_XMM_COUNT)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_XMM_COUNT)
            return false;
        *value = cpu->xmm[modrm->rm];
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_read(cpu, tlb, addr, value, sizeof(*value));
}

static inline bool amd64_write_xmm_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, const union xmm_reg *value) {
    if (modrm->reg >= AMD64_XMM_COUNT)
        return false;
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_XMM_COUNT)
            return false;
        cpu->xmm[modrm->rm] = *value;
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_write(cpu, tlb, addr, value, sizeof(*value));
}

static inline bool amd64_write_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned size, qword_t value) {
    if (modrm->is_reg) {
        if (size == 8)
            amd64_reg_set_encoded8(cpu, modrm->rm, modrm->rex_present, value);
        else
            amd64_reg_set(cpu, modrm->rm, size, value);
        return true;
    }

    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    switch (size) {
    case 8: {
        uint8_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 16: {
        uint16_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 32: {
        uint32_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    case 64: {
        uint64_t tmp = value;
        return amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp));
    }
    default:
        return false;
    }
}

// ---- LOCK-prefixed memory operands ----------------------------------------
//
// These wrap the host-atomic primitives in emu/tlb.c (x86_atomic_*) with the
// two things the amd64 guest adds on top of a raw guest address: the canonical
// address check, and the sizes-in-bits convention the interpreter uses
// everywhere else.
//
// Before build 553 every locked instruction on an amd64 guest was interpreted
// as a plain read/compute/write serialised on the global `atomic_l_lock` --
// which does not interlock with a host atomic (the kernel does those on guest
// memory; FUTEX_WAKE_OP is the live case) -- and the whole `<alu> [mem], reg`
// family did not even take that lock, so it lost updates against other guest
// threads. See the comment over x86_atomic_rmw in emu/tlb.c.
//
// Every one of these returns false with cpu->segfault_addr/was_write set on a
// fault, matching amd64_read_rm/amd64_write_rm.

static bool amd64_atomic_addr_ok(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, guest_addr_t *addr_out) {
    if (!amd64_guest_addr_ok(guest_addr, size / 8, addr_out)) {
        cpu->segfault_addr = guest_addr;
        cpu->segfault_was_write = true;
        return false;
    }
    tlb->segfault_addr = 0;
    return true;
}

// A fault out of the atomic helpers reports tlb->segfault_addr, which is 0
// when the failure was not a TLB miss on this exact address; fall back to the
// operand address so the page-fault handler has something to resolve.
static bool amd64_atomic_faulted(struct cpu_state *cpu, guest_addr_t addr) {
    if (cpu->segfault_addr == 0)
        cpu->segfault_addr = addr;
    return false;
}

// alu_op is the x86 /r group index, which is also (opcode >> 3) & 7 for the
// 00-3B one-byte forms: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor.
// (7 is cmp, which writes nothing and so is never locked -- #UD.)
struct amd64_alu_atomic_ctx {
    unsigned alu_op;
    qword_t rhs;
    unsigned size;
    unsigned carry_in;
};

// Pure, as x86_atomic_rmw requires: the aligned path re-runs it per CAS retry.
static qword_t amd64_alu_atomic_fn(qword_t old, void *ctxp) {
    const struct amd64_alu_atomic_ctx *c = ctxp;
    switch (c->alu_op) {
    case 0: return amd64_trunc(old + c->rhs, c->size);
    case 1: return amd64_trunc(old | c->rhs, c->size);
    case 2: return amd64_trunc(old + c->rhs + c->carry_in, c->size);
    case 3: return amd64_trunc(old - c->rhs - c->carry_in, c->size);
    case 4: return amd64_trunc(old & c->rhs, c->size);
    case 5: return amd64_trunc(old - c->rhs, c->size);
    default: return amd64_trunc(old ^ c->rhs, c->size);
    }
}

static void amd64_set_alu_flags(struct cpu_state *cpu, unsigned alu_op,
        qword_t lhs, qword_t rhs, unsigned carry_in, qword_t result, unsigned size) {
    switch (alu_op) {
    case 0: amd64_set_add_flags(cpu, lhs, rhs, result, size); break;
    case 2: amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size); break;
    case 3: amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size); break;
    case 5: amd64_set_sub_flags(cpu, lhs, rhs, result, size); break;
    default: amd64_set_logic_flags(cpu, result, size); break; /* or/and/xor */
    }
}

// LOCK <alu> [addr], rhs. Sets the flags itself, since every caller wants
// exactly this and the old/new pair is otherwise only useful for that.
static bool amd64_locked_alu(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, unsigned alu_op, qword_t rhs) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    struct amd64_alu_atomic_ctx ctx = {
        .alu_op = alu_op, .rhs = rhs, .size = size, .carry_in = cpu->cf,
    };
    qword_t old = 0, neu = 0;
    if (x86_atomic_rmw(cpu, tlb, addr, size / 8, amd64_alu_atomic_fn, &ctx,
                &old, &neu) != 0)
        return amd64_atomic_faulted(cpu, addr);
    amd64_set_alu_flags(cpu, alu_op, old, rhs, ctx.carry_in, neu, size);
    return true;
}

// LOCK INC / LOCK DEC [addr]. INC and DEC set every arithmetic flag EXCEPT
// CF, which they preserve -- the caller-visible reason this is not just
// amd64_locked_alu with rhs = 1.
static qword_t amd64_inc_atomic_fn(qword_t old, void *ctxp) {
    const struct amd64_alu_atomic_ctx *c = ctxp;
    return amd64_trunc(c->alu_op ? old - 1 : old + 1, c->size);
}

static bool amd64_locked_incdec(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, bool is_inc) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    bool saved_cf = cpu->cf;
    struct amd64_alu_atomic_ctx ctx = { .alu_op = is_inc ? 0 : 1, .size = size };
    qword_t old = 0, neu = 0;
    if (x86_atomic_rmw(cpu, tlb, addr, size / 8, amd64_inc_atomic_fn, &ctx,
                &old, &neu) != 0)
        return amd64_atomic_faulted(cpu, addr);
    if (is_inc)
        amd64_set_add_flags(cpu, old, 1, neu, size);
    else
        amd64_set_sub_flags(cpu, old, 1, neu, size);
    cpu->cf = saved_cf;
    collapse_flags(cpu);
    return true;
}

// XCHG reg, [addr] -- implicitly locked on x86, prefix or not.
static bool amd64_locked_xchg(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, qword_t value, qword_t *old_out) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    if (x86_atomic_xchg(cpu, tlb, addr, size / 8, value, old_out) != 0)
        return amd64_atomic_faulted(cpu, addr);
    return true;
}

// LOCK XADD reg, [addr]: [addr] += reg, reg = old [addr].
static bool amd64_locked_xadd(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, qword_t rhs,
        qword_t *old_out, qword_t *new_out) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    struct amd64_alu_atomic_ctx ctx = { .alu_op = 0, .rhs = rhs, .size = size };
    if (x86_atomic_rmw(cpu, tlb, addr, size / 8, amd64_alu_atomic_fn, &ctx,
                old_out, new_out) != 0)
        return amd64_atomic_faulted(cpu, addr);
    return true;
}

// LOCK CMPXCHG reg, [addr].
static bool amd64_locked_cmpxchg(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, qword_t expected, qword_t desired,
        qword_t *old_out, bool *swapped) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    if (x86_atomic_cas(cpu, tlb, addr, size / 8, expected, desired,
                old_out, swapped) != 0)
        return amd64_atomic_faulted(cpu, addr);
    return true;
}

// LOCK NEG / LOCK NOT [addr] (F7 /3, /2). NOT touches no flags; NEG is
// 0 - operand, with the full sub flag rule.
static qword_t amd64_negnot_atomic_fn(qword_t old, void *ctxp) {
    const struct amd64_alu_atomic_ctx *c = ctxp;
    return amd64_trunc(c->alu_op ? 0 - old : ~old, c->size);
}

static bool amd64_locked_negnot(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, bool is_neg,
        qword_t *old_out, qword_t *new_out) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    struct amd64_alu_atomic_ctx ctx = { .alu_op = is_neg ? 1 : 0, .size = size };
    if (x86_atomic_rmw(cpu, tlb, addr, size / 8, amd64_negnot_atomic_fn, &ctx,
                old_out, new_out) != 0)
        return amd64_atomic_faulted(cpu, addr);
    return true;
}

// LOCK BTS / BTR / BTC on a memory operand. Expressed as OR / AND-NOT / XOR of
// a one-bit mask, but it cannot go through amd64_locked_alu: the bit-test
// instructions set CF from the old bit and touch NOTHING else, where the ALU
// forms set the whole logic flag group. Returns the pre-image so the caller
// can extract CF. op: 0 = BTS (set), 1 = BTR (reset), 2 = BTC (complement).
static qword_t amd64_bitop_atomic_fn(qword_t old, void *ctxp) {
    const struct amd64_alu_atomic_ctx *c = ctxp;
    switch (c->alu_op) {
    case 0: return amd64_trunc(old | c->rhs, c->size);
    case 1: return amd64_trunc(old & ~c->rhs, c->size);
    default: return amd64_trunc(old ^ c->rhs, c->size);
    }
}

static bool amd64_locked_bitop(struct cpu_state *cpu, struct tlb *tlb,
        qword_t guest_addr, unsigned size, unsigned op, qword_t mask,
        qword_t *old_out) {
    guest_addr_t addr;
    if (!amd64_atomic_addr_ok(cpu, tlb, guest_addr, size, &addr))
        return false;
    struct amd64_alu_atomic_ctx ctx = { .alu_op = op, .rhs = mask, .size = size };
    qword_t neu = 0;
    if (x86_atomic_rmw(cpu, tlb, addr, size / 8, amd64_bitop_atomic_fn, &ctx,
                old_out, &neu) != 0)
        return amd64_atomic_faulted(cpu, addr);
    return true;
}

// ---- AVX/AVX-512 (VEX/EVEX) support (GH #525) ----
//
// Ground truth (disassembly of the real Bun binary bundled by
// @anthropic-ai/claude-code -- see GH #525) shows pervasive compiler/stdlib
// function-multiversioning: baseline/AVX2/AVX-512 variants of hot routines
// (checksums, memcpy-style loops, hashing) coexist in one binary and are
// selected by a runtime CPUID check. This implements the highest-frequency
// slice of that instruction traffic at the interpreter level. Only the
// interpreter needs to understand VEX/EVEX -- the JIT's own decoder
// (gen_decode_amd64) never recognizes these lead bytes and already bails to
// the interpreter for anything it doesn't translate (jit/gen.c, the
// amd64_bridge_step fallback), so no JIT codegen changes are required for
// correctness, only for performance of code that uses these instructions.
//
// Registers 16-31 (EVEX-only, via the R'/V' extension bits) are not
// supported -- cpu_state's xmm/ymm_hi/zmm_hi arrays are sized for 16, and a
// masked/zeroing EVEX form is likewise rejected -- both report INT_UNDEFINED
// (a guest SIGILL) rather than silently computing a wrong answer.

#define AMD64_AVX_MAX_REG 32

struct amd64_vex_prefix {
    bool present;
    bool is_evex;
    unsigned map;   // 1 = 0F, 2 = 0F38, 3 = 0F3A
    unsigned pp;    // 0 = none, 1 = 66, 2 = F3, 3 = F2
    bool w;
    unsigned vvvv;  // second source register operand, 0-15
    unsigned vlen;  // 128, 256, or 512 (0 = invalid/unsupported form)
    bool r, x, b;   // register-extension bits, already un-inverted
    unsigned mask;  // EVEX aaa: k1-k7 predicate, 0 = unmasked
    bool zeroing;   // EVEX z: zero masked-out elements instead of merging
    bool bcast;     // EVEX b: memory operand is one element, broadcast to all
    bool reg_hi;    // EVEX R': the reg operand is in the 16-31 range
};

// Decodes the VEX/EVEX payload following a 0xC4/0xC5/0x62 lead byte the
// caller already consumed. Returns false only on a genuine fetch failure
// (end of mapped memory); a form this doesn't support (masked/zeroing EVEX,
// register range 16-31) is reported via vex->present == false so the caller
// falls back to INT_UNDEFINED like any other unimplemented opcode.
static inline bool amd64_decode_vex(struct cpu_state *cpu, struct tlb *tlb,
        byte_t lead, struct amd64_vex_prefix *vex) {
    memset(vex, 0, sizeof(*vex));
    if (lead == 0xc5) {
        byte_t b1;
        if (!amd64_fetch_u8(cpu, tlb, &b1))
            return false;
        vex->r = (b1 & 0x80) == 0;
        // 2-byte VEX has no X/B fields; they are implicitly zero (no register
        // extension), NOT "set" -- treating them as set would add 8 to every
        // rm/base register number.
        vex->x = false;
        vex->b = false;
        vex->map = 1; // 2-byte VEX always implies the 0F map
        vex->vvvv = (~(b1 >> 3)) & 0xf;
        vex->vlen = (b1 & 0x4) ? 256 : 128;
        vex->pp = b1 & 0x3;
        vex->present = true;
        return true;
    }
    if (lead == 0xc4) {
        byte_t b1, b2;
        if (!amd64_fetch_u8(cpu, tlb, &b1) || !amd64_fetch_u8(cpu, tlb, &b2))
            return false;
        vex->r = (b1 & 0x80) == 0;
        vex->x = (b1 & 0x40) == 0;
        vex->b = (b1 & 0x20) == 0;
        vex->map = b1 & 0x1f;
        vex->w = (b2 & 0x80) != 0;
        vex->vvvv = (~(b2 >> 3)) & 0xf;
        vex->vlen = (b2 & 0x4) ? 256 : 128;
        vex->pp = b2 & 0x3;
        vex->present = vex->map >= 1 && vex->map <= 3;
        return true;
    }
    if (lead == 0x62) {
        byte_t p0, p1, p2;
        if (!amd64_fetch_u8(cpu, tlb, &p0) || !amd64_fetch_u8(cpu, tlb, &p1) || !amd64_fetch_u8(cpu, tlb, &p2))
            return false;
        vex->is_evex = true;
        vex->r = (p0 & 0x80) == 0;
        vex->x = (p0 & 0x40) == 0;
        vex->b = (p0 & 0x20) == 0;
        bool rprime = (p0 & 0x10) == 0; // R': high bit of the reg operand
        vex->map = p0 & 0x3;
        vex->w = (p1 & 0x80) != 0;
        vex->vvvv = (~(p1 >> 3)) & 0xf;
        if ((p1 & 0x4) == 0)
            return true; // reserved bit must be 1; leave vex->present false
        vex->pp = p1 & 0x3;
        vex->zeroing = (p2 & 0x80) != 0;
        unsigned ll = (p2 >> 5) & 0x3;
        vex->vlen = ll == 0 ? 128 : ll == 1 ? 256 : ll == 2 ? 512 : 0;
        vex->mask = p2 & 0x7;
        vex->bcast = (p2 & 0x10) != 0;
        bool vprime = (p2 & 0x8) == 0; // V': high bit of the vvvv operand
        if (rprime)
            vex->reg_hi = true;
        if (vprime)
            vex->vvvv |= 16;
        vex->present = vex->vlen != 0 && vex->map >= 1 && vex->map <= 3;
        return true;
    }
    return false;
}

static inline void amd64_vec_reg_read(struct cpu_state *cpu, unsigned idx, unsigned vlen, uint8_t *out) {
    memcpy(out, avx_xmm(cpu, idx), 16);
    if (vlen >= 256)
        memcpy(out + 16, &cpu->ymm_hi[idx], 16);
    if (vlen >= 512)
        memcpy(out + 32, cpu->zmm_hi[idx].u8, 32);
}

// Writes the low `vlen` bits of register idx and zeroes everything above it,
// matching real hardware's VEX/EVEX "zero the upper bits of the destination"
// semantics (e.g. a VEX.128 write to xmm3 zeroes ymm3's and zmm3's upper
// bits too).
static inline void amd64_vec_reg_write(struct cpu_state *cpu, unsigned idx, unsigned vlen, const uint8_t *in) {
    memcpy(avx_xmm(cpu, idx), in, 16);
    if (vlen >= 256)
        memcpy(&cpu->ymm_hi[idx], in + 16, 16);
    else
        memset(&cpu->ymm_hi[idx], 0, 16);
    if (vlen >= 512)
        memcpy(cpu->zmm_hi[idx].u8, in + 32, 32);
    else
        memset(cpu->zmm_hi[idx].u8, 0, 32);
}

static inline bool amd64_vec_read_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned vlen, uint8_t *out) {
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_AVX_MAX_REG)
            return false;
        amd64_vec_reg_read(cpu, modrm->rm, vlen, out);
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_read(cpu, tlb, addr, out, vlen / 8);
}

static inline bool amd64_vec_write_rm(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, unsigned vlen, const uint8_t *in) {
    if (modrm->is_reg) {
        if (modrm->rm >= AMD64_AVX_MAX_REG)
            return false;
        amd64_vec_reg_write(cpu, modrm->rm, vlen, in);
        return true;
    }
    qword_t addr = amd64_effective_addr(cpu, modrm, fs_prefix);
    return amd64_mem_write(cpu, tlb, addr, in, vlen / 8);
}

// ---- dispatch ----

struct amd64_vex_ctx {
    struct cpu_state *cpu;
    struct tlb *tlb;
    qword_t saved_rip;
    struct amd64_vex_prefix vex;
    struct amd64_rex_prefix rex;
    bool fs_prefix;
    unsigned vlen;
};

// Writes a result honouring an EVEX predicate: elements whose mask bit is
// clear either keep the destination's previous value (merging, the default) or
// become zero (z=1). elem_bytes is the masking granularity, which is a
// property of the specific instruction -- VPADDB masks per byte, VPADDQ per
// qword -- so every caller has to pass its own.
static void amd64_vec_write_masked(struct amd64_vex_ctx *c, unsigned reg,
        unsigned vlen, const uint8_t *result, unsigned elem_bytes) {
    if (!c->vex.is_evex || c->vex.mask == 0) {
        amd64_vec_reg_write(c->cpu, reg, vlen, result);
        return;
    }
    uint8_t merged[64];
    amd64_vec_reg_read(c->cpu, reg, vlen, merged);
    uint64_t k = c->cpu->avx512_k[c->vex.mask];
    unsigned n = (vlen / 8) / elem_bytes;
    for (unsigned i = 0; i < n; i++) {
        if (k & (UINT64_C(1) << i))
            memcpy(merged + i * elem_bytes, result + i * elem_bytes, elem_bytes);
        else if (c->vex.zeroing)
            memset(merged + i * elem_bytes, 0, elem_bytes);
    }
    amd64_vec_reg_write(c->cpu, reg, vlen, merged);
}

// AVX-512 opmask (k register) instructions. Like BMI these are VEX-encoded
// without being vector ops -- they manipulate the 8 predicate registers that
// every masked EVEX instruction selects between. Width comes from pp and W:
// pp 0 = W (16-bit) or Q (64-bit) by W, pp 1 (66) = B (8-bit) or D (32-bit).
static unsigned amd64_kreg_width(unsigned pp, bool w) {
    if (pp == 1)
        return w ? 32 : 8;
    return w ? 64 : 16;
}

static int amd64_vex_kreg(struct amd64_vex_ctx *c, byte_t op) {
    struct cpu_state *cpu = c->cpu;
    struct tlb *tlb = c->tlb;
    struct amd64_modrm modrm;
    unsigned width = amd64_kreg_width(c->vex.pp, c->vex.w);
    uint64_t wmask = width >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << width) - 1;

    if (op >= 0x90 && op <= 0x93) {
        if (!amd64_decode_modrm(cpu, tlb, c->rex, &modrm))
            return INT_GPF;
        unsigned kreg = modrm.reg & 7;
        if (op == 0x92) { // KMOV k, r32 -- width from pp: F2 = D/Q, F3 = B, none = W
            if (!modrm.is_reg)
                return INT_UNDEFINED;
            unsigned gw = c->vex.pp == 3 ? (c->vex.w ? 64 : 32)
                        : c->vex.pp == 2 ? 8 : 16;
            uint64_t gm = gw >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << gw) - 1;
            cpu->avx512_k[kreg] = amd64_reg_get(cpu, modrm.rm, 64) & gm;
            return INT_NONE;
        }
        if (op == 0x93) { // KMOV r32, k
            if (!modrm.is_reg)
                return INT_UNDEFINED;
            unsigned gw = c->vex.pp == 3 ? (c->vex.w ? 64 : 32)
                        : c->vex.pp == 2 ? 8 : 16;
            uint64_t gm = gw >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << gw) - 1;
            // Always a 32- or 64-bit GPR write even for the 8/16-bit forms.
            amd64_reg_set(cpu, modrm.reg, gw > 32 ? 64 : 32,
                          cpu->avx512_k[modrm.rm & 7] & gm);
            return INT_NONE;
        }
        if (op == 0x90) { // KMOV k, k/m
            if (modrm.is_reg) {
                cpu->avx512_k[kreg] = cpu->avx512_k[modrm.rm & 7] & wmask;
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                uint64_t v = 0;
                if (!amd64_mem_read(cpu, tlb, addr, &v, width / 8))
                    return INT_PF;
                cpu->avx512_k[kreg] = v & wmask;
            }
            return INT_NONE;
        }
        // op == 0x91: KMOV m, k
        if (modrm.is_reg)
            return INT_UNDEFINED;
        {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            uint64_t v = cpu->avx512_k[kreg] & wmask;
            if (!amd64_mem_write(cpu, tlb, addr, &v, width / 8))
                return INT_PF;
        }
        return INT_NONE;
    }

    // Binary and unary logic: KAND/KANDN/KNOT/KOR/KXNOR/KXOR/KADD, plus
    // KUNPCK which concatenates two half-width masks.
    if ((op >= 0x41 && op <= 0x47) || op == 0x4a || op == 0x4b) {
        if (!amd64_decode_modrm(cpu, tlb, c->rex, &modrm) || !modrm.is_reg)
            return INT_GPF;
        uint64_t a = cpu->avx512_k[c->vex.vvvv & 7];
        uint64_t b = cpu->avx512_k[modrm.rm & 7];
        uint64_t r;
        switch (op) {
        case 0x41: r = a & b; break;
        case 0x42: r = ~a & b; break;
        case 0x44: r = ~b; break;               // KNOT has no vvvv operand
        case 0x45: r = a | b; break;
        case 0x46: r = ~(a ^ b); break;
        case 0x47: r = a ^ b; break;
        case 0x4a: r = a + b; break;
        default: {                              // 0x4b KUNPCK
            unsigned half = width / 2;
            uint64_t hm = (UINT64_C(1) << half) - 1;
            r = ((a & hm) << half) | (b & hm);
            break;
        }
        }
        cpu->avx512_k[modrm.reg & 7] = r & wmask;
        return INT_NONE;
    }

    if (op == 0x98 || op == 0x99) { // KORTEST / KTEST
        if (!amd64_decode_modrm(cpu, tlb, c->rex, &modrm) || !modrm.is_reg)
            return INT_GPF;
        uint64_t a = cpu->avx512_k[modrm.reg & 7] & wmask;
        uint64_t b = cpu->avx512_k[modrm.rm & 7] & wmask;
        bool zf, cf;
        if (op == 0x98) {              // KORTEST: ZF = OR is zero, CF = OR is all ones
            uint64_t t = (a | b) & wmask;
            zf = t == 0;
            cf = t == wmask;
        } else {                       // KTEST: ZF = AND is zero, CF = ANDN is zero
            zf = ((a & b) & wmask) == 0;
            cf = ((~a & b) & wmask) == 0;
        }
        cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
        cpu->af_ops = 0;
        cpu->zf = zf;
        cpu->cf = cf;
        cpu->sf = cpu->pf = cpu->af = cpu->of = 0;
        collapse_flags(cpu);
        return INT_NONE;
    }

    return INT_UNDEFINED;
}

// Decodes ModRM and then applies EVEX's extra register-extension bits, which
// the shared decoder cannot know about: R' is the reg operand's bit 4, and for
// a REGISTER r/m operand X doubles as the r/m operand's bit 4 (for a memory
// operand X keeps its usual meaning as the SIB index extension).
static bool amd64_vex_decode_modrm(struct amd64_vex_ctx *c, struct amd64_modrm *modrm) {
    if (!amd64_decode_modrm(c->cpu, c->tlb, c->rex, modrm))
        return false;
    if (c->vex.is_evex) {
        if (c->vex.reg_hi)
            modrm->reg |= 16;
        if (modrm->is_reg && c->vex.x)
            modrm->rm |= 16;
    }
    return true;
}

// Masking granularity for an instruction, in bytes, or 0 if this front-end
// cannot apply a predicate to it. Having one function answer this means the
// gate in amd64_vex_step and the writebacks in the handlers can never
// disagree -- a handler that silently dropped the predicate would write
// elements the guest asked to preserve.
static unsigned amd64_vex_mask_elem(struct amd64_vex_ctx *c, byte_t op) {
    if (c->vex.map == 1) {
        switch (op) {
        // Moves: the element size comes from the prefix and W bit
        // (VMOVDQU8/16 vs VMOVDQA32/64 vs VMOVDQU32/64).
        case 0x6f: case 0x7f:
            return c->vex.pp == 3 ? (c->vex.w ? 2 : 1) : (c->vex.w ? 8 : 4);
        case 0x10: case 0x11: case 0x28: case 0x29:
            return c->vex.pp == 1 ? 8 : 4;
        // Bitwise ops are byte-wise in effect but mask per dword/qword by W.
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0xef: case 0xeb: case 0xdb: case 0xdf:
            return c->vex.w ? 8 : 4;
        // Everything else masks at its own lane width.
        case 0xfc: case 0xf8: case 0x74: case 0x64:
        case 0xd8: case 0xdc: case 0xe8: case 0xec:
        case 0xda: case 0xde: case 0xe0:
            return 1;
        case 0xfd: case 0xf9: case 0x75: case 0x65:
        case 0xd9: case 0xdd: case 0xe9: case 0xed:
        case 0xea: case 0xee: case 0xe3: case 0xd5: case 0xe5: case 0xe4:
        case 0x71: case 0xd1: case 0xe1: case 0xf1:
            return 2;
        case 0xfe: case 0xfa: case 0x76: case 0x66:
        case 0x72: case 0xd2: case 0xe2: case 0xf2:
        case 0x70:
            return 4;
        case 0xd4: case 0xfb: case 0x73: case 0xd3: case 0xf3: case 0xf4:
            return 8;
        // Packed FP arithmetic masks per element by prefix.
        case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x51:
            return c->vex.pp == 1 || c->vex.pp == 3 ? 8 : 4;
        default:
            return 0;
        }
    }
    if (c->vex.map == 2) {
        switch (op) {
        case 0x00: case 0x04: return 1;
        case 0x38: case 0x3c: case 0x1c: return 1;
        case 0x3a: case 0x3e: case 0x1d: return 2;
        case 0x39: case 0x3b: case 0x3d: case 0x3f:
        case 0x40: case 0x1e: case 0x36:
            return 4;
        case 0x29: case 0x37: return 8;
        case 0x45: case 0x46: case 0x47: return c->vex.w ? 8 : 4;
        case 0x50: case 0x52: return 4;
        default: return 0;
        }
    }
    return 0;
}

// Reads the r/m operand, honouring EVEX's embedded-broadcast bit: with b set
// and a memory operand the instruction reads a single element and replicates
// it across the register, so the access is 4 or 8 bytes rather than the full
// operation width. Embedded broadcast is only defined for the 32/64-bit
// element instructions, hence the W-derived element size.
static bool amd64_vex_read_rm(struct amd64_vex_ctx *c, const struct amd64_modrm *modrm,
        unsigned vlen, uint8_t *out) {
    if (c->vex.is_evex && c->vex.bcast && !modrm->is_reg) {
        unsigned lb = c->vex.w ? 8 : 4;
        uint8_t elem[8];
        qword_t addr = amd64_effective_addr(c->cpu, modrm, c->fs_prefix);
        if (!amd64_mem_read(c->cpu, c->tlb, addr, elem, lb))
            return false;
        avx_broadcast(lb, vlen, elem, out);
        return true;
    }
    return amd64_vec_read_rm(c->cpu, c->tlb, modrm, c->fs_prefix, vlen, out);
}

// Legacy-map (0F) instructions.
static int amd64_vex_map_0f(struct amd64_vex_ctx *c, byte_t op) {
    struct cpu_state *cpu = c->cpu;
    struct tlb *tlb = c->tlb;
    unsigned vlen = c->vlen;
    struct amd64_modrm modrm;
    uint8_t a[64], b[64], out[64];

    // Opmask (k register) instructions share this map but are not vector ops.
    if (!c->vex.is_evex &&
        ((op >= 0x90 && op <= 0x93) || (op >= 0x41 && op <= 0x47) ||
         op == 0x4a || op == 0x4b || op == 0x98 || op == 0x99))
        return amd64_vex_kreg(c, op);

    // VZEROUPPER (L=0) / VZEROALL (L=1) take no operands at all.
    if (op == 0x77 && c->vex.pp == 0) {
        for (unsigned i = 0; i < AMD64_AVX_MAX_REG; i++) {
            memset(&cpu->ymm_hi[i], 0, sizeof(cpu->ymm_hi[i]));
            memset(cpu->zmm_hi[i].u8, 0, sizeof(cpu->zmm_hi[i].u8));
            if (vlen == 256)
                memset(&cpu->xmm[i], 0, sizeof(cpu->xmm[i]));
        }
        return INT_NONE;
    }

    // Full-width moves. The legal prefixes differ per opcode: 10/11 and 28/29
    // are the packed-float moves (pp 0 = PS, pp 1 = PD), where F3/F2 would
    // instead mean the *scalar* MOVSS/MOVSD (different semantics, not handled
    // here); 6F/7F are the integer moves (pp 1 = MOVDQA, pp 2 = MOVDQU),
    // where pp 0 would be a legacy MMX form with no VEX encoding.
    // VMOVSS/VMOVSD (F3/F2 0F 10/11): scalar, and register-to-register form
    // merges with vvvv rather than zeroing the rest of the low 128 bits.
    if ((op == 0x10 || op == 0x11) && c->vex.pp >= 2) {
        bool is_double = c->vex.pp == 3;
        unsigned lb = is_double ? 8 : 4;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (op == 0x10) {
            memset(out, 0, sizeof(out));
            if (modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
                amd64_vec_reg_read(cpu, modrm.rm, 128, a);
                memcpy(out, a, lb);
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                if (!amd64_mem_read(cpu, tlb, addr, out, lb))
                    return INT_PF;
            }
            amd64_vec_reg_write(cpu, modrm.reg, 128, out);
        } else {
            amd64_vec_reg_read(cpu, modrm.reg, 128, a);
            if (modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
                memcpy(out, a, lb);
                amd64_vec_reg_write(cpu, modrm.rm, 128, out);
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                if (!amd64_mem_write(cpu, tlb, addr, a, lb))
                    return INT_PF;
            }
        }
        return INT_NONE;
    }

    bool is_load = op == 0x10 || op == 0x28 || op == 0x6f;
    bool is_store = op == 0x11 || op == 0x29 || op == 0x7f;
    if (is_load || is_store) {
        // 6F/7F: 66 = MOVDQA32/64, F3 = MOVDQU32/64, and under EVEX F2 adds
        // MOVDQU8/16 (the byte/word-granular masked moves).
        bool prefix_ok = (op == 0x6f || op == 0x7f)
            ? (c->vex.pp == 1 || c->vex.pp == 2 || (c->vex.pp == 3 && c->vex.is_evex))
            : (c->vex.pp == 0 || c->vex.pp == 1);
        if (!prefix_ok)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // Masking granularity for 6F/7F comes from the prefix and W bit:
        // F2 selects the byte/word forms (VMOVDQU8/16), 66 and F3 the
        // dword/qword ones (VMOVDQA32/64, VMOVDQU32/64).
        unsigned elem = (op == 0x6f || op == 0x7f)
            ? (c->vex.pp == 3 ? (c->vex.w ? 2 : 1) : (c->vex.w ? 8 : 4))
            : (c->vex.pp == 1 ? 8 : 4);
        if (is_load) {
            if (!amd64_vex_read_rm(c, &modrm, vlen, out))
                return INT_PF;
            amd64_vec_write_masked(c, modrm.reg, vlen, out, elem);
        } else {
            amd64_vec_reg_read(cpu, modrm.reg, vlen, out);
            if (modrm.is_reg) {
                // A masked register-to-register store still merges, so it
                // cannot take the plain write path.
                amd64_vec_write_masked(c, modrm.rm, vlen, out, elem);
            } else if (!amd64_vec_write_rm(cpu, tlb, &modrm, c->fs_prefix, vlen, out)) {
                return INT_PF;
            }
        }
        return INT_NONE;
    }

    // Three-operand elementwise ops: dst(modrm.reg) = vvvv OP rm.
    {
        enum avx_op kind;
        unsigned lb = 1;
        bool matched = true;
        switch (op) {
        case 0x57: kind = AVX_XOR; break;                        // xorps/xorpd
        case 0x54: kind = AVX_AND; break;                        // andps/andpd
        case 0x55: kind = AVX_ANDN; break;                       // andnps/andnpd
        case 0x56: kind = AVX_OR; break;                         // orps/orpd
        case 0xef: kind = AVX_XOR; break;                        // pxor
        case 0xeb: kind = AVX_OR; break;                         // por
        case 0xdb: kind = AVX_AND; break;                        // pand
        case 0xdf: kind = AVX_ANDN; break;                       // pandn
        case 0xfc: kind = AVX_ADD; lb = 1; break;                // paddb
        case 0xfd: kind = AVX_ADD; lb = 2; break;                // paddw
        case 0xfe: kind = AVX_ADD; lb = 4; break;                // paddd
        case 0xd4: kind = AVX_ADD; lb = 8; break;                // paddq
        case 0xf8: kind = AVX_SUB; lb = 1; break;                // psubb
        case 0xf9: kind = AVX_SUB; lb = 2; break;                // psubw
        case 0xfa: kind = AVX_SUB; lb = 4; break;                // psubd
        case 0xfb: kind = AVX_SUB; lb = 8; break;                // psubq
        case 0xec: kind = AVX_ADDS; lb = 1; break;               // paddsb
        case 0xed: kind = AVX_ADDS; lb = 2; break;               // paddsw
        case 0xdc: kind = AVX_ADDUS; lb = 1; break;              // paddusb
        case 0xdd: kind = AVX_ADDUS; lb = 2; break;              // paddusw
        case 0xe8: kind = AVX_SUBS; lb = 1; break;               // psubsb
        case 0xe9: kind = AVX_SUBS; lb = 2; break;               // psubsw
        case 0xd8: kind = AVX_SUBUS; lb = 1; break;              // psubusb
        case 0xd9: kind = AVX_SUBUS; lb = 2; break;              // psubusw
        case 0x74: kind = AVX_CMPEQ; lb = 1; break;              // pcmpeqb
        case 0x75: kind = AVX_CMPEQ; lb = 2; break;              // pcmpeqw
        case 0x76: kind = AVX_CMPEQ; lb = 4; break;              // pcmpeqd
        case 0x64: kind = AVX_CMPGT; lb = 1; break;              // pcmpgtb
        case 0x65: kind = AVX_CMPGT; lb = 2; break;              // pcmpgtw
        case 0x66: kind = AVX_CMPGT; lb = 4; break;              // pcmpgtd
        case 0xda: kind = AVX_MINU; lb = 1; break;               // pminub
        case 0xde: kind = AVX_MAXU; lb = 1; break;               // pmaxub
        case 0xea: kind = AVX_MINS; lb = 2; break;               // pminsw
        case 0xee: kind = AVX_MAXS; lb = 2; break;               // pmaxsw
        case 0xe0: kind = AVX_AVG; lb = 1; break;                // pavgb
        case 0xe3: kind = AVX_AVG; lb = 2; break;                // pavgw
        case 0xd5: kind = AVX_MULLO; lb = 2; break;              // pmullw
        case 0xe5: kind = AVX_MULHI; lb = 2; break;              // pmulhw
        case 0xe4: kind = AVX_MULHIU; lb = 2; break;             // pmulhuw
        default: matched = false; break;
        }
        if (matched && c->vex.is_evex &&
            (kind == AVX_CMPEQ || kind == AVX_CMPGT) && c->vex.pp == 1) {
            if (!amd64_vex_decode_modrm(c, &modrm))
                return INT_GPF;
            if (c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            if (!amd64_vex_read_rm(c, &modrm, vlen, b))
                return INT_PF;
            amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
            unsigned n = (vlen / 8) / lb;
            uint64_t result = 0;
            for (unsigned i = 0; i < n; i++) {
                uint64_t x = avx_lane_get(a + i * lb, lb);
                uint64_t y = avx_lane_get(b + i * lb, lb);
                bool hit = kind == AVX_CMPEQ
                    ? x == y
                    : avx_lane_sext(x, lb) > avx_lane_sext(y, lb);
                if (hit)
                    result |= UINT64_C(1) << i;
            }
            // A predicate on a compare is a zeroing AND, never a merge.
            if (c->vex.mask != 0)
                result &= cpu->avx512_k[c->vex.mask];
            cpu->avx512_k[modrm.reg & 7] = result;
            return INT_NONE;
        }
        if (matched) {
            // The float-logical forms (54-57) are pp 0 (PS) or 1 (PD); every
            // integer op here requires the 66 prefix.
            bool float_logical = op >= 0x54 && op <= 0x57;
            if (float_logical ? (c->vex.pp > 1) : (c->vex.pp != 1))
                return INT_UNDEFINED;
            if (!amd64_vex_decode_modrm(c, &modrm))
                return INT_GPF;
            if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            if (!amd64_vex_read_rm(c, &modrm, vlen, b))
                return INT_PF;
            amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
            avx_binop(kind, lb, vlen, a, b, out);
            // The bitwise ops are byte-wise here but mask per dword/qword by W
            // (VPANDD vs VPANDQ); the arithmetic ops mask at their lane width.
            amd64_vec_write_masked(c, modrm.reg, vlen, out,
                                   (op == 0x54 || op == 0x55 || op == 0x56 || op == 0x57 ||
                                    op == 0xef || op == 0xeb || op == 0xdb || op == 0xdf)
                                   ? (c->vex.w ? 8 : 4) : lb);
            return INT_NONE;
        }
    }

    // Packed / scalar floating point. pp picks the flavour: 0 = packed
    // single, 1 = packed double, 2 = scalar single, 3 = scalar double.
    // The scalar forms compute only lane 0 and take the rest of the low 128
    // bits from src1 (vvvv), which is why they can't share the packed path.
    {
        enum avx_fp_op fop;
        bool matched = true;
        switch (op) {
        case 0x58: fop = AVX_FADD; break;
        case 0x59: fop = AVX_FMUL; break;
        case 0x5c: fop = AVX_FSUB; break;
        case 0x5d: fop = AVX_FMIN; break;
        case 0x5e: fop = AVX_FDIV; break;
        case 0x5f: fop = AVX_FMAX; break;
        default: matched = false; break;
        }
        if (matched) {
            bool scalar = c->vex.pp >= 2;
            bool is_double = c->vex.pp == 1 || c->vex.pp == 3;
            if (!amd64_vex_decode_modrm(c, &modrm))
                return INT_GPF;
            if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            unsigned width = scalar ? 128 : vlen;
            if (scalar && modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                amd64_vec_reg_read(cpu, modrm.rm, 128, b);
            } else if (scalar) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                memset(b, 0, sizeof(b));
                if (!amd64_mem_read(cpu, tlb, addr, b, is_double ? 8 : 4))
                    return INT_PF;
            } else if (!amd64_vex_read_rm(c, &modrm, vlen, b)) {
                return INT_PF;
            }
            amd64_vec_reg_read(cpu, c->vex.vvvv, width, a);
            memcpy(out, a, sizeof(out));
            avx_fp_binop(fop, is_double, scalar ? (is_double ? 64 : 32) : vlen, a, b, out);
            amd64_vec_reg_write(cpu, modrm.reg, width, out);
            return INT_NONE;
        }
    }

    // Ops with their own shape.
    switch (op) {
    case 0x2a: { // VCVTSI2SS / VCVTSI2SD -- integer GPR/mem to scalar float
        if (c->vex.pp < 2)
            return INT_UNDEFINED;
        bool is_double = c->vex.pp == 3;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        qword_t src;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, c->vex.w ? 64 : 32, &src))
            return INT_PF;
        int64_t sv = c->vex.w ? (int64_t) src : (int64_t) (int32_t) src;
        amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
        if (is_double) {
            double d = (double) sv;
            memcpy(out, &d, 8);
        } else {
            float f = (float) sv;
            memcpy(out, &f, 4);
        }
        amd64_vec_reg_write(cpu, modrm.reg, 128, out);
        return INT_NONE;
    }
    case 0x2c: case 0x2d: { // VCVTTSS2SI / VCVTSS2SI (and the SD forms)
        if (c->vex.pp < 2)
            return INT_UNDEFINED;
        bool is_double = c->vex.pp == 3;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, 128, a))
            return INT_PF;
        double v;
        if (is_double) {
            memcpy(&v, a, 8);
        } else {
            float f;
            memcpy(&f, a, 4);
            v = f;
        }
        // 2C truncates toward zero; 2D rounds to nearest (the emulator runs
        // round-to-nearest and does not model MXCSR's rounding field).
        double r = op == 0x2c ? (v < 0 ? ceil(v) : floor(v)) : nearbyint(v);
        amd64_reg_set(cpu, modrm.reg, c->vex.w ? 64 : 32, (qword_t) (int64_t) r);
        return INT_NONE;
    }
    case 0x5a: { // VCVTSS2SD / VCVTSD2SS / VCVTPS2PD / VCVTPD2PS
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (c->vex.pp >= 2) { // scalar: merge the rest from vvvv
            bool to_double = c->vex.pp == 2; // F3 = SS->SD, F2 = SD->SS
            if (c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            if (!amd64_vex_read_rm(c, &modrm, 128, a))
                return INT_PF;
            amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
            if (to_double) {
                float f;
                memcpy(&f, a, 4);
                double d = f;
                memcpy(out, &d, 8);
            } else {
                double d;
                memcpy(&d, a, 8);
                float f = (float) d;
                memcpy(out, &f, 4);
            }
            amd64_vec_reg_write(cpu, modrm.reg, 128, out);
            return INT_NONE;
        }
        // Packed: PS->PD widens (source is half as wide), PD->PS narrows.
        bool widen = c->vex.pp == 0;
        if (!amd64_vex_read_rm(c, &modrm, widen ? vlen / 2 : vlen, a))
            return INT_PF;
        unsigned n = (vlen / 8) / 8;
        for (unsigned i = 0; i < n; i++) {
            if (widen) {
                float f;
                memcpy(&f, a + i * 4, 4);
                double d = f;
                memcpy(out + i * 8, &d, 8);
            } else {
                double d;
                memcpy(&d, a + i * 8, 8);
                float f = (float) d;
                memcpy(out + i * 4, &f, 4);
            }
        }
        amd64_vec_reg_write(cpu, modrm.reg, widen ? vlen : vlen / 2, out);
        return INT_NONE;
    }
    case 0xc4: { // VPINSRW xmm, r32/m16, imm8
        if (c->vex.pp != 1 || vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        qword_t value;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, modrm.is_reg ? 32 : 16, &value))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
        avx_lane_put(out + (imm & 7) * 2, 2, value & 0xffff);
        amd64_vec_reg_write(cpu, modrm.reg, 128, out);
        return INT_NONE;
    }
    case 0x13: case 0x17: { // VMOVLPS/VMOVLPD (13), VMOVHPS/VMOVHPD (17) store
        if (c->vex.pp > 1 || vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm) || modrm.is_reg)
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.reg, 128, a);
        qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
        // 13 stores the LOW qword, 17 the high one.
        if (!amd64_mem_write(cpu, tlb, addr, a + (op == 0x17 ? 8 : 0), 8))
            return INT_PF;
        return INT_NONE;
    }
    case 0xe6: { // VCVTTPD2DQ (66), VCVTDQ2PD (F3), VCVTPD2DQ (F2)
        if (c->vex.pp == 0)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (c->vex.pp == 2) { // VCVTDQ2PD: dwords widen to doubles
            if (!amd64_vex_read_rm(c, &modrm, vlen / 2, a))
                return INT_PF;
            unsigned n = (vlen / 8) / 8;
            for (unsigned i = 0; i < n; i++) {
                double d = (double) (int32_t) avx_lane_get(a + i * 4, 4);
                memcpy(out + i * 8, &d, 8);
            }
            amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
            return INT_NONE;
        }
        // Doubles narrow to dwords, so the result is half the source width.
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        unsigned n = (vlen / 8) / 8;
        memset(out, 0, sizeof(out));
        for (unsigned i = 0; i < n; i++) {
            double d;
            memcpy(&d, a + i * 8, 8);
            // 66 truncates toward zero, F2 rounds to nearest.
            double r = c->vex.pp == 1 ? (d < 0 ? ceil(d) : floor(d)) : nearbyint(d);
            avx_lane_put(out + i * 4, 4, (uint32_t) (int32_t) r);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen == 512 ? 256 : 128, out);
        return INT_NONE;
    }
    case 0x2e: case 0x2f: { // VUCOMISS/VUCOMISD (2E), VCOMISS/VCOMISD (2F)
        // Scalar compare that writes EFLAGS rather than a register. The two
        // differ only in which NaNs raise the invalid-operation exception,
        // which this emulator does not model, so they behave identically.
        if (c->vex.pp > 1)
            return INT_UNDEFINED;
        bool is_double = c->vex.pp == 1;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, 128, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, modrm.reg, 128, a);
        double x, y;
        if (is_double) {
            memcpy(&x, a, 8);
            memcpy(&y, b, 8);
        } else {
            float fx, fy;
            memcpy(&fx, a, 4);
            memcpy(&fy, b, 4);
            x = fx; y = fy;
        }
        bool unordered = x != x || y != y;
        cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
        cpu->af_ops = 0;
        // Unordered sets ZF/PF/CF; otherwise PF is clear and ZF/CF encode the
        // ordering (CF = less than, ZF = equal).
        cpu->zf = unordered || x == y;
        cpu->pf = unordered;
        cpu->cf = unordered || x < y;
        cpu->sf = cpu->af = cpu->of = 0;
        collapse_flags(cpu);
        return INT_NONE;
    }
    case 0x12: { // VMOVDDUP (F2): duplicate the low qword of each 128-bit lane
        if (c->vex.pp != 3)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
            memcpy(out + lane, a + lane, 8);
            memcpy(out + lane + 8, a + lane, 8);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x5b: { // VCVTDQ2PS (none) / VCVTPS2DQ (66) / VCVTTPS2DQ (F3)
        if (c->vex.pp == 3)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        for (unsigned i = 0; i < vlen / 8; i += 4) {
            if (c->vex.pp == 0) {            // int32 -> float
                float fv = (float) (int32_t) avx_lane_get(a + i, 4);
                memcpy(out + i, &fv, 4);
            } else {                          // float -> int32
                float fv;
                memcpy(&fv, a + i, 4);
                // 66 rounds to nearest, F3 truncates toward zero.
                double d = c->vex.pp == 1 ? nearbyint(fv) : (fv < 0 ? ceil(fv) : floor(fv));
                avx_lane_put(out + i, 4, (uint32_t) (int32_t) d);
            }
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, 4);
        return INT_NONE;
    }
    case 0x51: { // vsqrtps/pd/ss/sd
        bool scalar = c->vex.pp >= 2;
        bool is_double = c->vex.pp == 1 || c->vex.pp == 3;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned width = scalar ? 128 : vlen;
        if (scalar) {
            if (c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
        }
        if (!amd64_vec_read_rm(cpu, tlb, &modrm, c->fs_prefix, width, a))
            return INT_PF;
        unsigned lb = is_double ? 8 : 4;
        unsigned span = scalar ? lb : width / 8;
        for (unsigned i = 0; i < span; i += lb) {
            if (is_double) {
                double v;
                memcpy(&v, a + i, 8);
                v = sqrt(v);
                memcpy(out + i, &v, 8);
            } else {
                float v;
                memcpy(&v, a + i, 4);
                v = sqrtf(v);
                memcpy(out + i, &v, 4);
            }
        }
        amd64_vec_reg_write(cpu, modrm.reg, width, out);
        return INT_NONE;
    }
    case 0xc2: { // vcmpps/pd/ss/sd
        bool scalar = c->vex.pp >= 2;
        bool is_double = c->vex.pp == 1 || c->vex.pp == 3;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned width = scalar ? 128 : vlen;
        if (!amd64_vec_read_rm(cpu, tlb, &modrm, c->fs_prefix, width, b))
            return INT_PF;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, width, a);
        memcpy(out, a, sizeof(out));
        avx_fp_cmp(imm, is_double, scalar ? (is_double ? 64 : 32) : width, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, width, out);
        return INT_NONE;
    }
    case 0x14: case 0x15: { // vunpcklps/pd, vunpckhps/pd
        if (c->vex.pp > 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_unpack(op == 0x15, c->vex.pp == 1 ? 8 : 4, vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x50: { // vmovmskps/pd -- sign bits into a GPR
        if (c->vex.pp > 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!modrm.is_reg || modrm.rm >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.rm, vlen, a);
        unsigned lb = c->vex.pp == 1 ? 8 : 4;
        uint64_t mask = 0;
        for (unsigned i = 0, bit = 0; i < vlen / 8; i += lb, bit++)
            if (a[i + lb - 1] & 0x80)
                mask |= UINT64_C(1) << bit;
        amd64_reg_set(cpu, modrm.reg, 64, mask);
        return INT_NONE;
    }
    case 0xc6: { // vshufps/pd
        if (c->vex.pp > 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        // Lane-local: the low half of each destination lane comes from src1,
        // the high half from src2.
        for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
            uint8_t tmp[16];
            if (c->vex.pp == 1) { // shufpd: one selector bit per qword
                memcpy(tmp, a + lane + (((imm >> (lane / 8)) & 1) ? 8 : 0), 8);
                memcpy(tmp + 8, b + lane + (((imm >> (lane / 8 + 1)) & 1) ? 8 : 0), 8);
            } else {
                for (unsigned j = 0; j < 4; j++) {
                    const uint8_t *src = j < 2 ? a : b;
                    memcpy(tmp + j * 4, src + lane + ((imm >> (2 * j)) & 3) * 4, 4);
                }
            }
            memcpy(out + lane, tmp, 16);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x60: case 0x61: case 0x62: case 0x6c:   // punpckl bw/wd/dq/qdq
    case 0x68: case 0x69: case 0x6a: case 0x6d: { // punpckh bw/wd/dq/qdq
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        bool high = op >= 0x68;
        unsigned lb = (op == 0x60 || op == 0x68) ? 1
                    : (op == 0x61 || op == 0x69) ? 2
                    : (op == 0x62 || op == 0x6a) ? 4 : 8;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_unpack(high, lb, vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x63: case 0x67: case 0x6b: { // packsswb / packuswb / packssdw
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_pack(op != 0x67, op == 0x6b ? 4 : 2, vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x70: { // pshufd (66) / pshufhw (F3) / pshuflw (F2)
        if (c->vex.pp == 0)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        if (c->vex.pp == 1)
            avx_pshufd(vlen, imm, a, out);
        else
            avx_pshufw_half(vlen, imm, c->vex.pp == 2, a, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x71: case 0x72: case 0x73: { // shift group by imm8, sub-op in modrm.reg
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        // NDD form: destination is vvvv, source is the (register) rm operand.
        if (!modrm.is_reg || modrm.rm >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.rm, vlen, a);
        unsigned lb = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        unsigned sub = modrm.reg & 7;
        if (op == 0x73 && (sub == 3 || sub == 7)) { // psrldq / pslldq
            avx_byte_shift(sub == 7, vlen, imm > 16 ? 16 : imm, a, out);
        } else if (sub == 2) {
            avx_shift(AVX_SHR, lb, vlen, a, imm, out);
        } else if (sub == 4) {
            if (lb == 8)
                return INT_UNDEFINED; // no VPSRAQ outside AVX-512
            avx_shift(AVX_SAR, lb, vlen, a, imm, out);
        } else if (sub == 6) {
            avx_shift(AVX_SHL, lb, vlen, a, imm, out);
        } else {
            return INT_UNDEFINED;
        }
        amd64_vec_write_masked(c, c->vex.vvvv, vlen, out, lb);
        return INT_NONE;
    }
    case 0xd1: case 0xd2: case 0xd3:   // psrlw / psrld / psrlq
    case 0xe1: case 0xe2:              // psraw / psrad
    case 0xf1: case 0xf2: case 0xf3: { // psllw / pslld / psllq
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The count is the low 64 bits of a 128-bit operand, regardless of the
        // destination's width.
        uint8_t cnt[16];
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            memcpy(cnt, avx_xmm(cpu, modrm.rm), 16);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_read(cpu, tlb, addr, cnt, 16))
                return INT_PF;
        }
        uint64_t count = avx_lane_get(cnt, 8);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned lb = (op == 0xd1 || op == 0xe1 || op == 0xf1) ? 2
                    : (op == 0xd2 || op == 0xe2 || op == 0xf2) ? 4 : 8;
        enum avx_shift_kind kind = (op >= 0xf1) ? AVX_SHL : (op >= 0xe1) ? AVX_SAR : AVX_SHR;
        avx_shift(kind, lb, vlen, a, count, out);
        amd64_vec_write_masked(c, modrm.reg, vlen, out, lb);
        return INT_NONE;
    }
    case 0xc5: { // VPEXTRW r32, xmm, imm8 -- the 0F-map form
        if (c->vex.pp != 1 || vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm) || !modrm.is_reg)
            return INT_GPF;
        if (modrm.rm >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        amd64_vec_reg_read(cpu, modrm.rm, 128, a);
        amd64_reg_set(cpu, modrm.reg, 32, avx_lane_get(a + (imm & 7) * 2, 2));
        return INT_NONE;
    }
    case 0xd7: { // pmovmskb -- destination is a GPR, one bit per source byte
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!modrm.is_reg || modrm.rm >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.rm, vlen, a);
        uint64_t mask = 0;
        for (unsigned i = 0; i < vlen / 8; i++)
            if (a[i] & 0x80)
                mask |= UINT64_C(1) << i;
        amd64_reg_set(cpu, modrm.reg, 64, mask);
        return INT_NONE;
    }
    case 0xf4: case 0xf5: case 0xf6: { // pmuludq / pmaddwd / psadbw
        if (c->vex.pp != 1)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        if (op == 0xf4)
            avx_pmuludq(vlen, a, b, out);
        else if (op == 0xf5)
            avx_pmaddwd(vlen, a, b, out);
        else
            avx_psadbw(vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x6e: case 0x7e: case 0xd6: { // movd/movq
        // F3 0F 7E is a distinct instruction: load the low 64 bits, zeroing
        // the rest -- not the 66-prefixed store-to-GPR form below.
        if (op == 0x7e && c->vex.pp == 2) {
            if (vlen != 128)
                return INT_UNDEFINED;
            if (!amd64_vex_decode_modrm(c, &modrm))
                return INT_GPF;
            if (modrm.reg >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            memset(out, 0, sizeof(out));
            if (modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                amd64_vec_reg_read(cpu, modrm.rm, 128, a);
                memcpy(out, a, 8);
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                if (!amd64_mem_read(cpu, tlb, addr, out, 8))
                    return INT_PF;
            }
            amd64_vec_reg_write(cpu, modrm.reg, 128, out);
            return INT_NONE;
        }
        if (c->vex.pp != 1 || vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned gpr_size = c->vex.w ? 64 : 32;
        if (op == 0x6e) {
            qword_t value;
            if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, gpr_size, &value))
                return INT_PF;
            memset(out, 0, sizeof(out));
            memcpy(out, &value, gpr_size / 8);
            amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        } else if (op == 0x7e) {
            amd64_vec_reg_read(cpu, modrm.reg, vlen, out);
            qword_t value = 0;
            memcpy(&value, out, gpr_size / 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, c->fs_prefix, gpr_size, value))
                return INT_PF;
        } else {
            amd64_vec_reg_read(cpu, modrm.reg, vlen, out);
            if (modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                memset(a, 0, sizeof(a));
                memcpy(a, out, 8);
                amd64_vec_reg_write(cpu, modrm.rm, vlen, a);
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                if (!amd64_mem_write(cpu, tlb, addr, out, 8))
                    return INT_PF;
            }
        }
        return INT_NONE;
    }
    default:
        return INT_UNDEFINED;
    }
}

// Three-byte 0F38 map.
// BMI1/BMI2. These are VEX-encoded but operate on general-purpose registers,
// not vectors -- they were folded into the VEX encoding space purely to get
// three-operand forms. Measured on the GH #525 binary they are a bigger slice
// of its VEX traffic than most of the actual vector ops, because compilers
// emit SHLX/SHRX/MULX/RORX pervasively in ordinary integer code.
//
// Flag behaviour splits the family in two and is easy to get wrong: ANDN,
// BLSR, BLSMSK, BLSI, BZHI and BEXTR write flags, while SHLX/SHRX/SARX,
// MULX, RORX, PDEP and PEXT leave every flag untouched.
static int amd64_vex_bmi(struct amd64_vex_ctx *c, byte_t op) {
    struct cpu_state *cpu = c->cpu;
    struct tlb *tlb = c->tlb;
    struct amd64_modrm modrm;
    unsigned size = c->vex.w ? 64 : 32;
    qword_t src, vv;

    if (c->vex.map == 3 && op == 0xf0 && c->vex.pp == 3) { // rorx imm8
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        unsigned n = imm & (size - 1);
        qword_t val = amd64_trunc(src, size);
        qword_t r = n == 0 ? val : ((val >> n) | (val << (size - n)));
        amd64_reg_set(cpu, modrm.reg, size, amd64_trunc(r, size));
        return INT_NONE; // rorx does not touch flags
    }
    if (c->vex.map != 2)
        return INT_UNDEFINED;

    switch (op) {
    case 0xf2: { // andn: dst = ~vvvv & rm
        if (c->vex.pp != 0)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        vv = amd64_reg_get(cpu, c->vex.vvvv, size);
        qword_t r = amd64_trunc(~vv & src, size);
        amd64_reg_set(cpu, modrm.reg, size, r);
        amd64_set_logic_flags(cpu, r, size);
        return INT_NONE;
    }
    case 0xf3: { // group: /1 blsr, /2 blsmsk, /3 blsi -- destination is vvvv
        if (c->vex.pp != 0)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        qword_t v = amd64_trunc(src, size), r;
        unsigned sub = modrm.reg & 7;
        if (sub == 1) r = amd64_trunc(v & (v - 1), size);        // blsr
        else if (sub == 2) r = amd64_trunc(v ^ (v - 1), size);   // blsmsk
        else if (sub == 3) r = amd64_trunc(v & (~v + 1), size);  // blsi
        else return INT_UNDEFINED;
        amd64_reg_set(cpu, c->vex.vvvv, size, r);
        amd64_set_logic_flags(cpu, r, size);
        // CF is source-dependent rather than the logic-op zero: BLSR/BLSMSK
        // set it when the source was zero, BLSI when it was non-zero.
        cpu->cf = sub == 3 ? (v != 0) : (v == 0);
        collapse_flags(cpu);
        return INT_NONE;
    }
    case 0xf5: { // pp0 bzhi, pp2 pext, pp3 pdep
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        vv = amd64_trunc(amd64_reg_get(cpu, c->vex.vvvv, size), size);
        qword_t v = amd64_trunc(src, size);
        if (c->vex.pp == 0) { // bzhi: zero bits at and above index vvvv
            unsigned n = vv & 0xff;
            qword_t r = n >= size ? v : amd64_trunc(v & ((UINT64_C(1) << n) - 1), size);
            amd64_reg_set(cpu, modrm.reg, size, r);
            amd64_set_logic_flags(cpu, r, size);
            cpu->cf = n >= size;
            collapse_flags(cpu);
            return INT_NONE;
        }
        // pext/pdep gather/scatter source bits under a mask; no flags. The
        // mask is the r/m operand and the value is vvvv (the opposite way
        // round from BZHI/BEXTR just above, which take their control operand
        // from vvvv).
        qword_t mask = v, r = 0;
        v = vv;
        if (c->vex.pp == 2) { // pext
            for (unsigned i = 0, k = 0; i < size; i++)
                if ((mask >> i) & 1) {
                    if ((v >> i) & 1)
                        r |= UINT64_C(1) << k;
                    k++;
                }
        } else if (c->vex.pp == 3) { // pdep
            for (unsigned i = 0, k = 0; i < size; i++)
                if ((mask >> i) & 1) {
                    if ((v >> k) & 1)
                        r |= UINT64_C(1) << i;
                    k++;
                }
        } else {
            return INT_UNDEFINED;
        }
        amd64_reg_set(cpu, modrm.reg, size, amd64_trunc(r, size));
        return INT_NONE;
    }
    case 0xf6: { // mulx: unsigned multiply, no flags, high half into vvvv
        if (c->vex.pp != 3)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        qword_t implicit = amd64_trunc(amd64_reg_get(cpu, amd64_rdx, size), size);
        qword_t v = amd64_trunc(src, size);
        qword_t lo, hi;
        if (size == 32) {
            uint64_t p = (uint64_t) (uint32_t) implicit * (uint32_t) v;
            lo = (uint32_t) p;
            hi = (uint32_t) (p >> 32);
        } else {
            unsigned __int128 p = (unsigned __int128) implicit * v;
            lo = (qword_t) p;
            hi = (qword_t) (p >> 64);
        }
        // MULX r64a, r64b, r/m64 puts the HIGH half in r64a (ModRM.reg) and
        // the LOW half in r64b (vvvv). Write low first so that when both name
        // the same register the high half wins, per the architecture.
        amd64_reg_set(cpu, c->vex.vvvv, size, lo);
        amd64_reg_set(cpu, modrm.reg, size, hi);
        return INT_NONE;
    }
    case 0xf7: { // pp0 bextr, pp1 shlx, pp2 sarx, pp3 shrx
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, size, &src))
            return INT_PF;
        vv = amd64_trunc(amd64_reg_get(cpu, c->vex.vvvv, size), size);
        qword_t v = amd64_trunc(src, size), r;
        if (c->vex.pp == 0) { // bextr: start/length in the vvvv operand
            unsigned start = vv & 0xff;
            unsigned len = (vv >> 8) & 0xff;
            r = start >= size ? 0 : v >> start;
            if (len < size)
                r &= (UINT64_C(1) << len) - 1;
            r = amd64_trunc(r, size);
            amd64_reg_set(cpu, modrm.reg, size, r);
            amd64_set_logic_flags(cpu, r, size);
            return INT_NONE;
        }
        // The shift count is masked to the operand width, and these three
        // leave flags alone (unlike the legacy SHL/SHR/SAR).
        unsigned n = vv & (size - 1);
        if (c->vex.pp == 1) r = v << n;
        else if (c->vex.pp == 2) r = (qword_t) (amd64_sign_extend(v, size) >> n);
        else r = v >> n;
        amd64_reg_set(cpu, modrm.reg, size, amd64_trunc(r, size));
        return INT_NONE;
    }
    default:
        return INT_UNDEFINED;
    }
}

static int amd64_vex_map_0f38(struct amd64_vex_ctx *c, byte_t op) {
    struct cpu_state *cpu = c->cpu;
    struct tlb *tlb = c->tlb;
    unsigned vlen = c->vlen;
    struct amd64_modrm modrm;
    uint8_t a[64], b[64], out[64];

    if (op == 0xf2 || op == 0xf3 || op == 0xf5 || op == 0xf6 || op == 0xf7)
        return amd64_vex_bmi(c, op);

    // Mask <-> vector conversions are F3-prefixed, so they have to be handled
    // before the 66-only gate below.
    if (c->vex.pp == 2 && c->vex.is_evex &&
        (op == 0x28 || op == 0x29 || op == 0x38 || op == 0x39)) {
        // 28/38 = VPMOVM2{B,W}/{D,Q} (mask -> vector, all-ones per set bit)
        // 29/39 = VPMOV{B,W}2M/{D,Q}2M (vector -> mask, one bit per element MSB)
        bool to_vector = (op == 0x28 || op == 0x38);
        unsigned lb = (op == 0x28 || op == 0x29) ? (c->vex.w ? 2 : 1)
                                                 : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        unsigned n = (vlen / 8) / lb;
        if (to_vector) {
            if (modrm.reg >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            uint64_t k = cpu->avx512_k[modrm.rm & 7];
            memset(out, 0, sizeof(out));
            for (unsigned i = 0; i < n; i++)
                if (k & (UINT64_C(1) << i))
                    avx_lane_put(out + i * lb, lb, avx_lane_mask(lb));
            amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        } else {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, vlen, a);
            uint64_t k = 0;
            for (unsigned i = 0; i < n; i++)
                if (a[i * lb + lb - 1] & 0x80)
                    k |= UINT64_C(1) << i;
            cpu->avx512_k[modrm.reg & 7] = k;
        }
        return INT_NONE;
    }

    if (op == 0x26 || op == 0x27) { // VPTESTM{B,W,D,Q} / VPTESTNM* -> mask
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        bool negated = c->vex.pp == 2; // F3 selects the NM (not-mask) forms
        unsigned lb = op == 0x26 ? (c->vex.w ? 2 : 1) : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned n = (vlen / 8) / lb;
        uint64_t result = 0;
        for (unsigned i = 0; i < n; i++) {
            bool nz = (avx_lane_get(a + i * lb, lb) & avx_lane_get(b + i * lb, lb)) != 0;
            if (nz != negated)
                result |= UINT64_C(1) << i;
        }
        if (c->vex.mask != 0)
            result &= cpu->avx512_k[c->vex.mask];
        cpu->avx512_k[modrm.reg & 7] = result;
        return INT_NONE;
    }

    if (c->vex.pp != 1)
        return INT_UNDEFINED;

    if (op == 0x17) { // VPTEST -- sets ZF/CF, writes no register
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, modrm.reg, vlen, a);
        bool zf = true, cf = true;
        for (unsigned i = 0; i < vlen / 8; i++) {
            if (a[i] & b[i])
                zf = false;          // ZF = (DEST AND SRC) == 0
            if (~a[i] & b[i])
                cf = false;          // CF = (SRC AND NOT DEST) == 0
        }
        cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
        cpu->af_ops = 0;
        cpu->zf = zf;
        cpu->cf = cf;
        cpu->sf = cpu->pf = cpu->af = cpu->of = 0;
        collapse_flags(cpu);
        return INT_NONE;
    }

    if (op >= 0x7a && op <= 0x7c) { // VPBROADCAST{B,W,D,Q} from a GPR
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm) || !modrm.is_reg)
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned lb = op == 0x7a ? 1 : op == 0x7b ? 2 : (c->vex.w ? 8 : 4);
        uint64_t v = amd64_reg_get(cpu, modrm.rm, 64);
        memset(a, 0, sizeof(a));
        avx_lane_put(a, lb, v);
        avx_broadcast(lb, vlen, a, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    // Horizontal adds, the narrowing VPMOV*, VPACKUSDW, VPTESTM*, VPERMT2*,
    // and the compress/expand pair.
    if (op == 0x01 || op == 0x02 || op == 0x03 ||
        op == 0x05 || op == 0x06 || op == 0x07) {
        // VPHADD{W,D}/VPHADDSW and VPHSUB{W,D}/VPHSUBSW: adjacent pairs summed
        // (or subtracted) within each 128-bit lane, src1's results first.
        bool sub = op >= 0x05;
        unsigned lb = (op == 0x01 || op == 0x05) ? 2 : (op == 0x02 || op == 0x06) ? 4 : 2;
        bool sat = (op == 0x03 || op == 0x07);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
            uint8_t tmp[16];
            unsigned o = 0;
            for (unsigned which = 0; which < 2; which++) {
                const uint8_t *src = which == 0 ? a : b;
                for (unsigned j = 0; j < 16; j += lb * 2) {
                    int64_t x = avx_lane_sext(avx_lane_get(src + lane + j, lb), lb);
                    int64_t y = avx_lane_sext(avx_lane_get(src + lane + j + lb, lb), lb);
                    int64_t r = sub ? x - y : x + y;
                    if (sat) {
                        if (r > 32767) r = 32767;
                        if (r < -32768) r = -32768;
                    }
                    avx_lane_put(tmp + o, lb, (uint64_t) r & avx_lane_mask(lb));
                    o += lb;
                }
            }
            memcpy(out + lane, tmp, 16);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    if (op == 0x0e || op == 0x0f) { // VTESTPS / VTESTPD -- sign bits only
        unsigned lb = op == 0x0e ? 4 : 8;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, modrm.reg, vlen, a);
        bool zf = true, cf = true;
        for (unsigned i = 0; i < vlen / 8; i += lb) {
            // Unlike VPTEST these look at each element's SIGN BIT, not all bits.
            bool da = (a[i + lb - 1] & 0x80) != 0;
            bool sb = (b[i + lb - 1] & 0x80) != 0;
            if (da && sb) zf = false;
            if (!da && sb) cf = false;
        }
        cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
        cpu->af_ops = 0;
        cpu->zf = zf;
        cpu->cf = cf;
        cpu->sf = cpu->pf = cpu->af = cpu->of = 0;
        collapse_flags(cpu);
        return INT_NONE;
    }

    if (op == 0x64 || op == 0x65 || op == 0x66) { // VPBLENDM{D,Q}/{B,W}
        // Blend by mask: unlike a predicate this always writes every element,
        // choosing between the two sources rather than preserving the old dst.
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        unsigned lb = op == 0x66 ? (c->vex.w ? 2 : 1) : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        uint64_t k = c->vex.mask != 0 ? cpu->avx512_k[c->vex.mask] : ~UINT64_C(0);
        unsigned n = (vlen / 8) / lb;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *src = (k & (UINT64_C(1) << i)) ? b : a;
            memcpy(out + i * lb, src + i * lb, lb);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    if (op == 0x83) { // VPMULTISHIFTQB -- gather 8 unaligned bytes per qword
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a); // bit offsets
        for (unsigned q = 0; q < vlen / 8; q += 8) {
            uint64_t data = avx_lane_get(b + q, 8);
            for (unsigned j = 0; j < 8; j++) {
                unsigned off = a[q + j] & 63;
                // The extraction wraps around within the qword.
                uint64_t v = off == 0 ? data : ((data >> off) | (data << (64 - off)));
                out[q + j] = (uint8_t) v;
            }
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, 1);
        return INT_NONE;
    }

    if (op == 0x0b) { // VPMULHRSW -- rounded high word of a signed product
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        for (unsigned i = 0; i < vlen / 8; i += 2) {
            int32_t x = (int16_t) avx_lane_get(a + i, 2);
            int32_t y = (int16_t) avx_lane_get(b + i, 2);
            avx_lane_put(out + i, 2, (uint16_t) ((((x * y) >> 14) + 1) >> 1));
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, 2);
        return INT_NONE;
    }

    if (op == 0x28 && c->vex.pp == 1) { // VPMULDQ -- signed 32x32 -> 64 per lane
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        for (unsigned i = 0; i < vlen / 8; i += 8) {
            // Like VPMULUDQ this reads only the LOW dword of each qword lane.
            int64_t x = (int32_t) avx_lane_get(a + i, 4);
            int64_t y = (int32_t) avx_lane_get(b + i, 4);
            avx_lane_put(out + i, 8, (uint64_t) (x * y));
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, 8);
        return INT_NONE;
    }

    if (op == 0x8c || op == 0x8e) { // VPMASKMOVD/Q -- masked load (8c) / store (8e)
        unsigned lb = c->vex.w ? 8 : 4;
        if (!amd64_vex_decode_modrm(c, &modrm) || modrm.is_reg)
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a); // per-element sign-bit mask
        qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
        unsigned n = (vlen / 8) / lb;
        if (op == 0x8c) {
            memset(out, 0, sizeof(out));
            for (unsigned i = 0; i < n; i++)
                if (a[i * lb + lb - 1] & 0x80) {
                    // Only the selected elements are read, so an unselected
                    // element must not fault on an unmapped page.
                    if (!amd64_mem_read(cpu, tlb, addr + i * lb, out + i * lb, lb))
                        return INT_PF;
                }
            amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        } else {
            amd64_vec_reg_read(cpu, modrm.reg, vlen, b);
            for (unsigned i = 0; i < n; i++)
                if (a[i * lb + lb - 1] & 0x80)
                    if (!amd64_mem_write(cpu, tlb, addr + i * lb, b + i * lb, lb))
                        return INT_PF;
        }
        return INT_NONE;
    }

    if (op == 0x54 || op == 0x55) { // VPOPCNT{B,W} / VPOPCNT{D,Q}
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        unsigned lb = op == 0x54 ? (c->vex.w ? 2 : 1) : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        for (unsigned i = 0; i < vlen / 8; i += lb)
            avx_lane_put(out + i, lb, (uint64_t) __builtin_popcountll(avx_lane_get(a + i, lb)));
        amd64_vec_write_masked(c, modrm.reg, vlen, out, lb);
        return INT_NONE;
    }

    if (op == 0x2b) { // VPACKUSDW -- dwords to unsigned-saturated words
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_pack(false, 4, vlen, a, b, out);
        amd64_vec_write_masked(c, modrm.reg, vlen, out, 2);
        return INT_NONE;
    }

    if (c->vex.pp == 2 && c->vex.is_evex &&
        (op == 0x30 || op == 0x31 || op == 0x32 ||
         op == 0x33 || op == 0x34 || op == 0x35)) {
        // VPMOV{W,D,Q}{B,W,D}: truncate each element to a narrower one, writing
        // HALF (or a quarter/eighth) as many bytes than the source width.
        unsigned kind = op & 0xf;
        unsigned src_lb = kind <= 2 ? 2 : kind <= 4 ? 4 : 8;
        unsigned dst_lb = kind == 0 ? 1 : kind == 1 ? 1 : kind == 2 ? 2
                        : kind == 3 ? 2 : kind == 4 ? 4 : 4;
        if (kind == 1) { src_lb = 4; dst_lb = 1; }
        if (kind == 3) { src_lb = 8; dst_lb = 1; }
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.reg, vlen, a);
        unsigned n = (vlen / 8) / src_lb;
        for (unsigned i = 0; i < n; i++)
            avx_lane_put(out + i * dst_lb, dst_lb,
                         avx_lane_get(a + i * src_lb, src_lb) & avx_lane_mask(dst_lb));
        unsigned out_bytes = n * dst_lb;
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            memset(out + out_bytes, 0, sizeof(out) - out_bytes);
            amd64_vec_reg_write(cpu, modrm.rm, vlen, out);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_write(cpu, tlb, addr, out, out_bytes))
                return INT_PF;
        }
        return INT_NONE;
    }

    if (op == 0x62 || op == 0x63 || op == 0x88 || op == 0x89 ||
        op == 0x8a || op == 0x8b) {
        // VPEXPAND* (62/63/88/89) and VPCOMPRESS* (8a/8b/63). These are the
        // one family where the mask is not a predicate over fixed lanes: it
        // SELECTS which elements participate and packs them contiguously, so
        // it cannot go through amd64_vec_write_masked.
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        bool compress = (op == 0x63 || op == 0x8a || op == 0x8b);
        unsigned lb;
        if (op == 0x62 || op == 0x63)
            lb = c->vex.w ? 2 : 1;              // byte/word forms (VBMI2)
        else
            lb = c->vex.w ? 8 : 4;              // dword/qword forms
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned n = (vlen / 8) / lb;
        uint64_t k = c->vex.mask != 0 ? cpu->avx512_k[c->vex.mask] : ~UINT64_C(0);

        if (compress) {
            // Gather the selected elements into the low end of the result.
            amd64_vec_reg_read(cpu, modrm.reg, vlen, a);
            unsigned o = 0;
            for (unsigned i = 0; i < n; i++)
                if (k & (UINT64_C(1) << i)) {
                    memcpy(out + o * lb, a + i * lb, lb);
                    o++;
                }
            if (modrm.is_reg) {
                if (modrm.rm >= AMD64_AVX_MAX_REG)
                    return INT_UNDEFINED;
                // Register destination merges: elements past the compressed
                // count keep their old values unless zeroing was requested.
                amd64_vec_reg_read(cpu, modrm.rm, vlen, b);
                if (c->vex.zeroing)
                    memset(b + o * lb, 0, (n - o) * lb);
                memcpy(b, out, o * lb);
                amd64_vec_reg_write(cpu, modrm.rm, vlen, b);
            } else {
                // Memory destination writes ONLY the selected elements.
                qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
                if (o != 0 && !amd64_mem_write(cpu, tlb, addr, out, o * lb))
                    return INT_PF;
            }
            return INT_NONE;
        }

        // Expand: consume elements from the low end of the source and scatter
        // them into the positions the mask selects.
        unsigned needed = 0;
        for (unsigned i = 0; i < n; i++)
            if (k & (UINT64_C(1) << i))
                needed++;
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, vlen, a);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            memset(a, 0, sizeof(a));
            if (needed != 0 && !amd64_mem_read(cpu, tlb, addr, a, needed * lb))
                return INT_PF;
        }
        amd64_vec_reg_read(cpu, modrm.reg, vlen, out);
        unsigned src_i = 0;
        for (unsigned i = 0; i < n; i++) {
            if (k & (UINT64_C(1) << i)) {
                memcpy(out + i * lb, a + src_i * lb, lb);
                src_i++;
            } else if (c->vex.zeroing) {
                memset(out + i * lb, 0, lb);
            }
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    if ((op >= 0x75 && op <= 0x77) || (op >= 0x7d && op <= 0x7f)) {
        // VPERMI2* (75/76/77) and VPERMT2* (7d/7e/7f): index into the
        // CONCATENATION of two source registers, so an index's top bit picks
        // which source. The two differ only in which operand holds the
        // indices and which is overwritten.
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        bool t2 = op >= 0x7d;
        unsigned low = op & 0xf;
        unsigned lb = (low == 5 || low == 0xd) ? (c->vex.w ? 2 : 1)   // b/w
                    : (low == 6 || low == 0xe) ? (c->vex.w ? 8 : 4)   // d/q
                    : 4;                                              // ps/pd
        if (low == 7 || low == 0xf)
            lb = c->vex.w ? 8 : 4;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        uint8_t dstv[64];
        amd64_vec_reg_read(cpu, modrm.reg, vlen, dstv);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        // VPERMT2: dst holds the indices, and (vvvv, rm) are the two tables.
        // VPERMI2: vvvv holds the indices, and (dst, rm) are the tables.
        const uint8_t *idx = t2 ? dstv : a;
        const uint8_t *t0 = t2 ? a : dstv;
        unsigned n = (vlen / 8) / lb;
        for (unsigned i = 0; i < n; i++) {
            uint64_t sel = avx_lane_get(idx + i * lb, lb) & (2 * n - 1);
            const uint8_t *src = sel < n ? t0 : b;
            unsigned j = sel < n ? (unsigned) sel : (unsigned) (sel - n);
            memcpy(out + i * lb, src + j * lb, lb);
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, lb);
        return INT_NONE;
    }

    if (op == 0x8d) { // VPERMB -- byte permute across the WHOLE register
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);  // indices
        unsigned n = vlen / 8;
        for (unsigned i = 0; i < n; i++)
            out[i] = b[a[i] & (n - 1)];
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    // Two-operand (dst, src) forms with no vvvv operand.
    bool is_broadcast = op == 0x78 || op == 0x79 || op == 0x58 || op == 0x59 || op == 0x5a;
    bool is_widen = (op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35);
    bool is_abs = op >= 0x1c && op <= 0x1e;
    if (is_broadcast || is_widen || is_abs) {
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;

        // These read less than the destination's width, so the source read
        // size has to be computed per-op rather than using vlen.
        unsigned src_bits = vlen;
        unsigned src_lb = 0, dst_lb = 0;
        bool sign = false;
        if (is_broadcast) {
            src_lb = op == 0x78 ? 1 : op == 0x79 ? 2 : op == 0x58 ? 4 : op == 0x59 ? 8 : 16;
            src_bits = src_lb * 8;
        } else if (is_widen) {
            sign = op < 0x30;
            unsigned kind = (op & 0xf);
            // 0/1/2 = byte source (->w/d/q), 3/4 = word source (->d/q), 5 = dword source (->q)
            src_lb = kind <= 2 ? 1 : kind <= 4 ? 2 : 4;
            dst_lb = kind == 0 ? 2 : kind == 1 ? 4 : kind == 2 ? 8
                   : kind == 3 ? 4 : kind == 4 ? 8 : 8;
            src_bits = vlen / (dst_lb / src_lb);
        } else {
            src_lb = op == 0x1c ? 1 : op == 0x1d ? 2 : 4;
        }

        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, src_bits < 128 ? 128 : src_bits, a);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            unsigned nbytes = src_bits < 8 ? 1 : src_bits / 8;
            if (!amd64_mem_read(cpu, tlb, addr, a, nbytes))
                return INT_PF;
        }

        if (is_broadcast) {
            if (op == 0x5a) { // vbroadcasti128: replicate a 128-bit block
                if (vlen < 256)
                    return INT_UNDEFINED;
                for (unsigned i = 0; i < vlen / 8; i += 16)
                    memcpy(out + i, a, 16);
            } else {
                avx_broadcast(src_lb, vlen, a, out);
            }
        } else if (is_widen) {
            avx_widen(sign, src_lb, dst_lb, vlen, a, out);
        } else {
            avx_abs(src_lb, vlen, a, out);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    // Three-operand forms.
    {
        enum avx_op kind;
        unsigned lb = 1;
        bool matched = true;
        switch (op) {
        case 0x29: kind = AVX_CMPEQ; lb = 8; break;  // pcmpeqq
        case 0x37: kind = AVX_CMPGT; lb = 8; break;  // pcmpgtq
        case 0x38: kind = AVX_MINS; lb = 1; break;   // pminsb
        case 0x39: kind = AVX_MINS; lb = 4; break;   // pminsd
        case 0x3a: kind = AVX_MINU; lb = 2; break;   // pminuw
        case 0x3b: kind = AVX_MINU; lb = 4; break;   // pminud
        case 0x3c: kind = AVX_MAXS; lb = 1; break;   // pmaxsb
        case 0x3d: kind = AVX_MAXS; lb = 4; break;   // pmaxsd
        case 0x3e: kind = AVX_MAXU; lb = 2; break;   // pmaxuw
        case 0x3f: kind = AVX_MAXU; lb = 4; break;   // pmaxud
        case 0x40: kind = AVX_MULLO; lb = 4; break;  // pmulld
        default: matched = false; break;
        }
        if (matched) {
            if (!amd64_vex_decode_modrm(c, &modrm))
                return INT_GPF;
            if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            if (!amd64_vex_read_rm(c, &modrm, vlen, b))
                return INT_PF;
            amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
            avx_binop(kind, lb, vlen, a, b, out);
            amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
            return INT_NONE;
        }
    }

    // AES rounds and the VNNI dot products all take the classic 3-operand
    // shape; AES's "round key" is the rm operand.
    switch (op) {
    case 0xdc: case 0xdd: case 0xde: case 0xdf: { // vaesenc/enclast/dec/declast
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        bool decrypt = op >= 0xde;
        bool last = op == 0xdd || op == 0xdf;
        // VAES applies the round independently to each 128-bit lane.
        for (unsigned lane = 0; lane < vlen / 8; lane += 16)
            avx_aes_round(decrypt, last, a + lane, b + lane, out + lane);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x50: case 0x52: { // vpdpbusd / vpdpwssd -- accumulate into the dest
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        uint8_t acc[64];
        amd64_vec_reg_read(cpu, modrm.reg, vlen, acc);
        if (op == 0x50)
            avx_vpdpbusd(vlen, acc, a, b, out);
        else
            avx_vpdpwssd(vlen, acc, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x18: case 0x19: { // vbroadcastss / vbroadcastsd
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned lb = op == 0x18 ? 4 : 8;
        if (op == 0x19 && vlen < 256)
            return INT_UNDEFINED;
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, 128, a);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_read(cpu, tlb, addr, a, lb))
                return INT_PF;
        }
        avx_broadcast(lb, vlen, a, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    // FMA: 98/99 = 132, A8/A9 = 213, B8/B9 = 231; even opcode = packed
    // single, odd = packed double. 9C/AC/BC and friends are the negated-
    // multiply (FNMADD) and subtract (FMSUB) variants.
    case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: case 0x9b:
    case 0x9c: case 0x9d: case 0x9e: case 0x9f:
    case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xac: case 0xad: case 0xae: case 0xaf:
    case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf: {
        unsigned hi = op >> 4;
        unsigned lo = op & 0xf;
        unsigned form = hi == 0x9 ? 132 : hi == 0xa ? 213 : 231;
        // Within each group: 8/9 = FMADD, A/B = FMSUB, C/D = FNMADD,
        // E/F = FNMSUB; 6/7 = FMADDSUB/FMSUBADD, which alternate per lane and
        // are not implemented.
        if (lo < 0x8)
            return INT_UNDEFINED;
        bool is_double = (lo & 1) != 0;
        bool negate_mul = lo >= 0xc;
        bool subtract = (lo & 0x2) != 0;
        // Scalar FMA forms (odd nibble pairs with W-dependent scalar encodings)
        // share these opcodes in the 0F38 map only for the packed variants
        // handled here.
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        uint8_t dst_in[64];
        amd64_vec_reg_read(cpu, modrm.reg, vlen, dst_in);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_fma(form, is_double, negate_mul, subtract, vlen, dst_in, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x00: case 0x04: { // pshufb / pmaddubsw
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        if (op == 0x00)
            avx_pshufb(vlen, a, b, out);
        else
            avx_pmaddubsw(vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x36: { // vpermd: dword gather across the WHOLE register, not lane-local
        if (vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a); // indices
        unsigned n = (vlen / 8) / 4;
        for (unsigned i = 0; i < n; i++) {
            uint32_t idx = (uint32_t) avx_lane_get(a + i * 4, 4) & (n - 1);
            avx_lane_put(out + i * 4, 4, avx_lane_get(b + idx * 4, 4));
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x45: case 0x46: case 0x47: { // psrlvd/q, psravd, psllvd/q
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned lb = c->vex.w ? 8 : 4;
        enum avx_shift_kind kind = op == 0x45 ? AVX_SHR : op == 0x46 ? AVX_SAR : AVX_SHL;
        if (op == 0x46 && c->vex.w)
            return INT_UNDEFINED; // VPSRAVQ is AVX-512 only
        avx_shift_var(kind, lb, vlen, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    default:
        return INT_UNDEFINED;
    }
}

// Three-byte 0F3A map (everything here takes an imm8).
static int amd64_vex_map_0f3a(struct amd64_vex_ctx *c, byte_t op) {
    struct cpu_state *cpu = c->cpu;
    struct tlb *tlb = c->tlb;
    unsigned vlen = c->vlen;
    struct amd64_modrm modrm;
    uint8_t a[64], b[64], out[64];

    if (op == 0xf0)
        return amd64_vex_bmi(c, op); // rorx

    if (op >= 0x30 && op <= 0x33) { // KSHIFTR/KSHIFTL {B,W,D,Q}
        // 30/31 shift right, 32/33 shift left; W picks the wider of each pair.
        unsigned width = (op == 0x30 || op == 0x32) ? (c->vex.w ? 16 : 8)
                                                    : (c->vex.w ? 64 : 32);
        uint64_t wmask = width >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << width) - 1;
        bool left = op >= 0x32;
        if (!amd64_vex_decode_modrm(c, &modrm) || !modrm.is_reg)
            return INT_GPF;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        uint64_t v = cpu->avx512_k[modrm.rm & 7] & wmask;
        // A shift count at or beyond the mask width clears it outright rather
        // than invoking C's undefined over-shift.
        uint64_t r = imm >= width ? 0 : (left ? (v << imm) : (v >> imm));
        cpu->avx512_k[modrm.reg & 7] = r & wmask;
        return INT_NONE;
    }

    if (c->vex.pp != 1)
        return INT_UNDEFINED;

    // Compare into a mask register. imm8's low 3 bits pick the predicate; the
    // result is one bit per element, ANDed with the write-mask if present.
    if (op == 0x1e || op == 0x1f || op == 0x3e || op == 0x3f) {
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        bool is_unsigned = (op == 0x1e || op == 0x3e);
        unsigned lb = (op == 0x3e || op == 0x3f) ? (c->vex.w ? 2 : 1)
                                                 : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned n = (vlen / 8) / lb;
        uint64_t result = 0;
        for (unsigned i = 0; i < n; i++) {
            uint64_t x = avx_lane_get(a + i * lb, lb);
            uint64_t y = avx_lane_get(b + i * lb, lb);
            bool r;
            if (is_unsigned) {
                switch (imm & 7) {
                case 0: r = x == y; break;
                case 1: r = x < y; break;
                case 2: r = x <= y; break;
                case 3: r = false; break;
                case 4: r = x != y; break;
                case 5: r = x >= y; break;
                case 6: r = x > y; break;
                default: r = true; break;
                }
            } else {
                int64_t sx = avx_lane_sext(x, lb), sy = avx_lane_sext(y, lb);
                switch (imm & 7) {
                case 0: r = sx == sy; break;
                case 1: r = sx < sy; break;
                case 2: r = sx <= sy; break;
                case 3: r = false; break;
                case 4: r = sx != sy; break;
                case 5: r = sx >= sy; break;
                case 6: r = sx > sy; break;
                default: r = true; break;
                }
            }
            if (r)
                result |= UINT64_C(1) << i;
        }
        // A write-mask on a compare acts as a zeroing AND, never a merge.
        if (c->vex.mask != 0)
            result &= cpu->avx512_k[c->vex.mask];
        cpu->avx512_k[modrm.reg & 7] = result;
        return INT_NONE;
    }

    if (op >= 0x68 && op <= 0x6f) { // FMA4: VFMADD/VFMSUB {PS,PD,SS,SD}
        // AMD's 4-operand encoding. W selects whether the third source is the
        // r/m operand or the register named by the is4 byte's high nibble.
        bool is_scalar = (op & 1) != 0 ? (op == 0x6b || op == 0x6f) : (op == 0x6a || op == 0x6e);
        bool is_double = (op & 1) != 0;
        bool subtract = op >= 0x6c;
        unsigned width = is_scalar ? 128 : vlen;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t is4;
        if (!amd64_fetch_u8(cpu, tlb, &is4))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, width, b))
            return INT_PF;
        unsigned is4_reg = (is4 >> 4) & 0xf;
        if (is4_reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        uint8_t third[64];
        amd64_vec_reg_read(cpu, is4_reg, width, third);
        amd64_vec_reg_read(cpu, c->vex.vvvv, width, a);
        const uint8_t *src2 = c->vex.w ? b : third;
        const uint8_t *src3 = c->vex.w ? third : b;
        memcpy(out, a, sizeof(out));
        unsigned span = is_scalar ? (is_double ? 64 : 32) : width;
        // form 213 with (dst_in = src2) gives src1*src2 (+/-) src3.
        avx_fma(213, is_double, false, subtract, span, src2, a, src3, out);
        amd64_vec_reg_write(cpu, modrm.reg, width, out);
        return INT_NONE;
    }

    if (op == 0x17) { // VEXTRACTPS -- one dword out to a GPR or memory
        if (vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        amd64_vec_reg_read(cpu, modrm.reg, 128, a);
        if (!amd64_write_rm(cpu, tlb, &modrm, c->fs_prefix, 32,
                            avx_lane_get(a + (imm & 3) * 4, 4)))
            return INT_PF;
        return INT_NONE;
    }

    if (op == 0x70 || op == 0x71) { // VPSHLD{W,D,Q} -- funnel shift left by imm8
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        unsigned lb = op == 0x70 ? 2 : (c->vex.w ? 8 : 4);
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned bits = lb * 8;
        unsigned n = imm & (bits - 1);
        for (unsigned i = 0; i < vlen / 8; i += lb) {
            // Concatenate src1:src2 and take the top `bits` after shifting left.
            uint64_t hi = avx_lane_get(a + i, lb), lo = avx_lane_get(b + i, lb);
            uint64_t r = n == 0 ? hi : ((hi << n) | (lo >> (bits - n)));
            avx_lane_put(out + i, lb, r & avx_lane_mask(lb));
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, lb);
        return INT_NONE;
    }

    if (op == 0x19) { // VEXTRACTF128 / VEXTRACTF32X4 -- same shape as 0x39
        if (vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        amd64_vec_reg_read(cpu, modrm.reg, 256, a);
        memcpy(out, a + ((imm & 1) ? 16 : 0), 16);
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_write(cpu, modrm.rm, 128, out);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_write(cpu, tlb, addr, out, 16))
                return INT_PF;
        }
        return INT_NONE;
    }

    if (op == 0x08 || op == 0x09) { // VROUNDPS / VROUNDPD
        bool is_double = op == 0x09;
        unsigned lb = is_double ? 8 : 4;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        for (unsigned i = 0; i < vlen / 8; i += lb) {
            double v;
            if (is_double) memcpy(&v, a + i, 8);
            else { float fv; memcpy(&fv, a + i, 4); v = fv; }
            // imm[2] set means "use MXCSR's mode", which this emulator runs as
            // round-to-nearest; otherwise imm[1:0] picks the mode directly.
            double r;
            switch ((imm & 4) ? 0 : (imm & 3)) {
            case 1: r = floor(v); break;
            case 2: r = ceil(v); break;
            case 3: r = v < 0 ? ceil(v) : floor(v); break;
            default: r = nearbyint(v); break;
            }
            if (is_double) memcpy(out + i, &r, 8);
            else { float fr = (float) r; memcpy(out + i, &fr, 4); }
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    if (op == 0x4a || op == 0x4b) { // VBLENDVPS / VBLENDVPD -- sign-bit select
        unsigned lb = op == 0x4a ? 4 : 8;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t is4;
        if (!amd64_fetch_u8(cpu, tlb, &is4))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        unsigned mask_reg = (is4 >> 4) & 0xf;
        if (mask_reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        uint8_t mask[64];
        amd64_vec_reg_read(cpu, mask_reg, vlen, mask);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        for (unsigned i = 0; i < vlen / 8; i += lb) {
            const uint8_t *src = (mask[i + lb - 1] & 0x80) ? b : a;
            memcpy(out + i, src + i, lb);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }

    if (op == 0x43) { // VSHUFI32X4 / VSHUFI64X2 -- select 128-bit chunks
        if (!c->vex.is_evex || vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned chunks = (vlen / 8) / 16;
        unsigned bits = chunks == 2 ? 1 : 2;   // 256-bit picks 1 bit, 512 two
        for (unsigned i = 0; i < chunks; i++) {
            unsigned sel = (imm >> (i * bits)) & ((1u << bits) - 1);
            // The low half of the destination comes from src1, the high from src2.
            const uint8_t *src = i < chunks / 2 ? a : b;
            memcpy(out + i * 16, src + sel * 16, 16);
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, c->vex.w ? 8 : 4);
        return INT_NONE;
    }

    if (op == 0x03) { // VALIGND / VALIGNQ -- element-granular concat-and-shift
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        unsigned lb = c->vex.w ? 8 : 4;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        // Unlike VPALIGNR this is NOT lane-local: the two sources concatenate
        // across the whole register and the window slides by whole elements.
        unsigned n = (vlen / 8) / lb;
        for (unsigned i = 0; i < n; i++) {
            unsigned idx = (imm & (2 * n - 1)) + i;
            const uint8_t *src = idx < n ? b : a;
            unsigned j = idx < n ? idx : idx - n;
            if (idx >= 2 * n)
                avx_lane_put(out + i * lb, lb, 0);
            else
                memcpy(out + i * lb, src + j * lb, lb);
        }
        amd64_vec_write_masked(c, modrm.reg, vlen, out, lb);
        return INT_NONE;
    }

    if (op == 0x3a) { // VINSERTI32X8 / VINSERTI64X4 -- 256-bit half into a zmm
        if (!c->vex.is_evex || vlen != 512)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, 256, b);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_read(cpu, tlb, addr, b, 32))
                return INT_PF;
        }
        amd64_vec_reg_read(cpu, c->vex.vvvv, 512, out);
        memcpy(out + ((imm & 1) ? 32 : 0), b, 32);
        amd64_vec_reg_write(cpu, modrm.reg, 512, out);
        return INT_NONE;
    }

    if (op == 0x3b) { // VEXTRACTI32X8 / VEXTRACTI64X4 -- 256-bit half of a zmm
        if (!c->vex.is_evex || vlen != 512)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.reg, 512, a);
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        memcpy(out, a + ((imm & 1) ? 32 : 0), 32);
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_write(cpu, modrm.rm, 256, out);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_write(cpu, tlb, addr, out, 32))
                return INT_PF;
        }
        return INT_NONE;
    }

    switch (op) {
    case 0x44: { // vpclmulqdq
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_pclmulqdq(vlen, imm, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0xce: { // vgf2p8affineqb
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_gf2p8affine(vlen, imm, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x25: { // vpternlogd/q -- EVEX only
        if (!c->vex.is_evex)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        uint8_t dst_in[64];
        amd64_vec_reg_read(cpu, modrm.reg, vlen, dst_in);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_pternlog(vlen, imm, dst_in, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x0f: { // vpalignr
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        avx_palignr(vlen, imm, a, b, out);
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x00: { // vpermq (W=1): qword gather across the whole register
        if (!c->vex.w || vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, a))
            return INT_PF;
        for (unsigned i = 0; i < 4; i++)
            avx_lane_put(out + i * 8, 8, avx_lane_get(a + ((imm >> (2 * i)) & 3) * 8, 8));
        amd64_vec_reg_write(cpu, modrm.reg, 256, out);
        return INT_NONE;
    }
    case 0x38: { // vinserti128
        if (vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // imm8 first: a RIP-relative displacement counts it as part of the
        // instruction's length.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_read(cpu, modrm.rm, 128, b);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_read(cpu, tlb, addr, b, 16))
                return INT_PF;
        }
        amd64_vec_reg_read(cpu, c->vex.vvvv, 256, out);
        memcpy(out + ((imm & 1) ? 16 : 0), b, 16);
        amd64_vec_reg_write(cpu, modrm.reg, 256, out);
        return INT_NONE;
    }
    case 0x39: { // vextracti128 -- destination is the rm operand
        if (vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        amd64_vec_reg_read(cpu, modrm.reg, 256, a);
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        memcpy(out, a + ((imm & 1) ? 16 : 0), 16);
        if (modrm.is_reg) {
            if (modrm.rm >= AMD64_AVX_MAX_REG)
                return INT_UNDEFINED;
            amd64_vec_reg_write(cpu, modrm.rm, 128, out);
        } else {
            qword_t addr = amd64_effective_addr(cpu, &modrm, c->fs_prefix);
            if (!amd64_mem_write(cpu, tlb, addr, out, 16))
                return INT_PF;
        }
        return INT_NONE;
    }
    case 0x46: { // vperm2i128
        if (vlen < 256)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vec_read_rm(cpu, tlb, &modrm, c->fs_prefix, 256, b))
            return INT_PF;
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, 256, a);
        for (unsigned half = 0; half < 2; half++) {
            unsigned sel = (imm >> (half * 4)) & 0xf;
            if (sel & 0x8) { // bit 3 forces a zero result for that half
                memset(out + half * 16, 0, 16);
            } else {
                const uint8_t *src = (sel & 0x2) ? b : a;
                memcpy(out + half * 16, src + ((sel & 1) ? 16 : 0), 16);
            }
        }
        amd64_vec_reg_write(cpu, modrm.reg, 256, out);
        return INT_NONE;
    }
    case 0x02: case 0x0e: { // vpblendd / vpblendw
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        // The imm8 must be consumed before the effective address is
        // computed: a RIP-relative displacement is relative to the END
        // of the instruction, imm8 included.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        unsigned lb = op == 0x02 ? 4 : 2;
        unsigned n = (vlen / 8) / lb;
        for (unsigned i = 0; i < n; i++) {
            // vpblendw's imm8 repeats every 8 words (per 128-bit lane);
            // vpblendd's covers all 8 dwords of a 256-bit register directly.
            unsigned bit = op == 0x02 ? i : (i & 7);
            const uint8_t *src = (imm >> bit) & 1 ? b : a;
            memcpy(out + i * lb, src + i * lb, lb);
        }
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x4c: { // vpblendvb -- mask register comes from the is4 immediate byte
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        if (!amd64_vex_read_rm(c, &modrm, vlen, b))
            return INT_PF;
        byte_t is4;
        if (!amd64_fetch_u8(cpu, tlb, &is4))
            return INT_GPF;
        unsigned mask_reg = (is4 >> 4) & 0xf;
        if (mask_reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        uint8_t mask[64];
        amd64_vec_reg_read(cpu, mask_reg, vlen, mask);
        amd64_vec_reg_read(cpu, c->vex.vvvv, vlen, a);
        for (unsigned i = 0; i < vlen / 8; i++)
            out[i] = (mask[i] & 0x80) ? b[i] : a[i];
        amd64_vec_reg_write(cpu, modrm.reg, vlen, out);
        return INT_NONE;
    }
    case 0x14: case 0x15: case 0x16: { // vpextrb / vpextrw / vpextrd,q
        if (vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        byte_t imm;
        amd64_vec_reg_read(cpu, modrm.reg, 128, a);
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        unsigned lb = op == 0x14 ? 1 : op == 0x15 ? 2 : (c->vex.w ? 8 : 4);
        unsigned idx = imm & (16 / lb - 1);
        uint64_t value = avx_lane_get(a + idx * lb, lb);
        if (!amd64_write_rm(cpu, tlb, &modrm, c->fs_prefix, lb == 1 ? 32 : lb * 8, value))
            return INT_PF;
        return INT_NONE;
    }
    case 0x20: case 0x22: { // vpinsrb / vpinsrd,q
        if (vlen != 128)
            return INT_UNDEFINED;
        if (!amd64_vex_decode_modrm(c, &modrm))
            return INT_GPF;
        if (modrm.reg >= AMD64_AVX_MAX_REG || c->vex.vvvv >= AMD64_AVX_MAX_REG)
            return INT_UNDEFINED;
        unsigned lb = op == 0x20 ? 1 : (c->vex.w ? 8 : 4);
        // imm8 first: a RIP-relative displacement counts it as part of the
        // instruction's length.
        byte_t imm;
        if (!amd64_fetch_u8(cpu, tlb, &imm))
            return INT_GPF;
        qword_t value;
        if (!amd64_read_rm(cpu, tlb, &modrm, c->fs_prefix, lb == 1 ? 8 : lb * 8, &value))
            return INT_PF;
        amd64_vec_reg_read(cpu, c->vex.vvvv, 128, out);
        avx_lane_put(out + (imm & (16 / lb - 1)) * lb, lb, value);
        amd64_vec_reg_write(cpu, modrm.reg, 128, out);
        return INT_NONE;
    }
    default:
        return INT_UNDEFINED;
    }
}

static int amd64_vex_step(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, struct amd64_vex_prefix vex, bool fs_prefix) {
    byte_t op;
    if (!amd64_fetch_u8(cpu, tlb, &op)) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = saved_rip;
        return INT_GPF;
    }

    struct amd64_vex_ctx ctx = {
        .cpu = cpu,
        .tlb = tlb,
        .saved_rip = saved_rip,
        .vex = vex,
        .rex = { .present = true, .w = vex.w, .r = vex.r, .x = vex.x, .b = vex.b },
        .fs_prefix = fs_prefix,
        .vlen = vex.vlen,
    };

    // Refuse a predicate on any instruction whose handler cannot apply one:
    // silently ignoring it would write elements the guest asked to preserve.
    if (vex.is_evex && vex.mask != 0) {
        // Compares write a mask; compress/expand/testm consume the mask as a
        // selector rather than as a per-lane predicate. All handle it inline.
        bool mask_is_operand =
            (vex.map == 2 && (op == 0x62 || op == 0x63 || op == 0x88 || op == 0x89 ||
                              op == 0x8a || op == 0x8b || op == 0x26 || op == 0x27 ||
                              op == 0x64 || op == 0x65 || op == 0x66)) ||
            (vex.map == 2 && vex.pp == 2 && op >= 0x30 && op <= 0x35);
        bool compare_into_mask = mask_is_operand ||
            (vex.map == 3 && (op == 0x1e || op == 0x1f || op == 0x3e || op == 0x3f)) ||
            (vex.map == 1 && vex.pp == 1 &&
             (op == 0x74 || op == 0x75 || op == 0x76 ||
              op == 0x64 || op == 0x65 || op == 0x66));
        if (!compare_into_mask && amd64_vex_mask_elem(&ctx, op) == 0)
            return INT_UNDEFINED;
    }

    int result;
    switch (vex.map) {
    case 1: result = amd64_vex_map_0f(&ctx, op); break;
    case 2: result = amd64_vex_map_0f38(&ctx, op); break;
    case 3: result = amd64_vex_map_0f3a(&ctx, op); break;
    default: result = INT_UNDEFINED; break;
    }

    switch (result) {
    case INT_NONE:
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    case INT_GPF:
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = saved_rip;
        return INT_GPF;
    case INT_PF:
        cpu->amd64_rip = saved_rip;
        return INT_PF;
    default:
        // Not implemented -- report a guest SIGILL rather than silently
        // computing a wrong answer. Remaining gaps, roughly by how often they
        // appear in real binaries: the crypto family (VAES*, VPCLMULQDQ,
        // VGF2P8AFFINEQB), the FP/FMA family (VFMADD*, VCVT*, VADDPS...),
        // VNNI (VPDPBUSD/VPDPWSSD), VPTEST's flag semantics, and every
        // masked/zeroing EVEX form.
        return INT_UNDEFINED;
    }
}

static void amd64_fill_fxsave_area(struct cpu_state *cpu, struct fxsave_area *area) {
    fxsave_fill(cpu, area, AMD64_FXSAVE_XMM_COUNT);
}

static void amd64_restore_fxsave_area(struct cpu_state *cpu, const struct fxsave_area *area) {
    fxsave_restore(cpu, area, AMD64_FXSAVE_XMM_COUNT);
}

static inline int amd64_fxsave_op(struct cpu_state *cpu, struct tlb *tlb,
        const struct amd64_modrm *modrm, bool fs_prefix, qword_t saved_rip) {
    struct fxsave_area area;
    qword_t addr;

    if (modrm->is_reg) {
        // Register-form 0F AE: /5 LFENCE, /6 MFENCE, /7 SFENCE. The interpreter
        // runs each guest thread's instructions in order, but other guest
        // threads are host pthreads sharing this address space, so emit a host
        // full barrier to make the ordering visible to them. (Go's cputicks
        // uses 'mfence; lfence; rdtsc' when rdtscp isn't advertised.)
        if (modrm->reg >= 5 && modrm->reg <= 7) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return INT_NONE;
        }
        return INT_UNDEFINED;
    }
    addr = amd64_effective_addr(cpu, modrm, fs_prefix);

    // /2 LDMXCSR, /3 STMXCSR: 32-bit MXCSR, no alignment requirement. The
    // emulator runs SSE round-to-nearest with all exceptions masked and does not
    // honor MXCSR's rounding/exception bits, but stores/loads the value so a
    // control-word read-modify-write round-trips.
    if (modrm->reg == 2 || modrm->reg == 3) {
        dword_t mxcsr;
        if (modrm->reg == 2) {
            if (!amd64_mem_read(cpu, tlb, addr, &mxcsr, sizeof(mxcsr))) {
                cpu->amd64_rip = saved_rip;
                return INT_PF;
            }
            cpu->mxcsr = mxcsr & 0xffff;
        } else {
            mxcsr = cpu->mxcsr;
            if (!amd64_mem_write(cpu, tlb, addr, &mxcsr, sizeof(mxcsr))) {
                cpu->amd64_rip = saved_rip;
                return INT_PF;
            }
        }
        return INT_NONE;
    }

    // /7 with a memory operand is CLFLUSH (and, with a 66 prefix, CLFLUSHOPT).
    // There is one coherent view of guest memory here, so there is no cache
    // line to write back and the correct emulation is to do nothing. Missing
    // it entirely meant SIGILL, which is a real crash for anything that emits
    // the instruction -- the i386 engine reaches the same conclusion in
    // emu/decode.h's 0f ae group.
    if (modrm->reg == 7)
        return INT_NONE;

    if (modrm->reg != 0 && modrm->reg != 1)
        return INT_UNDEFINED;

    if ((addr & 0xf) != 0) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = addr;
        return INT_GPF;
    }

    if (modrm->reg == 0) {
        amd64_fill_fxsave_area(cpu, &area);
        if (!amd64_mem_write(cpu, tlb, addr, &area, sizeof(area))) {
            cpu->amd64_rip = saved_rip;
            return INT_PF;
        }
    } else {
        if (!amd64_mem_read(cpu, tlb, addr, &area, sizeof(area))) {
            cpu->amd64_rip = saved_rip;
            return INT_PF;
        }
        amd64_restore_fxsave_area(cpu, &area);
    }

    return INT_NONE;
}

static inline int amd64_handle_x87(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, struct amd64_rex_prefix rex, bool fs_prefix, byte_t opcode) {
    struct amd64_modrm modrm;
    unsigned rm;
    unsigned subop;
    unsigned fullop;
    qword_t addr = 0;

    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_fpu_gpf_restore;

    rm = modrm.rm & 7;
    subop = ((unsigned) opcode << 4) | (modrm.reg & 7);
    fullop = ((unsigned) opcode << 8) | ((modrm.reg & 7) << 4) | rm;
    if (!modrm.is_reg)
        addr = amd64_effective_addr(cpu, &modrm, fs_prefix);

    if (!modrm.is_reg) {
        switch (subop) {
        case 0xd80: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_addm32(cpu, &value);
            break;
        }
        case 0xd81: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_mulm32(cpu, &value);
            break;
        }
        case 0xd82: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm32(cpu, &value);
            break;
        }
        case 0xd83: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm32(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xd84: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subm32(cpu, &value);
            break;
        }
        case 0xd85: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subrm32(cpu, &value);
            break;
        }
        case 0xd86: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divm32(cpu, &value);
            break;
        }
        case 0xd87: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divrm32(cpu, &value);
            break;
        }
        case 0xd90: {
            float value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldm32(cpu, &value);
            break;
        }
        case 0xd92: {
            float value;
            fpu_stm32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xd93: {
            float value;
            fpu_stm32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xd94: {
            struct fpu_env32 env;
            if (!amd64_mem_read(cpu, tlb, addr, &env, sizeof(env)))
                goto amd64_fpu_gpf_restore;
            fpu_ldenv32(cpu, &env);
            break;
        }
        case 0xd95: {
            uint16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldcw16(cpu, &value);
            break;
        }
        case 0xd96: {
            struct fpu_env32 env;
            fpu_stenv32(cpu, &env);
            if (!amd64_mem_write(cpu, tlb, addr, &env, sizeof(env)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xd97: {
            uint16_t value;
            fpu_stcw16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xda0: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_iadd32(cpu, &value);
            break;
        }
        case 0xda1: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_imul32(cpu, &value);
            break;
        }
        case 0xda2: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom32(cpu, &value);
            break;
        }
        case 0xda3: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom32(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xda4: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isub32(cpu, &value);
            break;
        }
        case 0xda5: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isubr32(cpu, &value);
            break;
        }
        case 0xda6: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idiv32(cpu, &value);
            break;
        }
        case 0xda7: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idivr32(cpu, &value);
            break;
        }
        case 0xdb0: {
            int32_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild32(cpu, &value);
            break;
        }
        case 0xdb1: {
            int32_t value;
            fpu_istt32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdb2: {
            int32_t value;
            fpu_ist32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdb3: {
            int32_t value;
            fpu_ist32(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdb5: {
            float80 value = {};
            if (!amd64_mem_read(cpu, tlb, addr, &value, 10))
                goto amd64_fpu_gpf_restore;
            fpu_ldm80(cpu, &value);
            break;
        }
        case 0xdb7: {
            float80 value;
            fpu_stm80(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, 10))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdc0: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_addm64(cpu, &value);
            break;
        }
        case 0xdc1: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_mulm64(cpu, &value);
            break;
        }
        case 0xdc2: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm64(cpu, &value);
            break;
        }
        case 0xdc3: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_comm64(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xdc4: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subm64(cpu, &value);
            break;
        }
        case 0xdc5: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_subrm64(cpu, &value);
            break;
        }
        case 0xdc6: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divm64(cpu, &value);
            break;
        }
        case 0xdc7: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_divrm64(cpu, &value);
            break;
        }
        case 0xdd0: {
            double value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ldm64(cpu, &value);
            break;
        }
        case 0xdd1: {
            int64_t value;
            fpu_istt64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdd2: {
            double value;
            fpu_stm64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdd3: {
            double value;
            fpu_stm64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdd4: {
            struct fpu_state32 state;
            if (!amd64_mem_read(cpu, tlb, addr, &state, sizeof(state)))
                goto amd64_fpu_gpf_restore;
            fpu_restore32(cpu, &state);
            break;
        }
        case 0xdd6: {
            struct fpu_state32 state;
            fpu_save32(cpu, &state);
            if (!amd64_mem_write(cpu, tlb, addr, &state, sizeof(state)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdd7: {
            uint16_t value;
            fpu_stsw16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xde0: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_iadd16(cpu, &value);
            break;
        }
        case 0xde1: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_imul16(cpu, &value);
            break;
        }
        case 0xde2: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom16(cpu, &value);
            break;
        }
        case 0xde3: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_icom16(cpu, &value);
            fpu_pop(cpu);
            break;
        }
        case 0xde4: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isub16(cpu, &value);
            break;
        }
        case 0xde5: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_isubr16(cpu, &value);
            break;
        }
        case 0xde6: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idiv16(cpu, &value);
            break;
        }
        case 0xde7: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_idivr16(cpu, &value);
            break;
        }
        case 0xdf0: {
            int16_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild16(cpu, &value);
            break;
        }
        case 0xdf1: {
            int16_t value;
            fpu_istt16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdf2: {
            int16_t value;
            fpu_ist16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            break;
        }
        case 0xdf3: {
            int16_t value;
            fpu_ist16(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        case 0xdf5: {
            int64_t value;
            if (!amd64_mem_read(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_ild64(cpu, &value);
            break;
        }
        case 0xdf7: {
            int64_t value;
            fpu_ist64(cpu, &value);
            if (!amd64_mem_write(cpu, tlb, addr, &value, sizeof(value)))
                goto amd64_fpu_gpf_restore;
            fpu_pop(cpu);
            break;
        }
        default:
            return INT_UNDEFINED;
        }
        return INT_NONE;
    }

    switch (subop) {
    case 0xd80:
        fpu_add(cpu, rm, 0);
        return INT_NONE;
    case 0xd81:
        fpu_mul(cpu, rm, 0);
        return INT_NONE;
    case 0xd82:
        fpu_com(cpu, rm);
        return INT_NONE;
    case 0xd83:
        fpu_com(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xd84:
        fpu_sub(cpu, rm, 0);
        return INT_NONE;
    case 0xd85:
        fpu_subr(cpu, rm, 0);
        return INT_NONE;
    case 0xd86:
        fpu_div(cpu, rm, 0);
        return INT_NONE;
    case 0xd87:
        fpu_divr(cpu, rm, 0);
        return INT_NONE;
    case 0xd90:
        fpu_ld(cpu, rm);
        return INT_NONE;
    case 0xd91:
        fpu_xch(cpu, rm);
        return INT_NONE;
    case 0xda0:
        fpu_cmovb(cpu, rm);
        return INT_NONE;
    case 0xda1:
        fpu_cmove(cpu, rm);
        return INT_NONE;
    case 0xda2:
        fpu_cmovbe(cpu, rm);
        return INT_NONE;
    case 0xda3:
        fpu_cmovu(cpu, rm);
        return INT_NONE;
    case 0xdb0:
        fpu_cmovnb(cpu, rm);
        return INT_NONE;
    case 0xdb1:
        fpu_cmovne(cpu, rm);
        return INT_NONE;
    case 0xdb2:
        fpu_cmovnbe(cpu, rm);
        return INT_NONE;
    case 0xdb3:
        fpu_cmovnu(cpu, rm);
        return INT_NONE;
    case 0xdb5:
        fpu_ucomi(cpu, rm);
        return INT_NONE;
    case 0xdb6:
        fpu_comi(cpu, rm);
        return INT_NONE;
    case 0xdc0:
        fpu_add(cpu, 0, rm);
        return INT_NONE;
    case 0xdc1:
        fpu_mul(cpu, 0, rm);
        return INT_NONE;
    case 0xdc4:
        fpu_subr(cpu, 0, rm);
        return INT_NONE;
    case 0xdc5:
        fpu_sub(cpu, 0, rm);
        return INT_NONE;
    case 0xdc6:
        fpu_divr(cpu, 0, rm);
        return INT_NONE;
    case 0xdc7:
        fpu_div(cpu, 0, rm);
        return INT_NONE;
    case 0xdd0:
        return INT_NONE;
    case 0xdd2:
        fpu_st(cpu, rm);
        return INT_NONE;
    case 0xdd3:
        fpu_st(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdd4:
        fpu_ucom(cpu, rm);
        return INT_NONE;
    case 0xdd5:
        fpu_ucom(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde0:
        fpu_add(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde1:
        fpu_mul(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde4:
        fpu_subr(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde5:
        fpu_sub(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde6:
        fpu_divr(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xde7:
        fpu_div(cpu, 0, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf0:
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf5:
        fpu_ucomi(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf6:
        fpu_comi(cpu, rm);
        fpu_pop(cpu);
        return INT_NONE;
    default:
        break;
    }

    switch (fullop) {
    case 0xd940:
        fpu_chs(cpu);
        return INT_NONE;
    case 0xd941:
        fpu_abs(cpu);
        return INT_NONE;
    case 0xd944:
        fpu_tst(cpu);
        return INT_NONE;
    case 0xd945:
        fpu_xam(cpu);
        return INT_NONE;
    case 0xd950:
        fpu_ldc(cpu, fconst_one);
        return INT_NONE;
    case 0xd951:
        fpu_ldc(cpu, fconst_log2t);
        return INT_NONE;
    case 0xd952:
        fpu_ldc(cpu, fconst_log2e);
        return INT_NONE;
    case 0xd953:
        fpu_ldc(cpu, fconst_pi);
        return INT_NONE;
    case 0xd954:
        fpu_ldc(cpu, fconst_log2);
        return INT_NONE;
    case 0xd955:
        fpu_ldc(cpu, fconst_ln2);
        return INT_NONE;
    case 0xd956:
        fpu_ldc(cpu, fconst_zero);
        return INT_NONE;
    case 0xd960:
        fpu_2xm1(cpu);
        return INT_NONE;
    case 0xd961:
        fpu_yl2x(cpu);
        return INT_NONE;
    case 0xd963:
        fpu_patan(cpu);
        return INT_NONE;
    case 0xd964:
        fpu_xtract(cpu);
        return INT_NONE;
    case 0xd967:
        fpu_incstp(cpu);
        return INT_NONE;
    case 0xd970:
        fpu_prem(cpu);
        return INT_NONE;
    case 0xd973:
        fpu_sincos(cpu);
        return INT_NONE;
    case 0xd972:
        fpu_sqrt(cpu);
        return INT_NONE;
    case 0xd974:
        fpu_rndint(cpu);
        return INT_NONE;
    case 0xd975:
        fpu_scale(cpu);
        return INT_NONE;
    case 0xd976:
        fpu_sin(cpu);
        return INT_NONE;
    case 0xd977:
        fpu_cos(cpu);
        return INT_NONE;
    case 0xdb43:
        fpu_init(cpu);
        return INT_NONE;
    case 0xdb42:
        fpu_clex(cpu);
        return INT_NONE;
    case 0xde31:
        fpu_com(cpu, 1);
        fpu_pop(cpu);
        fpu_pop(cpu);
        return INT_NONE;
    case 0xdf40:
        amd64_reg_set(cpu, amd64_rax, 16, cpu->fsw);
        return INT_NONE;
    default:
        return INT_UNDEFINED;
    }

amd64_fpu_gpf_restore:
    cpu->amd64_rip = saved_rip;
    cpu->segfault_addr = saved_rip;
    return INT_GPF;
}

static inline void amd64_trace_qword_store(struct cpu_state *cpu, qword_t rip,
        byte_t opcode, qword_t addr, qword_t value) {
    unsigned slot = cpu->amd64_store_trace_next++ % AMD64_STORE_TRACE_COUNT;
    cpu->amd64_store_trace[slot] = (struct amd64_store_trace) {
        .rip = rip,
        .addr = addr,
        .value = value,
        .opcode = opcode,
    };
}

static inline bool amd64_cond_eval(struct cpu_state *cpu, unsigned cc) {
    switch (cc & 0xf) {
    case 0x0: return OF;
    case 0x1: return !OF;
    case 0x2: return CF;
    case 0x3: return !CF;
    case 0x4: return ZF;
    case 0x5: return !ZF;
    case 0x6: return CF || ZF;
    case 0x7: return !CF && !ZF;
    case 0x8: return SF;
    case 0x9: return !SF;
    case 0xa: return PF;
    case 0xb: return !PF;
    case 0xc: return SF != OF;
    case 0xd: return SF == OF;
    case 0xe: return ZF || (SF != OF);
    case 0xf: return !ZF && (SF == OF);
    default: return false;
    }
}

enum amd64_rep_mode {
    AMD64_REP_NONE,
    AMD64_REPZ,
    AMD64_REPNZ,
};

static inline void amd64_bump_string_reg(struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t delta = size / 8;
    if (cpu->amd64_address_size_prefix) {
        uint32_t value = (uint32_t) cpu->amd64_regs[reg];
        value = !cpu->df ? value + (uint32_t) delta : value - (uint32_t) delta;
        amd64_reg_set(cpu, reg, 32, value);
        return;
    }
    if (!cpu->df)
        cpu->amd64_regs[reg] += delta;
    else
        cpu->amd64_regs[reg] -= delta;
}

static inline qword_t amd64_string_addr(const struct cpu_state *cpu, unsigned reg) {
    if (cpu->amd64_address_size_prefix)
        return (uint32_t) cpu->amd64_regs[reg];
    return cpu->amd64_regs[reg];
}

// Forward, page-batched REP movs/stos -- the hot musl memcpy/memset path. Moves
// whole runs within a page via memmove/memset on the host backing instead of one
// guest element per TLB lookup (~15x on amd64 memset/memcpy). 64-bit addressing
// only (what musl's str ops use); stops -- leaving rcx/rdi/rsi at the boundary
// for the per-element loop to finish or fault -- on an inaccessible/COW page or a
// forward-overlapping movs run. cpu->df == 0 (forward) is checked by the caller.
static inline void amd64_rep_string_fast(struct cpu_state *cpu, struct tlb *tlb,
        unsigned elem_size, byte_t opcode) {
    bool is_movs = (opcode == 0xa4 || opcode == 0xa5);
    qword_t rcx = cpu->amd64_regs[amd64_rcx];
    qword_t rdi = cpu->amd64_regs[amd64_rdi];
    qword_t rsi = cpu->amd64_regs[amd64_rsi];
    qword_t rax = cpu->amd64_regs[amd64_rax];

    while (rcx != 0) {
        qword_t run = (PAGE_SIZE - PGOFFSET(rdi)) / elem_size; // whole elems in dst page
        if (run > rcx)
            run = rcx;
        if (is_movs) {
            qword_t src_room = (PAGE_SIZE - PGOFFSET(rsi)) / elem_size;
            if (run > src_room)
                run = src_room;
        }
        // memmove matches x86 ascending semantics only when the run does not
        // forward-overlap; defer those (and an element straddling a page, run==0)
        // to the precise per-element loop.
        bool overlap_smear = is_movs && rdi > rsi && (rdi - rsi) < run * elem_size;
        if (run < 1 || overlap_smear)
            break;
        char *dst = (char *) __tlb_write_ptr(tlb, rdi);
        if (dst == NULL)
            break; // unmapped / read-only (COW): let amd64_mem_write fault it
        if (is_movs) {
            char *src = (char *) __tlb_read_ptr(tlb, rsi);
            if (src == NULL)
                break;
            memmove(dst, src, (size_t) run * elem_size);
            rsi += run * elem_size;
        } else {
            switch (elem_size) {
            case 1: memset(dst, (int) (rax & 0xff), (size_t) run); break;
            case 2: { uint16_t v = (uint16_t) rax; for (qword_t i = 0; i < run; i++) ((uint16_t *) dst)[i] = v; break; }
            case 4: { uint32_t v = (uint32_t) rax; for (qword_t i = 0; i < run; i++) ((uint32_t *) dst)[i] = v; break; }
            default: { uint64_t v = rax;            for (qword_t i = 0; i < run; i++) ((uint64_t *) dst)[i] = v; break; }
            }
        }
        rdi += run * elem_size;
        rcx -= run;
    }
    cpu->amd64_regs[amd64_rcx] = rcx;
    cpu->amd64_regs[amd64_rdi] = rdi;
    cpu->amd64_regs[amd64_rsi] = rsi;
}

static inline int amd64_string_op(struct cpu_state *cpu, struct tlb *tlb,
        qword_t saved_rip, byte_t opcode, unsigned size, enum amd64_rep_mode rep_mode) {
    unsigned count_size = cpu->amd64_address_size_prefix ? 32 : 64;
    qword_t count = rep_mode == AMD64_REP_NONE ? 1 : amd64_reg_get(cpu, amd64_rcx, count_size);

    // Bulk fast path for forward 64-bit REP movs/stos; falls through to the
    // per-element loop for any tail (overlap / page-straddle / fault element).
    if (rep_mode != AMD64_REP_NONE && count > 1 && !cpu->df &&
            !cpu->amd64_address_size_prefix &&
            (opcode == 0xa4 || opcode == 0xa5 || opcode == 0xaa || opcode == 0xab)) {
        amd64_rep_string_fast(cpu, tlb, size / 8, opcode);
        count = amd64_reg_get(cpu, amd64_rcx, count_size);
        if (count == 0) {
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
    }

    while (count != 0) {
        qword_t value;
        switch (opcode) {
        case 0xa4:
        case 0xa5:
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &value, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_write(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xaa:
        case 0xab:
            value = amd64_reg_get(cpu, amd64_rax, size);
            qword_t guest_addr = amd64_string_addr(cpu, amd64_rdi);
            if (!amd64_mem_write(cpu, tlb, guest_addr, &value, size / 8))
                goto amd64_string_pf;
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        case 0xac:
        case 0xad:
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &value, size / 8))
                goto amd64_string_pf;
            amd64_reg_set(cpu, amd64_rax, size, value);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            break;
        case 0xae:
        case 0xaf: {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        default: {
            qword_t lhs;
            qword_t rhs;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rsi), &lhs, size / 8))
                goto amd64_string_pf;
            if (!amd64_mem_read(cpu, tlb, amd64_string_addr(cpu, amd64_rdi), &rhs, size / 8))
                goto amd64_string_pf;
            amd64_set_sub_flags(cpu, lhs, rhs, lhs - rhs, size);
            amd64_bump_string_reg(cpu, amd64_rsi, size);
            amd64_bump_string_reg(cpu, amd64_rdi, size);
            break;
        }
        }

        if (rep_mode != AMD64_REP_NONE) {
            count--;
            amd64_reg_set(cpu, amd64_rcx, count_size, count);
            if (opcode == 0xa6 || opcode == 0xa7 || opcode == 0xae || opcode == 0xaf) {
                if (rep_mode == AMD64_REPZ && !cpu->zf)
                    break;
                if (rep_mode == AMD64_REPNZ && cpu->zf)
                    break;
            }
        } else {
            break;
        }
    }
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_string_pf:
    cpu->amd64_rip = saved_rip;
    return INT_PF;
}

static inline int amd64_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t saved_rip = cpu->amd64_rip;
    cpu->amd64_current_insn_rip = saved_rip;
    if (amd64_bash_trace_enabled())
        amd64_trace_bash_cond_probe(cpu);
    if (amd64_cc1_trace_record_enabled())
        amd64_trace_cc1_step(cpu);
    if (amd64_as_trace_enabled())
        amd64_trace_as_step(cpu);
    cpu->amd64_address_size_prefix = false;
    if (amd64_cargo_trace_enabled)
        amd64_trace_cargo_start_call(cpu);
    if (amd64_htop_legacy_trace_enabled)
        amd64_trace_htop_window(cpu, tlb);
    if (amd64_cargo_trace_enabled)
        amd64_trace_cargo_pf_window(cpu, tlb);
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    struct amd64_rex_prefix rex = {};
    byte_t opcode;

restart_prefix:
    if (!amd64_fetch_u8(cpu, tlb, &opcode)) {
        cpu->amd64_rip = saved_rip;
        cpu->segfault_addr = saved_rip;
        return INT_GPF;
    }

    if (opcode == 0x66) {
        operand_size_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0x2e || opcode == 0x3e) {
        goto restart_prefix;
    }
    if (opcode == 0x67) {
        cpu->amd64_address_size_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0x64) {
        fs_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0xf0) {
        lock_prefix = true;
        goto restart_prefix;
    }
    if (opcode == 0xf3) {
        rep_mode = AMD64_REPZ;
        goto restart_prefix;
    }
    if (opcode == 0xf2) {
        rep_mode = AMD64_REPNZ;
        goto restart_prefix;
    }
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex.present = true;
        rex.w = (opcode & 0x8) != 0;
        rex.r = (opcode & 0x4) != 0;
        rex.x = (opcode & 0x2) != 0;
        rex.b = (opcode & 0x1) != 0;
        goto restart_prefix;
    }
    // 0xC4/0xC5/0x62 are unambiguous VEX/EVEX lead bytes in 64-bit mode
    // (LES/LDS, which use these opcodes in 32-bit mode, don't exist in long
    // mode). No legacy prefix or REX can legally precede VEX/EVEX, so it's
    // safe to check for it here regardless of what's already been consumed
    // above -- real VEX-encoded instructions never combine the two anyway.
    if (opcode == 0xc4 || opcode == 0xc5 || opcode == 0x62) {
        struct amd64_vex_prefix vex;
        if (!amd64_decode_vex(cpu, tlb, opcode, &vex)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!vex.present)
            return INT_UNDEFINED;
        return amd64_vex_step(cpu, tlb, saved_rip, vex, fs_prefix);
    }

    unsigned op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    (void) lock_prefix;
    switch (opcode) {
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3: {
        qword_t addr;
        qword_t value;
        unsigned size = (opcode == 0xa0 || opcode == 0xa2) ? 8 : op_size;
        if (!amd64_fetch_moffs_addr(cpu, tlb, &addr))
            goto amd64_gpf_restore;
        if (fs_prefix)
            addr += cpu->tls_ptr;
        if (opcode == 0xa0 || opcode == 0xa1) {
            if (!amd64_mem_read(cpu, tlb, addr, &value, size / 8))
                goto amd64_gpf_restore;
            amd64_reg_set(cpu, amd64_rax, size, value);
        } else {
            value = amd64_reg_get(cpu, amd64_rax, size);
            if (!amd64_mem_write(cpu, tlb, addr, &value, size / 8))
                goto amd64_gpf_restore;
        }
        break;
    }
    case 0xa4:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xa5:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xa6:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xa7:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xaa:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xab:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xac:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xad:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0xae:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, 8, rep_mode);
    case 0xaf:
        return amd64_string_op(cpu, tlb, saved_rip, opcode, op_size, rep_mode);
    case 0x0f: {
        byte_t op2;
        if (!amd64_fetch_u8(cpu, tlb, &op2)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (op2 == 0x05)
            return INT_AMD64_SYSCALL;
        if (op2 == 0x31) {
            qword_t tsc = amd64_rdtsc_value();
            amd64_reg_set(cpu, amd64_rax, 32, (dword_t) tsc);
            amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (tsc >> 32));
            break;
        }
        if (op2 == 0xa2) {
            dword_t eax = (dword_t) cpu->amd64_regs[amd64_rax];
            dword_t ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
            dword_t ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
            dword_t edx = (dword_t) cpu->amd64_regs[amd64_rdx];
            do_cpuid(&eax, &ebx, &ecx, &edx);
            cpu->amd64_regs[amd64_rax] = eax;
            cpu->amd64_regs[amd64_rbx] = ebx;
            cpu->amd64_regs[amd64_rcx] = ecx;
            cpu->amd64_regs[amd64_rdx] = edx;
            cpu->eax = eax;
            cpu->ebx = ebx;
            cpu->ecx = ecx;
            cpu->edx = edx;
            break;
        }
        if (op2 == 0x18) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg > 3)
                return INT_UNDEFINED;
            break;
        }
        if (op2 == 0xae) {
            struct amd64_modrm modrm;
            int interrupt;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            interrupt = amd64_fxsave_op(cpu, tlb, &modrm, fs_prefix, saved_rip);
            if (interrupt != INT_NONE)
                return interrupt;
            break;
        }
        if (op2 == 0x1e && rep_mode == AMD64_REPZ) {
            byte_t op3;
            if (!amd64_fetch_u8(cpu, tlb, &op3)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (op3 == 0xfa || op3 == 0xfb)
                break;  // ENDBR64 / ENDBR32 -> NOP
            // RDSSPD/RDSSPQ (F3 0F 1E /1, mod==11): read the CET shadow stack
            // pointer. iSH has no shadow stack, so NOP it (op3 is the whole
            // ModRM; the register form has no SIB/displacement). glibc
            // pre-zeroes the destination before the rdssp, so its "shadow
            // stack disabled" path is taken; without this, C++ throw/unwind,
            // gdb, and longjmp probes hit INT_UNDEFINED -> SIGILL.
            if (((op3 >> 3) & 7) == 1 && (op3 >> 6) == 3)
                break;
            return INT_UNDEFINED;
        }
        if (op2 == 0x38) {
            // Three-byte 0F 38 escape (SSSE3 / SSE4.1). Implemented: pshufb
            // (66 0F 38 00), pblendvb (10), blendvps/blendvpd (14/15), ptest (17),
            // pcmpeqq (29), pmulld (40), and pmovsx/pmovzx packed sign/zero-extend
            // moves (20-25 sign, 30-35 zero) — emitted by auto-vectorizers and by
            // optimized string/format code. They require the 66 operand-size
            // prefix and no F2/F3 prefix.
            byte_t op3;
            if (!amd64_fetch_u8(cpu, tlb, &op3)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            // crc32 (F2 0F 38 F0/F1): accumulate CRC32C of the r/m source into the
            // GP reg dest. A GP op (not xmm), so handle it before the xmm prefix
            // guard. F0 = r/m8; F1 = r/m16 (66) / r/m32 / r/m64 (REX.W).
            if ((op3 == 0xf0 || op3 == 0xf1) && rep_mode == AMD64_REPNZ) {
                struct amd64_modrm cmodrm;
                if (!amd64_decode_modrm(cpu, tlb, rex, &cmodrm)) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                unsigned src_size = (op3 == 0xf0) ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
                qword_t srcv;
                if (!amd64_read_rm(cpu, tlb, &cmodrm, fs_prefix, src_size, &srcv)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                if (op3 == 0xf1 && rex.w) {
                    uint64_t acc = (uint32_t) amd64_reg_get(cpu, cmodrm.reg, 32);
                    vec_crc32_64(NULL, &srcv, &acc);
                    amd64_reg_set(cpu, cmodrm.reg, 64, acc);
                } else {
                    uint32_t acc = (uint32_t) amd64_reg_get(cpu, cmodrm.reg, 32);
                    if (op3 == 0xf0) { uint8_t b = (uint8_t) srcv; vec_crc32_8(NULL, &b, &acc); }
                    else if (operand_size_prefix) { uint16_t w = (uint16_t) srcv; vec_crc32_16(NULL, &w, &acc); }
                    else { uint32_t d = (uint32_t) srcv; vec_crc32_32(NULL, &d, &acc); }
                    amd64_reg_set(cpu, cmodrm.reg, 32, acc); // zero-extends to 64
                }
                break;
            }
            bool is_pshufb = op3 == 0x00;
            bool is_pblendvb = op3 == 0x10;
            bool is_blendvps = op3 == 0x14;
            bool is_blendvpd = op3 == 0x15;
            bool is_ptest = op3 == 0x17;
            bool is_pcmpeqq = op3 == 0x29;
            bool is_pmulld = op3 == 0x40;
            bool is_pmovx = (op3 >= 0x20 && op3 <= 0x25) ||
                            (op3 >= 0x30 && op3 <= 0x35);
            // Additional xmm<-xmm/m128 ops handled via the shared vec.c helpers
            // (validated bit-exact vs real Intel by tests/remote/corpus):
            // SSSE3 phaddw/d/sw, pmaddubsw, phsubw/d/sw, psignb/w/d, pmulhrsw
            // (01-0b); pabsb/w/d (1c-1e); pmuldq (28), packusdw (2b),
            // pcmpgtq (37), pmin/pmax sb/sd/uw/ud (38-3f).
            // movntdqa (2a) is a plain aligned 128-bit load (copy); phminposuw
            // (41) reduces src to its min unsigned word + index. Both read r/m
            // into the local src and write the reg, so they share this path.
            bool is_vec38 = (op3 >= 0x01 && op3 <= 0x0b) ||
                            (op3 >= 0x1c && op3 <= 0x1e) || op3 == 0x28 ||
                            op3 == 0x2a || op3 == 0x2b || op3 == 0x37 ||
                            op3 == 0x41 || (op3 >= 0x38 && op3 <= 0x3f);
            if ((!is_pshufb && !is_pblendvb && !is_blendvps && !is_blendvpd &&
                    !is_ptest && !is_pcmpeqq && !is_pmulld && !is_pmovx && !is_vec38) ||
                    !operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT ||
                    (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
                return INT_UNDEFINED;
            if (is_ptest) {
                // PTEST xmm1, xmm2/m128 (66 0F 38 17). ZF = ((DEST & SRC) == 0),
                // CF = ((SRC & ~DEST) == 0); OF/AF/PF/SF cleared. Commonly emitted
                // for "is this vector all-zero / a subset" tests (e.g. memcmp).
                union xmm_reg src;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                unsigned __int128 d = cpu->xmm[modrm.reg].u128;
                unsigned __int128 s = src.u128;
                cpu->cf = (s & ~d) == 0;
                cpu->of = 0;
                cpu->af = 0;
                cpu->af_ops = 0;
                cpu->zf = (d & s) == 0;
                cpu->sf = 0;
                cpu->pf = 0;
                cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
                collapse_flags(cpu);
                break;
            }
            if (is_pshufb) {
                // PSHUFB xmm1, xmm2/m128 (66 0F 38 00). dest=reg=xmm1 is BOTH the
                // byte source and the destination; control=r/m. For each byte i:
                // result[i] = (control[i] & 0x80) ? 0 : dest_orig[control[i] & 0xF].
                // Snapshot dest first so the in-place store never corrupts a later
                // index lookup.
                union xmm_reg control;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &control)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                union xmm_reg dst = cpu->xmm[modrm.reg];
                union xmm_reg result;
                for (unsigned i = 0; i < 16; i++)
                    result.u8[i] = (control.u8[i] & 0x80)
                            ? 0 : dst.u8[control.u8[i] & 0x0F];
                cpu->xmm[modrm.reg] = result;
                break;
            }
            if (is_pblendvb) {
                // PBLENDVB xmm1, xmm2/m128 (66 0F 38 10). Per-byte variable blend
                // controlled by the high bit of each byte in the IMPLICIT XMM0
                // mask: result[i] = mask[i]&0x80 ? src[i] : dst[i]. dst=reg,
                // src=r/m, mask=xmm0. Snapshot all three before writing so an
                // operand that aliases xmm0 (or the destination) stays correct.
                union xmm_reg src;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                union xmm_reg dst = cpu->xmm[modrm.reg];
                union xmm_reg mask = cpu->xmm[0];
                union xmm_reg result;
                for (unsigned i = 0; i < 16; i++)
                    result.u8[i] = (mask.u8[i] & 0x80) ? src.u8[i] : dst.u8[i];
                cpu->xmm[modrm.reg] = result;
                break;
            }
            if (is_blendvps || is_blendvpd) {
                // BLENDVPS (66 0F 38 14) / BLENDVPD (15) xmm1, xmm2/m128, <XMM0>.
                // Same implicit-XMM0 variable blend as pblendvb, but per 32-bit
                // (blendvps) or 64-bit (blendvpd) lane, selected by that lane's
                // high (sign) bit in XMM0. dst=reg, src=r/m, mask=xmm0; snapshot
                // all three before the store so an operand aliasing xmm0 (or the
                // destination) stays correct.
                union xmm_reg src;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                union xmm_reg dst = cpu->xmm[modrm.reg];
                union xmm_reg mask = cpu->xmm[0];
                union xmm_reg result;
                if (is_blendvps) {
                    for (unsigned i = 0; i < 4; i++)
                        result.u32[i] = (mask.u32[i] & 0x80000000u)
                                ? src.u32[i] : dst.u32[i];
                } else {
                    for (unsigned i = 0; i < 2; i++)
                        result.qw[i] = (mask.qw[i] & 0x8000000000000000ull)
                                ? src.qw[i] : dst.qw[i];
                }
                cpu->xmm[modrm.reg] = result;
                break;
            }
            if (is_pmulld || is_pcmpeqq) {
                // PMULLD (66 0F 38 40): 4x packed 32-bit multiply, low 32 bits of
                // each product (the low half is identical for signed/unsigned).
                // PCMPEQQ (66 0F 38 29): 2x packed 64-bit equality, all-ones where
                // the lanes are equal else 0. dst=reg, src=r/m; read both operands
                // before the store in case src aliases the destination.
                union xmm_reg src;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                union xmm_reg dst = cpu->xmm[modrm.reg];
                union xmm_reg result;
                if (is_pmulld) {
                    for (unsigned i = 0; i < 4; i++)
                        result.u32[i] = dst.u32[i] * src.u32[i];
                } else {
                    for (unsigned i = 0; i < 2; i++)
                        result.qw[i] = (dst.qw[i] == src.qw[i])
                                ? 0xFFFFFFFFFFFFFFFFull : 0;
                }
                cpu->xmm[modrm.reg] = result;
                break;
            }
            if (is_vec38) {
                // Read the source into a local copy (no aliasing with the
                // destination) and dispatch to the shared vec.c helper, which
                // modifies cpu->xmm[modrm.reg] in place.
                union xmm_reg src;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                union xmm_reg *dst = &cpu->xmm[modrm.reg];
                switch (op3) {
                    case 0x01: vec_phaddw128(cpu, &src, dst); break;
                    case 0x02: vec_phaddd128(cpu, &src, dst); break;
                    case 0x03: vec_phaddsw128(cpu, &src, dst); break;
                    case 0x04: vec_pmaddubsw128(cpu, &src, dst); break;
                    case 0x05: vec_phsubw128(cpu, &src, dst); break;
                    case 0x06: vec_phsubd128(cpu, &src, dst); break;
                    case 0x07: vec_phsubsw128(cpu, &src, dst); break;
                    case 0x08: vec_psignb128(cpu, &src, dst); break;
                    case 0x09: vec_psignw128(cpu, &src, dst); break;
                    case 0x0a: vec_psignd128(cpu, &src, dst); break;
                    case 0x0b: vec_pmulhrsw128(cpu, &src, dst); break;
                    case 0x1c: vec_pabsb128(cpu, &src, dst); break;
                    case 0x1d: vec_pabsw128(cpu, &src, dst); break;
                    case 0x1e: vec_pabsd128(cpu, &src, dst); break;
                    case 0x28: vec_pmuldq128(cpu, &src, dst); break;
                    case 0x2b: vec_packusdw128(cpu, &src, dst); break;
                    case 0x37: vec_pcmpgtq128(cpu, &src, dst); break;
                    case 0x38: vec_pminsb128(cpu, &src, dst); break;
                    case 0x39: vec_pminsd128(cpu, &src, dst); break;
                    case 0x3a: vec_pminuw128(cpu, &src, dst); break;
                    case 0x3b: vec_pminud128(cpu, &src, dst); break;
                    case 0x3c: vec_pmaxsb128(cpu, &src, dst); break;
                    case 0x3d: vec_pmaxsd128(cpu, &src, dst); break;
                    case 0x3e: vec_pmaxuw128(cpu, &src, dst); break;
                    case 0x3f: vec_pmaxud128(cpu, &src, dst); break;
                    case 0x2a: *dst = src; break; // movntdqa (aligned 128-bit load)
                    case 0x41: vec_phminposuw128(cpu, &src, dst); break;
                    default: return INT_UNDEFINED;
                }
                break;
            }
            bool zero_extend = (op3 & 0xf0) == 0x30;
            unsigned src_elem_bytes, dst_elem_bytes;
            switch (op3 & 0x0f) {
                case 0x0: src_elem_bytes = 1; dst_elem_bytes = 2; break; // b->w
                case 0x1: src_elem_bytes = 1; dst_elem_bytes = 4; break; // b->d
                case 0x2: src_elem_bytes = 1; dst_elem_bytes = 8; break; // b->q
                case 0x3: src_elem_bytes = 2; dst_elem_bytes = 4; break; // w->d
                case 0x4: src_elem_bytes = 2; dst_elem_bytes = 8; break; // w->q
                default:  src_elem_bytes = 4; dst_elem_bytes = 8; break; // d->q (0x5)
            }
            unsigned count = 16 / dst_elem_bytes;          // result lanes
            unsigned src_bytes = count * src_elem_bytes;   // bytes consumed
            // Source is the low src_bytes of an xmm register or a src_bytes-sized
            // memory operand. Read only those bytes for the memory form so a
            // qword/dword/word operand at a page boundary cannot over-read.
            union xmm_reg src = {0};
            if (modrm.is_reg) {
                src = cpu->xmm[modrm.rm];
            } else {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (!amd64_mem_read(cpu, tlb, addr, &src, src_bytes)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
            }
            union xmm_reg result = {0};
            for (unsigned i = 0; i < count; i++) {
                int64_t elem;
                if (src_elem_bytes == 1)
                    elem = zero_extend ? (int64_t) src.u8[i]
                                       : (int64_t) (int8_t) src.u8[i];
                else if (src_elem_bytes == 2)
                    elem = zero_extend ? (int64_t) src.u16[i]
                                       : (int64_t) (int16_t) src.u16[i];
                else
                    elem = zero_extend ? (int64_t) src.u32[i]
                                       : (int64_t) (int32_t) src.u32[i];
                if (dst_elem_bytes == 2)
                    result.u16[i] = (uint16_t) elem;
                else if (dst_elem_bytes == 4)
                    result.u32[i] = (uint32_t) elem;
                else
                    result.qw[i] = (uint64_t) elem;
            }
            cpu->xmm[modrm.reg] = result;
            break;
        }
        if (op2 == 0x3a) {
            // Three-byte 0F 3A escape: SSSE3 palignr + SSE4.1 insert/extract,
            // imm-blend and round. Mirrors the i386 JIT decode (emu/decode.h)
            // and reuses the shared vec.c helpers; validated bit-exact vs real
            // Intel by tests/remote/corpus/sse4.c. imm8 follows the ModRM (and
            // any SIB/disp), so it is fetched BEFORE any memory operand read so
            // a RIP-relative effective address is computed at the instruction
            // end. pinsr/pextr take a GP/mem r/m (not xmm); REX.W selects the
            // q forms (pinsrq/pextrq).
            byte_t op3;
            if (!amd64_fetch_u8(cpu, tlb, &op3)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bool known = (op3 >= 0x08 && op3 <= 0x0f) ||
                         (op3 >= 0x14 && op3 <= 0x17) ||
                         op3 == 0x20 || op3 == 0x21 || op3 == 0x22 ||
                         (op3 >= 0x40 && op3 <= 0x42) ||
                         (op3 >= 0x60 && op3 <= 0x63);
            if (!known || !operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            byte_t imm;
            if (!amd64_fetch_u8(cpu, tlb, &imm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            union xmm_reg *xreg = &cpu->xmm[modrm.reg];
            if (op3 >= 0x08 && op3 <= 0x0f) {
                // round/blend/palignr: xmm/m source. Scalar rounds read only
                // their m32/m64 lane so a narrow operand can't over-read a page.
                unsigned src_bytes = (op3 == 0x0a) ? 4 : (op3 == 0x0b) ? 8 : 16;
                union xmm_reg src = {0};
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src = cpu->xmm[modrm.rm];
                } else {
                    qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                    if (!amd64_mem_read(cpu, tlb, addr, &src, src_bytes)) {
                        cpu->amd64_rip = saved_rip;
                        amd64_sync_legacy_regs(cpu);
                        return INT_PF;
                    }
                }
                switch (op3) {
                    case 0x08: vec_round_ps128(cpu, &src, xreg, imm); break;
                    case 0x09: vec_round_pd128(cpu, &src, xreg, imm); break;
                    case 0x0a: vec_round_ss32(cpu, &src, xreg, imm); break;
                    case 0x0b: vec_round_sd64(cpu, &src, xreg, imm); break;
                    case 0x0c: vec_blend_ps128(cpu, &src, xreg, imm); break;
                    case 0x0d: vec_blend_pd128(cpu, &src, xreg, imm); break;
                    case 0x0e: vec_blend_w128(cpu, &src, xreg, imm); break;
                    default:   vec_palignr128(cpu, &src, xreg, imm); break; // 0x0f
                }
                break;
            }
            if (op3 == 0x21) {
                // insertps: register source selects a dword via imm[7:6]; a
                // memory source is a single m32 (imm[7:6] ignored). imm[5:4]
                // picks the dest lane, imm[3:0] is a per-lane zero mask.
                uint32_t v;
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    v = cpu->xmm[modrm.rm].u32[(imm >> 6) & 3];
                } else {
                    qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                    if (!amd64_mem_read(cpu, tlb, addr, &v, 4)) {
                        cpu->amd64_rip = saved_rip;
                        amd64_sync_legacy_regs(cpu);
                        return INT_PF;
                    }
                }
                xreg->u32[(imm >> 4) & 3] = v;
                for (int i = 0; i < 4; i++)
                    if (imm & (1 << i)) xreg->u32[i] = 0;
                break;
            }
            if (op3 >= 0x40 && op3 <= 0x42) {
                // dpps/dppd/mpsadbw: xmm/m128 source, imm8, dst in/out.
                union xmm_reg src = {0};
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src = cpu->xmm[modrm.rm];
                } else {
                    qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                    if (!amd64_mem_read(cpu, tlb, addr, &src, 16)) {
                        cpu->amd64_rip = saved_rip;
                        amd64_sync_legacy_regs(cpu);
                        return INT_PF;
                    }
                }
                switch (op3) {
                    case 0x40: vec_dpps128(cpu, &src, xreg, imm); break;
                    case 0x41: vec_dppd128(cpu, &src, xreg, imm); break;
                    default:   vec_mpsadbw128(cpu, &src, xreg, imm); break; // 0x42
                }
                break;
            }
            if (op3 >= 0x60 && op3 <= 0x63) {
                // pcmp{e,i}str{m,i}: xmm2/m128 source, xmm1 reg = dst. The shared
                // helpers read EAX/EDX (explicit lengths) and write ECX (index
                // forms) / XMM0 (mask forms) / EFLAGS via the legacy register
                // views, so mirror them into the amd64 registers around the call.
                union xmm_reg src = {0};
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src = cpu->xmm[modrm.rm];
                } else {
                    qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                    if (!amd64_mem_read(cpu, tlb, addr, &src, 16)) {
                        cpu->amd64_rip = saved_rip;
                        amd64_sync_legacy_regs(cpu);
                        return INT_PF;
                    }
                }
                cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
                cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
                switch (op3) {
                    case 0x60: vec_pcmpestrm128(cpu, &src, xreg, imm); break;
                    case 0x61: vec_pcmpestri128(cpu, &src, xreg, imm); break;
                    case 0x62: vec_pcmpistrm128(cpu, &src, xreg, imm); break;
                    default:   vec_pcmpistri128(cpu, &src, xreg, imm); break; // 0x63
                }
                if (op3 == 0x61 || op3 == 0x63)
                    cpu->amd64_regs[amd64_rcx] = (uint32_t) cpu->ecx; // index -> RCX (zero-extend)
                collapse_flags(cpu);
                break;
            }
            if (op3 == 0x20 || op3 == 0x22) {
                // pinsrb (m8) / pinsrd|pinsrq: insert a GP/mem r/m into a lane.
                unsigned size = op3 == 0x20 ? 8 : (rex.w ? 64 : 32);
                qword_t v;
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &v)) {
                    cpu->amd64_rip = saved_rip;
                    amd64_sync_legacy_regs(cpu);
                    return INT_PF;
                }
                if (op3 == 0x20)
                    xreg->u8[imm & 15] = (uint8_t) v;
                else if (rex.w)
                    xreg->qw[imm & 1] = v;
                else
                    xreg->u32[imm & 3] = (uint32_t) v;
                break;
            }
            // pextrb/pextrw/pextrd|pextrq/extractps: extract a lane to a GP/mem
            // r/m. A register destination is zero-extended (write size 32/64).
            qword_t v;
            unsigned size;
            switch (op3) {
                case 0x14: v = xreg->u8[imm & 15]; size = modrm.is_reg ? 32 : 8;  break;
                case 0x15: v = xreg->u16[imm & 7]; size = modrm.is_reg ? 32 : 16; break;
                case 0x16: if (rex.w) { v = xreg->qw[imm & 1]; size = 64; }
                           else       { v = xreg->u32[imm & 3]; size = 32; } break;
                default:   v = xreg->u32[imm & 3]; size = 32; break; // 0x17 extractps
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, size, v)) {
                cpu->amd64_rip = saved_rip;
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            break;
        }
        if (op2 >= 0x80 && op2 <= 0x8f) {
            int32_t rel32;
            if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bool taken = amd64_cond_eval(cpu, op2 & 0xf);
            if (amd64_as_alu_stderr_enabled() &&
                    current != NULL &&
                    current->abi == GUEST_ABI_AMD64 &&
                    strcmp(current->comm, "as") == 0) {
                fprintf(stderr,
                        "amd64 as jcc32: rip=%#llx cc=%u taken=%u target=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                        (unsigned long long) saved_rip,
                        op2 & 0xf,
                        taken,
                        (unsigned long long) (cpu->amd64_rip + rel32),
                        cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
            }
            amd64_trace_cc1_je_probe(cpu, saved_rip, taken, cpu->amd64_rip + rel32);
            if (taken) {
                qword_t target = cpu->amd64_rip + rel32;
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcc");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
                cpu->amd64_rip = target;
            }
            break;
        }
        if (op2 >= 0x40 && op2 <= 0x4f) {
            struct amd64_modrm modrm;
            qword_t src;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src)) {
                cpu->amd64_rip = saved_rip;
                return INT_GPF;
            }
            if (amd64_cond_eval(cpu, op2 & 0xf))
                amd64_reg_set(cpu, modrm.reg, op_size, src);
            break;
        }
        if (op2 >= 0x90 && op2 <= 0x9f) {
            struct amd64_modrm modrm;
            qword_t value = amd64_cond_eval(cpu, op2 & 0xf) ? 1 : 0;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, value))
                goto amd64_gpf_restore;
            break;
        }
        if (op2 == 0xa4 || op2 == 0xa5 || op2 == 0xac || op2 == 0xad) {
            struct amd64_modrm modrm;
            qword_t lhs, rhs, result;
            unsigned count;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (op2 == 0xa4 || op2 == 0xac) {
                uint8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                count = imm8 & (op_size == 64 ? 0x3f : 0x1f);
            } else {
                count = amd64_reg_get(cpu, amd64_rcx, 8) & (op_size == 64 ? 0x3f : 0x1f);
            }
            if (count == 0)
                break;
            if (count > op_size)
                count %= op_size;
            if (count == 0)
                break;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (op2 == 0xa4 || op2 == 0xa5) {
                result = amd64_trunc((lhs << count) | (rhs >> (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_gpf_restore;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, true);
            } else {
                result = amd64_trunc((amd64_trunc(lhs, op_size) >> count) | (rhs << (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_gpf_restore;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, false);
            }
            break;
        }
        if (op2 == 0xb8) {
            // POPCNT r, r/m (F3 0F B8). The F3 (REPZ) prefix is mandatory — bare
            // 0F B8 is not POPCNT. Counts set bits in the source operand; ZF is
            // set iff the source is zero, and CF/OF/SF/AF/PF are cleared.
            struct amd64_modrm modrm;
            qword_t src;
            qword_t src_masked;
            qword_t count;
            if (rep_mode != AMD64_REPZ)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
                goto amd64_gpf_restore;
            src_masked = amd64_trunc(src, op_size);
            count = (op_size == 64)
                    ? (qword_t) __builtin_popcountll(src_masked)
                    : (qword_t) __builtin_popcount((uint32_t) src_masked);
            cpu->cf = 0;
            cpu->of = 0;
            cpu->af = 0;
            cpu->af_ops = 0;
            cpu->zf = src_masked == 0;
            cpu->sf = 0;
            cpu->pf = 0;
            cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
            collapse_flags(cpu);
            amd64_reg_set(cpu, modrm.reg, op_size, count);
            break;
        }
        if (op2 == 0xbc || op2 == 0xbd) {
            struct amd64_modrm modrm;
            qword_t src;
            qword_t src_masked;
            qword_t index;
            bool count_zeroes;
            if (rep_mode != AMD64_REP_NONE && rep_mode != AMD64_REPZ)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
                goto amd64_gpf_restore;
            src_masked = amd64_trunc(src, op_size);
            count_zeroes = rep_mode == AMD64_REPZ;
            collapse_flags(cpu);
            if (count_zeroes) {
                cpu->cf = src_masked == 0;
                cpu->cf_bit = cpu->cf;
                cpu->zf = 0;
            } else {
                cpu->zf = src_masked == 0;
            }
            cpu->zf_res = 0;
            if (src_masked == 0) {
                if (count_zeroes)
                    amd64_reg_set(cpu, modrm.reg, op_size, op_size);
                break;
            }
            if (op2 == 0xbc) {
                // TZCNT/BSF: trailing-zero count from the LSB doesn't depend
                // on the field's total width, so ctz needs no adjustment.
                index = (op_size == 64)
                        ? (qword_t) __builtin_ctzll(src_masked)
                        : (qword_t) __builtin_ctz((uint32_t) src_masked);
            } else if (count_zeroes) {
                // LZCNT: leading-zero count relative to the operand WIDTH.
                // __builtin_clz always counts against a 32-bit field, so a
                // 16-bit-truncated src (upper 16 bits zero) over-counts by
                // exactly (32 - op_size) leading zeros that aren't part of
                // the real 16-bit field; subtract that back out. (Found via
                // tests/remote/corpus/popcnt_lzcnt_tzcnt.c: this path used
                // to fall through to the BSR bit-index formula below, which
                // is a different value -- e.g. lzcnt16(1) is 15, not 0.)
                index = (op_size == 64)
                        ? (qword_t) __builtin_clzll(src_masked)
                        : (qword_t) (__builtin_clz((uint32_t) src_masked) - (32 - op_size));
            } else {
                // BSR: bit-index of the highest set bit, width-independent.
                index = (op_size == 64)
                        ? (qword_t) (63 - __builtin_clzll(src_masked))
                        : (qword_t) (31 - __builtin_clz((uint32_t) src_masked));
            }
            if (count_zeroes)
                cpu->zf = index == 0;
            amd64_reg_set(cpu, modrm.reg, op_size, index);
            break;
        }
        if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) {
            struct amd64_modrm modrm;
            qword_t src;
            unsigned src_size = (op2 == 0xb6 || op2 == 0xbe) ? 8 : 16;
            unsigned dst_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, src_size, &src))
                goto amd64_gpf_restore;
            if (op2 == 0xbe || op2 == 0xbf)
                src = (qword_t) amd64_sign_extend(src, src_size);
            amd64_reg_set(cpu, modrm.reg, dst_size, src);
            break;
        }
        if (op2 == 0x6e) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (operand_size_prefix) {
                union xmm_reg value;
                if (modrm.reg >= AMD64_XMM_COUNT)
                    return INT_UNDEFINED;
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                    goto amd64_gpf_restore;
                value.u128 = 0;
                if (rex.w)
                    value.qw[0] = src_scalar;
                else
                    value.u32[0] = (uint32_t) src_scalar;
                cpu->xmm[modrm.reg] = value;
            } else {
                if (modrm.reg >= 8)
                    return INT_UNDEFINED;
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                    goto amd64_gpf_restore;
                cpu->mm[modrm.reg].qw = rex.w ? src_scalar : (uint32_t) src_scalar;
            }
            break;
        }
        if ((op2 == 0x2c || op2 == 0x2d) && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            qword_t result;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    src_double = *(double *) &src_scalar;
                }
                result = (op2 == 0x2d) ? amd64_cvt_scalar_to_int(src_double, rex.w)
                                       : amd64_cvtt_scalar_to_int(src_double, rex.w);
            } else {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    if (modrm.rm >= AMD64_XMM_COUNT)
                        return INT_UNDEFINED;
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                result = (op2 == 0x2d) ? amd64_cvt_scalar_to_int((double) src_float, rex.w)
                                       : amd64_cvtt_scalar_to_int((double) src_float, rex.w);
            }
            amd64_reg_set(cpu, modrm.reg, rex.w ? 64 : 32, result);
            break;
        }
        if (op2 == 0x2a && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            union xmm_reg value;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_gpf_restore;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                value.f64[0] = rex.w ? (double) (sqword_t) src_scalar
                                     : (double) (int32_t) src_scalar;
            } else {
                value.f32[0] = rex.w ? (float) (sqword_t) src_scalar
                                     : (float) (int32_t) src_scalar;
            }
            cpu->xmm[modrm.reg] = value;
            break;
        }
        if (op2 == 0x5a && (rep_mode == AMD64_REPZ || rep_mode == AMD64_REPNZ)) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            union xmm_reg value;
            if (operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT || (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    src_double = *(double *) &src_scalar;
                }
                value.f32[0] = (float) src_double;
            } else {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                value.f64[0] = (double) src_float;
            }
            cpu->xmm[modrm.reg] = value;
            break;
        }
        if ((op2 == 0x2e || op2 == 0x2f) && rep_mode == AMD64_REP_NONE) {
            struct amd64_modrm modrm;
            qword_t src_scalar;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg >= AMD64_XMM_COUNT || (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT))
                return INT_UNDEFINED;
            if (operand_size_prefix) {
                double lhs, rhs;
                lhs = cpu->xmm[modrm.reg].f64[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_gpf_restore;
                    rhs = *(double *) &src_scalar;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            } else {
                float lhs, rhs;
                uint32_t src_word;
                lhs = cpu->xmm[modrm.reg].f32[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_gpf_restore;
                    src_word = (uint32_t) src_scalar;
                    rhs = *(float *) &src_word;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            }
            break;
        }
        // SSE2/MMX packed-integer ops the inline decoder below does not implement:
        // PMULLW/PMULHW (d5/e5), PMULHUW (e4), packed averages (e0/e3), packed
        // shifts by xmm/imm (d1-d3, e1/e2, f1-f3, 71-73), saturating add/sub
        // (e8/e9/ec/ed), signed min/max (ea/ee), PMADDWD (f5) and MOVNTDQ (e7).
        // The shared vector bridge (amd64_jit_0f_vec_rm) implements the complete
        // set the JIT bridges, so decode the modrm here purely to find the
        // instruction end, rewind rip to the instruction start, and hand off.
        // Without this, a basic block that fell back to the interpreter (e.g. a
        // crypto routine touching xmm8-15) hits one of these and raises a bogus
        // #UD even though the JIT path handles it — see sshd-auth pmullw crash.
        if (op2 == 0xd1 || op2 == 0xd2 || op2 == 0xd3 || op2 == 0xd5 ||
                (op2 >= 0xe0 && op2 <= 0xe5) || op2 == 0xe7 ||
                (op2 >= 0xe8 && op2 <= 0xea) || (op2 >= 0xec && op2 <= 0xee) ||
                op2 == 0xf1 || op2 == 0xf2 || op2 == 0xf3 || op2 == 0xf5 ||
                op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            // 71-73 carry a one-byte immediate shift count after the modrm; the
            // rest of these forms have no immediate, so rip already marks the end.
            unsigned long vec_next_ip = (unsigned long) cpu->amd64_rip;
            if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73)
                vec_next_ip += 1;
            cpu->amd64_rip = saved_rip;
            return amd64_jit_0f_vec_rm(cpu, tlb, op2, vec_next_ip);
        }
        // no-66 MMX forms of the packed-int ops added later: punpck{l,h}{bw,wd}
        // + punpckhdq (60/61/68/69/6a), pack ss/us (63/67/6b), saturating
        // add/sub (d8/d9/dc/dd), unsigned min/max (da/de), psadbw (f6). The
        // inline decoder already handles the 66 (XMM) forms, so this is gated to
        // the no-prefix MMX forms and hands them to the bridge. (The saturating
        // signed/min-max e8-ee and pavg/pmulhuw e0/e3/e4 + pmaddwd f5 forms are
        // already delegated by the block above.)
        if (!operand_size_prefix && rep_mode == AMD64_REP_NONE &&
                ((op2 >= 0x60 && op2 <= 0x6b && op2 != 0x62 && op2 != 0x64 &&
                  op2 != 0x65 && op2 != 0x66) ||
                 op2 == 0xd8 || op2 == 0xd9 || op2 == 0xda || op2 == 0xdc ||
                 op2 == 0xdd || op2 == 0xde || op2 == 0xf6)) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            unsigned long vec_next_ip = (unsigned long) cpu->amd64_rip;
            cpu->amd64_rip = saved_rip;
            return amd64_jit_0f_vec_rm(cpu, tlb, op2, vec_next_ip);
        }
        // SSE floating-point ops the inline decoder below does not implement:
        // movmskps/pd (50), sqrt (51), rsqrt (52), rcp (53), packed
        // cvtps2pd/cvtpd2ps (5a, scalar cvtss2sd/cvtsd2ss handled above),
        // cvtdq2ps/cvtps2dq/cvttps2dq (5b), and cvtdq2pd/cvttpd2dq/cvtpd2dq
        // (e6). None carry an immediate, so the modrm end marks the instruction
        // end; hand off to the complete vector bridge exactly as the JIT does.
        if (op2 == 0x50 || op2 == 0x51 || op2 == 0x52 || op2 == 0x53 ||
                op2 == 0x5a || op2 == 0x5b || op2 == 0xe6) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            unsigned long vec_next_ip = (unsigned long) cpu->amd64_rip;
            cpu->amd64_rip = saved_rip;
            return amd64_jit_0f_vec_rm(cpu, tlb, op2, vec_next_ip);
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x12 || op2 == 0x13 ||
                op2 == 0x14 || op2 == 0x15 ||
                op2 == 0x16 || op2 == 0x17 ||
                op2 == 0x28 || op2 == 0x29 || op2 == 0x58 || op2 == 0x59 ||
                op2 == 0x5c || op2 == 0x5d || op2 == 0x5e || op2 == 0x54 || op2 == 0x55 ||
                op2 == 0x5f ||
                op2 == 0x56 || op2 == 0x57 || op2 == 0x60 || op2 == 0x61 ||
                op2 == 0x62 || op2 == 0x63 || op2 == 0x67 || op2 == 0x68 || op2 == 0x69 || op2 == 0x6a || op2 == 0x6b || op2 == 0x6c || op2 == 0x6d ||
                op2 == 0x6f || op2 == 0x70 || op2 == 0x7c || op2 == 0x7d || op2 == 0x7e || op2 == 0x7f ||
                op2 == 0x64 || op2 == 0x65 || op2 == 0x66 || op2 == 0x74 || op2 == 0x75 || op2 == 0x76 || op2 == 0xc2 || op2 == 0xc4 || op2 == 0xc5 || op2 == 0xc6 ||
                op2 == 0xd0 || op2 == 0xd4 || op2 == 0xd6 || op2 == 0xd7 ||
                op2 == 0xd8 || op2 == 0xd9 || op2 == 0xda || op2 == 0xdb || op2 == 0xdc || op2 == 0xdd || op2 == 0xde || op2 == 0xdf ||
                op2 == 0xeb || op2 == 0xef ||
                op2 == 0xf0 || op2 == 0xf4 || op2 == 0xf6 ||
                op2 == 0xf8 || op2 == 0xf9 || op2 == 0xfa || op2 == 0xfb ||
                op2 == 0xfc || op2 == 0xfd || op2 == 0xfe) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            union xmm_reg src_xmm;
            qword_t src_scalar;
            uint8_t imm8;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if ((op2 != 0xc5 && modrm.reg >= AMD64_XMM_COUNT) ||
                    (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT &&
                     !(op2 == 0x7e && operand_size_prefix && rep_mode == AMD64_REP_NONE) &&
                     op2 != 0xc4 && op2 != 0xc5))
                return INT_UNDEFINED;
            if (op2 == 0x10 || op2 == 0x28 || op2 == 0x6f) {
                if (op2 == 0x6f && !(operand_size_prefix || rep_mode == AMD64_REPZ))
                    return INT_UNDEFINED;
                if (op2 == 0x10 && rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.u32[0] = cpu->xmm[modrm.rm].u32[0];
                    } else {
                        value.u128 = 0;
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                            goto amd64_gpf_restore;
                        value.u32[0] = (uint32_t) src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else if (op2 == 0x10 && rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                    } else {
                        value.u128 = 0;
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[0] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else {
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                        goto amd64_gpf_restore;
                    cpu->xmm[modrm.reg] = value;
                }
            } else if (op2 == 0x11 || op2 == 0x29 || op2 == 0x7f) {
                if (op2 == 0x7f && !operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                    // 0F 7F (no mandatory prefix): movq mm/m64, mm — MMX store.
                    // The shared JIT bridge (amd64_jit_0f_vec_rm, movq_mm_store)
                    // implements this; mirror it here so a basic block that falls
                    // back to the interpreter mid-MMX (e.g. libgcrypt SHA
                    // shuttling state through mm0-7) does not raise a bogus #UD.
                    // The MMX load (0F 6F) and the 0xd6 MMX<->XMM moves are
                    // already handled; this completes the store side. Guard the
                    // MMX register indices to <8, as the bridge and 0x7e do.
                    if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
                        return INT_UNDEFINED;
                    if (modrm.is_reg) {
                        cpu->mm[modrm.rm] = cpu->mm[modrm.reg];
                    } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                                   cpu->mm[modrm.reg].qw)) {
                        goto amd64_gpf_restore;
                    }
                } else if (op2 == 0x7f && !(operand_size_prefix || rep_mode == AMD64_REPZ)) {
                    return INT_UNDEFINED;
                } else if (op2 == 0x11 && rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    if (modrm.is_reg) {
                        cpu->xmm[modrm.rm].u32[0] = cpu->xmm[modrm.reg].u32[0];
                    } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 32,
                                   cpu->xmm[modrm.reg].u32[0])) {
                        goto amd64_gpf_restore;
                    }
                } else if (op2 == 0x11 && rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    if (modrm.is_reg) {
                        cpu->xmm[modrm.rm].qw[0] = cpu->xmm[modrm.reg].qw[0];
                    } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                                   cpu->xmm[modrm.reg].qw[0])) {
                        goto amd64_gpf_restore;
                    }
                } else {
                    value = cpu->xmm[modrm.reg];
                    if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                        goto amd64_gpf_restore;
                }
            } else if (op2 == 0x12) {
                if (rep_mode == AMD64_REPZ) {
                    // movsldup (F3 0F 12): duplicate even singles [s0,s0,s2,s2].
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                        goto amd64_gpf_restore;
                    vec_movsldup128(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
                } else if (rep_mode == AMD64_REPNZ) {
                    // movddup (F2 0F 12): duplicate the low double; mem reads m64.
                    if (modrm.is_reg) {
                        src_xmm = cpu->xmm[modrm.rm];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        src_xmm.qw[0] = src_scalar;
                    }
                    vec_movddup64(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
                } else {
                    // movhlps (reg form) / movlps / movlpd (mem form).
                    if (operand_size_prefix && modrm.is_reg)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.qw[0] = cpu->xmm[modrm.rm].qw[1];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[0] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                }
            } else if (op2 == 0x13) {
                // movlps (NP) / movlpd (66) m64, xmm: both store xmm[63:0] to
                // memory, so the 66 (movlpd) form must be accepted too -- it was
                // wrongly #UD'd (chronyd movlpd [rsp+x],xmm). reg form is #UD.
                if (rep_mode != AMD64_REP_NONE || modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                    goto amd64_gpf_restore;
            } else if (op2 == 0x14 || op2 == 0x15) {
                if (rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (operand_size_prefix) {
                    // unpcklpd/unpckhpd: interleave the 64-bit lanes.
                    if (op2 == 0x14) {
                        value.qw[1] = src_xmm.qw[0];
                    } else {
                        value.qw[0] = value.qw[1];
                        value.qw[1] = src_xmm.qw[1];
                    }
                } else {
                    // unpcklps/unpckhps: interleave the 32-bit lanes.
                    if (op2 == 0x14)
                        vec_unpackl_ps128(NULL, &src_xmm, &value);
                    else
                        vec_unpackh_ps128(NULL, &src_xmm, &value);
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x58 || op2 == 0x59 || op2 == 0x5c || op2 == 0x5d || op2 == 0x5e || op2 == 0x5f) {
                value = cpu->xmm[modrm.reg];
                if (rep_mode == AMD64_REPZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    {
                        float lhs, rhs;
                        uint32_t src_word;
                        lhs = value.f32[0];
                        if (modrm.is_reg) {
                            rhs = cpu->xmm[modrm.rm].f32[0];
                        } else {
                            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                                goto amd64_gpf_restore;
                            src_word = (uint32_t) src_scalar;
                            rhs = *(float *) &src_word;
                        }
                        switch (op2) {
                        case 0x58:
                            value.f32[0] = lhs + rhs;
                            break;
                        case 0x59:
                            value.f32[0] = lhs * rhs;
                            break;
                        case 0x5c:
                            value.f32[0] = lhs - rhs;
                            break;
                        case 0x5d:
                            value.f32[0] = lhs < rhs ? lhs : rhs;
                            break;
                        case 0x5e:
                            value.f32[0] = lhs / rhs;
                            break;
                        case 0x5f:
                            value.f32[0] = lhs > rhs ? lhs : rhs;
                            break;
                        }
                    }
                } else if (rep_mode == AMD64_REPNZ) {
                    if (operand_size_prefix)
                        return INT_UNDEFINED;
                    {
                        double lhs, rhs;
                        lhs = value.f64[0];
                        if (modrm.is_reg) {
                            rhs = cpu->xmm[modrm.rm].f64[0];
                        } else {
                            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                                goto amd64_gpf_restore;
                            rhs = *(double *) &src_scalar;
                        }
                        switch (op2) {
                        case 0x58:
                            value.f64[0] = lhs + rhs;
                            break;
                        case 0x59:
                            value.f64[0] = lhs * rhs;
                            break;
                        case 0x5c:
                            value.f64[0] = lhs - rhs;
                            break;
                        case 0x5d:
                            value.f64[0] = lhs < rhs ? lhs : rhs;
                            break;
                        case 0x5e:
                            value.f64[0] = lhs / rhs;
                            break;
                        case 0x5f:
                            value.f64[0] = lhs > rhs ? lhs : rhs;
                            break;
                        }
                    }
                } else {
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                        goto amd64_gpf_restore;
                    if (operand_size_prefix) {
                        switch (op2) {
                        case 0x58:
                            value.f64[0] += src_xmm.f64[0];
                            value.f64[1] += src_xmm.f64[1];
                            break;
                        case 0x59:
                            value.f64[0] *= src_xmm.f64[0];
                            value.f64[1] *= src_xmm.f64[1];
                            break;
                        case 0x5c:
                            value.f64[0] -= src_xmm.f64[0];
                            value.f64[1] -= src_xmm.f64[1];
                            break;
                        case 0x5d:
                            value.f64[0] = value.f64[0] < src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                            value.f64[1] = value.f64[1] < src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                            break;
                        case 0x5e:
                            value.f64[0] /= src_xmm.f64[0];
                            value.f64[1] /= src_xmm.f64[1];
                            break;
                        case 0x5f:
                            value.f64[0] = value.f64[0] > src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                            value.f64[1] = value.f64[1] > src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                            break;
                        }
                    } else {
                        switch (op2) {
                        case 0x58:
                            value.f32[0] += src_xmm.f32[0];
                            value.f32[1] += src_xmm.f32[1];
                            value.f32[2] += src_xmm.f32[2];
                            value.f32[3] += src_xmm.f32[3];
                            break;
                        case 0x59:
                            value.f32[0] *= src_xmm.f32[0];
                            value.f32[1] *= src_xmm.f32[1];
                            value.f32[2] *= src_xmm.f32[2];
                            value.f32[3] *= src_xmm.f32[3];
                            break;
                        case 0x5c:
                            value.f32[0] -= src_xmm.f32[0];
                            value.f32[1] -= src_xmm.f32[1];
                            value.f32[2] -= src_xmm.f32[2];
                            value.f32[3] -= src_xmm.f32[3];
                            break;
                        case 0x5d:
                            value.f32[0] = value.f32[0] < src_xmm.f32[0] ? value.f32[0] : src_xmm.f32[0];
                            value.f32[1] = value.f32[1] < src_xmm.f32[1] ? value.f32[1] : src_xmm.f32[1];
                            value.f32[2] = value.f32[2] < src_xmm.f32[2] ? value.f32[2] : src_xmm.f32[2];
                            value.f32[3] = value.f32[3] < src_xmm.f32[3] ? value.f32[3] : src_xmm.f32[3];
                            break;
                        case 0x5e:
                            value.f32[0] /= src_xmm.f32[0];
                            value.f32[1] /= src_xmm.f32[1];
                            value.f32[2] /= src_xmm.f32[2];
                            value.f32[3] /= src_xmm.f32[3];
                            break;
                        case 0x5f:
                            value.f32[0] = value.f32[0] > src_xmm.f32[0] ? value.f32[0] : src_xmm.f32[0];
                            value.f32[1] = value.f32[1] > src_xmm.f32[1] ? value.f32[1] : src_xmm.f32[1];
                            value.f32[2] = value.f32[2] > src_xmm.f32[2] ? value.f32[2] : src_xmm.f32[2];
                            value.f32[3] = value.f32[3] > src_xmm.f32[3] ? value.f32[3] : src_xmm.f32[3];
                            break;
                        }
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 >= 0x54 && op2 <= 0x57) {
                if (rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x54:
                    value.qw[0] &= src_xmm.qw[0];
                    value.qw[1] &= src_xmm.qw[1];
                    break;
                case 0x55:
                    value.qw[0] = ~value.qw[0] & src_xmm.qw[0];
                    value.qw[1] = ~value.qw[1] & src_xmm.qw[1];
                    break;
                case 0x56:
                    value.qw[0] |= src_xmm.qw[0];
                    value.qw[1] |= src_xmm.qw[1];
                    break;
                case 0x57:
                    value.qw[0] ^= src_xmm.qw[0];
                    value.qw[1] ^= src_xmm.qw[1];
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if ((op2 >= 0x64 && op2 <= 0x66) || (op2 >= 0x74 && op2 <= 0x76)) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x64:
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = (int8_t) value.u8[i] > (int8_t) src_xmm.u8[i] ? 0xff : 0x00;
                    break;
                case 0x65:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = (int16_t) value.u16[i] > (int16_t) src_xmm.u16[i] ? 0xffff : 0x0000;
                    break;
                case 0x66:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = (int32_t) value.u32[i] > (int32_t) src_xmm.u32[i] ? 0xffffffffu : 0;
                    break;
                case 0x74:
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] == src_xmm.u8[i] ? 0xff : 0x00;
                    break;
                case 0x75:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = value.u16[i] == src_xmm.u16[i] ? 0xffff : 0x0000;
                    break;
                case 0x76:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = value.u32[i] == src_xmm.u32[i] ? 0xffffffffu : 0;
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x16) {
                if (rep_mode == AMD64_REPZ) {
                    // movshdup (F3 0F 16): duplicate odd singles [s1,s1,s3,s3].
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                        goto amd64_gpf_restore;
                    vec_movshdup128(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
                } else if (rep_mode != AMD64_REP_NONE) {
                    return INT_UNDEFINED;
                } else {
                    // movlhps (reg form) / movhps / movhpd (mem form).
                    if (operand_size_prefix && modrm.is_reg)
                        return INT_UNDEFINED;
                    value = cpu->xmm[modrm.reg];
                    if (modrm.is_reg) {
                        value.qw[1] = cpu->xmm[modrm.rm].qw[0];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[1] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                }
            } else if (op2 == 0x17) {
                // movhps (NP) / movhpd (66) m64, xmm: both store xmm[127:64], so
                // accept the 66 (movhpd) form too (was wrongly #UD'd). reg #UD.
                if (modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[1]))
                    goto amd64_gpf_restore;
            } else if (op2 == 0x7c || op2 == 0x7d || op2 == 0xd0) {
                // SSE3 alternating/horizontal add-sub: F2 -> *ps, 66 -> *pd.
                bool is_ps = (rep_mode == AMD64_REPNZ && !operand_size_prefix);
                bool is_pd = (rep_mode == AMD64_REP_NONE && operand_size_prefix);
                if (!is_ps && !is_pd)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                union xmm_reg *d = &cpu->xmm[modrm.reg];
                if (op2 == 0x7c)
                    is_ps ? vec_haddps128(NULL, &src_xmm, d) : vec_haddpd128(NULL, &src_xmm, d);
                else if (op2 == 0x7d)
                    is_ps ? vec_hsubps128(NULL, &src_xmm, d) : vec_hsubpd128(NULL, &src_xmm, d);
                else
                    is_ps ? vec_addsubps128(NULL, &src_xmm, d) : vec_addsubpd128(NULL, &src_xmm, d);
            } else if (op2 == 0xf0) {
                // lddqu (F2 0F F0): unaligned 128-bit load (behaves like movdqu).
                if (rep_mode != AMD64_REPNZ || operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                cpu->xmm[modrm.reg] = src_xmm;
            } else if (op2 == 0x60 || op2 == 0x61 || op2 == 0x62 ||
                       op2 == 0x68 || op2 == 0x69 || op2 == 0x6a ||
                       op2 == 0x6c || op2 == 0x6d) {
                union xmm_reg dst = cpu->xmm[modrm.reg];
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (op2 == 0x60) {
                    value.u8[0] = dst.u8[0];
                    value.u8[1] = src_xmm.u8[0];
                    value.u8[2] = dst.u8[1];
                    value.u8[3] = src_xmm.u8[1];
                    value.u8[4] = dst.u8[2];
                    value.u8[5] = src_xmm.u8[2];
                    value.u8[6] = dst.u8[3];
                    value.u8[7] = src_xmm.u8[3];
                    value.u8[8] = dst.u8[4];
                    value.u8[9] = src_xmm.u8[4];
                    value.u8[10] = dst.u8[5];
                    value.u8[11] = src_xmm.u8[5];
                    value.u8[12] = dst.u8[6];
                    value.u8[13] = src_xmm.u8[6];
                    value.u8[14] = dst.u8[7];
                    value.u8[15] = src_xmm.u8[7];
                } else if (op2 == 0x61) {
                    value.u16[0] = dst.u16[0];
                    value.u16[1] = src_xmm.u16[0];
                    value.u16[2] = dst.u16[1];
                    value.u16[3] = src_xmm.u16[1];
                    value.u16[4] = dst.u16[2];
                    value.u16[5] = src_xmm.u16[2];
                    value.u16[6] = dst.u16[3];
                    value.u16[7] = src_xmm.u16[3];
                } else if (op2 == 0x62) {
                    value.u32[0] = dst.u32[0];
                    value.u32[1] = src_xmm.u32[0];
                    value.u32[2] = dst.u32[1];
                    value.u32[3] = src_xmm.u32[1];
                } else if (op2 == 0x68) {
                    value.u8[0] = dst.u8[8];
                    value.u8[1] = src_xmm.u8[8];
                    value.u8[2] = dst.u8[9];
                    value.u8[3] = src_xmm.u8[9];
                    value.u8[4] = dst.u8[10];
                    value.u8[5] = src_xmm.u8[10];
                    value.u8[6] = dst.u8[11];
                    value.u8[7] = src_xmm.u8[11];
                    value.u8[8] = dst.u8[12];
                    value.u8[9] = src_xmm.u8[12];
                    value.u8[10] = dst.u8[13];
                    value.u8[11] = src_xmm.u8[13];
                    value.u8[12] = dst.u8[14];
                    value.u8[13] = src_xmm.u8[14];
                    value.u8[14] = dst.u8[15];
                    value.u8[15] = src_xmm.u8[15];
                } else if (op2 == 0x69) {
                    value.u16[0] = dst.u16[4];
                    value.u16[1] = src_xmm.u16[4];
                    value.u16[2] = dst.u16[5];
                    value.u16[3] = src_xmm.u16[5];
                    value.u16[4] = dst.u16[6];
                    value.u16[5] = src_xmm.u16[6];
                    value.u16[6] = dst.u16[7];
                    value.u16[7] = src_xmm.u16[7];
                } else if (op2 == 0x6a) {
                    value.u32[0] = dst.u32[2];
                    value.u32[1] = src_xmm.u32[2];
                    value.u32[2] = dst.u32[3];
                    value.u32[3] = src_xmm.u32[3];
                } else if (op2 == 0x6c) {
                    value = dst;
                    value.qw[1] = src_xmm.qw[0];
                } else {
                    value.qw[0] = dst.qw[1];
                    value.qw[1] = src_xmm.qw[1];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x63 || op2 == 0x67 || op2 == 0x6b) {
                union xmm_reg dst = cpu->xmm[modrm.reg];
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (op2 == 0x63) {
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) dst.u16[i];
                        value.u8[i] = word > INT8_MAX ? INT8_MAX : word < INT8_MIN ? INT8_MIN : (int8_t) word;
                    }
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) src_xmm.u16[i];
                        value.u8[8 + i] = word > INT8_MAX ? INT8_MAX : word < INT8_MIN ? INT8_MIN : (int8_t) word;
                    }
                } else if (op2 == 0x67) {
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) dst.u16[i];
                        value.u8[i] = word > UINT8_MAX ? UINT8_MAX : word < 0 ? 0 : (uint8_t) word;
                    }
                    for (int i = 0; i < 8; i++) {
                        int16_t word = (int16_t) src_xmm.u16[i];
                        value.u8[8 + i] = word > UINT8_MAX ? UINT8_MAX : word < 0 ? 0 : (uint8_t) word;
                    }
                } else {
                    for (int i = 0; i < 4; i++) {
                        int32_t dword = (int32_t) dst.u32[i];
                        value.u16[i] = dword > INT16_MAX ? INT16_MAX : dword < INT16_MIN ? INT16_MIN : (int16_t) dword;
                    }
                    for (int i = 0; i < 4; i++) {
                        int32_t dword = (int32_t) src_xmm.u32[i];
                        value.u16[4 + i] = dword > INT16_MAX ? INT16_MAX : dword < INT16_MIN ? INT16_MIN : (int16_t) dword;
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x7e) {
                if (rep_mode == AMD64_REPZ && !operand_size_prefix) {
                    value.u128 = 0;
                    if (modrm.is_reg) {
                        value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        value.qw[0] = src_scalar;
                    }
                    cpu->xmm[modrm.reg] = value;
                } else if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                    qword_t scalar = rex.w ? cpu->xmm[modrm.reg].qw[0]
                                           : cpu->xmm[modrm.reg].u32[0];
                    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, scalar))
                        goto amd64_gpf_restore;
                } else if (rep_mode == AMD64_REP_NONE && !operand_size_prefix) {
                    // 0F 7E (no prefix): movd/movq r/m, mm — MMX store to a GPR
                    // or memory (movd = 32-bit, movq with REX.W = 64-bit). This
                    // is the sibling of the 0F 7F MMX store above; the JIT bridge
                    // (amd64_jit_0f_vec_rm 0x7e) already handles it, so mirror it
                    // here for the interpreter fallback. reg is an MMX index <8.
                    if (modrm.reg >= 8)
                        return INT_UNDEFINED;
                    qword_t scalar = rex.w ? cpu->mm[modrm.reg].qw
                                           : (uint32_t) cpu->mm[modrm.reg].qw;
                    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, scalar))
                        goto amd64_gpf_restore;
                } else {
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0x70) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                if (operand_size_prefix) {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = src_xmm.u32[(imm8 >> (i * 2)) & 3];
                } else if (rep_mode == AMD64_REPNZ) {
                    value = src_xmm;
                    for (int i = 0; i < 4; i++)
                        value.u16[i] = src_xmm.u16[(imm8 >> (i * 2)) & 3];
                } else if (rep_mode == AMD64_REPZ) {
                    value = src_xmm;
                    for (int i = 0; i < 4; i++)
                        value.u16[4 + i] = src_xmm.u16[4 + ((imm8 >> (i * 2)) & 3)];
                } else {
                    return INT_UNDEFINED;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc2) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                imm8 &= 7;
                if (rep_mode == AMD64_REPNZ) {
                    vec_single_fcmp64(cpu, &src_xmm.f64[0], &value, imm8);
                } else if (rep_mode == AMD64_REPZ) {
                    vec_single_fcmp32(cpu, &src_xmm.f32[0], &value, imm8);
                } else if (operand_size_prefix) {
                    vec_fcmp_p64(cpu, &src_xmm, &value, imm8);
                } else {
                    for (int i = 0; i < 4; i++) {
                        float lhs = value.f32[i];
                        float rhs = src_xmm.f32[i];
                        switch (imm8) {
                        case 0:
                            value.u32[i] = lhs == rhs ? 0xffffffffu : 0;
                            break;
                        case 1:
                            value.u32[i] = lhs < rhs ? 0xffffffffu : 0;
                            break;
                        case 2:
                            value.u32[i] = lhs <= rhs ? 0xffffffffu : 0;
                            break;
                        case 3:
                            value.u32[i] = isnan(lhs) || isnan(rhs) ? 0xffffffffu : 0;
                            break;
                        case 4:
                            value.u32[i] = lhs != rhs ? 0xffffffffu : 0;
                            break;
                        case 5:
                            value.u32[i] = !(lhs < rhs) ? 0xffffffffu : 0;
                            break;
                        case 6:
                            value.u32[i] = !(lhs <= rhs) ? 0xffffffffu : 0;
                            break;
                        case 7:
                            value.u32[i] = !(isnan(lhs) || isnan(rhs)) ? 0xffffffffu : 0;
                            break;
                        }
                    }
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc4) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 16, &src_scalar))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.u16[imm8 & 7] = (uint16_t) src_scalar;
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xc5) {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                amd64_reg_set(cpu, modrm.reg, 32, src_xmm.u16[imm8 & 7]);
            } else if (op2 == 0xc6) {
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (operand_size_prefix) {
                    value.qw[0] = cpu->xmm[modrm.reg].qw[(imm8 >> 0) & 1];
                    value.qw[1] = src_xmm.qw[(imm8 >> 1) & 1];
                } else {
                    value.u32[0] = cpu->xmm[modrm.reg].u32[(imm8 >> 0) & 3];
                    value.u32[1] = cpu->xmm[modrm.reg].u32[(imm8 >> 2) & 3];
                    value.u32[2] = src_xmm.u32[(imm8 >> 4) & 3];
                    value.u32[3] = src_xmm.u32[(imm8 >> 6) & 3];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xd6) {
                if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                    // 66 0F D6: movq xmm/m64, xmm (store low qword)
                    if (modrm.is_reg)
                        return INT_UNDEFINED;
                    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                        goto amd64_gpf_restore;
                } else if (rep_mode == AMD64_REPZ && !operand_size_prefix &&
                           modrm.is_reg && modrm.rm < 8) {
                    // F3 0F D6: movq2dq xmm, mm — copy the 64-bit MMX register
                    // into the low qword of the XMM register, zero the upper
                    // qword. Register-only (gpgv SHA shuttles MMX<->XMM here).
                    cpu->xmm[modrm.reg].qw[0] = cpu->mm[modrm.rm].qw;
                    cpu->xmm[modrm.reg].qw[1] = 0;
                } else if (rep_mode == AMD64_REPNZ && !operand_size_prefix &&
                           modrm.is_reg && modrm.reg < 8) {
                    // F2 0F D6: movdq2q mm, xmm — copy the low qword of the XMM
                    // register into the MMX register. Register-only.
                    cpu->mm[modrm.reg].qw = cpu->xmm[modrm.rm].qw[0];
                } else {
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0xd7) {
                uint32_t mask = 0;
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                for (int i = 0; i < 16; i++)
                    mask |= ((src_xmm.u8[i] >> 7) & 1u) << i;
                amd64_reg_set(cpu, modrm.reg, 32, mask);
            } else if (op2 == 0xd4) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] += src_xmm.qw[0];
                value.qw[1] += src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf4) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] = (uint64_t) value.u32[0] * src_xmm.u32[0];
                value.qw[1] = (uint64_t) value.u32[2] * src_xmm.u32[2];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xfc || op2 == 0xfd || op2 == 0xfe) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xfc) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] += src_xmm.u8[i];
                } else if (op2 == 0xfd) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] += src_xmm.u16[i];
                } else {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] += src_xmm.u32[i];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf6) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value.u128 = 0;
                for (int lane = 0; lane < 2; lane++) {
                    uint16_t sum = 0;
                    for (int i = 0; i < 8; i++) {
                        unsigned idx = lane * 8 + i;
                        uint8_t lhs = cpu->xmm[modrm.reg].u8[idx];
                        uint8_t rhs = src_xmm.u8[idx];
                        sum += lhs > rhs ? (uint16_t) (lhs - rhs) : (uint16_t) (rhs - lhs);
                    }
                    value.u16[lane * 4] = sum;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xf8 || op2 == 0xf9 || op2 == 0xfa || op2 == 0xfb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xf8) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] -= src_xmm.u8[i];
                } else if (op2 == 0xf9) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] -= src_xmm.u16[i];
                } else if (op2 == 0xfa) {
                    for (int i = 0; i < 4; i++)
                        value.u32[i] -= src_xmm.u32[i];
                } else {
                    value.qw[0] -= src_xmm.qw[0];
                    value.qw[1] -= src_xmm.qw[1];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xd8 || op2 == 0xd9 || op2 == 0xdc || op2 == 0xdd || op2 == 0xde) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xd8) {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] > src_xmm.u8[i] ? (uint8_t) (value.u8[i] - src_xmm.u8[i]) : 0;
                } else if (op2 == 0xd9) {
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = value.u16[i] > src_xmm.u16[i] ? (uint16_t) (value.u16[i] - src_xmm.u16[i]) : 0;
                } else if (op2 == 0xdc) {
                    for (int i = 0; i < 16; i++) {
                        uint16_t sum = (uint16_t) value.u8[i] + (uint16_t) src_xmm.u8[i];
                        value.u8[i] = sum > 0xff ? 0xff : (uint8_t) sum;
                    }
                } else if (op2 == 0xdd) {
                    for (int i = 0; i < 8; i++) {
                        uint32_t sum = (uint32_t) value.u16[i] + (uint32_t) src_xmm.u16[i];
                        value.u16[i] = sum > 0xffff ? 0xffff : (uint16_t) sum;
                    }
                } else {
                    for (int i = 0; i < 16; i++)
                        value.u8[i] = value.u8[i] > src_xmm.u8[i] ? value.u8[i] : src_xmm.u8[i];
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xda) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                for (int i = 0; i < 16; i++)
                    value.u8[i] = value.u8[i] < src_xmm.u8[i] ? value.u8[i] : src_xmm.u8[i];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xdb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] &= src_xmm.qw[0];
                value.qw[1] &= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xdf) {
                if (operand_size_prefix) {
                    // 66 0F DF: pandn xmm (existing).
                    if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                        goto amd64_gpf_restore;
                    value = cpu->xmm[modrm.reg];
                    value.qw[0] = ~value.qw[0] & src_xmm.qw[0];
                    value.qw[1] = ~value.qw[1] & src_xmm.qw[1];
                    cpu->xmm[modrm.reg] = value;
                } else if (rep_mode == AMD64_REP_NONE) {
                    // 0F DF (no prefix): pandn mm — MMX (dst = ~dst & src). The
                    // JIT bridge handles this; mirror it for the interpreter
                    // fallback. mm[] has 8 entries, so guard the index <8.
                    if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
                        return INT_UNDEFINED;
                    union mm_reg src_mm, dst_mm;
                    if (modrm.is_reg) {
                        src_mm = cpu->mm[modrm.rm];
                    } else {
                        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                            goto amd64_gpf_restore;
                        src_mm.qw = src_scalar;
                    }
                    dst_mm = cpu->mm[modrm.reg];
                    vec_andn64(NULL, &src_mm, &dst_mm);
                    cpu->mm[modrm.reg] = dst_mm;
                } else {
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0xeb) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] |= src_xmm.qw[0];
                value.qw[1] |= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0xef) {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[0] ^= src_xmm.qw[0];
                value.qw[1] ^= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else {
                if (!operand_size_prefix)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_gpf_restore;
                value = cpu->xmm[modrm.reg];
                value.qw[1] = src_xmm.qw[0];
                cpu->xmm[modrm.reg] = value;
            }
            break;
        }
        if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
            struct amd64_modrm modrm;
            union xmm_reg value;
            uint8_t imm8;
            unsigned count;
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!modrm.is_reg || modrm.rm >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = cpu->xmm[modrm.rm];
            if (op2 == 0x71) {
                count = imm8 > 15 ? 15 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? 0 : (value.u16[i] >> count);
                    break;
                case 4:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? ((int16_t) value.u16[i] < 0 ? UINT16_MAX : 0)
                                                 : (uint16_t) (((int16_t) value.u16[i]) >> count);
                    break;
                case 6:
                    for (int i = 0; i < 8; i++)
                        value.u16[i] = imm8 > 15 ? 0 : (uint16_t) (value.u16[i] << count);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0x72) {
                count = imm8 > 31 ? 31 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? 0 : (value.u32[i] >> count);
                    break;
                case 4:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? ((int32_t) value.u32[i] < 0 ? UINT32_MAX : 0)
                                                 : (uint32_t) (((int32_t) value.u32[i]) >> count);
                    break;
                case 6:
                    for (int i = 0; i < 4; i++)
                        value.u32[i] = imm8 > 31 ? 0 : (value.u32[i] << count);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else {
                count = imm8 > 63 ? 63 : imm8;
                switch (modrm.reg) {
                case 2:
                    for (int i = 0; i < 2; i++)
                        value.qw[i] = imm8 > 63 ? 0 : (value.qw[i] >> count);
                    break;
                case 3:
                    if (imm8 >= 16)
                        value.u128 = 0;
                    else
                        value.u128 >>= imm8 * 8;
                    break;
                case 6:
                    for (int i = 0; i < 2; i++)
                        value.qw[i] = imm8 > 63 ? 0 : (value.qw[i] << count);
                    break;
                case 7:
                    if (imm8 >= 16)
                        value.u128 = 0;
                    else
                        value.u128 <<= imm8 * 8;
                    break;
                default:
                    return INT_UNDEFINED;
                }
            }
            cpu->xmm[modrm.rm] = value;
            break;
        }
        if (op2 == 0xa3) {
            struct amd64_modrm modrm;
            qword_t lhs;
            qword_t addr;
            qword_t bit;
            qword_t bit_index;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                    bit_index, true, true, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            (void) addr;
            collapse_flags(cpu);
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, bit_index, bit, addr, lhs, lhs);
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
            struct amd64_modrm modrm;
            qword_t addr;
            qword_t lhs, result;
            qword_t bit;
            qword_t bit_index;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                    bit_index, true, true, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            collapse_flags(cpu);
            // LOCK BTS/BTR/BTC on memory: one host-atomic RMW on the addressed
            // word. The read/modify/write below is not atomic against anything,
            // and these are exactly the instructions a bitmap lock is built on.
            if (lock_prefix && !modrm.is_reg) {
                unsigned bop = op2 == 0xab ? 0 : (op2 == 0xb3 ? 1 : 2);
                if (!amd64_locked_bitop(cpu, tlb, addr, op_size, bop,
                            1ull << bit, &lhs))
                    goto amd64_gpf_restore;
                cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
                cpu->cf_bit = cpu->cf;
                break;
            }
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            result = lhs;
            switch (op2) {
            case 0xab:
                result = amd64_trunc(lhs | (1ull << bit), op_size);
                break;
            case 0xb3:
                result = amd64_trunc(lhs & ~(1ull << bit), op_size);
                break;
            case 0xbb:
                result = amd64_trunc(lhs ^ (1ull << bit), op_size);
                break;
            }
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, bit_index, bit, addr, lhs, result);
            if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr, result);
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xba) {
            struct amd64_modrm modrm;
            qword_t addr;
            qword_t lhs, result;
            qword_t bit;
            uint8_t imm8;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg < 4 || modrm.reg > 7)
                return INT_UNDEFINED;
            if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, imm8,
                    true, false, &lhs, &addr, &bit))
                goto amd64_gpf_restore;
            collapse_flags(cpu);
            // /4 is BT, which writes nothing and so is never locked.
            if (lock_prefix && !modrm.is_reg && modrm.reg != 4) {
                if (!amd64_locked_bitop(cpu, tlb, addr, op_size, modrm.reg - 5,
                            1ull << bit, &lhs))
                    goto amd64_gpf_restore;
                cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
                cpu->cf_bit = cpu->cf;
                break;
            }
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            result = lhs;
            switch (modrm.reg) {
            case 4:
                break;
            case 5:
                result = amd64_trunc(lhs | (1ull << bit), op_size);
                break;
            case 6:
                result = amd64_trunc(lhs & ~(1ull << bit), op_size);
                break;
            case 7:
                result = amd64_trunc(lhs ^ (1ull << bit), op_size);
                break;
            }
            amd64_trace_as_bt(cpu, op2, op_size, !modrm.is_reg, imm8, bit, addr, lhs, result);
            if (modrm.reg != 4) {
                if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                    goto amd64_gpf_restore;
                if (!modrm.is_reg && op_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr, result);
            }
            cpu->cf_bit = cpu->cf;
            break;
        }
        if (op2 == 0xaf) {
            struct amd64_modrm modrm;
            qword_t rhs, lhs, result;
            __int128_t full;
            bool overflow;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            // 128-bit product so 64-bit signed overflow is detectable; a 64-bit
            // product truncates, leaving CF/OF always clear.
            full = (__int128_t) (sqword_t) amd64_sign_extend(lhs, op_size) *
                   (__int128_t) (sqword_t) amd64_sign_extend(rhs, op_size);
            result = amd64_trunc((qword_t) full, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            overflow = full != (__int128_t) (sqword_t) amd64_sign_extend(result, op_size);
            amd64_set_mul_flags(cpu, overflow);
            break;
        }
        if (op2 == 0xc0 || op2 == 0xc1) {
            struct amd64_modrm modrm;
            unsigned xadd_size = op2 == 0xc0 ? 8 : op_size;
            qword_t lhs, rhs, result;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            atomic_locked = lock_prefix && !modrm.is_reg;
            rhs = op2 == 0xc0
                    ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                    : amd64_reg_get(cpu, modrm.reg, xadd_size);
            if (atomic_locked) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (!amd64_locked_xadd(cpu, tlb, addr, xadd_size, rhs, &lhs, &result))
                    goto amd64_gpf_restore;
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, &lhs))
                    goto amd64_gpf_restore;
                result = amd64_trunc(lhs + rhs, xadd_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, result))
                    goto amd64_gpf_restore;
            }
            if (op2 == 0xc0)
                amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
            else
                amd64_reg_set(cpu, modrm.reg, xadd_size, lhs);
            amd64_set_add_flags(cpu, lhs, rhs, result, xadd_size);
            break;
        }
        if (op2 == 0xb0 || op2 == 0xb1) {
            struct amd64_modrm modrm;
            qword_t dst, src, acc, result;
            unsigned cmpxchg_size = op2 == 0xb0 ? 8 : op_size;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            atomic_locked = lock_prefix && !modrm.is_reg;
            src = op2 == 0xb0
                    ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                    : amd64_reg_get(cpu, modrm.reg, cmpxchg_size);
            acc = amd64_reg_get(cpu, amd64_rax, cmpxchg_size);
            if (atomic_locked) {
                // One host compare-exchange. The read/compare/write it replaces
                // had a genuine ABA window even under the global lock's weaker
                // guarantee: nothing stopped a host-side atomic writing between
                // the compare and the store.
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                bool swapped = false;
                if (!amd64_locked_cmpxchg(cpu, tlb, addr, cmpxchg_size, acc, src,
                            &dst, &swapped))
                    goto amd64_gpf_restore;
                result = amd64_trunc(acc - dst, cmpxchg_size);
                amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
                if (swapped) {
                    if (cmpxchg_size == 64)
                        amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr, src);
                    cpu->zf = 1;
                } else {
                    amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
                    cpu->zf = 0;
                }
                cpu->zf_res = 0;
                break;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, &dst))
                goto amd64_gpf_restore;
            result = amd64_trunc(acc - dst, cmpxchg_size);
            amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
            if (acc == dst) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, src))
                    goto amd64_gpf_restore;
                if (!modrm.is_reg && cmpxchg_size == 64)
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, amd64_effective_addr(cpu, &modrm, fs_prefix), src);
                cpu->zf = 1;
                cpu->zf_res = 0;
            } else {
                amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
                cpu->zf = 0;
                cpu->zf_res = 0;
            }
            break;
        }
        if (op2 == 0xc7) {
            struct amd64_modrm modrm;
            qword_t dst, expected, desired;
            bool atomic_locked = false;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg != 1 || modrm.is_reg)
                return INT_UNDEFINED;

            atomic_locked = lock_prefix;
            if (rex.w) {
                // CMPXCHG16B: 128-bit compare-exchange. Compare RDX:RAX with the
                // 16-byte memory operand; on equal store RCX:RBX (ZF=1), else
                // reload RDX:RAX from memory (ZF=0). The operand must be 16-byte
                // aligned or the instruction raises #GP. (Without REX.W this is
                // CMPXCHG8B -- the 64-bit path below.)
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (addr & 0xf) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = addr;
                    return INT_GPF;
                }
                // The 16-byte alignment enforced just above is also what lets
                // this be a single host 128-bit compare-exchange.
                qword_t exp128[2] = {
                    amd64_reg_get(cpu, amd64_rax, 64),
                    amd64_reg_get(cpu, amd64_rdx, 64),
                };
                qword_t des128[2] = {
                    amd64_reg_get(cpu, amd64_rbx, 64),
                    amd64_reg_get(cpu, amd64_rcx, 64),
                };
                qword_t mem128[2];  // [0] = low qword, [1] = high qword
                bool eq = false;
                guest_addr_t checked_addr;
                if (!amd64_atomic_addr_ok(cpu, tlb, addr, 128, &checked_addr))
                    goto amd64_gpf_restore;
                if (x86_atomic_cas16b(cpu, tlb, checked_addr, exp128, des128,
                            mem128, &eq) != 0) {
                    amd64_atomic_faulted(cpu, checked_addr);
                    goto amd64_gpf_restore;
                }
                collapse_flags(cpu);
                cpu->zf = eq;
                cpu->zf_res = 0;
                if (!eq) {
                    amd64_reg_set(cpu, amd64_rax, 64, mem128[0]);
                    amd64_reg_set(cpu, amd64_rdx, 64, mem128[1]);
                }
                break;
            }
            expected = ((qword_t) (dword_t) amd64_reg_get(cpu, amd64_rdx, 32) << 32) |
                    (dword_t) amd64_reg_get(cpu, amd64_rax, 32);
            desired = ((qword_t) (dword_t) amd64_reg_get(cpu, amd64_rcx, 32) << 32) |
                    (dword_t) amd64_reg_get(cpu, amd64_rbx, 32);
            if (atomic_locked) {
                qword_t addr8 = amd64_effective_addr(cpu, &modrm, fs_prefix);
                bool swapped = false;
                if (!amd64_locked_cmpxchg(cpu, tlb, addr8, 64, expected, desired,
                            &dst, &swapped))
                    goto amd64_gpf_restore;
                collapse_flags(cpu);
                cpu->zf = swapped;
                cpu->zf_res = 0;
                if (swapped) {
                    amd64_trace_qword_store(cpu, saved_rip, 0x0f, addr8, desired);
                } else {
                    amd64_reg_set(cpu, amd64_rax, 32, (dword_t) dst);
                    amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (dst >> 32));
                }
                break;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &dst))
                goto amd64_gpf_restore;
            collapse_flags(cpu);
            cpu->zf = expected == dst;
            cpu->zf_res = 0;
            if (expected == dst) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, desired))
                    goto amd64_gpf_restore;
                amd64_trace_qword_store(cpu, saved_rip, 0x0f,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), desired);
            } else {
                amd64_reg_set(cpu, amd64_rax, 32, (dword_t) dst);
                amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (dst >> 32));
            }
            break;
        }
        if (op2 >= 0xc8 && op2 <= 0xcf) {
            unsigned reg = (op2 - 0xc8) | (rex.b ? 8 : 0);
            if (rex.w) {
                qword_t value = amd64_reg_get(cpu, reg, 64);
                amd64_reg_set(cpu, reg, 64, __builtin_bswap64(value));
            } else {
                dword_t value = (dword_t) amd64_reg_get(cpu, reg, 32);
                amd64_reg_set(cpu, reg, 32, __builtin_bswap32(value));
            }
            break;
        }
        if (op2 == 0x1f) {
            struct amd64_modrm modrm;
            if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (modrm.reg != 0)
                return INT_UNDEFINED;
            break;
        }
        if (op2 == 0x77) {
            // emms (0F 77): empties the x87 FPU tag word. This emulator models
            // no x87 tag state that gates MMX register access (the i386 decoder
            // likewise treats emms as ignored), so it is a no-op. No modrm or
            // operands, so rip is already past the opcode — just continue.
            // Without this an MMX routine running under (or falling back to) the
            // interpreter SIGILLs on the trailing emms.
            break;
        }
        if (op2 == 0x0b)
            return INT_UNDEFINED;
        return INT_UNDEFINED;
    }
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
        return amd64_handle_x87(cpu, tlb, saved_rip, rex, fs_prefix, opcode);
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x08:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x03:
    case 0x13:
    case 0x1b:
    case 0x23:
    case 0x28:
    case 0x2a:
    case 0x2b:
    case 0x29:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x39:
    case 0x3b:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8d:
    case 0x63:
    case 0x69:
    case 0x6b: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        // LOCK <alu> [mem], reg -- the 00/08/10/18/20/28/30 byte forms and
        // their 01/09/... word forms, which are the only ones in this switch
        // that both write memory and accept a LOCK prefix. Handled here rather
        // than in each case below because the atomic form is one operation,
        // not the read/compute/write pair the unlocked cases are.
        //
        // These used to fall through to the plain cases and lose updates
        // outright: unlike xadd/cmpxchg/inc/dec nothing here ever took
        // atomic_l_lock, so `lock addl %reg, (mem)` dropped 3876 of 200000
        // increments across four guest threads. (LOCK with a register
        // destination, and on the reg-destination 02/03/... directions, is #UD
        // on hardware; the JIT rejects it and the unlocked path below is
        // reached only without the prefix.)
        if (lock_prefix && !modrm.is_reg &&
                (opcode & 7) <= 1 && opcode < 0x38) {
            unsigned alu_size = (opcode & 1) == 0 ? 8 : op_size;
            unsigned alu_op = (opcode >> 3) & 7;
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            rhs = alu_size == 8
                ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                : amd64_reg_get(cpu, modrm.reg, alu_size);
            if (!amd64_locked_alu(cpu, tlb, addr, alu_size, alu_op, rhs))
                goto amd64_gpf_restore;
            break;
        }
        switch (opcode) {
        case 0x00:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x01:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x02:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x08:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs | rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x10: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs + carry_in, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x11: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs + carry_in, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x12: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs + rhs + carry_in, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x18: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs - carry_in, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x19: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs - carry_in, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x1a: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs - carry_in, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, 8);
            break;
        }
        case 0x20:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs & rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x21:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs & rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x22:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs & rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x09:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs | rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x0a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs | rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x0b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs | rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x03:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x13: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs + rhs + carry_in, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x1b: {
            unsigned carry_in = cpu->cf;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs - carry_in, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, op_size);
            break;
        }
        case 0x23:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs & rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x28:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x2b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x29:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x2a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs - rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, 8);
            break;
        case 0x30:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs ^ rhs, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x31:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs ^ rhs, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x32:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            result = amd64_trunc(lhs ^ rhs, 8);
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, result);
            amd64_set_logic_flags(cpu, result, 8);
            break;
        case 0x33:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs ^ rhs, op_size);
            amd64_reg_set(cpu, modrm.reg, op_size, result);
            amd64_set_logic_flags(cpu, result, op_size);
            break;
        case 0x39:
            if (amd64_verbose_boot_trace_enabled() &&
                    saved_rip == AMD64_BUSYBOX_INIT_CMP_RIP && !modrm.is_reg) {
                amd64_busybox_watch_addr(amd64_reg_get(cpu, amd64_rax, 64));
                printk("amd64 init cmp: rip=%#llx rax=%#llx rbx=%#llx addr=%#llx\n",
                       (unsigned long long) saved_rip,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbx, 64),
                       (unsigned long long) amd64_effective_addr(cpu, &modrm, fs_prefix));
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            if (!modrm.is_reg) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                amd64_trace_cc1_cmp_probe(cpu, saved_rip, addr, lhs, rhs, result, op_size);
            }
            break;
        case 0x3b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get(cpu, modrm.reg, op_size);
            result = amd64_trunc(lhs - rhs, op_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, op_size);
            break;
        case 0x85:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            amd64_set_logic_flags(cpu, lhs & rhs, op_size);
            if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_TEST_RIP) {
                printk("amd64 init test: rip=%#llx lhs=%#llx rhs=%#llx zf=%d rax=%#llx\n",
                       (unsigned long long) saved_rip,
                       (unsigned long long) lhs,
                       (unsigned long long) rhs,
                       cpu->zf,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64));
            }
            break;
        case 0x86:
        case 0x87: {
            unsigned xchg_size = opcode == 0x86 ? 8 : op_size;
            rhs = opcode == 0x86 ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                                 : amd64_reg_get(cpu, modrm.reg, xchg_size);
            if (!modrm.is_reg) {
                // XCHG with a memory operand is atomic whether or not a LOCK
                // prefix is present -- it is the store half of every spinlock.
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (!amd64_locked_xchg(cpu, tlb, addr, xchg_size, rhs, &lhs))
                    goto amd64_gpf_restore;
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, &lhs))
                    goto amd64_gpf_restore;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xchg_size, rhs))
                    goto amd64_gpf_restore;
            }
            if (opcode == 0x86)
                amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
            else
                amd64_reg_set(cpu, modrm.reg, xchg_size, lhs);
            break;
        }
        case 0x88:
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, rhs))
                goto amd64_gpf_restore;
            break;
        case 0x89:
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, opcode,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            break;
        case 0x8a:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, rhs);
            break;
        case 0x8b:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_cc1_slot_probe(cpu, saved_rip, amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            amd64_reg_set(cpu, modrm.reg, op_size, rhs);
            if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_LOAD_RIP) {
                amd64_busybox_watch_addr(rhs);
                printk("amd64 init load: rip=%#llx dst=%u value=%#llx rax=%#llx\n",
                       (unsigned long long) saved_rip,
                       modrm.reg,
                       (unsigned long long) rhs,
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64));
            }
            break;
        case 0x8d:
            if (modrm.is_reg)
                return INT_UNDEFINED;
            amd64_reg_set(cpu, modrm.reg, op_size, amd64_effective_addr(cpu, &modrm, false));
            break;
        case 0x63:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 32 : op_size, &rhs))
                goto amd64_gpf_restore;
            amd64_reg_set(cpu, modrm.reg, op_size,
                    (qword_t) amd64_sign_extend(rhs, rex.w ? 32 : op_size));
            break;
        case 0x69:
        case 0x6b: {
            sqword_t src_signed;
            sqword_t imm_signed;
            // Fetch the immediate before touching the r/m operand: a
            // RIP-relative operand resolves against the END of the
            // instruction, and the fetch cursor only reaches it once the
            // immediate has been consumed.
            if (opcode == 0x69) {
                if (op_size == 16) {
                    int16_t imm16;
                    if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    imm_signed = imm16;
                } else {
                    int32_t imm32;
                    if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    imm_signed = imm32;
                }
            } else {
                int8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                imm_signed = imm8;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
                goto amd64_gpf_restore;
            src_signed = amd64_sign_extend(rhs, op_size);
            if (op_size == 64) {
                __int128_t full = (__int128_t) src_signed * (__int128_t) imm_signed;
                result = (qword_t) full;
                amd64_reg_set(cpu, modrm.reg, op_size, result);
                amd64_set_mul_flags(cpu, full != (__int128_t) (sqword_t) (uint64_t) result);
            } else {
                int64_t full = (int64_t) src_signed * (int64_t) imm_signed;
                result = amd64_trunc((qword_t) full, op_size);
                amd64_reg_set(cpu, modrm.reg, op_size, result);
                amd64_set_mul_flags(cpu, full != (int64_t) amd64_sign_extend(result, op_size));
            }
            break;
        }
        }
        break;
    }
    case 0x50 ... 0x57: {
        unsigned reg = (opcode - 0x50) | (rex.b ? 8 : 0);
        unsigned push_size = operand_size_prefix ? 16 : 64;
        qword_t value = amd64_reg_get(cpu, reg, push_size);
        if (!amd64_push_size(cpu, tlb, push_size, value))
            goto amd64_gpf_restore;
        break;
    }
    case 0x58 ... 0x5f: {
        unsigned reg = (opcode - 0x58) | (rex.b ? 8 : 0);
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, reg, pop_size, value);
        break;
    }
    case 0x9c: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        collapse_flags(cpu);
        if (!amd64_push_size(cpu, tlb, push_size, cpu->eflags))
            goto amd64_gpf_restore;
        break;
    }
    case 0x9d: {
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        cpu->eflags = (cpu->eflags & ~0xcd5u) | ((dword_t) value & 0xcd5u);
        expand_flags(cpu);
        break;
    }
    case 0x9e: { // sahf: SF,ZF,AF,PF,CF <- AH; OF and the rest preserved.
        byte_t ah = (cpu->amd64_regs[amd64_rax] >> 8) & 0xff;
        cpu->cf = ah & 1;
        cpu->pf = (ah >> 2) & 1;
        cpu->af = (ah >> 4) & 1;
        cpu->zf = (ah >> 6) & 1;
        cpu->sf = (ah >> 7) & 1;
        cpu->zf_res = cpu->sf_res = cpu->pf_res = cpu->af_ops = 0;
        break;
    }
    case 0x9f: { // lahf: AH <- SF:ZF:0:AF:0:PF:1:CF (the low byte of EFLAGS).
        byte_t ah = (SF << 7) | (ZF << 6) | (AF << 4) | (PF << 2) | (1 << 1) | (CF << 0);
        cpu->amd64_regs[amd64_rax] =
            (cpu->amd64_regs[amd64_rax] & ~0xff00ULL) | ((qword_t) ah << 8);
        break;
    }
    case 0x8f: {
        struct amd64_modrm modrm;
        qword_t value;
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg != 0)
            return INT_UNDEFINED;
        // See amd64_jit_pop_rm's identical fix (#487): amd64_pop_size() commits
        // the RSP advance on a successful read, before the destination write
        // below is attempted. If that write then faults, restore RSP here too
        // -- otherwise the re-executed instruction pops from the wrong (already
        // advanced) stack slot instead of retrying the original one.
        {
            qword_t rsp_before_pop = cpu->amd64_regs[amd64_rsp];
            if (!amd64_pop_size(cpu, tlb, pop_size, &value))
                goto amd64_gpf_restore;
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, pop_size, value)) {
                cpu->amd64_regs[amd64_rsp] = rsp_before_pop;
                goto amd64_gpf_restore;
            }
        }
        break;
    }
    case 0x68: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        if (push_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            value = (qword_t) (sqword_t) imm32;
        }
        if (!amd64_push_size(cpu, tlb, push_size, value))
            goto amd64_gpf_restore;
        break;
    }
    case 0x6a: {
        unsigned push_size = operand_size_prefix ? 16 : 64;
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_push_size(cpu, tlb, push_size, (qword_t) amd64_sign_extend((uint8_t) imm8, 8)))
            goto amd64_gpf_restore;
        break;
    }
    case 0x84: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
            goto amd64_gpf_restore;
        rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        amd64_set_logic_flags(cpu, lhs & rhs, 8);
        break;
    }
    case 0x38:
    case 0x3a: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (opcode == 0x38) {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            rhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        } else {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &rhs))
                goto amd64_gpf_restore;
            lhs = amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present);
        }
        amd64_set_sub_flags(cpu, lhs, rhs, amd64_trunc(lhs - rhs, 8), 8);
        break;
    }
    case 0xf6:
    case 0xf7: {
        struct amd64_modrm modrm;
        unsigned size = opcode == 0xf6 ? 8 : op_size;
        int result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg == 0) {
            qword_t lhs, rhs;
            if (opcode == 0xf6) {
                uint8_t imm8;
                if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                    cpu->amd64_rip = saved_rip;
                    cpu->segfault_addr = saved_rip;
                    return INT_GPF;
                }
                rhs = imm8;
            } else {
                if (size == 16) {
                    uint16_t imm16;
                    if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    rhs = imm16;
                } else {
                    int32_t imm32;
                    if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                        cpu->amd64_rip = saved_rip;
                        cpu->segfault_addr = saved_rip;
                        return INT_GPF;
                    }
                    rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
                }
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, lhs & rhs, size);
            break;
        }
        // LOCK NOT (/2) / LOCK NEG (/3) on memory: the only two group-3
        // members that write their operand, and so the only ones a LOCK
        // prefix is legal on. Nothing here was ever atomic.
        if (lock_prefix && !modrm.is_reg &&
                (modrm.reg == 2 || modrm.reg == 3)) {
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            qword_t old_val, new_val;
            if (!amd64_locked_negnot(cpu, tlb, addr, size, modrm.reg == 3,
                        &old_val, &new_val))
                goto amd64_gpf_restore;
            if (modrm.reg == 3)
                amd64_set_sub_flags(cpu, 0, old_val, new_val, size);
            break;
        }
        result = amd64_grp3_muldiv(cpu, tlb, &modrm, fs_prefix, size);
        if (result == INT_PF)
            goto amd64_gpf_restore;
        if (result != INT_NONE)
            return result;
        break;
    }
    case 0x70 ... 0x7f: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        bool taken = amd64_cond_eval(cpu, opcode & 0xf);
        if (amd64_as_alu_stderr_enabled() &&
                current != NULL &&
                current->abi == GUEST_ABI_AMD64 &&
                strcmp(current->comm, "as") == 0) {
            fprintf(stderr,
                    "amd64 as jcc8: rip=%#llx cc=%u taken=%u target=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    opcode & 0xf,
                    taken,
                    (unsigned long long) (cpu->amd64_rip + rel8),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        amd64_trace_cc1_va_list_branch_probe(cpu, saved_rip, taken, cpu->amd64_rip + rel8, "jcc");
        if (amd64_verbose_boot_trace_enabled() && saved_rip == AMD64_BUSYBOX_INIT_JNE_RIP) {
            printk("amd64 init jne: rip=%#llx zf=%d taken=%d rax=%#llx target=%#llx\n",
                   (unsigned long long) saved_rip,
                   cpu->zf,
                   taken,
                   (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                   (unsigned long long) (cpu->amd64_rip + rel8));
        }
        if (taken) {
            qword_t target = cpu->amd64_rip + rel8;
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcc");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jcc");
            cpu->amd64_rip = target;
        }
        break;
    }
    case 0x80:
    case 0xc0:
    case 0x81:
    case 0x83:
    case 0xc1:
    case 0xc6:
    case 0xc7: {
        struct amd64_modrm modrm;
        qword_t lhs, rhs, result;
        unsigned rm_size = (opcode == 0x80 || opcode == 0xc0) ? 8 : op_size;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if ((opcode == 0xc6 || opcode == 0xc7) && modrm.reg != 0)
            return INT_UNDEFINED;
        if (opcode == 0xc6) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, imm8))
                goto amd64_gpf_restore;
            break;
        }
        if (opcode == 0xc0 || opcode == 0xc1) {
            uint8_t imm8;
            unsigned count, effective_count;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            count = imm8 & (rm_size == 64 ? 0x3f : 0x1f);
            effective_count = (modrm.reg == 0 || modrm.reg == 1) ? count :
                ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
            if (effective_count == 0)
                break;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
                goto amd64_gpf_restore;
            switch (modrm.reg) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
                break;
            case 2:
            case 3:
                result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
                break;
            case 4:
                result = amd64_trunc(lhs << count, rm_size);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            if (modrm.reg == 0 || modrm.reg == 1)
                amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
            else if (modrm.reg != 2 && modrm.reg != 3)
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        }
        if (opcode == 0x80) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = imm8;
        } else if (opcode == 0x83) {
            int8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = (qword_t) amd64_sign_extend((uint8_t) imm8, 8);
        } else if (op_size == 16 && (opcode == 0x81 || opcode == 0xc7)) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            rhs = opcode == 0xc7 && !rex.w ? (uint32_t) imm32 : (qword_t) (sqword_t) imm32;
        }
        if (opcode == 0xc6 || opcode == 0xc7) {
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, rhs))
                goto amd64_gpf_restore;
            if (!modrm.is_reg && op_size == 64)
                amd64_trace_qword_store(cpu, saved_rip, opcode,
                        amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
            break;
        }

        // LOCK <alu> [mem], imm. This is the main interpreter's OWN copy of
        // the group -- amd64_jit_modrm_imm is a second one, reached through the
        // JIT bridge -- and neither the lock prefix nor any atomicity was ever
        // honoured here. /7 is CMP, which writes nothing and cannot be locked.
        if (lock_prefix && !modrm.is_reg &&
                (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) &&
                modrm.reg != 7) {
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            if (!amd64_locked_alu(cpu, tlb, addr, rm_size, modrm.reg, rhs))
                goto amd64_gpf_restore;
            break;
        }

        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_gpf_restore;

        bool trace_as_alu = amd64_as_alu_stderr_enabled() &&
            current != NULL &&
            current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            (opcode == 0x81 || opcode == 0x83) &&
            modrm.is_reg &&
            rm_size == 32;
        if (trace_as_alu) {
            uint8_t insn_bytes[8] = {};
            bool have_insn_bytes = amd64_trace_read_guest(saved_rip, insn_bytes, sizeof(insn_bytes));
            fprintf(stderr,
                    "amd64 as alu: pre rip=%#llx subop=%u rm=%u lhs=%#llx rhs=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u%s\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) lhs,
                    (unsigned long long) rhs,
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af,
                    have_insn_bytes ? "" : " bytes=?");
            if (have_insn_bytes) {
                fprintf(stderr,
                        "amd64 as alu: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                        insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
            }
        }

        switch (modrm.reg) {
        case 0:
            result = amd64_trunc(lhs + rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_add_flags(cpu, lhs, rhs, result, rm_size);
            break;
        case 1:
            result = amd64_trunc(lhs | rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 2: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, rm_size);
            break;
        }
        case 3: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, rm_size);
            break;
        }
        case 4:
            result = amd64_trunc(lhs & rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 5:
            result = amd64_trunc(lhs - rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
            break;
        case 6:
            result = amd64_trunc(lhs ^ rhs, rm_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_gpf_restore;
            amd64_set_logic_flags(cpu, result, rm_size);
            break;
        case 7:
            result = amd64_trunc(lhs - rhs, rm_size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (trace_as_alu) {
            fprintf(stderr,
                    "amd64 as alu: post rip=%#llx subop=%u rm=%u result=%#llx reg=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) result,
                    (unsigned long long) amd64_reg_get(cpu, modrm.rm, rm_size),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        break;
    }
    case 0x90 ... 0x97: {
        unsigned reg = (opcode - 0x90) | (rex.b ? 8 : 0);
        if (reg != amd64_rax) {
            qword_t lhs = amd64_reg_get(cpu, amd64_rax, op_size);
            qword_t rhs = amd64_reg_get(cpu, reg, op_size);
            amd64_reg_set(cpu, amd64_rax, op_size, rhs);
            amd64_reg_set(cpu, reg, op_size, lhs);
        }
        break;
    }
    case 0x9b:
        // FWAIT/WAIT: wait for pending x87 exceptions. The emulator raises FPU
        // exceptions synchronously and has none pending here, so it is a no-op.
        // musl's printf double->long-double path (fldl; fwait; fstpt) needs it.
        break;
    case 0x98:
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rax, 64, (qword_t) (sqword_t) (int32_t) amd64_reg_get(cpu, amd64_rax, 32));
        } else if (operand_size_prefix) {
            // cbw: sign-extend AL into AX. Reinterpret the byte as signed first;
            // (int16_t) alone treats 0x80 as +128, not -128.
            amd64_reg_set(cpu, amd64_rax, 16, (word_t) (int16_t) (int8_t) amd64_reg_get(cpu, amd64_rax, 8));
        } else {
            amd64_reg_set(cpu, amd64_rax, 32, (dword_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 16));
        }
        break;
    case 0x99:
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rdx, 64,
                    ((sqword_t) amd64_reg_get(cpu, amd64_rax, 64) < 0) ? ~0ull : 0);
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rdx, 16,
                    ((int16_t) amd64_reg_get(cpu, amd64_rax, 16) < 0) ? 0xffff : 0);
        } else {
            amd64_reg_set(cpu, amd64_rdx, 32,
                    ((int32_t) amd64_reg_get(cpu, amd64_rax, 32) < 0) ? 0xffffffffu : 0);
        }
        break;
    case 0x1f: {
        // Toolchains use 0f 1f /0 for alignment NOPs. Be tolerant if dispatch
        // lands on the second byte and consume the ModRM form here as well.
        struct amd64_modrm modrm;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (modrm.reg != 0)
            return INT_UNDEFINED;
        break;
    }
    case 0x04:
    case 0x05:
    case 0x0c:
    case 0x0d:
    case 0x14:
    case 0x15:
    case 0x1c:
    case 0x1d:
    case 0x24:
    case 0x25:
    case 0x2c:
    case 0x2d:
    case 0x34:
    case 0x35:
    case 0x3c:
    case 0x3d: {
        unsigned size = (opcode & 0x1) == 0 ? 8 : op_size;
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, size);
        qword_t rhs;
        qword_t result;
        unsigned carry_in;
        if (!amd64_fetch_accum_imm(cpu, tlb, size, true, &rhs)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (opcode) {
        case 0x04:
        case 0x05:
            result = amd64_trunc(lhs + rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x0c:
        case 0x0d:
            result = amd64_trunc(lhs | rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x14:
        case 0x15:
            carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        case 0x1c:
        case 0x1d:
            carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        case 0x24:
        case 0x25:
            result = amd64_trunc(lhs & rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x2c:
        case 0x2d:
            result = amd64_trunc(lhs - rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x34:
        case 0x35:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_reg_set(cpu, amd64_rax, size, result);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x3c:
        case 0x3d:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    case 0xa9: {
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, op_size);
        qword_t rhs;
        if (!amd64_fetch_accum_imm(cpu, tlb, op_size, true, &rhs)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_set_logic_flags(cpu, lhs & rhs, op_size);
        break;
    }
    case 0xa8: {
        uint8_t imm8;
        qword_t lhs = amd64_reg_get(cpu, amd64_rax, 8);
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_set_logic_flags(cpu, lhs & imm8, 8);
        break;
    }
    case 0xb0 ... 0xb7: {
        unsigned reg = (opcode - 0xb0) | (rex.b ? 8 : 0);
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        amd64_reg_set_encoded8(cpu, reg, rex.present, imm8);
        break;
    }
    case 0xb8 ... 0xbf: {
        unsigned reg = (opcode - 0xb8) | (rex.b ? 8 : 0);
        if (rex.w) {
            uint64_t imm64;
            if (!amd64_fetch_u64(cpu, tlb, &imm64)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 64, imm64);
        } else if (operand_size_prefix) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 16, imm16);
        } else {
            uint32_t imm32;
            if (!amd64_fetch_u32(cpu, tlb, &imm32)) {
                cpu->amd64_rip = saved_rip;
                cpu->segfault_addr = saved_rip;
                return INT_GPF;
            }
            amd64_reg_set(cpu, reg, 32, imm32);
        }
        break;
    }
    case 0xc2: {
        uint16_t imm16;
        qword_t target;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_pop(cpu, tlb, &target))
            goto amd64_gpf_restore;
        qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
        cpu->amd64_regs[amd64_rsp] = old_rsp + imm16;
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, target, "ret-imm");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "ret-imm");
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "ret");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        cpu->amd64_rip = target;
        break;
    }
    case 0xc3: {
        qword_t target;
        if (!amd64_pop(cpu, tlb, &target))
            goto amd64_gpf_restore;
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, target, "ret");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "ret");
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "ret");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        cpu->amd64_rip = target;
        break;
    }
    case 0xc9: {
        unsigned pop_size = operand_size_prefix ? 16 : 64;
        qword_t value;
        qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
        cpu->amd64_regs[amd64_rsp] = cpu->amd64_regs[amd64_rbp];
        amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
        amd64_trace_as_stack(amd64_as_stack_leave, pop_size, old_rsp, cpu->amd64_regs[amd64_rsp],
                cpu->amd64_regs[amd64_rbp]);
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_gpf_restore;
        amd64_reg_set(cpu, amd64_rbp, pop_size, value);
        break;
    }
    // int3: the standard x86/x86_64 software breakpoint instruction (what gdb
    // itself plants for every software breakpoint on this architecture, and
    // what its own linux_ptrace_test_ret_to_nx startup self-test executes to
    // probe host ptrace/NX behavior). Missing here entirely -- unlike the
    // shared i386 decoder (emu/decode.h's "case 0xcc: INT(INT_BREAKPOINT)"),
    // which already handles it -- so it fell through to the default
    // unrecognized-opcode path and raised SIGILL instead of SIGTRAP. rip is
    // already past this single opcode byte by this point (same as the other
    // no-operand opcodes above), matching real int3's "trap reports PC after
    // the instruction" semantics; kernel/calls.c's existing INT_BREAKPOINT
    // case delivers SIGTRAP/TRAP_BRKPT_ at current_fault_ip(cpu), which reads
    // amd64_rip for this ABI.
    case 0xcc:
        return INT_BREAKPOINT;
    case 0xf4:
        return INT_PRIV;
    case 0xf5:
        cpu->cf = !cpu->cf;
        cpu->cf_bit = cpu->cf;
        break;
    case 0xf8:
        cpu->cf = 0;
        cpu->cf_bit = 0;
        break;
    case 0xf9:
        cpu->cf = 1;
        cpu->cf_bit = 1;
        break;
    case 0xfa:
    case 0xfb:
        return INT_PRIV;
    case 0xfc:
        cpu->df = 0;
        cpu->df_offset = 1;
        break;
    case 0xfd:
        cpu->df = 1;
        cpu->df_offset = -1;
        break;
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        unsigned count, effective_count;
        unsigned rm_size = (opcode == 0xd0 || opcode == 0xd2) ? 8 : op_size;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_gpf_restore;
        count = (opcode == 0xd0 || opcode == 0xd1) ? 1 :
            (amd64_reg_get(cpu, amd64_rcx, 8) & (rm_size == 64 ? 0x3f : 0x1f));
        // rotates: a full turn (masked count a nonzero multiple of the operand
        // size) leaves the value unchanged but still updates CF/OF, so gate on
        // the masked count, not count % size.
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? count :
            ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
        bool trace_as_shift = amd64_as_alu_stderr_enabled() &&
            current != NULL &&
            current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            modrm.is_reg &&
            rm_size == 32;
        if (trace_as_shift) {
            uint8_t insn_bytes[8] = {};
            bool have_insn_bytes = amd64_trace_read_guest(saved_rip, insn_bytes, sizeof(insn_bytes));
            fprintf(stderr,
                    "amd64 as shift: pre rip=%#llx subop=%u rm=%u lhs=%#llx count=%u effective=%u cf=%u zf=%u sf=%u of=%u pf=%u af=%u%s\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) lhs,
                    count,
                    effective_count,
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af,
                    have_insn_bytes ? "" : " bytes=?");
            if (have_insn_bytes) {
                fprintf(stderr,
                        "amd64 as shift: bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        insn_bytes[0], insn_bytes[1], insn_bytes[2], insn_bytes[3],
                        insn_bytes[4], insn_bytes[5], insn_bytes[6], insn_bytes[7]);
            }
        }
        if (effective_count == 0)
            break;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
            break;
        case 2:
        case 3:
            result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
            break;
        case 4:
            result = amd64_trunc(lhs << count, rm_size);
            break;
        case 5:
            result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
            break;
        case 7:
            result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_gpf_restore;
        if (modrm.reg == 0 || modrm.reg == 1)
            amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
        else if (modrm.reg != 2 && modrm.reg != 3)
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
        if (trace_as_shift) {
            fprintf(stderr,
                    "amd64 as shift: post rip=%#llx subop=%u rm=%u result=%#llx reg=%#llx cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                    (unsigned long long) saved_rip,
                    modrm.reg,
                    modrm.rm,
                    (unsigned long long) result,
                    (unsigned long long) amd64_reg_get(cpu, modrm.rm, rm_size),
                    cpu->cf, cpu->zf, cpu->sf, cpu->of, cpu->pf, cpu->af);
        }
        break;
    }
    case 0xe8: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t return_rip = cpu->amd64_rip;
        qword_t target = return_rip + rel32;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "call");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_gpf_restore;
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "call-rel32");
        cpu->amd64_rip = target;
        break;
    }
    case 0xe9: {
        int32_t rel32;
        if (!amd64_fetch(cpu, tlb, &rel32, sizeof(rel32))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t target = cpu->amd64_rip + rel32;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jmp");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cc1_va_list_branch_probe(cpu, saved_rip, true, target, "jmp-rel32");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jmp-rel32");
        cpu->amd64_rip = target;
        break;
    }
    case 0xeb: {
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        qword_t target = cpu->amd64_rip + rel8;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jmp");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, target, "jmp-rel8");
        cpu->amd64_rip = target;
        break;
    }
    case 0xe3: {
        int8_t rel8;
        qword_t count;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        count = cpu->amd64_address_size_prefix
                ? amd64_reg_get(cpu, amd64_rcx, 32)
                : amd64_reg_get(cpu, amd64_rcx, 64);
        if (count == 0) {
            qword_t target = cpu->amd64_rip + rel8;
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "jcxz");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
            cpu->amd64_rip = target;
        }
        break;
    }
    case 0xe0:   // loopne/loopnz: dec (R|E)CX; branch if count!=0 && ZF==0
    case 0xe1:   // loope/loopz:   dec (R|E)CX; branch if count!=0 && ZF==1
    case 0xe2: { // loop:          dec (R|E)CX; branch if count!=0
        int8_t rel8;
        if (!amd64_fetch(cpu, tlb, &rel8, sizeof(rel8))) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        unsigned csize = cpu->amd64_address_size_prefix ? 32 : 64;
        qword_t count = amd64_reg_get(cpu, amd64_rcx, csize);
        count = (csize == 32) ? (qword_t) (dword_t) (count - 1) : (count - 1);
        amd64_reg_set(cpu, amd64_rcx, csize, count);  // loop does NOT touch flags
        bool take = count != 0;
        if (opcode == 0xe1)
            take = take && ZF;
        else if (opcode == 0xe0)
            take = take && !ZF;
        if (take) {
            qword_t target = cpu->amd64_rip + rel8;
            int ti = amd64_validate_transfer_target(cpu, tlb, saved_rip, target, "loop");
            if (ti != INT_NONE)
                return ti;
            cpu->amd64_rip = target;
        }
        break;
    }
    case 0xd7: { // xlatb: AL = [(R|E)BX + AL]
        qword_t base = cpu->amd64_address_size_prefix
                ? amd64_reg_get(cpu, amd64_rbx, 32)
                : amd64_reg_get(cpu, amd64_rbx, 64);
        qword_t addr = base + (amd64_reg_get(cpu, amd64_rax, 64) & 0xff);
        qword_t value;
        if (!amd64_mem_read_value(cpu, tlb, addr, 8, &value)) {
            cpu->amd64_rip = saved_rip;
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        cpu->amd64_regs[amd64_rax] =
            (cpu->amd64_regs[amd64_rax] & ~0xffULL) | (value & 0xff);
        break;
    }
    case 0xfe: {
        struct amd64_modrm modrm;
        qword_t lhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (modrm.reg) {
        case 0:
        case 1: {
            bool is_inc = modrm.reg == 0;
            bool saved_cf = cpu->cf;
            if (lock_prefix && !modrm.is_reg) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (!amd64_locked_incdec(cpu, tlb, addr, 8, is_inc))
                    goto amd64_gpf_restore;
                break;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
                goto amd64_gpf_restore;
            result = is_inc ? amd64_trunc(lhs + 1, 8) : amd64_trunc(lhs - 1, 8);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
                goto amd64_gpf_restore;
            if (is_inc)
                amd64_set_add_flags(cpu, lhs, 1, result, 8);
            else
                amd64_set_sub_flags(cpu, lhs, 1, result, 8);
            cpu->cf = saved_cf;
            collapse_flags(cpu);
            break;
        }
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    case 0xff: {
        struct amd64_modrm modrm;
        qword_t value, lhs, result;
        if (!amd64_decode_modrm(cpu, tlb, rex, &modrm)) {
            cpu->amd64_rip = saved_rip;
            cpu->segfault_addr = saved_rip;
            return INT_GPF;
        }
        switch (modrm.reg) {
        case 0:
        case 1: {
            bool is_inc = modrm.reg == 0;
            bool saved_cf = cpu->cf;
            if (lock_prefix && !modrm.is_reg) {
                qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
                if (!amd64_locked_incdec(cpu, tlb, addr, op_size, is_inc))
                    goto amd64_gpf_restore;
                break;
            }
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_gpf_restore;
            result = is_inc ? amd64_trunc(lhs + 1, op_size) : amd64_trunc(lhs - 1, op_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                goto amd64_gpf_restore;
            if (is_inc)
                amd64_set_add_flags(cpu, lhs, 1, result, op_size);
            else
                amd64_set_sub_flags(cpu, lhs, 1, result, op_size);
            cpu->cf = saved_cf;
            collapse_flags(cpu);
            break;
        }
        case 2: {
            qword_t return_rip = cpu->amd64_rip;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            {
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "call-rm64");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
            }
            if (!amd64_push(cpu, tlb, return_rip))
                goto amd64_gpf_restore;
            amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "call-rm64");
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "call-rm64");
            cpu->amd64_rip = value;
            break;
        }
        case 4:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
                goto amd64_gpf_restore;
            {
                int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "jmp-rm64");
                if (target_interrupt != INT_NONE)
                    return target_interrupt;
            }
            amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "jmp-rm64");
            amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "jmp-rm64");
            cpu->amd64_rip = value;
            break;
        case 6:
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix,
                    operand_size_prefix ? 16 : 64, &value))
                goto amd64_gpf_restore;
            if (!amd64_push_size(cpu, tlb, operand_size_prefix ? 16 : 64, value))
                goto amd64_gpf_restore;
            break;
        default:
            return INT_UNDEFINED;
        }
        break;
    }
    default:
        return INT_UNDEFINED;
    }

    amd64_trace_cargo_predecessor(cpu, tlb, saved_rip);
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_gpf_restore:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_step_to_interrupt_jit(struct cpu_state *cpu, struct tlb *tlb) {
    static int debug_enabled = -1;
    qword_t before_rip = cpu->amd64_rip;
    qword_t before_rsp = cpu->amd64_regs[amd64_rsp];
    guest_addr_t checked_rip;
    if (debug_enabled == -1)
        debug_enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    int interrupt = amd64_step_to_interrupt(cpu, tlb);
    if (before_rip != 0 && cpu->amd64_rip == 0) {
        printk("[amd64-jit] helper zero-rip comm=%s pid=%d before=%#llx rsp=%#llx->%#llx int=%d insn=%#llx\n",
               current != NULL ? current->comm : "?",
               current != NULL ? current->pid : -1,
               (unsigned long long) before_rip,
               (unsigned long long) before_rsp,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               interrupt,
               (unsigned long long) cpu->amd64_current_insn_rip);
    }
    if (debug_enabled == 1) {
        fprintf(stderr,
                "[amd64-jit] helper result rip=%llx->%llx rsp=%llx->%llx int=%d\n",
                (unsigned long long) before_rip,
                (unsigned long long) cpu->amd64_rip,
                (unsigned long long) before_rsp,
                (unsigned long long) cpu->amd64_regs[amd64_rsp],
                interrupt);
    }
    if (interrupt != INT_NONE && !amd64_guest_addr_ok(cpu->amd64_rip, 1, &checked_rip)) {
        printk("[amd64-jit] helper bad-rip insn=%#llx rip=%#llx rsp=%#llx int=%d\n",
               (unsigned long long) cpu->amd64_current_insn_rip,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               interrupt);
    }
    if (interrupt != INT_NONE) {
        cpu->trapno = interrupt;
        amd64_sync_legacy_regs(cpu);
    }
    return interrupt;
}

int amd64_jit_ret(struct cpu_state *cpu, struct tlb *tlb) {
    qword_t target;
    guest_addr_t checked_target;
    qword_t saved_rip = cpu->amd64_rip;
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
    if (!amd64_pop(cpu, tlb, &target)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    if (!amd64_guest_addr_ok(target, 1, &checked_target)) {
        if (getenv("ISH_TRACE_GUEST_FATAL") != NULL ||
                getenv("ISH_TRACE_AMD64_AS_STDERR") != NULL) {
            fprintf(stderr,
                    "[amd64-jit] bad-ret-target from=%#llx target=%#llx old-rsp=%#llx new-rsp=%#llx\n",
                    (unsigned long long) saved_rip,
                    (unsigned long long) target,
                    (unsigned long long) old_rsp,
                    (unsigned long long) cpu->amd64_regs[amd64_rsp]);
        }
        uint8_t slot_bytes[8] = {};
        uint8_t direct_slot_bytes[8] = {};
        bool have_slot = amd64_mem_read(cpu, tlb, old_rsp, slot_bytes, sizeof(slot_bytes));
        bool have_direct_slot = amd64_mem_read_direct(old_rsp, direct_slot_bytes, sizeof(direct_slot_bytes));
        printk("[amd64-jit] bad-ret-target-v2 comm=%s pid=%d from=%#llx target=%#llx old-rsp=%#llx new-rsp=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               current != NULL ? current->comm : "?",
               current != NULL ? current->pid : -1,
               (unsigned long long) saved_rip,
               (unsigned long long) target,
               (unsigned long long) old_rsp,
               (unsigned long long) cpu->amd64_regs[amd64_rsp],
               have_slot ? "" : "unreadable ",
               slot_bytes[0], slot_bytes[1], slot_bytes[2], slot_bytes[3],
               slot_bytes[4], slot_bytes[5], slot_bytes[6], slot_bytes[7]);
        printk("[amd64-jit] bad-ret-target-direct addr=%#llx slot=%s%02x %02x %02x %02x %02x %02x %02x %02x\n",
               (unsigned long long) old_rsp,
               have_direct_slot ? "" : "unreadable ",
               direct_slot_bytes[0], direct_slot_bytes[1], direct_slot_bytes[2], direct_slot_bytes[3],
               direct_slot_bytes[4], direct_slot_bytes[5], direct_slot_bytes[6], direct_slot_bytes[7]);
        amd64_dump_tlb_slot(tlb, old_rsp, sizeof(slot_bytes), "bad-ret-target");
        amd64_dump_guest_bytes(cpu, tlb, saved_rip, 8, "bad-ret-target-insn");
        amd64_dump_stack_window(cpu, tlb, old_rsp, 2, 4, "bad-ret-target");
        if (current != NULL)
            amd64_dump_recent_suspects(current->pid, "bad-ret-target");
        cpu->amd64_rip = target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_ret_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long imm16) {
    qword_t target;
    guest_addr_t checked_target;
    qword_t saved_rip = cpu->amd64_rip;
    qword_t old_rsp = cpu->amd64_regs[amd64_rsp];
    if (imm16 > 0xffff)
        return INT_GPF;
    if (!amd64_pop(cpu, tlb, &target)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_regs[amd64_rsp] += (uint16_t) imm16;
    amd64_trace_suspicious_rsp_write(cpu, old_rsp, cpu->amd64_regs[amd64_rsp], 64);
    if (!amd64_guest_addr_ok(target, 1, &checked_target)) {
        cpu->amd64_rip = target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_leave(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long pop_size, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    qword_t old_rsp;
    qword_t value;
    if (pop_size != 16 && pop_size != 64)
        return INT_GPF;
    old_rsp = cpu->amd64_regs[amd64_rsp];
    cpu->amd64_regs[amd64_rsp] = cpu->amd64_regs[amd64_rbp];
    amd64_trace_as_stack(amd64_as_stack_leave, pop_size, old_rsp, cpu->amd64_regs[amd64_rsp],
                         cpu->amd64_regs[amd64_rbp]);
    if (!amd64_pop_size(cpu, tlb, pop_size, &value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    amd64_reg_set(cpu, amd64_rbp, pop_size, value);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg, unsigned long next_ip) {
    if (reg >= amd64_reg_count)
        return INT_GPF;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_push(cpu, tlb, cpu->amd64_regs[reg])) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_pop_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg, unsigned long next_ip) {
    qword_t value;
    qword_t saved_rip = cpu->amd64_rip;
    if (reg >= amd64_reg_count)
        return INT_GPF;
    if (!amd64_pop(cpu, tlb, &value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_regs[reg] = value;
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_pop_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t value;
    unsigned pop_size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_pop_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x8f || lock_prefix)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_pop_rm_pf;
    if (modrm.reg != 0)
        return INT_UNDEFINED;

    pop_size = operand_size_prefix ? 16 : 64;
    {
        // amd64_pop_size() commits the RSP advance as soon as its read succeeds,
        // before the destination write below is attempted. If that write then
        // faults (e.g. a first-touch or COW page needing a fault-in), the whole
        // instruction bails to amd64_pop_rm_pf for a re-execute -- but without
        // restoring RSP here, the retry re-reads from the *already-advanced*
        // stack slot instead of the original one, silently dropping the real
        // popped value (e.g. a return address) and substituting whatever
        // garbage sits one slot up. Found via #487: musl's sigsetjmp does
        // `popq off(%rdi)` to relocate its own return address into the
        // jmp_buf; the first attempt's write to the jmp_buf (freshly-touched
        // .bss) faulted, and the retry's mis-popped value corrupted the saved
        // return address, later crashing with rip set to that garbage value.
        qword_t rsp_before_pop = cpu->amd64_regs[amd64_rsp];
        if (!amd64_pop_size(cpu, tlb, pop_size, &value))
            goto amd64_pop_rm_pf;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, pop_size, value)) {
            cpu->amd64_regs[amd64_rsp] = rsp_before_pop;
            goto amd64_pop_rm_pf;
        }
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_pop_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_bswap(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (size == 64) {
        qword_t value = amd64_reg_get(cpu, reg, 64);
        amd64_reg_set(cpu, reg, 64, __builtin_bswap64(value));
    } else {
        dword_t value = (dword_t) amd64_reg_get(cpu, reg, 32);
        amd64_reg_set(cpu, reg, 32, __builtin_bswap32(value));
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_flags(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long push_size, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    if (push_size != 16 && push_size != 64)
        return INT_GPF;
    collapse_flags(cpu);
    if (!amd64_push_size(cpu, tlb, push_size, cpu->eflags)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_pop_flags(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long pop_size, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    qword_t value;
    if (pop_size != 16 && pop_size != 64)
        return INT_GPF;
    if (!amd64_pop_size(cpu, tlb, pop_size, &value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->eflags = (cpu->eflags & ~0xcd5u) | ((dword_t) value & 0xcd5u);
    expand_flags(cpu);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_push_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-push-imm-next from=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (!amd64_push(cpu, tlb, (qword_t) value)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_xchg_rax_reg(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    qword_t lhs;
    qword_t rhs;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-xchg-next from=%#llx reg=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               reg,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (reg != amd64_rax) {
        lhs = amd64_reg_get(cpu, amd64_rax, size);
        rhs = amd64_reg_get(cpu, reg, size);
        amd64_reg_set(cpu, amd64_rax, size, rhs);
        amd64_reg_set(cpu, reg, size, lhs);
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_xchg_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t lhs, rhs;


    if (opcode != 0x86 && opcode != 0x87)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_xchg_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            // xchg with a memory operand is implicitly locked; the explicit
            // prefix only needs to be consumed. LOCK with a register form is #UD.
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_xchg_rm_pf;

    if (lock_prefix && modrm.is_reg)
        return INT_UNDEFINED;
    size = opcode == 0x86 ? 8 : (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    rhs = opcode == 0x86
        ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
        : amd64_reg_get(cpu, modrm.reg, size);
    // XCHG on memory is atomic with or without the prefix. It used to be a
    // read/write pair under the global atomic_l_lock, which is not just weaker
    // than a host atomic -- it LIVELOCKED: two guest threads contending on a
    // one-word spinlock managed 78 acquisitions and then one spun 200 million
    // times without ever seeing the word released.
    if (!modrm.is_reg) {
        qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
        if (!amd64_locked_xchg(cpu, tlb, addr, size, rhs, &lhs))
            goto amd64_xchg_rm_pf;
    } else {
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
            goto amd64_xchg_rm_pf;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, size, rhs))
            goto amd64_xchg_rm_pf;
    }
    if (opcode == 0x86)
        amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
    else
        amd64_reg_set(cpu, modrm.reg, size, lhs);

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_xchg_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_jmp_abs(struct cpu_state *cpu, struct tlb *tlb, unsigned long target) {
    guest_addr_t checked_target;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target)) {
        printk("[amd64-jit] bad-jmp-target from=%#llx target=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) target);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = (qword_t) target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_call_abs(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long target, unsigned long next_ip) {
    guest_addr_t checked_target;
    guest_addr_t checked_next_ip;
    qword_t saved_rip = cpu->amd64_rip;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target) ||
            !amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-call-target from=%#llx target=%#llx next=%#llx\n",
               (unsigned long long) saved_rip,
               (unsigned long long) target,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (!amd64_push(cpu, tlb, (qword_t) next_ip)) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return INT_PF;
    }
    cpu->amd64_rip = (qword_t) target;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_jcc_abs(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long cc, unsigned long target, unsigned long next_ip) {
    guest_addr_t checked_target;
    guest_addr_t checked_next_ip;
    (void) tlb;
    if (cc > 0xf)
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) target, 1, &checked_target) ||
            !amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-jcc-target from=%#llx cc=%lu target=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               cc,
               (unsigned long long) target,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) target;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = amd64_cond_eval(cpu, (unsigned) cc)
        ? (qword_t) target
        : (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_syscall(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-syscall-next from=%#llx next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_AMD64_SYSCALL;
}

int amd64_jit_rdtsc(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    qword_t tsc;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;
    tsc = amd64_rdtsc_value();
    amd64_reg_set(cpu, amd64_rax, 32, (dword_t) tsc);
    amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (tsc >> 32));
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

// XGETBV (0f 01 d0). Not optional once CPUID advertises OSXSAVE: glibc runs
// this immediately after seeing that bit to decide whether the OS enabled the
// YMM/ZMM state, so SIGILL here would kill every glibc process at startup.
//
// JIT-side only, deliberately: the amd64 interpreter is being retired, so a
// parallel copy there would be work with a known expiry date, and worse, it
// would let this instruction appear to work today via the interpreter fallback
// and then vanish when the fallback goes.
int amd64_jit_xgetbv(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    qword_t xcr0;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;
    // ECX selects the register. XCR0 is the only one that exists; hardware
    // raises #GP for anything else.
    if ((dword_t) cpu->amd64_regs[amd64_rcx] != 0) {
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    xcr0 = xcr0_value();
    amd64_reg_set(cpu, amd64_rax, 32, (dword_t) xcr0);
    amd64_reg_set(cpu, amd64_rdx, 32, (dword_t) (xcr0 >> 32));
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

// Port I/O: IN/OUT (e4/e5 imm8, e6/e7 imm8, ec/ed dx, ee/ef dx). These are
// ring-0 instructions -- a user-mode process needs IOPL(3) or an ioperm bitmap
// bit, neither of which iSH grants -- so the only correct outcome is the same
// one real hardware gives: #GP(0), which Linux turns into SIGSEGV/SI_KERNEL.
//
// This matters because probing for a hypervisor by faulting is a real,
// deliberate userspace idiom, not a bug in the guest. util-linux's lscpu
// detects VMware with the "VMXh"/port-0x5658 backdoor: it arms a SIGSEGV
// handler, runs `in eax, dx`, and siglongjmps out of the fault to conclude
// "not VMware". Falling through to the generic unrecognized-opcode path gave
// SIGILL instead, which lscpu does not catch, so `lscpu` died outright on the
// amd64 guest rather than printing a single line of output.
//
// rip stays AT the faulting instruction (not past it): a fault, unlike a trap,
// reports the instruction that caused it, and the interpreter's own INT_PRIV
// path rewinds to amd64_current_insn_rip for exactly this reason.
//
// JIT-side only, deliberately -- see amd64_jit_xgetbv above for why the
// interpreter is not the place for this.
int amd64_jit_port_io(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long insn_ip) {
    (void) tlb;
    cpu->amd64_rip = (qword_t) insn_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_PRIV;
}

int amd64_jit_cpuid(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    dword_t eax;
    dword_t ebx;
    dword_t ecx;
    dword_t edx;
    (void) tlb;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;
    eax = (dword_t) cpu->amd64_regs[amd64_rax];
    ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    do_cpuid(&eax, &ebx, &ecx, &edx);
    cpu->amd64_regs[amd64_rax] = eax;
    cpu->amd64_regs[amd64_rbx] = ebx;
    cpu->amd64_regs[amd64_rcx] = ecx;
    cpu->amd64_regs[amd64_rdx] = edx;
    cpu->eax = eax;
    cpu->ebx = ebx;
    cpu->ecx = ecx;
    cpu->edx = edx;
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_moffs_accum(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    bool fs_prefix = false;
    byte_t byte;
    qword_t addr;
    qword_t value;
    unsigned size;

    if (opcode < 0xa0 || opcode > 0xa3)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_moffs_accum_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte == 0x67) {
            cpu->amd64_address_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_fetch_moffs_addr(cpu, tlb, &addr))
        goto amd64_moffs_accum_pf;
    if (fs_prefix)
        addr += cpu->tls_ptr;

    size = (opcode == 0xa0 || opcode == 0xa2) ? 8 :
        (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    if (opcode == 0xa0 || opcode == 0xa1) {
        if (!amd64_mem_read(cpu, tlb, addr, &value, size / 8))
            goto amd64_moffs_accum_pf;
        amd64_reg_set(cpu, amd64_rax, size, value);
    } else {
        value = amd64_reg_get(cpu, amd64_rax, size);
        if (!amd64_mem_write(cpu, tlb, addr, &value, size / 8))
            goto amd64_moffs_accum_pf;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_moffs_accum_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_sign_extend(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    byte_t byte;

    if (opcode != 0x98 && opcode != 0x99)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_sign_extend_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    if (opcode == 0x98) {
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rax, 64,
                    (qword_t) (sqword_t) (int32_t) amd64_reg_get(cpu, amd64_rax, 32));
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rax, 16,
                    (word_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 8));
        } else {
            amd64_reg_set(cpu, amd64_rax, 32,
                    (dword_t) (int16_t) amd64_reg_get(cpu, amd64_rax, 16));
        }
    } else {
        if (rex.w) {
            amd64_reg_set(cpu, amd64_rdx, 64,
                    ((sqword_t) amd64_reg_get(cpu, amd64_rax, 64) < 0) ? ~0ull : 0);
        } else if (operand_size_prefix) {
            amd64_reg_set(cpu, amd64_rdx, 16,
                    ((int16_t) amd64_reg_get(cpu, amd64_rax, 16) < 0) ? 0xffff : 0);
        } else {
            amd64_reg_set(cpu, amd64_rdx, 32,
                    ((int32_t) amd64_reg_get(cpu, amd64_rax, 32) < 0) ? 0xffffffffu : 0);
        }
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_sign_extend_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_string_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    byte_t byte;
    unsigned size;
    int interrupt;

    if (!((opcode >= 0xa4 && opcode <= 0xa7) ||
          (opcode >= 0xaa && opcode <= 0xaf)))
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_string_op_jit_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte == 0x67) {
            cpu->amd64_address_size_prefix = true;
            continue;
        }
        if (byte == 0xf3) {
            rep_mode = AMD64_REPZ;
            continue;
        }
        if (byte == 0xf2) {
            rep_mode = AMD64_REPNZ;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    size = (opcode & 1) == 0 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    interrupt = amd64_string_op(cpu, tlb, saved_rip, (byte_t) opcode, size, rep_mode);
    if (interrupt != INT_NONE) {
        amd64_sync_legacy_regs(cpu);
        return interrupt;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_string_op_jit_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_mov_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned reg = reg_size & 0xf;
    unsigned size = (reg_size >> 8) & 0xff;
    bool rex_present = (reg_size & (1ul << 16)) != 0;
    (void) tlb;
    if (reg >= amd64_reg_count || (size != 8 && size != 16 && size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-mov-imm-next from=%#llx reg=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               reg,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }
    if (size == 8)
        amd64_reg_set_encoded8(cpu, reg, rex_present, value);
    else
        amd64_reg_set(cpu, reg, size, (qword_t) value);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_accum_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    bool operand_size_prefix = false;
    byte_t byte;
    qword_t lhs, rhs, result;
    unsigned size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_accum_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;

    size = (opcode & 1) == 0 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    if (size == 8) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_accum_imm_pf;
        rhs = imm8;
        lhs = amd64_reg_get_encoded8(cpu, amd64_rax, rex.present);
    } else if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            goto amd64_accum_imm_pf;
        rhs = imm16;
        lhs = amd64_reg_get(cpu, amd64_rax, size);
    } else {
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
            goto amd64_accum_imm_pf;
        rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
        lhs = amd64_reg_get(cpu, amd64_rax, size);
    }

    switch (opcode & 0xfe) {
    case 0x04:
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x0c:
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x14: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x1c: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x24:
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x2c:
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x34:
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, amd64_rax, rex.present, result);
        else
            amd64_reg_set(cpu, amd64_rax, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x3c:
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0xa8:
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_accum_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_reg_reg_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_regs_size, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = op_regs_size & 0xff;
    unsigned reg = (op_regs_size >> 8) & 0xf;
    unsigned rm = (op_regs_size >> 12) & 0xf;
    unsigned size = (op_regs_size >> 16) & 0xff;
    bool rex_present = ((op_regs_size >> 24) & 1) != 0;
    qword_t lhs;
    qword_t rhs;
    qword_t result;

    (void) tlb;
    if (reg >= amd64_reg_count || rm >= amd64_reg_count ||
            (size != 8 && size != 16 && size != 32 && size != 64))
        return INT_GPF;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-reg-reg-next from=%#llx opcode=%#x reg=%u rm=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               reg,
               rm,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    switch (opcode) {
    case 0x00:
    case 0x01:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x02:
    case 0x03:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x08:
    case 0x09:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x0a:
    case 0x0b:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x10:
    case 0x11: {
        unsigned carry_in = cpu->cf;
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x12:
    case 0x13: {
        unsigned carry_in = cpu->cf;
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x18:
    case 0x19: {
        unsigned carry_in = cpu->cf;
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x1a:
    case 0x1b: {
        unsigned carry_in = cpu->cf;
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 0x20:
    case 0x21:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x22:
    case 0x23:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x28:
    case 0x29:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x2a:
    case 0x2b:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x30:
    case 0x31:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x32:
    case 0x33:
        lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, rm, rex_present)
            : amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 0x38:
        lhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x39:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x3a:
        lhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x3b:
        lhs = amd64_reg_get(cpu, reg, size);
        rhs = amd64_reg_get(cpu, rm, size);
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 0x63:
        amd64_reg_set(cpu, reg, size,
                (qword_t) amd64_sign_extend(amd64_reg_get(cpu, rm, size == 64 ? 32 : size),
                        size == 64 ? 32 : size));
        break;
    case 0x84:
        lhs = amd64_reg_get_encoded8(cpu, rm, rex_present);
        rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    case 0x85:
        lhs = amd64_reg_get(cpu, rm, size);
        rhs = amd64_reg_get(cpu, reg, size);
        amd64_set_logic_flags(cpu, lhs & rhs, size);
        break;
    case 0x88:
        amd64_reg_set_encoded8(cpu, rm, rex_present,
                amd64_reg_get_encoded8(cpu, reg, rex_present));
        break;
    case 0x89:
        amd64_reg_set(cpu, rm, size, amd64_reg_get(cpu, reg, size));
        break;
    case 0x8a:
        amd64_reg_set_encoded8(cpu, reg, rex_present,
                amd64_reg_get_encoded8(cpu, rm, rex_present));
        break;
    case 0x8b:
        amd64_reg_set(cpu, reg, size, amd64_reg_get(cpu, rm, size));
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_reg_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_group_rm_size, unsigned long value, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = op_group_rm_size & 0xff;
    unsigned group = (op_group_rm_size >> 8) & 0xf;
    unsigned rm = (op_group_rm_size >> 12) & 0xf;
    unsigned size = (op_group_rm_size >> 16) & 0xff;
    bool rex_present = (op_group_rm_size & (1ul << 24)) != 0;
    qword_t lhs;
    qword_t rhs = (qword_t) value;
    qword_t result;
    unsigned count;
    unsigned effective_count;

    (void) tlb;
    if (rm >= amd64_reg_count || group > 7 ||
            (size != 8 && size != 16 && size != 32 && size != 64))
        return INT_GPF;
    if (opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
            opcode != 0xc0 && opcode != 0xc1 &&
            opcode != 0xc6 && opcode != 0xc7)
        return INT_UNDEFINED;
    if ((opcode == 0xc6 || opcode == 0xc7) && group != 0)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-reg-imm-next from=%#llx opcode=%#x group=%u rm=%u size=%u next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               group,
               rm,
               size,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    if (opcode == 0xc6 || opcode == 0xc7) {
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, rhs);
        else
            amd64_reg_set(cpu, rm, size, rhs);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc0 || opcode == 0xc1) {
        count = (unsigned) rhs & (size == 64 ? 0x3f : 0x1f);
        // rotates update flags even on a full turn; gate on the masked count.
        effective_count = (group == 0 || group == 1) ? count : count;
        if (effective_count != 0) {
            lhs = size == 8
                ? amd64_reg_get_encoded8(cpu, rm, rex_present)
                : amd64_reg_get(cpu, rm, size);
            switch (group) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, size, count, group);
                amd64_set_rotate_flags(cpu, result, size, count, group);
                break;
            case 4:
                result = amd64_trunc(lhs << count, size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, size) >> count, size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, size) >> count), size);
                amd64_set_shift_flags(cpu, lhs, result, size, count, group);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (size == 8)
                amd64_reg_set_encoded8(cpu, rm, rex_present, result);
            else
                amd64_reg_set(cpu, rm, size, result);
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    lhs = size == 8 ? amd64_reg_get_encoded8(cpu, rm, rex_present) :
        amd64_reg_get(cpu, rm, size);
    switch (group) {
    case 0:
        result = amd64_trunc(lhs + rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_add_flags(cpu, lhs, rhs, result, size);
        break;
    case 1:
        result = amd64_trunc(lhs | rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 2: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 3: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
        break;
    }
    case 4:
        result = amd64_trunc(lhs & rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 5:
        result = amd64_trunc(lhs - rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    case 6:
        result = amd64_trunc(lhs ^ rhs, size);
        if (size == 8)
            amd64_reg_set_encoded8(cpu, rm, rex_present, result);
        else
            amd64_reg_set(cpu, rm, size, result);
        amd64_set_logic_flags(cpu, result, size);
        break;
    case 7:
        result = amd64_trunc(lhs - rhs, size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, size);
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_imul_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t rhs, result;
    sqword_t src_signed;
    sqword_t imm_signed;

    if (opcode != 0x69 && opcode != 0x6b)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_imul_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_imul_imm_pf;

    size = operand_size_prefix ? 16 : (rex.w ? 64 : 32);
    // Fetch the immediate before touching the r/m operand: a RIP-relative
    // operand resolves against the END of the instruction, and the fetch
    // cursor only reaches it once the immediate has been consumed.
    if (opcode == 0x69) {
        if (size == 16) {
            int16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_imul_imm_pf;
            imm_signed = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_imul_imm_pf;
            imm_signed = imm32;
        }
    } else {
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_imul_imm_pf;
        imm_signed = imm8;
    }
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &rhs))
        goto amd64_imul_imm_pf;

    src_signed = amd64_sign_extend(rhs, size);
    if (size == 64) {
        __int128_t full = (__int128_t) src_signed * (__int128_t) imm_signed;
        result = (qword_t) full;
        amd64_reg_set(cpu, modrm.reg, size, result);
        amd64_set_mul_flags(cpu, full != (__int128_t) (sqword_t) (uint64_t) result);
    } else {
        int64_t full = (int64_t) src_signed * (int64_t) imm_signed;
        result = amd64_trunc((qword_t) full, size);
        amd64_reg_set(cpu, modrm.reg, size, result);
        amd64_set_mul_flags(cpu, full != (int64_t) amd64_sign_extend(result, size));
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_imul_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

enum amd64_jit_mem_meta {
    AMD64_JIT_MEM_OPCODE_SHIFT = 0,
    AMD64_JIT_MEM_REG_SHIFT = 8,
    AMD64_JIT_MEM_SIZE_SHIFT = 12,
    AMD64_JIT_MEM_BASE_SHIFT = 20,
    AMD64_JIT_MEM_INDEX_SHIFT = 24,
    AMD64_JIT_MEM_SCALE_SHIFT = 28,
    AMD64_JIT_MEM_HAS_BASE = 1ul << 30,
    AMD64_JIT_MEM_HAS_INDEX = 1ul << 31,
    AMD64_JIT_MEM_RIP_REL = 1ul << 32,
    AMD64_JIT_MEM_FS = 1ul << 33,
    AMD64_JIT_MEM_REX_PRESENT = 1ul << 34,
    // Set for a LOCK-prefixed <alu> [mem], reg. The gen.c block that emits
    // this helper is the one amd64 JIT path that never rejected the prefix,
    // so a locked ALU op was compiled straight into the non-atomic
    // read/compute/write below and lost updates against other guest threads.
    AMD64_JIT_MEM_LOCK = 1ul << 35,
};

int amd64_jit_mem_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long meta, unsigned long disp, unsigned long next_ip) {
    guest_addr_t checked_next_ip;
    unsigned opcode = (meta >> AMD64_JIT_MEM_OPCODE_SHIFT) & 0xff;
    unsigned reg = (meta >> AMD64_JIT_MEM_REG_SHIFT) & 0xf;
    unsigned size = (meta >> AMD64_JIT_MEM_SIZE_SHIFT) & 0xff;
    unsigned base = (meta >> AMD64_JIT_MEM_BASE_SHIFT) & 0xf;
    unsigned index = (meta >> AMD64_JIT_MEM_INDEX_SHIFT) & 0xf;
    unsigned scale = (meta >> AMD64_JIT_MEM_SCALE_SHIFT) & 0x3;
    bool rex_present = (meta & AMD64_JIT_MEM_REX_PRESENT) != 0;
    qword_t addr = (qword_t) disp;
    qword_t value;

    if (reg >= amd64_reg_count || (size != 8 && size != 16 && size != 32 && size != 64))
        return INT_GPF;
    if (opcode != 0x00 && opcode != 0x01 && opcode != 0x02 && opcode != 0x03 &&
            opcode != 0x08 && opcode != 0x09 && opcode != 0x0a && opcode != 0x0b &&
            opcode != 0x10 && opcode != 0x11 && opcode != 0x12 && opcode != 0x13 &&
            opcode != 0x18 && opcode != 0x19 && opcode != 0x1a && opcode != 0x1b &&
            opcode != 0x20 && opcode != 0x21 && opcode != 0x22 && opcode != 0x23 &&
            opcode != 0x28 && opcode != 0x29 && opcode != 0x2a && opcode != 0x2b &&
            opcode != 0x30 && opcode != 0x31 && opcode != 0x32 && opcode != 0x33 &&
            opcode != 0x38 && opcode != 0x39 && opcode != 0x3a &&
            opcode != 0x3b && opcode != 0x84 && opcode != 0x85 &&
            opcode != 0x88 && opcode != 0x89 && opcode != 0x8a &&
            opcode != 0x8b && opcode != 0x8d && opcode != 0x63)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip)) {
        printk("[amd64-jit] bad-mem-op-next from=%#llx opcode=%#x next=%#llx\n",
               (unsigned long long) cpu->amd64_rip,
               opcode,
               (unsigned long long) next_ip);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_GPF;
    }

    if ((meta & AMD64_JIT_MEM_RIP_REL) != 0)
        addr += (qword_t) next_ip;
    if ((meta & AMD64_JIT_MEM_HAS_BASE) != 0)
        addr += cpu->amd64_regs[base];
    if ((meta & AMD64_JIT_MEM_HAS_INDEX) != 0)
        addr += cpu->amd64_regs[index] << scale;
    if ((meta & AMD64_JIT_MEM_FS) != 0 && opcode != 0x8d)
        addr += cpu->tls_ptr;

    switch (opcode) {
    case 0x00:
    case 0x01:
    case 0x08:
    case 0x09:
    case 0x10:
    case 0x11:
    case 0x18:
    case 0x19:
    case 0x20:
    case 0x21:
    case 0x28:
    case 0x29:
    case 0x30:
    case 0x31: {
        uint64_t tmp64;
        uint32_t tmp32;
        uint16_t tmp16;
        uint8_t tmp8;
        void *dst = size == 64 ? (void *) &tmp64 :
            (size == 32 ? (void *) &tmp32 :
             (size == 16 ? (void *) &tmp16 : (void *) &tmp8));
        // LOCK <alu> [mem], reg: one host-atomic RMW instead of the
        // read/compute/write below, which is not atomic against anything.
        if ((meta & AMD64_JIT_MEM_LOCK) != 0) {
            qword_t lrhs = size == 8
                ? amd64_reg_get_encoded8(cpu, reg, rex_present)
                : amd64_reg_get(cpu, reg, size);
            if (!amd64_locked_alu(cpu, tlb, addr, size, (opcode >> 3) & 7, lrhs)) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            break;
        }
        if (!amd64_mem_read(cpu, tlb, addr, dst, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        qword_t lhs = size == 64 ? tmp64 :
            (size == 32 ? tmp32 : (size == 16 ? tmp16 : tmp8));
        qword_t rhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        qword_t result;
        switch (opcode) {
        case 0x00:
        case 0x01:
            result = amd64_trunc(lhs + rhs, size);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x08:
        case 0x09:
            result = amd64_trunc(lhs | rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x10:
        case 0x11: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x18:
        case 0x19: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x20:
        case 0x21:
            result = amd64_trunc(lhs & rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x28:
        case 0x29:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x30:
        case 0x31:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        tmp64 = result;
        tmp32 = result;
        tmp16 = result;
        tmp8 = result;
        if (!amd64_mem_write(cpu, tlb, addr, dst, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        if (size == 64)
            amd64_trace_qword_store(cpu, cpu->amd64_rip, opcode, addr, result);
        break;
    }
    case 0x02:
    case 0x03:
    case 0x0a:
    case 0x0b:
    case 0x12:
    case 0x13:
    case 0x1a:
    case 0x1b:
    case 0x22:
    case 0x23:
    case 0x2a:
    case 0x2b:
    case 0x32:
    case 0x33: {
        if (!amd64_mem_read_value(cpu, tlb, addr, size, &value)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        qword_t lhs = size == 8
            ? amd64_reg_get_encoded8(cpu, reg, rex_present)
            : amd64_reg_get(cpu, reg, size);
        qword_t rhs = value;
        qword_t result;
        switch (opcode) {
        case 0x02:
        case 0x03:
            result = amd64_trunc(lhs + rhs, size);
            amd64_set_add_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x0a:
        case 0x0b:
            result = amd64_trunc(lhs | rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x12:
        case 0x13: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs + rhs + carry_in, size);
            amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x1a:
        case 0x1b: {
            unsigned carry_in = cpu->cf;
            result = amd64_trunc(lhs - rhs - carry_in, size);
            amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, size);
            break;
        }
        case 0x22:
        case 0x23:
            result = amd64_trunc(lhs & rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        case 0x2a:
        case 0x2b:
            result = amd64_trunc(lhs - rhs, size);
            amd64_set_sub_flags(cpu, lhs, rhs, result, size);
            break;
        case 0x32:
        case 0x33:
            result = amd64_trunc(lhs ^ rhs, size);
            amd64_set_logic_flags(cpu, result, size);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (size == 8)
            amd64_reg_set_encoded8(cpu, reg, rex_present, result);
        else
            amd64_reg_set(cpu, reg, size, result);
        break;
    }
    case 0x38: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        value = tmp;
        qword_t rhs = amd64_reg_get_encoded8(cpu, reg, rex_present);
        amd64_set_sub_flags(cpu, value, rhs, amd64_trunc(value - rhs, 8), 8);
        break;
    }
    case 0x39:
        if (!amd64_mem_read_value(cpu, tlb, addr, size, &value)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        {
            qword_t rhs = amd64_reg_get(cpu, reg, size);
            qword_t result = amd64_trunc(value - rhs, size);
            amd64_set_sub_flags(cpu, value, rhs, result, size);
            amd64_trace_cc1_cmp_probe(cpu, cpu->amd64_rip, addr, value, rhs, result, size);
        }
        break;
    case 0x3a: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        value = amd64_reg_get_encoded8(cpu, reg, rex_present);
        qword_t rhs = tmp;
        amd64_set_sub_flags(cpu, value, rhs, amd64_trunc(value - rhs, 8), 8);
        break;
    }
    case 0x3b:
        if (!amd64_mem_read_value(cpu, tlb, addr, size, &value)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        {
            qword_t lhs = amd64_reg_get(cpu, reg, size);
            amd64_set_sub_flags(cpu, lhs, value, amd64_trunc(lhs - value, size), size);
        }
        break;
    case 0x84: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        amd64_set_logic_flags(cpu, tmp & amd64_reg_get_encoded8(cpu, reg, rex_present), 8);
        break;
    }
    case 0x85:
        if (!amd64_mem_read_value(cpu, tlb, addr, size, &value)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        amd64_set_logic_flags(cpu, value & amd64_reg_get(cpu, reg, size), size);
        break;
    case 0x88: {
        uint8_t tmp = amd64_reg_get_encoded8(cpu, reg, rex_present);
        if (!amd64_mem_write(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        break;
    }
    case 0x89: {
        value = amd64_reg_get(cpu, reg, size);
        uint64_t tmp64 = value;
        uint32_t tmp32 = value;
        const void *src = size == 64 ? (const void *) &tmp64 : (const void *) &tmp32;
        if (!amd64_mem_write(cpu, tlb, addr, src, size / 8)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        if (size == 64)
            amd64_trace_qword_store(cpu, cpu->amd64_rip, opcode, addr, value);
        break;
    }
    case 0x8a: {
        uint8_t tmp;
        if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        amd64_reg_set_encoded8(cpu, reg, rex_present, tmp);
        break;
    }
    case 0x8b:
        if (!amd64_mem_read_value(cpu, tlb, addr, size, &value)) {
            amd64_sync_legacy_regs(cpu);
            return INT_PF;
        }
        if (size == 64)
            amd64_trace_cc1_slot_probe(cpu, cpu->amd64_rip, addr, value);
        amd64_reg_set(cpu, reg, size, value);
        break;
    case 0x8d:
        amd64_reg_set(cpu, reg, size, addr);
        break;
    case 0x63: {
        unsigned src_size = size == 64 ? 32 : size;
        if (src_size == 16) {
            uint16_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        } else {
            uint32_t tmp;
            if (!amd64_mem_read(cpu, tlb, addr, &tmp, sizeof(tmp))) {
                amd64_sync_legacy_regs(cpu);
                return INT_PF;
            }
            value = tmp;
        }
        amd64_reg_set(cpu, reg, size, (qword_t) amd64_sign_extend(value, src_size));
        break;
    }
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;
}

int amd64_jit_movx(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    byte_t byte;
    qword_t value;
    unsigned src_size;
    unsigned dst_size;

    if (op2 != 0xb6 && op2 != 0xb7 && op2 != 0xbe && op2 != 0xbf)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_movx_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_movx_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_movx_pf;

    src_size = (op2 == 0xb6 || op2 == 0xbe) ? 8 : 16;
    dst_size = rex.w ? 64 : 32;
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, src_size, &value))
        goto amd64_movx_pf;
    if (op2 == 0xbe || op2 == 0xbf)
        value = (qword_t) amd64_sign_extend(value, src_size);
    amd64_reg_set(cpu, modrm.reg, dst_size, value);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_movx_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_0f_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    bool repz_prefix = false;
    byte_t byte;
    unsigned op_size;

    if ((op2 != 0x1f && op2 != 0xa3 && op2 != 0xa4 && op2 != 0xa5 &&
                op2 != 0xab && op2 != 0xac && op2 != 0xad && op2 != 0xaf &&
                op2 != 0xae &&
                op2 != 0xb0 && op2 != 0xb1 &&
                op2 != 0xb3 && op2 != 0xba && op2 != 0xbb &&
                op2 != 0xbc && op2 != 0xbd &&
                op2 != 0xc0 && op2 != 0xc1) &&
            !(op2 >= 0x40 && op2 <= 0x4f) &&
            !(op2 >= 0x90 && op2 <= 0x9f))
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_0f_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0xf3) {
            repz_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_0f_rm_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_0f_rm_pf;

    op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    if (repz_prefix && op2 != 0xbc && op2 != 0xbd)
        return INT_UNDEFINED;
    if (op2 == 0x1f) {
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xae) {
        int interrupt = amd64_fxsave_op(cpu, tlb, &modrm, fs_prefix, saved_rip);
        if (interrupt != INT_NONE) {
            amd64_sync_legacy_regs(cpu);
            return interrupt;
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 >= 0x40 && op2 <= 0x4f) {
        qword_t src;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
            goto amd64_0f_rm_pf;
        if (amd64_cond_eval(cpu, op2 & 0xf))
            amd64_reg_set(cpu, modrm.reg, op_size, src);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 >= 0x90 && op2 <= 0x9f) {
        qword_t value = amd64_cond_eval(cpu, op2 & 0xf) ? 1 : 0;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, value))
            goto amd64_0f_rm_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xa4 || op2 == 0xa5 || op2 == 0xac || op2 == 0xad) {
        qword_t lhs, rhs, result;
        unsigned count;
        if (lock_prefix)
            return INT_UNDEFINED;
        if (op2 == 0xa4 || op2 == 0xac) {
            uint8_t imm8;
            if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
                goto amd64_0f_rm_pf;
            count = imm8 & (op_size == 64 ? 0x3f : 0x1f);
        } else {
            count = amd64_reg_get(cpu, amd64_rcx, 8) & (op_size == 64 ? 0x3f : 0x1f);
        }
        if (count != 0) {
            if (count > op_size)
                count %= op_size;
            if (count == 0)
                goto amd64_0f_rm_done;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
                goto amd64_0f_rm_pf;
            rhs = amd64_reg_get(cpu, modrm.reg, op_size);
            if (op2 == 0xa4 || op2 == 0xa5) {
                result = amd64_trunc((lhs << count) | (rhs >> (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_0f_rm_pf;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, true);
            } else {
                result = amd64_trunc((amd64_trunc(lhs, op_size) >> count) |
                        (rhs << (op_size - count)), op_size);
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
                    goto amd64_0f_rm_pf;
                amd64_set_double_shift_flags(cpu, lhs, result, op_size, count, false);
            }
        }
amd64_0f_rm_done:
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xbc || op2 == 0xbd) {
        qword_t src;
        qword_t src_masked;
        qword_t index;
        bool count_zeroes = repz_prefix;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &src))
            goto amd64_0f_rm_pf;
        src_masked = amd64_trunc(src, op_size);
        collapse_flags(cpu);
        if (count_zeroes) {
            cpu->cf = src_masked == 0;
            cpu->cf_bit = cpu->cf;
            cpu->zf = 0;
        } else {
            cpu->zf = src_masked == 0;
        }
        cpu->zf_res = 0;
        if (src_masked == 0) {
            if (count_zeroes)
                amd64_reg_set(cpu, modrm.reg, op_size, op_size);
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        if (op2 == 0xbc) {
            index = (op_size == 64)
                    ? (qword_t) __builtin_ctzll(src_masked)
                    : (qword_t) __builtin_ctz((uint32_t) src_masked);
        } else if (count_zeroes) {
            // LZCNT: leading-zero count relative to the operand WIDTH, not
            // the BSR bit-index (see the mirrored fix in the interp path
            // above, found via tests/remote/corpus/popcnt_lzcnt_tzcnt.c).
            index = (op_size == 64)
                    ? (qword_t) __builtin_clzll(src_masked)
                    : (qword_t) (__builtin_clz((uint32_t) src_masked) - (32 - op_size));
        } else {
            index = (op_size == 64)
                    ? (qword_t) (63 - __builtin_clzll(src_masked))
                    : (qword_t) (31 - __builtin_clz((uint32_t) src_masked));
        }
        if (count_zeroes)
            cpu->zf = index == 0;
        amd64_reg_set(cpu, modrm.reg, op_size, index);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xa3) {
        qword_t lhs;
        qword_t addr;
        qword_t bit;
        qword_t bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                bit_index, true, true, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        (void) addr;
        collapse_flags(cpu);
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
        qword_t addr;
        qword_t lhs, result;
        qword_t bit;
        qword_t bit_index = amd64_reg_get(cpu, modrm.reg, op_size);
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size,
                bit_index, true, true, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        collapse_flags(cpu);
        if (lock_prefix && !modrm.is_reg) {
            unsigned bop = op2 == 0xab ? 0 : (op2 == 0xb3 ? 1 : 2);
            if (!amd64_locked_bitop(cpu, tlb, addr, op_size, bop,
                        1ull << bit, &lhs))
                goto amd64_0f_rm_pf;
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            cpu->cf_bit = cpu->cf;
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        result = lhs;
        switch (op2) {
        case 0xab:
            result = amd64_trunc(lhs | (1ull << bit), op_size);
            break;
        case 0xb3:
            result = amd64_trunc(lhs & ~(1ull << bit), op_size);
            break;
        case 0xbb:
            result = amd64_trunc(lhs ^ (1ull << bit), op_size);
            break;
        }
        if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
            goto amd64_0f_rm_pf;
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xba) {
        qword_t addr;
        qword_t lhs, result;
        qword_t bit;
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_0f_rm_pf;
        if (modrm.reg < 4 || modrm.reg > 7)
            return INT_UNDEFINED;
        if (!amd64_read_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, imm8,
                true, false, &lhs, &addr, &bit))
            goto amd64_0f_rm_pf;
        collapse_flags(cpu);
        if (lock_prefix && !modrm.is_reg && modrm.reg != 4) {
            if (!amd64_locked_bitop(cpu, tlb, addr, op_size, modrm.reg - 5,
                        1ull << bit, &lhs))
                goto amd64_0f_rm_pf;
            cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
            cpu->cf_bit = cpu->cf;
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        cpu->cf = (amd64_trunc(lhs, op_size) >> bit) & 1;
        result = lhs;
        switch (modrm.reg) {
        case 4:
            break;
        case 5:
            result = amd64_trunc(lhs | (1ull << bit), op_size);
            break;
        case 6:
            result = amd64_trunc(lhs & ~(1ull << bit), op_size);
            break;
        case 7:
            result = amd64_trunc(lhs ^ (1ull << bit), op_size);
            break;
        }
        if (modrm.reg != 4) {
            if (!amd64_write_bt_operand(cpu, tlb, &modrm, fs_prefix, op_size, addr, result))
                goto amd64_0f_rm_pf;
        }
        cpu->cf_bit = cpu->cf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xaf) {
        qword_t rhs, lhs, result;
        __int128_t full;
        bool overflow;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &rhs))
            goto amd64_0f_rm_pf;
        lhs = amd64_reg_get(cpu, modrm.reg, op_size);
        // 128-bit product so 64-bit signed overflow is detectable; a 64-bit
        // product truncates, leaving CF/OF always clear.
        full = (__int128_t) (sqword_t) amd64_sign_extend(lhs, op_size) *
               (__int128_t) (sqword_t) amd64_sign_extend(rhs, op_size);
        result = amd64_trunc((qword_t) full, op_size);
        amd64_reg_set(cpu, modrm.reg, op_size, result);
        overflow = full != (__int128_t) (sqword_t) amd64_sign_extend(result, op_size);
        amd64_set_mul_flags(cpu, overflow);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xc0 || op2 == 0xc1) {
        unsigned xadd_size = op2 == 0xc0 ? 8 : op_size;
        qword_t lhs, rhs, result;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        rhs = op2 == 0xc0
                ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                : amd64_reg_get(cpu, modrm.reg, xadd_size);
        if (atomic_locked) {
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            if (!amd64_locked_xadd(cpu, tlb, addr, xadd_size, rhs, &lhs, &result))
                goto amd64_0f_rm_pf;
        } else {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, &lhs))
                goto amd64_0f_rm_pf;
            result = amd64_trunc(lhs + rhs, xadd_size);
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, xadd_size, result))
                goto amd64_0f_rm_pf;
        }
        if (op2 == 0xc0)
            amd64_reg_set_encoded8(cpu, modrm.reg, modrm.rex_present, lhs);
        else
            amd64_reg_set(cpu, modrm.reg, xadd_size, lhs);
        amd64_set_add_flags(cpu, lhs, rhs, result, xadd_size);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (op2 == 0xb0 || op2 == 0xb1) {
        unsigned cmpxchg_size = op2 == 0xb0 ? 8 : op_size;
        qword_t dst, src, acc, result;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        src = op2 == 0xb0
                ? amd64_reg_get_encoded8(cpu, modrm.reg, modrm.rex_present)
                : amd64_reg_get(cpu, modrm.reg, cmpxchg_size);
        acc = amd64_reg_get(cpu, amd64_rax, cmpxchg_size);
        if (atomic_locked) {
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            bool swapped = false;
            if (!amd64_locked_cmpxchg(cpu, tlb, addr, cmpxchg_size, acc, src,
                        &dst, &swapped))
                goto amd64_0f_rm_pf;
            result = amd64_trunc(acc - dst, cmpxchg_size);
            amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
            if (!swapped)
                amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
            cpu->zf = swapped;
            cpu->zf_res = 0;
        } else {
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, &dst))
            goto amd64_0f_rm_pf;
        result = amd64_trunc(acc - dst, cmpxchg_size);
        amd64_set_sub_flags(cpu, acc, dst, result, cmpxchg_size);
        if (acc == dst) {
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, cmpxchg_size, src))
                goto amd64_0f_rm_pf;
            cpu->zf = 1;
            cpu->zf_res = 0;
        } else {
            amd64_reg_set(cpu, amd64_rax, cmpxchg_size, dst);
            cpu->zf = 0;
            cpu->zf_res = 0;
        }
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    return INT_UNDEFINED;

amd64_0f_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

// Instrumentation: every 0F vector op the amd64 JIT can't do as a native gadget
// is COMPILED into a bridge to this C helper (no compile-fallback — it runs
// per-op at runtime). Count by op2 so we can rank which vector ops are worth a
// real gadget. ISH_TRACE_AMD64_JIT_STATS=1.
//
// This comment used to claim "that bridge is the dominant amd64-JIT cost". It is
// not, and the claim was load-bearing enough to misdirect optimization work, so:
// measured on an -O2 build, the amd64_jit_0f_vec_rm subtree is 0.68% of busy
// samples on `gcc -O2 -S`, and 0% on a busybox shell loop and on python3.
// The counter here is what made it look dominant -- it records >1,048,576 bridges
// per cc1 compile (48% `0f db` PAND, 46% `0f df` PANDN) -- but a large COUNT of a
// cheap operation is not a large share of TIME. The genuinely dominant cost in
// this engine is the OTHER bridge family, gadget_helper_tlb_N_retint (41.6% of
// busy time on a shell loop, 28.0% on python), whose helpers re-decode their own
// instruction in C on every execution. Rank by profile share, not by counter.
static unsigned long amd64_jit_vec_bridge_by_op2[256];
static unsigned long amd64_jit_vec_bridge_total;

static void amd64_jit_note_vec_bridge(unsigned long op2) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT_STATS") != NULL ? 1 : 0;
    if (!enabled)
        return;
    __atomic_fetch_add(&amd64_jit_vec_bridge_by_op2[op2 & 0xff], 1, __ATOMIC_RELAXED);
    unsigned long total = __atomic_add_fetch(&amd64_jit_vec_bridge_total, 1, __ATOMIC_RELAXED);
    if (total < 256 || (total & (total - 1)) != 0)
        return; // dump a sorted ranking at each power of two >= 256
    struct { unsigned op2; unsigned long count; } ent[256];
    unsigned n = 0;
    for (unsigned k = 0; k < 256; k++) {
        unsigned long c = __atomic_load_n(&amd64_jit_vec_bridge_by_op2[k], __ATOMIC_RELAXED);
        if (c != 0) { ent[n].op2 = k; ent[n].count = c; n++; }
    }
    for (unsigned i = 1; i < n; i++) {
        unsigned o = ent[i].op2; unsigned long c = ent[i].count; unsigned j = i;
        while (j > 0 && ent[j - 1].count < c) { ent[j] = ent[j - 1]; j--; }
        ent[j].op2 = o; ent[j].count = c;
    }
    fprintf(stderr, "[amd64-jit-bridge] vec-bridges=%lu distinct-op2=%u (top by frequency):\n", total, n);
    unsigned show = n < 20 ? n : 20;
    for (unsigned i = 0; i < show; i++) {
        unsigned long pm = 1000UL * ent[i].count / total;
        fprintf(stderr, "[amd64-jit-bridge]   0f %02x  count=%lu  (%lu.%lu%%)\n",
               ent[i].op2, ent[i].count, pm / 10, pm % 10);
    }
}

// --- Packed/scalar SSE floating-point helpers with x86-faithful semantics ---
//
// These back the sqrt/reciprocal/conversion families decoded below (0F 51/52/53,
// 0F 5A packed, 0F 5B, 0F E6). The host is arm64, whose FP corner cases differ
// from x86 in two places we must correct: the sign of a sqrt-of-negative QNaN,
// and float->int overflow saturation. Everything else (NaN propagation, the
// quieting of widened/narrowed NaNs, round-to-nearest of in-range values) is
// IEEE-754 and already matches between arm64 and x86.

// x86 SQRT of a negative (non-NaN) operand returns the real-indefinite QNaN
// 0xffc00000 / 0xfff8000000000000 (sign bit set). arm64's sqrtf/sqrt yield a
// positive-signed NaN there, so substitute the x86 indefinite explicitly.
// -0.0 is not < 0 (sqrt(-0)=-0) and NaN inputs are not < 0 (sqrtf quiets them):
// both fall through to the host, which already matches x86.
static inline float amd64_sse_sqrt_f32(float x) {
    if (x < 0.0f) {
        uint32_t bits = 0xffc00000u;
        float r;
        memcpy(&r, &bits, sizeof(r));
        return r;
    }
    return sqrtf(x);
}
static inline double amd64_sse_sqrt_f64(double x) {
    if (x < 0.0) {
        uint64_t bits = 0xfff8000000000000ull;
        double r;
        memcpy(&r, &bits, sizeof(r));
        return r;
    }
    return sqrt(x);
}

// RSQRTPS/RSQRTSS and RCPPS/RCPSS are defined by the ISA only to ~11-12 bits of
// precision with an implementation-specific result, so no bit-exact reference
// exists across CPUs (or vs. Rosetta). We return the precisely-rounded
// reciprocal: error 0 is well within the architectural 1.5*2^-12 tolerance, and
// all the special values (0, -0, +/-inf, NaN, negative) match real hardware
// exactly. Code that uses these as Newton-Raphson seeds converges either way.
static inline float amd64_sse_rsqrt_f32(float x) {
    if (x < 0.0f) {
        uint32_t bits = 0xffc00000u; // rsqrt(-x) follows sqrt(-x) -> QNaN indefinite
        float r;
        memcpy(&r, &bits, sizeof(r));
        return r;
    }
    return 1.0f / sqrtf(x); // +0 -> +inf, -0 -> -inf, +inf -> +0, NaN -> NaN
}
static inline float amd64_sse_rcp_f32(float x) {
    return 1.0f / x; // 0 -> +inf, -0 -> -inf, +/-inf -> +/-0, NaN -> NaN
}

// x86 float/double -> signed int32 conversion. Out-of-range or NaN yields the
// integer indefinite 0x80000000 (NOT arm64's saturated INT_MAX/INT_MIN, which
// is why we cannot lean on a bare C cast). cvtt* truncates toward zero; cvt*
// rounds. There is no MXCSR in this emulator, so the rounding form uses the host
// default rounding direction (round-to-nearest-even) which is the x86 power-on
// MXCSR.RC; nothing in the runtime ever calls fesetround.
static inline int32_t amd64_sse_f2i_trunc(double x) {
    if (isnan(x) || x >= 2147483648.0 || x < -2147483648.0)
        return INT32_MIN;
    return (int32_t) x;
}
static inline int32_t amd64_sse_f2i_round(double x) {
    if (isnan(x))
        return INT32_MIN;
    double r = rint(x);
    if (r >= 2147483648.0 || r < -2147483648.0)
        return INT32_MIN;
    return (int32_t) r;
}

// sqrtps/sqrtpd: read all source lanes first (src may alias dst).
static inline void amd64_sse_sqrtps(const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->f32[0] = amd64_sse_sqrt_f32(s0);
    dst->f32[1] = amd64_sse_sqrt_f32(s1);
    dst->f32[2] = amd64_sse_sqrt_f32(s2);
    dst->f32[3] = amd64_sse_sqrt_f32(s3);
}
static inline void amd64_sse_sqrtpd(const union xmm_reg *src, union xmm_reg *dst) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->f64[0] = amd64_sse_sqrt_f64(s0);
    dst->f64[1] = amd64_sse_sqrt_f64(s1);
}
static inline void amd64_sse_rsqrtps(const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->f32[0] = amd64_sse_rsqrt_f32(s0);
    dst->f32[1] = amd64_sse_rsqrt_f32(s1);
    dst->f32[2] = amd64_sse_rsqrt_f32(s2);
    dst->f32[3] = amd64_sse_rsqrt_f32(s3);
}
static inline void amd64_sse_rcpps(const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->f32[0] = amd64_sse_rcp_f32(s0);
    dst->f32[1] = amd64_sse_rcp_f32(s1);
    dst->f32[2] = amd64_sse_rcp_f32(s2);
    dst->f32[3] = amd64_sse_rcp_f32(s3);
}
// cvtps2dq/cvttps2dq: four packed floats -> four int32 (same lane count).
static inline void amd64_sse_cvtps2dq(const union xmm_reg *src, union xmm_reg *dst, bool trunc) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->u32[0] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s0) : amd64_sse_f2i_round(s0));
    dst->u32[1] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s1) : amd64_sse_f2i_round(s1));
    dst->u32[2] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s2) : amd64_sse_f2i_round(s2));
    dst->u32[3] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s3) : amd64_sse_f2i_round(s3));
}
// cvtpd2dq/cvttpd2dq: two packed doubles -> two int32 in the low 64 bits;
// the high 64 bits are zeroed.
static inline void amd64_sse_cvtpd2dq(const union xmm_reg *src, union xmm_reg *dst, bool trunc) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->u32[0] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s0) : amd64_sse_f2i_round(s0));
    dst->u32[1] = (uint32_t) (trunc ? amd64_sse_f2i_trunc(s1) : amd64_sse_f2i_round(s1));
    dst->u32[2] = 0;
    dst->u32[3] = 0;
}

int amd64_jit_0f_vec_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip) {
    amd64_jit_note_vec_bridge(op2);
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    enum amd64_rep_mode rep_mode = AMD64_REP_NONE;
    byte_t byte;
    union xmm_reg value, src_xmm;
    union mm_reg value_mm, src_mm;
    qword_t src_scalar;
    uint8_t imm8;

    if (op2 != 0x10 && op2 != 0x11 && op2 != 0x12 && op2 != 0x13 &&
            op2 != 0x14 && op2 != 0x15 && op2 != 0x16 && op2 != 0x17 &&
            op2 != 0x2a && op2 != 0x2c && op2 != 0x2d && op2 != 0x2e && op2 != 0x2f &&
            op2 != 0x28 &&
            op2 != 0x29 && op2 != 0x50 && !(op2 >= 0x51 && op2 <= 0x5b) &&
            !(op2 >= 0x5c && op2 <= 0x5f) &&
            !(op2 >= 0x60 && op2 <= 0x62) &&
            op2 != 0x63 &&
            !(op2 >= 0x64 && op2 <= 0x66) &&
            op2 != 0x67 &&
            !(op2 >= 0x68 && op2 <= 0x6a) &&
            op2 != 0x6b &&
            op2 != 0x6c && op2 != 0x6d && op2 != 0x6e &&
            op2 != 0x6f && op2 != 0x70 && !(op2 >= 0x71 && op2 <= 0x73) &&
            !(op2 >= 0x74 && op2 <= 0x76) &&
            op2 != 0x7e && op2 != 0x7f &&
            op2 != 0xc2 &&
            op2 != 0xc4 && op2 != 0xc5 &&
            op2 != 0xc6 &&
            !(op2 >= 0xd1 && op2 <= 0xd3) &&
            op2 != 0xd4 && op2 != 0xd5 && op2 != 0xd6 && op2 != 0xd7 &&
            !(op2 >= 0xd8 && op2 <= 0xe0) &&
            op2 != 0xe1 && op2 != 0xe2 &&
            !(op2 >= 0xe3 && op2 <= 0xe5) &&
            op2 != 0xe6 && op2 != 0xe7 &&
            !(op2 >= 0xe8 && op2 <= 0xee) &&
            op2 != 0xdb && op2 != 0xeb &&
            !(op2 >= 0xf1 && op2 <= 0xf3) &&
            op2 != 0xf4 &&
            op2 != 0xf6 &&
            !(op2 >= 0xf8 && op2 <= 0xfe) &&
            op2 != 0xef)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_0f_vec_rm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte == 0xf2) {
            rep_mode = AMD64_REPNZ;
            continue;
        }
        if (byte == 0xf3) {
            rep_mode = AMD64_REPZ;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0x0f)
        return INT_UNDEFINED;
    if (!amd64_fetch_u8(cpu, tlb, &byte))
        goto amd64_0f_vec_rm_pf;
    if (byte != op2)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_0f_vec_rm_pf;
    if ((op2 == 0x70 || (op2 >= 0x71 && op2 <= 0x73) || op2 == 0xc2 ||
                op2 == 0xc4 || op2 == 0xc5 || op2 == 0xc6) &&
            !amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
        goto amd64_0f_vec_rm_pf;
    cpu->amd64_rip = (qword_t) next_ip;

    if (op2 == 0x6e) {
        if (operand_size_prefix) {
            if (modrm.reg >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            value.u128 = 0;
            if (rex.w)
                value.qw[0] = src_scalar;
            else
                value.u32[0] = (uint32_t) src_scalar;
            cpu->xmm[modrm.reg] = value;
        } else {
            if (modrm.reg >= 8)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            cpu->mm[modrm.reg].qw = rex.w ? src_scalar : (uint32_t) src_scalar;
        }
    } else {
        bool pshufw = op2 == 0x70 && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool movq_mm_load = op2 == 0x6f && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool movq_mm_store = op2 == 0x7f && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool movnt_mm_store = op2 == 0xe7 && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool pmovmskb_mm = op2 == 0xd7 && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool pcmpeq_mm = op2 >= 0x74 && op2 <= 0x76 && !operand_size_prefix &&
            rep_mode == AMD64_REP_NONE;
        bool pcmpgt_mm = op2 >= 0x64 && op2 <= 0x66 && !operand_size_prefix &&
            rep_mode == AMD64_REP_NONE;
        bool punpckldq_mm = op2 == 0x62 && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool logic_mm = (op2 == 0xdb || op2 == 0xeb || op2 == 0xef) &&
            !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool packed_int = op2 == 0xd4 || (op2 >= 0xf8 && op2 <= 0xfe);
        bool packed_int_mm = packed_int && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool packed_shift = (op2 >= 0xd1 && op2 <= 0xd3) || op2 == 0xe1 || op2 == 0xe2 ||
            (op2 >= 0xf1 && op2 <= 0xf3);
        bool packed_shift_mm = packed_shift && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool packed_imm_shift = op2 >= 0x71 && op2 <= 0x73;
        bool packed_mul = op2 == 0xd5 || op2 == 0xf4;
        bool packed_mul_mm = packed_mul && !operand_size_prefix && rep_mode == AMD64_REP_NONE;
        bool packed_xmm_misc = (op2 >= 0xd8 && op2 <= 0xe0) ||
            (op2 >= 0xe3 && op2 <= 0xe5) || (op2 >= 0xe8 && op2 <= 0xee);
        bool pack_xmm = op2 == 0x63 || op2 == 0x67 || op2 == 0x6b;
        // no-66 MMX forms of the packed-int ops the original decoder omitted:
        // punpck{l,h}{bw,wd} + punpckhdq (0x60/61/68/69/6a), pack ss/us
        // (0x63/67/6b), saturating add/sub (d8/d9/dc/dd/e8/e9/ec/ed), unsigned
        // min/max (da/de), signed min/max (ea/ee), pavg (e0/e3), pmulhuw (e4),
        // psadbw (f6). Handled by the consolidated block below; vec_*64 mirror
        // the XMM vec_*128. (0x62/64/65/66 are punpckldq/pcmpgt, already handled
        // above.) NOTE: pmaddwd (f5) is intentionally NOT here -- un-gating it in
        // the top guard exposed a JIT block-chaining bug where the instruction
        // *after* an MMX pmaddwd faults (next_ip is correct, yet the next block
        // dies); left as a follow-up. i386 pmaddwd works (decode.h).
        bool mmx_extra = !operand_size_prefix && rep_mode == AMD64_REP_NONE &&
            ((op2 >= 0x60 && op2 <= 0x6b && op2 != 0x62 && op2 != 0x64 &&
              op2 != 0x65 && op2 != 0x66) ||
             op2 == 0xd8 || op2 == 0xd9 || op2 == 0xda || op2 == 0xdc ||
             op2 == 0xdd || op2 == 0xde || op2 == 0xe0 || op2 == 0xe3 ||
             op2 == 0xe4 || op2 == 0xe8 || op2 == 0xe9 || op2 == 0xea ||
             op2 == 0xec || op2 == 0xed || op2 == 0xee || op2 == 0xf6);
        if (pshufw || movq_mm_load || movq_mm_store || movnt_mm_store || pcmpeq_mm || pcmpgt_mm ||
                punpckldq_mm || logic_mm || packed_int_mm || packed_shift_mm || packed_mul_mm ||
                mmx_extra) {
            if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
                return INT_UNDEFINED;
        } else if ((modrm.reg >= AMD64_XMM_COUNT) ||
                (modrm.is_reg && modrm.rm >= AMD64_XMM_COUNT &&
                 !(op2 == 0x7e && !operand_size_prefix && rep_mode == AMD64_REP_NONE)))
            return INT_UNDEFINED;
        if (mmx_extra) {
            if (modrm.is_reg) {
                src_mm = cpu->mm[modrm.rm];
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                    goto amd64_0f_vec_rm_pf;
                src_mm.qw = src_scalar;
            }
            value_mm = cpu->mm[modrm.reg];
            switch (op2) {
            case 0x60: vec_unpackl_bw64(NULL, &src_mm, &value_mm); break;
            case 0x61: vec_unpackl_w64(NULL, &src_mm, &value_mm); break;
            case 0x63: vec_packss_w64(NULL, &src_mm, &value_mm); break;
            case 0x67: vec_packsu_w64(NULL, &src_mm, &value_mm); break;
            case 0x68: vec_unpackh_bw64(NULL, &src_mm, &value_mm); break;
            case 0x69: vec_unpackh_w64(NULL, &src_mm, &value_mm); break;
            case 0x6a: vec_unpackh_d64(NULL, &src_mm, &value_mm); break;
            case 0x6b: vec_packss_d64(NULL, &src_mm, &value_mm); break;
            case 0xd8: vec_subus_b64(NULL, &src_mm, &value_mm); break;
            case 0xd9: vec_subus_w64(NULL, &src_mm, &value_mm); break;
            case 0xda: vec_min_ub64(NULL, &src_mm, &value_mm); break;
            case 0xdc: vec_addus_b64(NULL, &src_mm, &value_mm); break;
            case 0xdd: vec_addus_w64(NULL, &src_mm, &value_mm); break;
            case 0xde: vec_max_ub64(NULL, &src_mm, &value_mm); break;
            case 0xe0: vec_avg_b64(NULL, &src_mm, &value_mm); break;
            case 0xe3: vec_avg_w64(NULL, &src_mm, &value_mm); break;
            case 0xe4: vec_muluu64(NULL, &src_mm, &value_mm); break;
            case 0xe8: vec_subss_b64(NULL, &src_mm, &value_mm); break;
            case 0xe9: vec_subss_w64(NULL, &src_mm, &value_mm); break;
            case 0xea: vec_mins_w64(NULL, &src_mm, &value_mm); break;
            case 0xec: vec_addss_b64(NULL, &src_mm, &value_mm); break;
            case 0xed: vec_addss_w64(NULL, &src_mm, &value_mm); break;
            case 0xee: vec_maxs_w64(NULL, &src_mm, &value_mm); break;
            case 0xf6: vec_sumabs_w64(NULL, &src_mm, &value_mm); break;
            }
            cpu->mm[modrm.reg] = value_mm;
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        if (op2 == 0x10 || op2 == 0x28) {
            if (op2 == 0x10 && rep_mode == AMD64_REPZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.u32[0] = cpu->xmm[modrm.rm].u32[0];
                } else {
                    value.u128 = 0;
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.u32[0] = (uint32_t) src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (op2 == 0x10 && rep_mode == AMD64_REPNZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    value.u128 = 0;
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
                cpu->xmm[modrm.reg] = value;
            }
        } else if (op2 == 0x2a) {
            if (operand_size_prefix ||
                    (rep_mode != AMD64_REPZ && rep_mode != AMD64_REPNZ))
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                value.f64[0] = rex.w ? (double) (sqword_t) src_scalar
                                     : (double) (int32_t) src_scalar;
            } else {
                value.f32[0] = rex.w ? (float) (sqword_t) src_scalar
                                     : (float) (int32_t) src_scalar;
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x2c || op2 == 0x2d) {
            qword_t result;
            if (operand_size_prefix ||
                    (rep_mode != AMD64_REPZ && rep_mode != AMD64_REPNZ))
                return INT_UNDEFINED;
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_double = *(double *) &src_scalar;
                }
                result = (op2 == 0x2d) ? amd64_cvt_scalar_to_int(src_double, rex.w)
                                       : amd64_cvtt_scalar_to_int(src_double, rex.w);
            } else {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                result = (op2 == 0x2d) ? amd64_cvt_scalar_to_int((double) src_float, rex.w)
                                       : amd64_cvtt_scalar_to_int((double) src_float, rex.w);
            }
            amd64_reg_set(cpu, modrm.reg, rex.w ? 64 : 32, result);
        } else if (op2 == 0x2e || op2 == 0x2f) {
            if (rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (operand_size_prefix) {
                double lhs = cpu->xmm[modrm.reg].f64[0];
                double rhs;
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    rhs = *(double *) &src_scalar;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            } else {
                float lhs = cpu->xmm[modrm.reg].f32[0];
                float rhs;
                uint32_t src_word;
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_word = (uint32_t) src_scalar;
                    rhs = *(float *) &src_word;
                }
                amd64_set_fp_compare_flags(cpu, lhs < rhs ? -1 : (lhs > rhs ? 1 : 0),
                        isnan(lhs) || isnan(rhs));
            }
        } else if (op2 == 0x50) {
            uint32_t mask = 0;
            if (rep_mode != AMD64_REP_NONE || !modrm.is_reg || modrm.rm >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            src_xmm = cpu->xmm[modrm.rm];
            if (operand_size_prefix) {
                mask = ((src_xmm.qw[0] >> 63) & 1u) |
                    (((src_xmm.qw[1] >> 63) & 1u) << 1);
            } else {
                for (int i = 0; i < 4; i++)
                    mask |= ((src_xmm.u32[i] >> 31) & 1u) << i;
            }
            amd64_reg_set(cpu, modrm.reg, 32, mask);
        } else if (op2 == 0x5a) {
            // F2=cvtsd2ss, F3=cvtss2sd (scalar); 66=cvtpd2ps, none=cvtps2pd (packed).
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPNZ) {
                double src_double;
                if (modrm.is_reg) {
                    src_double = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_double = *(double *) &src_scalar;
                }
                value.f32[0] = (float) src_double;
            } else if (rep_mode == AMD64_REPZ) {
                float src_float;
                uint32_t src_word;
                if (modrm.is_reg) {
                    src_float = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_word = (uint32_t) src_scalar;
                    src_float = *(float *) &src_word;
                }
                value.f64[0] = (double) src_float;
            } else if (operand_size_prefix) {
                // cvtpd2ps: two doubles (xmm/m128) -> two floats, high 64 zeroed.
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                vec_cvtpd2ps128(NULL, &src_xmm, &value);
            } else {
                // cvtps2pd: two floats (low 64 of xmm, or m64) -> two doubles.
                if (modrm.is_reg) {
                    src_xmm = cpu->xmm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_xmm.qw[0] = src_scalar;
                }
                vec_cvtps2pd64(NULL, &src_xmm, &value);
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x51) {
            // sqrt: none=sqrtps, 66=sqrtpd, F3=sqrtss, F2=sqrtsd.
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPZ) {
                float s;
                if (modrm.is_reg) {
                    s = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    uint32_t w = (uint32_t) src_scalar;
                    s = *(float *) &w;
                }
                value.f32[0] = amd64_sse_sqrt_f32(s);
            } else if (rep_mode == AMD64_REPNZ) {
                double s;
                if (modrm.is_reg) {
                    s = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    s = *(double *) &src_scalar;
                }
                value.f64[0] = amd64_sse_sqrt_f64(s);
            } else {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                if (operand_size_prefix)
                    amd64_sse_sqrtpd(&src_xmm, &value);
                else
                    amd64_sse_sqrtps(&src_xmm, &value);
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x52 || op2 == 0x53) {
            // 0x52 rsqrt, 0x53 rcp: none=packed-ps, F3=scalar-ss; no 66/F2 form.
            if (operand_size_prefix || rep_mode == AMD64_REPNZ)
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPZ) {
                float s;
                if (modrm.is_reg) {
                    s = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    uint32_t w = (uint32_t) src_scalar;
                    s = *(float *) &w;
                }
                value.f32[0] = op2 == 0x52 ? amd64_sse_rsqrt_f32(s)
                                           : amd64_sse_rcp_f32(s);
            } else {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                if (op2 == 0x52)
                    amd64_sse_rsqrtps(&src_xmm, &value);
                else
                    amd64_sse_rcpps(&src_xmm, &value);
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x5b) {
            // none=cvtdq2ps, 66=cvtps2dq (round), F3=cvttps2dq (truncate).
            if (rep_mode == AMD64_REPNZ)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPZ)
                amd64_sse_cvtps2dq(&src_xmm, &value, true);
            else if (operand_size_prefix)
                amd64_sse_cvtps2dq(&src_xmm, &value, false);
            else
                vec_cvtdq2ps128(NULL, &src_xmm, &value);
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xe6) {
            // F3=cvtdq2pd (m64 src), 66=cvttpd2dq (truncate), F2=cvtpd2dq (round).
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPZ) {
                if (modrm.is_reg) {
                    src_xmm = cpu->xmm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_xmm.qw[0] = src_scalar;
                }
                vec_cvtdq2pd64(NULL, &src_xmm, &value);
            } else if (operand_size_prefix) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                amd64_sse_cvtpd2dq(&src_xmm, &value, true);
            } else if (rep_mode == AMD64_REPNZ) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                amd64_sse_cvtpd2dq(&src_xmm, &value, false);
            } else {
                return INT_UNDEFINED; // no-prefix 0F E6 is not a valid encoding
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x58 || op2 == 0x59 || op2 == 0x5c || op2 == 0x5d ||
                op2 == 0x5e || op2 == 0x5f) {
            value = cpu->xmm[modrm.reg];
            if (rep_mode == AMD64_REPZ) {
                float lhs, rhs;
                uint32_t src_word;
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                lhs = value.f32[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f32[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 32, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_word = (uint32_t) src_scalar;
                    rhs = *(float *) &src_word;
                }
                switch (op2) {
                case 0x58:
                    value.f32[0] = lhs + rhs;
                    break;
                case 0x59:
                    value.f32[0] = lhs * rhs;
                    break;
                case 0x5c:
                    value.f32[0] = lhs - rhs;
                    break;
                case 0x5d:
                    value.f32[0] = lhs < rhs ? lhs : rhs;
                    break;
                case 0x5e:
                    value.f32[0] = lhs / rhs;
                    break;
                case 0x5f:
                    value.f32[0] = lhs > rhs ? lhs : rhs;
                    break;
                }
            } else if (rep_mode == AMD64_REPNZ) {
                double lhs, rhs;
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                lhs = value.f64[0];
                if (modrm.is_reg) {
                    rhs = cpu->xmm[modrm.rm].f64[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    rhs = *(double *) &src_scalar;
                }
                switch (op2) {
                case 0x58:
                    value.f64[0] = lhs + rhs;
                    break;
                case 0x59:
                    value.f64[0] = lhs * rhs;
                    break;
                case 0x5c:
                    value.f64[0] = lhs - rhs;
                    break;
                case 0x5d:
                    value.f64[0] = lhs < rhs ? lhs : rhs;
                    break;
                case 0x5e:
                    value.f64[0] = lhs / rhs;
                    break;
                case 0x5f:
                    value.f64[0] = lhs > rhs ? lhs : rhs;
                    break;
                }
            } else {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                if (operand_size_prefix) {
                    switch (op2) {
                    case 0x58:
                        vec_add_p64(NULL, &src_xmm, &value);
                        break;
                    case 0x59:
                        vec_mul_p64(NULL, &src_xmm, &value);
                        break;
                    case 0x5c:
                        vec_sub_p64(NULL, &src_xmm, &value);
                        break;
                    case 0x5d:
                        value.f64[0] = value.f64[0] < src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                        value.f64[1] = value.f64[1] < src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                        break;
                    case 0x5e:
                        value.f64[0] /= src_xmm.f64[0];
                        value.f64[1] /= src_xmm.f64[1];
                        break;
                    case 0x5f:
                        value.f64[0] = value.f64[0] > src_xmm.f64[0] ? value.f64[0] : src_xmm.f64[0];
                        value.f64[1] = value.f64[1] > src_xmm.f64[1] ? value.f64[1] : src_xmm.f64[1];
                        break;
                    }
                } else {
                    switch (op2) {
                    case 0x58:
                        vec_add_p32(NULL, &src_xmm, &value);
                        break;
                    case 0x59:
                        vec_mul_p32(NULL, &src_xmm, &value);
                        break;
                    case 0x5c:
                        vec_sub_p32(NULL, &src_xmm, &value);
                        break;
                    case 0x5d:
                        for (int i = 0; i < 4; i++)
                            value.f32[i] = value.f32[i] < src_xmm.f32[i] ? value.f32[i] : src_xmm.f32[i];
                        break;
                    case 0x5e:
                        for (int i = 0; i < 4; i++)
                            value.f32[i] /= src_xmm.f32[i];
                        break;
                    case 0x5f:
                        for (int i = 0; i < 4; i++)
                            value.f32[i] = value.f32[i] > src_xmm.f32[i] ? value.f32[i] : src_xmm.f32[i];
                        break;
                    }
                }
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x12) {
            if (rep_mode == AMD64_REPZ) {
                // movsldup (F3 0F 12)
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                vec_movsldup128(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
            } else if (rep_mode == AMD64_REPNZ) {
                // movddup (F2 0F 12), mem reads m64
                if (modrm.is_reg) {
                    src_xmm = cpu->xmm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_xmm.qw[0] = src_scalar;
                }
                vec_movddup64(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
            } else {
                if (operand_size_prefix && modrm.is_reg)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[1];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            }
        } else if (op2 == 0x13) {
            // movlps (NP) / movlpd (66) m64, xmm: both store xmm[63:0]; accept
            // the 66 (movlpd) form (was wrongly #UD'd). reg form is #UD.
            if (rep_mode != AMD64_REP_NONE || modrm.is_reg)
                return INT_UNDEFINED;
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                goto amd64_0f_vec_rm_pf;
        } else if (op2 == 0x14 || op2 == 0x15) {
            if (rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (operand_size_prefix) {
                // unpcklpd/unpckhpd: interleave the 64-bit lanes.
                if (op2 == 0x14) {
                    value.qw[1] = src_xmm.qw[0];
                } else {
                    value.qw[0] = value.qw[1];
                    value.qw[1] = src_xmm.qw[1];
                }
            } else {
                // unpcklps/unpckhps: interleave the 32-bit lanes.
                if (op2 == 0x14)
                    vec_unpackl_ps128(NULL, &src_xmm, &value);
                else
                    vec_unpackh_ps128(NULL, &src_xmm, &value);
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0x16) {
            if (rep_mode == AMD64_REPZ) {
                // movshdup (F3 0F 16)
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                vec_movshdup128(NULL, &src_xmm, &cpu->xmm[modrm.reg]);
            } else if (rep_mode != AMD64_REP_NONE) {
                return INT_UNDEFINED;
            } else {
                if (operand_size_prefix && modrm.is_reg)
                    return INT_UNDEFINED;
                value = cpu->xmm[modrm.reg];
                if (modrm.is_reg) {
                    value.qw[1] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.qw[1] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            }
        } else if (op2 == 0x17) {
            // movhps (NP) / movhpd (66) m64, xmm: both store xmm[127:64]; accept
            // the 66 (movhpd) form (was wrongly #UD'd). reg form is #UD.
            if (modrm.is_reg)
                return INT_UNDEFINED;
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[1]))
                goto amd64_0f_vec_rm_pf;
        } else if (op2 == 0x11 || op2 == 0x29) {
            if (op2 == 0x11 && rep_mode == AMD64_REPZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                if (modrm.is_reg) {
                    cpu->xmm[modrm.rm].u32[0] = cpu->xmm[modrm.reg].u32[0];
                } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 32,
                            cpu->xmm[modrm.reg].u32[0])) {
                    goto amd64_0f_vec_rm_pf;
                }
            } else if (op2 == 0x11 && rep_mode == AMD64_REPNZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                if (modrm.is_reg) {
                    cpu->xmm[modrm.rm].qw[0] = cpu->xmm[modrm.reg].qw[0];
                } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                            cpu->xmm[modrm.reg].qw[0])) {
                    goto amd64_0f_vec_rm_pf;
                }
            } else {
                value = cpu->xmm[modrm.reg];
                if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
            }
        } else if (op2 == 0x6f) {
            if (operand_size_prefix || rep_mode == AMD64_REPZ) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
                cpu->xmm[modrm.reg] = value;
            } else if (movq_mm_load) {
                if (modrm.is_reg) {
                    cpu->mm[modrm.reg] = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    cpu->mm[modrm.reg].qw = src_scalar;
                }
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0x70) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = src_xmm;
                vec_shuffle_d128(NULL, &src_xmm, &value, imm8);
                cpu->xmm[modrm.reg] = value;
            } else if (!operand_size_prefix && rep_mode == AMD64_REPNZ) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = src_xmm;
                vec_shuffle_lw128(NULL, &src_xmm, &value, imm8);
                cpu->xmm[modrm.reg] = value;
            } else if (!operand_size_prefix && rep_mode == AMD64_REPZ) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = src_xmm;
                vec_shuffle_hw128(NULL, &src_xmm, &value, imm8);
                cpu->xmm[modrm.reg] = value;
            } else if (pshufw) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = src_mm;
                vec_shuffle_w64(NULL, &src_mm, &value_mm, imm8);
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (pack_xmm) {
            if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (op2 == 0x63)
                vec_packss_w128(NULL, &src_xmm, &value);
            else if (op2 == 0x67)
                vec_packsu_w128(NULL, &src_xmm, &value);
            else
                vec_packss_d128(NULL, &src_xmm, &value);
            cpu->xmm[modrm.reg] = value;
        } else if (op2 >= 0x74 && op2 <= 0x76) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x74:
                    vec_compare_eqb128(NULL, &src_xmm, &value);
                    break;
                case 0x75:
                    vec_compare_eqw128(NULL, &src_xmm, &value);
                    break;
                case 0x76:
                    vec_compare_eqd128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (pcmpeq_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                switch (op2) {
                case 0x74:
                    vec_compare_eqb64(NULL, &src_mm, &value_mm);
                    break;
                case 0x75:
                    vec_compare_eqw64(NULL, &src_mm, &value_mm);
                    break;
                case 0x76:
                    vec_compare_eqd64(NULL, &src_mm, &value_mm);
                    break;
                }
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 >= 0x64 && op2 <= 0x66) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x64:
                    vec_compares_gtb128(NULL, &src_xmm, &value);
                    break;
                case 0x65:
                    vec_compares_gtw128(NULL, &src_xmm, &value);
                    break;
                case 0x66:
                    vec_compares_gtd128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (pcmpgt_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                switch (op2) {
                case 0x64:
                    vec_compares_gtb64(NULL, &src_mm, &value_mm);
                    break;
                case 0x65:
                    vec_compares_gtw64(NULL, &src_mm, &value_mm);
                    break;
                case 0x66:
                    vec_compares_gtd64(NULL, &src_mm, &value_mm);
                    break;
                }
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if ((op2 >= 0x60 && op2 <= 0x62) || (op2 >= 0x68 && op2 <= 0x6a)) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0x60:
                    vec_unpackl_bw128(NULL, &src_xmm, &value);
                    break;
                case 0x61:
                    vec_unpackl_w128(NULL, &src_xmm, &value);
                    break;
                case 0x62:
                    vec_unpackl_dq128(NULL, &src_xmm, &value);
                    break;
                case 0x68:
                    vec_unpackh_bw128(NULL, &src_xmm, &value);
                    break;
                case 0x69:
                    vec_unpackh_w128(NULL, &src_xmm, &value);
                    break;
                case 0x6a:
                    vec_unpackh_d128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (punpckldq_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                vec_unpackl_dq64(NULL, &src_mm, &value_mm);
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (packed_int) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0xd4:
                    vec_add_q128(NULL, &src_xmm, &value);
                    break;
                case 0xf8:
                    vec_sub_b128(NULL, &src_xmm, &value);
                    break;
                case 0xf9:
                    vec_sub_w128(NULL, &src_xmm, &value);
                    break;
                case 0xfa:
                    vec_sub_d128(NULL, &src_xmm, &value);
                    break;
                case 0xfb:
                    vec_sub_q128(NULL, &src_xmm, &value);
                    break;
                case 0xfc:
                    vec_add_b128(NULL, &src_xmm, &value);
                    break;
                case 0xfd:
                    vec_add_w128(NULL, &src_xmm, &value);
                    break;
                case 0xfe:
                    vec_add_d128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (packed_int_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                switch (op2) {
                case 0xd4:
                    vec_add_q64(NULL, &src_mm, &value_mm);
                    break;
                case 0xf8:
                    vec_sub_b64(NULL, &src_mm, &value_mm);
                    break;
                case 0xf9:
                    vec_sub_w64(NULL, &src_mm, &value_mm);
                    break;
                case 0xfa:
                    vec_sub_d64(NULL, &src_mm, &value_mm);
                    break;
                case 0xfb:
                    vec_sub_q64(NULL, &src_mm, &value_mm);
                    break;
                case 0xfc:
                    vec_add_b64(NULL, &src_mm, &value_mm);
                    break;
                case 0xfd:
                    vec_add_w64(NULL, &src_mm, &value_mm);
                    break;
                case 0xfe:
                    vec_add_d64(NULL, &src_mm, &value_mm);
                    break;
                }
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (packed_imm_shift) {
            if (!operand_size_prefix || rep_mode != AMD64_REP_NONE || !modrm.is_reg ||
                    modrm.rm >= AMD64_XMM_COUNT)
                return INT_UNDEFINED;
            value = cpu->xmm[modrm.rm];
            if (op2 == 0x71) {
                switch (modrm.reg) {
                case 2:
                    vec_imm_shiftr_w128(NULL, imm8, &value);
                    break;
                case 4:
                    vec_imm_shiftrs_w128(NULL, imm8, &value);
                    break;
                case 6:
                    vec_imm_shiftl_w128(NULL, imm8, &value);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else if (op2 == 0x72) {
                switch (modrm.reg) {
                case 2:
                    vec_imm_shiftr_d128(NULL, imm8, &value);
                    break;
                case 4:
                    vec_imm_shiftrs_d128(NULL, imm8, &value);
                    break;
                case 6:
                    vec_imm_shiftl_d128(NULL, imm8, &value);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            } else {
                switch (modrm.reg) {
                case 2:
                    vec_imm_shiftr_q128(NULL, imm8, &value);
                    break;
                case 3:
                    vec_imm_shiftr_dq128(NULL, imm8, &value);
                    break;
                case 6:
                    vec_imm_shiftl_q128(NULL, imm8, &value);
                    break;
                case 7:
                    vec_imm_shiftl_dq128(NULL, imm8, &value);
                    break;
                default:
                    return INT_UNDEFINED;
                }
            }
            cpu->xmm[modrm.rm] = value;
        } else if (packed_shift) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0xd1:
                    vec_shiftr_w128(NULL, &src_xmm, &value);
                    break;
                case 0xd2:
                    vec_shiftr_d128(NULL, &src_xmm, &value);
                    break;
                case 0xd3:
                    vec_shiftr_q128(NULL, &src_xmm, &value);
                    break;
                case 0xe1:
                    vec_shiftrs_w128(NULL, &src_xmm, &value);
                    break;
                case 0xe2:
                    vec_shiftrs_d128(NULL, &src_xmm, &value);
                    break;
                case 0xf1:
                    vec_shiftl_w128(NULL, &src_xmm, &value);
                    break;
                case 0xf2:
                    vec_shiftl_d128(NULL, &src_xmm, &value);
                    break;
                case 0xf3:
                    vec_shiftl_q128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (packed_shift_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                switch (op2) {
                case 0xd1:
                    vec_shiftr_w64(NULL, &src_mm, &value_mm);
                    break;
                case 0xd2:
                    vec_shiftr_d64(NULL, &src_mm, &value_mm);
                    break;
                case 0xd3:
                    vec_shiftr_q64(NULL, &src_mm, &value_mm);
                    break;
                case 0xe1:
                    vec_shiftrs_w64(NULL, &src_mm, &value_mm);
                    break;
                case 0xe2:
                    vec_shiftrs_d64(NULL, &src_mm, &value_mm);
                    break;
                case 0xf1:
                    vec_shiftl_w64(NULL, &src_mm, &value_mm);
                    break;
                case 0xf2:
                    vec_shiftl_d64(NULL, &src_mm, &value_mm);
                    break;
                case 0xf3:
                    vec_shiftl_q64(NULL, &src_mm, &value_mm);
                    break;
                }
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (packed_mul) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                if (op2 == 0xd5)
                    vec_mull128(NULL, &src_xmm, &value);
                else
                    vec_mulu_dq128(NULL, &src_xmm, &value);
                cpu->xmm[modrm.reg] = value;
            } else if (packed_mul_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                if (op2 == 0xd5)
                    vec_mull64(NULL, &src_mm, &value_mm);
                else
                    vec_mulu_dq64(NULL, &src_mm, &value_mm);
                cpu->mm[modrm.reg] = value_mm;
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0xf6) {
            if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            vec_sumabs_w128(NULL, &src_xmm, &value);
            cpu->xmm[modrm.reg] = value;
        } else if (packed_xmm_misc) {
            if (!operand_size_prefix && rep_mode == AMD64_REP_NONE &&
                    (op2 == 0xdb || op2 == 0xdf || op2 == 0xe5 || op2 == 0xeb)) {
                // MMX (no-prefix) pand (0xdb) / pandn (0xdf) / pmulhw (0xe5) /
                // por (0xeb). mm[] has only 8 entries; a REX.R/REX.B-extended
                // index is an invalid MMX encoding (matches the logic_mm guard).
                if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
                    return INT_UNDEFINED;
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                value_mm = cpu->mm[modrm.reg];
                if (op2 == 0xdb)
                    vec_and_q64(NULL, &src_mm, &value_mm);
                else if (op2 == 0xdf)
                    vec_andn64(NULL, &src_mm, &value_mm);
                else if (op2 == 0xe5)
                    vec_mulu64(NULL, &src_mm, &value_mm);
                else
                    vec_or_q64(NULL, &src_mm, &value_mm);
                cpu->mm[modrm.reg] = value_mm;
            } else {
                if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                    return INT_UNDEFINED;
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                switch (op2) {
                case 0xd8:
                    vec_subus_b128(NULL, &src_xmm, &value);
                    break;
                case 0xd9:
                    vec_subus_w128(NULL, &src_xmm, &value);
                    break;
                case 0xda:
                    vec_min_ub128(NULL, &src_xmm, &value);
                    break;
                case 0xdb:
                    vec_and_dq128(NULL, &src_xmm, &value);
                    break;
                case 0xdc:
                    vec_addus_b128(NULL, &src_xmm, &value);
                    break;
                case 0xdd:
                    vec_addus_w128(NULL, &src_xmm, &value);
                    break;
                case 0xde:
                    vec_max_ub128(NULL, &src_xmm, &value);
                    break;
                case 0xdf:
                    vec_andn128(NULL, &src_xmm, &value);
                    break;
                case 0xe0:
                    vec_avg_b128(NULL, &src_xmm, &value);
                    break;
                case 0xe3:
                    vec_avg_w128(NULL, &src_xmm, &value);
                    break;
                case 0xe4:
                    vec_muluu128(NULL, &src_xmm, &value);
                    break;
                case 0xe5:
                    vec_mulu128(NULL, &src_xmm, &value);
                    break;
                case 0xe8:
                    vec_subss_b128(NULL, &src_xmm, &value);
                    break;
                case 0xe9:
                    vec_subss_w128(NULL, &src_xmm, &value);
                    break;
                case 0xea:
                    vec_mins_w128(NULL, &src_xmm, &value);
                    break;
                case 0xeb:
                    vec_or_dq128(NULL, &src_xmm, &value);
                    break;
                case 0xec:
                    vec_addss_b128(NULL, &src_xmm, &value);
                    break;
                case 0xed:
                    vec_addss_w128(NULL, &src_xmm, &value);
                    break;
                case 0xee:
                    vec_maxs_w128(NULL, &src_xmm, &value);
                    break;
                }
                cpu->xmm[modrm.reg] = value;
            }
        } else if (op2 == 0xc2) {
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            imm8 &= 7;
            if (rep_mode == AMD64_REPNZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                vec_single_fcmp64(NULL, &src_xmm.f64[0], &value, imm8);
            } else if (rep_mode == AMD64_REPZ) {
                if (operand_size_prefix)
                    return INT_UNDEFINED;
                vec_single_fcmp32(NULL, &src_xmm.f32[0], &value, imm8);
            } else if (operand_size_prefix) {
                vec_fcmp_p64(NULL, &src_xmm, &value, imm8);
            } else {
                for (int i = 0; i < 4; i++) {
                    float lhs = value.f32[i];
                    float rhs = src_xmm.f32[i];
                    switch (imm8) {
                    case 0:
                        value.u32[i] = lhs == rhs ? 0xffffffffu : 0;
                        break;
                    case 1:
                        value.u32[i] = lhs < rhs ? 0xffffffffu : 0;
                        break;
                    case 2:
                        value.u32[i] = lhs <= rhs ? 0xffffffffu : 0;
                        break;
                    case 3:
                        value.u32[i] = isnan(lhs) || isnan(rhs) ? 0xffffffffu : 0;
                        break;
                    case 4:
                        value.u32[i] = lhs != rhs ? 0xffffffffu : 0;
                        break;
                    case 5:
                        value.u32[i] = !(lhs < rhs) ? 0xffffffffu : 0;
                        break;
                    case 6:
                        value.u32[i] = !(lhs <= rhs) ? 0xffffffffu : 0;
                        break;
                    case 7:
                        value.u32[i] = !(isnan(lhs) || isnan(rhs)) ? 0xffffffffu : 0;
                        break;
                    }
                }
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xc4) {
            if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 16, &src_scalar))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            value.u16[imm8 & 7] = (uint16_t) src_scalar;
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xc5) {
            if (!operand_size_prefix || rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            amd64_reg_set(cpu, modrm.reg, 32, src_xmm.u16[imm8 & 7]);
        } else if (op2 == 0xd7) {
            uint32_t mask = 0;
            if (rep_mode != AMD64_REP_NONE || !modrm.is_reg)
                return INT_UNDEFINED;
            if (operand_size_prefix) {
                if (modrm.rm >= AMD64_XMM_COUNT)
                    return INT_UNDEFINED;
                src_xmm = cpu->xmm[modrm.rm];
                for (int i = 0; i < 16; i++)
                    mask |= ((src_xmm.u8[i] >> 7) & 1u) << i;
            } else if (pmovmskb_mm) {
                src_mm = cpu->mm[modrm.rm];
                for (int i = 0; i < 8; i++)
                    mask |= ((src_mm.qw >> (i * 8 + 7)) & 1u) << i;
            } else {
                return INT_UNDEFINED;
            }
            amd64_reg_set(cpu, modrm.reg, 32, mask);
        } else if (op2 == 0xc6) {
            if (rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (operand_size_prefix)
                vec_shuffle_pd128(NULL, &src_xmm, &value, imm8);
            else
                vec_shuffle_ps128(NULL, &src_xmm, &value, imm8);
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xd6) {
            if (operand_size_prefix && rep_mode == AMD64_REP_NONE) {
                // 66 0F D6: movq xmm/m64, xmm (store low qword)
                if (modrm.is_reg)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->xmm[modrm.reg].qw[0]))
                    goto amd64_0f_vec_rm_pf;
            } else if (rep_mode == AMD64_REPZ && !operand_size_prefix &&
                       modrm.is_reg && modrm.rm < 8) {
                // F3 0F D6: movq2dq xmm, mm — 64-bit MMX register into the low
                // qword of the XMM register, upper qword zeroed. Register-only.
                cpu->xmm[modrm.reg].qw[0] = cpu->mm[modrm.rm].qw;
                cpu->xmm[modrm.reg].qw[1] = 0;
            } else if (rep_mode == AMD64_REPNZ && !operand_size_prefix &&
                       modrm.is_reg && modrm.reg < 8) {
                // F2 0F D6: movdq2q mm, xmm — low qword of the XMM register
                // into the MMX register. Register-only.
                cpu->mm[modrm.reg].qw = cpu->xmm[modrm.rm].qw[0];
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0xe7) {
            if (rep_mode != AMD64_REP_NONE || modrm.is_reg)
                return INT_UNDEFINED;
            if (operand_size_prefix) {
                value = cpu->xmm[modrm.reg];
                if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
            } else if (movnt_mm_store) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64, cpu->mm[modrm.reg].qw))
                    goto amd64_0f_vec_rm_pf;
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0x7f) {
            if (operand_size_prefix || rep_mode == AMD64_REPZ) {
                value = cpu->xmm[modrm.reg];
                if (!amd64_write_xmm_rm(cpu, tlb, &modrm, fs_prefix, &value))
                    goto amd64_0f_vec_rm_pf;
            } else if (movq_mm_store) {
                if (modrm.is_reg) {
                    cpu->mm[modrm.rm] = cpu->mm[modrm.reg];
                } else if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 64,
                            cpu->mm[modrm.reg].qw)) {
                    goto amd64_0f_vec_rm_pf;
                }
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0x7e) {
            if (rep_mode == AMD64_REPZ && !operand_size_prefix) {
                value.u128 = 0;
                if (modrm.is_reg) {
                    value.qw[0] = cpu->xmm[modrm.rm].qw[0];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    value.qw[0] = src_scalar;
                }
                cpu->xmm[modrm.reg] = value;
            } else if (rep_mode == AMD64_REP_NONE && !operand_size_prefix) {
                if (modrm.reg >= 8)
                    return INT_UNDEFINED;
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32,
                            rex.w ? cpu->mm[modrm.reg].qw : (uint32_t) cpu->mm[modrm.reg].qw))
                    goto amd64_0f_vec_rm_pf;
            } else if (rep_mode == AMD64_REP_NONE && operand_size_prefix) {
                if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rex.w ? 64 : 32,
                            rex.w ? cpu->xmm[modrm.reg].qw[0] : cpu->xmm[modrm.reg].u32[0]))
                    goto amd64_0f_vec_rm_pf;
            } else {
                return INT_UNDEFINED;
            }
        } else if (op2 == 0x6c || op2 == 0x6d) {
            if (!operand_size_prefix)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            if (op2 == 0x6c) {
                value.qw[1] = src_xmm.qw[0];
            } else {
                value.qw[0] = value.qw[1];
                value.qw[1] = src_xmm.qw[1];
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xdb || op2 == 0xeb) {
            if (!logic_mm)
                return INT_UNDEFINED;
            if (modrm.is_reg) {
                src_mm = cpu->mm[modrm.rm];
            } else {
                if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                    goto amd64_0f_vec_rm_pf;
                src_mm.qw = src_scalar;
            }
            if (op2 == 0xdb)
                cpu->mm[modrm.reg].qw &= src_mm.qw;
            else
                cpu->mm[modrm.reg].qw |= src_mm.qw;
        } else if (op2 >= 0x54 && op2 <= 0x57) {
            if (rep_mode != AMD64_REP_NONE)
                return INT_UNDEFINED;
            if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                goto amd64_0f_vec_rm_pf;
            value = cpu->xmm[modrm.reg];
            switch (op2) {
            case 0x54:
                value.qw[0] &= src_xmm.qw[0];
                value.qw[1] &= src_xmm.qw[1];
                break;
            case 0x55:
                value.qw[0] = ~value.qw[0] & src_xmm.qw[0];
                value.qw[1] = ~value.qw[1] & src_xmm.qw[1];
                break;
            case 0x56:
                value.qw[0] |= src_xmm.qw[0];
                value.qw[1] |= src_xmm.qw[1];
                break;
            case 0x57:
                value.qw[0] ^= src_xmm.qw[0];
                value.qw[1] ^= src_xmm.qw[1];
                break;
            }
            cpu->xmm[modrm.reg] = value;
        } else if (op2 == 0xef) {
            if (operand_size_prefix) {
                if (!amd64_read_xmm_rm(cpu, tlb, &modrm, fs_prefix, &src_xmm))
                    goto amd64_0f_vec_rm_pf;
                value = cpu->xmm[modrm.reg];
                value.qw[0] ^= src_xmm.qw[0];
                value.qw[1] ^= src_xmm.qw[1];
                cpu->xmm[modrm.reg] = value;
            } else if (logic_mm) {
                if (modrm.is_reg) {
                    src_mm = cpu->mm[modrm.rm];
                } else {
                    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &src_scalar))
                        goto amd64_0f_vec_rm_pf;
                    src_mm.qw = src_scalar;
                }
                cpu->mm[modrm.reg].qw ^= src_mm.qw;
            } else {
                return INT_UNDEFINED;
            }
        }
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_0f_vec_rm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_grp3_test(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    qword_t lhs, rhs;

    if (opcode != 0xf6 && opcode != 0xf7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_grp3_test_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_grp3_test_pf;
    if (modrm.reg != 0)
        return INT_UNDEFINED;

    size = opcode == 0xf6 ? 8 : (rex.w ? 64 : (operand_size_prefix ? 16 : 32));
    if (opcode == 0xf6) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_grp3_test_pf;
        rhs = imm8;
    } else if (size == 16) {
        uint16_t imm16;
        if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
            goto amd64_grp3_test_pf;
        rhs = imm16;
    } else {
        int32_t imm32;
        if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
            goto amd64_grp3_test_pf;
        rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
    }

    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, size, &lhs))
        goto amd64_grp3_test_pf;
    amd64_set_logic_flags(cpu, lhs & rhs, size);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_grp3_test_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_grp3_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    unsigned size;
    int interrupt;

    if (opcode != 0xf6 && opcode != 0xf7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_grp3_op_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_grp3_op_pf;
    if (modrm.reg < 2)
        return INT_UNDEFINED;

    size = opcode == 0xf6 ? 8 : (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    interrupt = amd64_grp3_muldiv(cpu, tlb, &modrm, fs_prefix, size);
    if (interrupt != INT_NONE) {
        cpu->amd64_rip = saved_rip;
        amd64_sync_legacy_regs(cpu);
        return interrupt;
    }
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_grp3_op_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_modrm_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t lhs, rhs, result;
    unsigned rm_size;

    if (opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
            opcode != 0xc0 && opcode != 0xc1 && opcode != 0xc6 &&
            opcode != 0xc7)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_modrm_imm_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_modrm_imm_pf;

    rm_size = (opcode == 0x80 || opcode == 0xc0 || opcode == 0xc6) ? 8 :
        (operand_size_prefix ? 16 : (rex.w ? 64 : 32));

    if (opcode == 0xc6) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        if (modrm.reg != 0 || lock_prefix)
            return INT_UNDEFINED;
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, imm8))
            goto amd64_modrm_imm_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc7) {
        if (modrm.reg != 0 || lock_prefix)
            return INT_UNDEFINED;
        if (rm_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_modrm_imm_pf;
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_modrm_imm_pf;
            rhs = rex.w ? (qword_t) (sqword_t) imm32 : (uint32_t) imm32;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, rhs))
            goto amd64_modrm_imm_pf;
        if (rm_size == 64)
            amd64_trace_qword_store(cpu, saved_rip, opcode, amd64_effective_addr(cpu, &modrm, fs_prefix), rhs);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0xc0 || opcode == 0xc1) {
        uint8_t imm8;
        unsigned count, effective_count;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        count = imm8 & (rm_size == 64 ? 0x3f : 0x1f);
        // rotates: a full turn (masked count a nonzero multiple of the operand
        // size) leaves the value unchanged but still updates CF/OF, so gate on
        // the masked count, not count % size.
        effective_count = (modrm.reg == 0 || modrm.reg == 1) ? count :
            ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
        if (effective_count != 0) {
            if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
                goto amd64_modrm_imm_pf;
            switch (modrm.reg) {
            case 0:
            case 1:
                result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
                amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
                break;
            case 2:
            case 3:
                result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
                break;
            case 4:
                result = amd64_trunc(lhs << count, rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            case 5:
                result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            case 7:
                result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
                amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
                break;
            default:
                return INT_UNDEFINED;
            }
            if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
                goto amd64_modrm_imm_pf;
        }
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (opcode == 0x80) {
        uint8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        rhs = imm8;
    } else if (opcode == 0x83) {
        int8_t imm8;
        if (!amd64_fetch(cpu, tlb, &imm8, sizeof(imm8)))
            goto amd64_modrm_imm_pf;
        rhs = (qword_t) amd64_sign_extend((uint8_t) imm8, 8);
    } else {
        if (rm_size == 16) {
            uint16_t imm16;
            if (!amd64_fetch(cpu, tlb, &imm16, sizeof(imm16)))
                goto amd64_modrm_imm_pf;
            rhs = imm16;
        } else {
            int32_t imm32;
            if (!amd64_fetch(cpu, tlb, &imm32, sizeof(imm32)))
                goto amd64_modrm_imm_pf;
            rhs = (qword_t) (sqword_t) imm32;
        }
    }

    bool atomic_locked = lock_prefix && !modrm.is_reg && modrm.reg != 7;
    if (lock_prefix && (modrm.is_reg || modrm.reg == 7))
        return INT_UNDEFINED;
    // LOCK <alu> [mem], imm: one host-atomic RMW. /7 (CMP) writes nothing and
    // so cannot be locked (rejected above), which is why every remaining group
    // index maps straight onto amd64_locked_alu's op numbering.
    if (atomic_locked) {
        qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
        if (!amd64_locked_alu(cpu, tlb, addr, rm_size, modrm.reg, rhs))
            goto amd64_modrm_imm_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }

    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
        goto amd64_modrm_imm_unlock_pf;

    switch (modrm.reg) {
    case 0:
        result = amd64_trunc(lhs + rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_add_flags(cpu, lhs, rhs, result, rm_size);
        break;
    case 1:
        result = amd64_trunc(lhs | rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 2: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs + rhs + carry_in, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_adc_flags(cpu, lhs, rhs, carry_in, result, rm_size);
        break;
    }
    case 3: {
        unsigned carry_in = cpu->cf;
        result = amd64_trunc(lhs - rhs - carry_in, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_sbb_flags(cpu, lhs, rhs, carry_in, result, rm_size);
        break;
    }
    case 4:
        result = amd64_trunc(lhs & rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 5:
        result = amd64_trunc(lhs - rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
        break;
    case 6:
        result = amd64_trunc(lhs ^ rhs, rm_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_modrm_imm_unlock_pf;
        amd64_set_logic_flags(cpu, result, rm_size);
        break;
    case 7:
        result = amd64_trunc(lhs - rhs, rm_size);
        amd64_set_sub_flags(cpu, lhs, rhs, result, rm_size);
        break;
    default:
        return INT_UNDEFINED;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_modrm_imm_unlock_pf:
amd64_modrm_imm_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_shift(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    byte_t byte;
    qword_t lhs, result;
    unsigned rm_size;
    unsigned count;
    unsigned effective_count;

    if (opcode != 0xd0 && opcode != 0xd1 && opcode != 0xd2 && opcode != 0xd3)
        return INT_UNDEFINED;
    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_shift_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != opcode)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_shift_pf;

    rm_size = (opcode == 0xd0 || opcode == 0xd2) ? 8 :
        (operand_size_prefix ? 16 : (rex.w ? 64 : 32));
    count = (opcode == 0xd0 || opcode == 0xd1) ? 1 :
        ((unsigned) amd64_reg_get(cpu, amd64_rcx, 8) & (rm_size == 64 ? 0x3f : 0x1f));
    effective_count = (modrm.reg == 0 || modrm.reg == 1) ? count :
        ((modrm.reg == 2 || modrm.reg == 3) ? amd64_rotate_carry_count(rm_size, count) : count);
    if (effective_count != 0) {
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, rm_size, &lhs))
            goto amd64_shift_pf;
        switch (modrm.reg) {
        case 0:
        case 1:
            result = amd64_rotate_value(lhs, rm_size, count, modrm.reg);
            amd64_set_rotate_flags(cpu, result, rm_size, count, modrm.reg);
            break;
        case 2:
        case 3:
            result = amd64_rotate_carry_value(cpu, lhs, rm_size, count, modrm.reg);
            break;
        case 4:
            result = amd64_trunc(lhs << count, rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        case 5:
            result = amd64_trunc(amd64_trunc(lhs, rm_size) >> count, rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        case 7:
            result = amd64_trunc((qword_t) (amd64_sign_extend(lhs, rm_size) >> count), rm_size);
            amd64_set_shift_flags(cpu, lhs, result, rm_size, count, modrm.reg);
            break;
        default:
            return INT_UNDEFINED;
        }
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, rm_size, result))
            goto amd64_shift_pf;
    }

    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_shift_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_fe_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t lhs, result;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_fe_group_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0xfe)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_fe_group_pf;
    if (modrm.reg > 1 || (lock_prefix && modrm.is_reg))
        return INT_UNDEFINED;

    bool is_inc = modrm.reg == 0;
    bool saved_cf = cpu->cf;
    bool atomic_locked = lock_prefix && !modrm.is_reg;
    if (atomic_locked) {
        // amd64_locked_incdec restores CF and collapses the flags itself.
        qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
        if (!amd64_locked_incdec(cpu, tlb, addr, 8, is_inc))
            goto amd64_fe_group_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 8, &lhs))
        goto amd64_fe_group_unlock_pf;
    result = is_inc ? amd64_trunc(lhs + 1, 8) : amd64_trunc(lhs - 1, 8);
    if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, 8, result))
        goto amd64_fe_group_unlock_pf;
    if (is_inc)
        amd64_set_add_flags(cpu, lhs, 1, result, 8);
    else
        amd64_set_sub_flags(cpu, lhs, 1, result, 8);
    cpu->cf = saved_cf;
    collapse_flags(cpu);
    cpu->amd64_rip = (qword_t) next_ip;
    amd64_sync_legacy_regs(cpu);
    return INT_NONE;

amd64_fe_group_unlock_pf:
amd64_fe_group_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

int amd64_jit_ff_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip) {
    qword_t saved_rip = cpu->amd64_rip;
    guest_addr_t checked_next_ip;
    struct amd64_rex_prefix rex = {0};
    struct amd64_modrm modrm;
    bool fs_prefix = false;
    bool operand_size_prefix = false;
    bool lock_prefix = false;
    byte_t byte;
    qword_t value, lhs, result;
    unsigned op_size;

    if (!amd64_guest_addr_ok((qword_t) next_ip, 1, &checked_next_ip))
        return INT_GPF;

    cpu->amd64_address_size_prefix = false;
    for (;;) {
        if (!amd64_fetch_u8(cpu, tlb, &byte))
            goto amd64_ff_group_pf;
        if (amd64_ignored_segment_prefix(byte))
            continue;
        if (byte == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (byte == 0xf0) {
            lock_prefix = true;
            continue;
        }
        if (byte == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex.present = true;
            rex.w = (byte & 8) != 0;
            rex.r = (byte & 4) != 0;
            rex.x = (byte & 2) != 0;
            rex.b = (byte & 1) != 0;
            continue;
        }
        break;
    }
    if (byte != 0xff)
        return INT_UNDEFINED;
    if (!amd64_decode_modrm(cpu, tlb, rex, &modrm))
        goto amd64_ff_group_pf;

    op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    switch (modrm.reg) {
    case 0:
    case 1: {
        bool is_inc = modrm.reg == 0;
        bool saved_cf = cpu->cf;
        bool atomic_locked = lock_prefix && !modrm.is_reg;
        if (lock_prefix && modrm.is_reg)
            return INT_UNDEFINED;
        if (atomic_locked) {
            qword_t addr = amd64_effective_addr(cpu, &modrm, fs_prefix);
            if (!amd64_locked_incdec(cpu, tlb, addr, op_size, is_inc))
                goto amd64_ff_group_pf;
            cpu->amd64_rip = (qword_t) next_ip;
            amd64_sync_legacy_regs(cpu);
            return INT_NONE;
        }
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, op_size, &lhs))
            goto amd64_ff_group_unlock_pf;
        result = is_inc ? amd64_trunc(lhs + 1, op_size) : amd64_trunc(lhs - 1, op_size);
        if (!amd64_write_rm(cpu, tlb, &modrm, fs_prefix, op_size, result))
            goto amd64_ff_group_unlock_pf;
        if (is_inc)
            amd64_set_add_flags(cpu, lhs, 1, result, op_size);
        else
            amd64_set_sub_flags(cpu, lhs, 1, result, op_size);
        cpu->cf = saved_cf;
        collapse_flags(cpu);
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
amd64_ff_group_unlock_pf:
        goto amd64_ff_group_pf;
    }
    case 2: {
        qword_t return_rip = (qword_t) next_ip;
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
            goto amd64_ff_group_pf;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "call-rm64");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        if (!amd64_push(cpu, tlb, return_rip))
            goto amd64_ff_group_pf;
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "call-rm64");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "call-rm64");
        cpu->amd64_rip = value;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    }
    case 4:
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix, 64, &value))
            goto amd64_ff_group_pf;
        {
            int target_interrupt = amd64_validate_transfer_target(cpu, tlb, saved_rip, value, "jmp-rm64");
            if (target_interrupt != INT_NONE)
                return target_interrupt;
        }
        amd64_trace_cc1_xfer_probe(cpu, tlb, saved_rip, value, "jmp-rm64");
        amd64_trace_cargo_transfer(cpu, tlb, saved_rip, value, "jmp-rm64");
        cpu->amd64_rip = value;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    case 6:
        if (!amd64_read_rm(cpu, tlb, &modrm, fs_prefix,
                operand_size_prefix ? 16 : 64, &value))
            goto amd64_ff_group_pf;
        if (!amd64_push_size(cpu, tlb, operand_size_prefix ? 16 : 64, value))
            goto amd64_ff_group_pf;
        cpu->amd64_rip = (qword_t) next_ip;
        amd64_sync_legacy_regs(cpu);
        return INT_NONE;
    default:
        return INT_UNDEFINED;
    }

amd64_ff_group_pf:
    cpu->amd64_rip = saved_rip;
    amd64_sync_legacy_regs(cpu);
    return INT_PF;
}

void amd64_jit_bridge_set_tlb(struct tlb *tlb) {
    amd64_jit_bridge_tlb = tlb;
}

int amd64_step_to_interrupt_jit_bridge(struct cpu_state *cpu) {
    if (amd64_jit_bridge_tlb == NULL)
        return INT_GPF;
    return amd64_step_to_interrupt_jit(cpu, amd64_jit_bridge_tlb);
}

int cpu_run_to_interrupt_amd64(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->poked_ptr = &cpu->_poked;
    tlb_refresh(tlb, cpu->mmu);

    int steps = 0;
    guest_addr_t last_step_rip = 0;
    unsigned same_rip_steps = 0;
    static __thread guest_addr_t last_watchdog_rip;
    static __thread unsigned same_rip_timer_yields;
    while (true) {
        guest_addr_t step_rip = cpu->amd64_rip;
        if (step_rip == last_step_rip)
            same_rip_steps++;
        else {
            last_step_rip = step_rip;
            same_rip_steps = 0;
        }

        int interrupt = amd64_step_to_interrupt(cpu, tlb);
        if (interrupt == INT_UNDEFINED || interrupt == INT_PRIV) {
            if (interrupt == INT_UNDEFINED && amd64_trace_undefined_enabled()) {
                uint8_t bytes[12] = {0};
                unsigned read = 0;
                for (; read < sizeof(bytes); read++) {
                    if (!amd64_mem_read(cpu, tlb, cpu->amd64_current_insn_rip + read, &bytes[read], 1))
                        break;
                }
                printk("amd64 undefined: rip=%#llx bytes=",
                       (unsigned long long) cpu->amd64_current_insn_rip);
                for (unsigned i = 0; i < read; i++)
                    printk("%s%02x", i == 0 ? "" : " ", bytes[i]);
                printk(" rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsp=%#llx rbp=%#llx\n",
                       (unsigned long long) amd64_reg_get(cpu, amd64_rax, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rcx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rdx, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rsp, 64),
                       (unsigned long long) amd64_reg_get(cpu, amd64_rbp, 64));
            }
            cpu->amd64_rip = cpu->amd64_current_insn_rip;
        }
        if (interrupt == INT_NONE && cpu->tf)
            interrupt = INT_DEBUG;
        if (interrupt == INT_NONE && __atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && ++steps >= 1024) {
            if (same_rip_steps >= 1023) {
                if (cpu->amd64_rip == last_watchdog_rip)
                    same_rip_timer_yields++;
                else {
                    last_watchdog_rip = cpu->amd64_rip;
                    same_rip_timer_yields = 1;
                }
                if ((same_rip_timer_yields & (same_rip_timer_yields - 1)) == 0) {
                    byte_t bytes[12] = {0};
                    unsigned read = 0;
                    for (; read < sizeof(bytes); read++) {
                        if (!amd64_mem_read(cpu, tlb, cpu->amd64_rip + read, &bytes[read], 1))
                            break;
                    }
                    printk("amd64 watchdog: pid=%d comm=%s rip=%#llx repeated=%u bytes=",
                           current ? current->pid : -1,
                           current ? current->comm : "(null)",
                           (unsigned long long) cpu->amd64_rip,
                           same_rip_timer_yields);
                    for (unsigned i = 0; i < read; i++)
                        printk("%s%02x", i == 0 ? "" : " ", bytes[i]);
                    printk("\n");
                }
            } else {
                same_rip_timer_yields = 0;
            }
            steps = 0;
            interrupt = INT_TIMER;
        }
        if (interrupt != INT_NONE) {
            cpu->trapno = interrupt;
            amd64_sync_legacy_regs(cpu);
            return interrupt;
        }
    }
}
