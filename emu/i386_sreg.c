// i386 segment registers: what MOV to and from a segment register (8C, 8E),
// PUSH and POP of one, an FS or GS override, set_thread_area, signal delivery,
// sigreturn, ptrace and exec see. Measured on x86_64 Linux 6.12 with 32-bit
// tasks (camd); tests/manual/x86/i386_segment_regs.c is the record.
//
// A 32-bit task on a 64-bit kernel sees that kernel's GDT, and that is the
// one presented here, as set_thread_area's entry 12 already implied:
//
//   entry 4  0x23  __USER32_CS: CS, always
//   entry 5  0x2b  __USER_DS: SS, DS and ES from exec on
//   entry 6  0x33  __USER_CS, 64-bit code
//   entries 12-14  the TLS descriptors set_thread_area fills
//   entry 15 0x7b  the CPUNODE descriptor, read-only data
//
// Every other GDT entry is the kernel's, a TSS or LDT descriptor, or past the
// table, and no task has an LDT. So a selector loads into ES, DS, FS or GS if
// it is null, or names entry 4, 5, 6 or 15, or a filled TLS entry, whatever
// its RPL; SS takes RPL 3 only, and only writable data: 0x2b or a TLS entry
// set without read_exec_only. Anything else is #GP.
//
// State (struct cpu_state): the selectors in i386_sreg, the TLS entries in
// i386_tls, and two bases, since only FS and GS are given one: an FS or GS
// override adds the base of the descriptor the register selected when it was
// loaded, as the hardware's descriptor cache does. That is i386_fs_base for FS
// and tls_ptr for GS, where the JIT's seg_fs and seg_gs gadgets read them. The
// flat descriptors have base 0, so only a TLS entry gives one. DS, ES and SS
// carry no base here: a TLS selector loaded into one of them addresses flat
// memory, where Linux would add the TLS base. No libc does that. Nor is a
// null FS or GS a fault when used; it is base 0 here too.
//
// Before this, the i386 engine had one selector, FS and GS both, read by 8C
// for either and written by 8E for either, with no rules; and tls_ptr was
// whatever set_thread_area was last given, whichever entry it named and
// whatever GS held.
#include <string.h>
#include "emu/i386_sreg.h"
#include "emu/interrupt.h"

static const struct i386_tls_desc *tls_desc(const struct cpu_state *cpu, word_t sel) {
    unsigned index = sel >> 3;
    if (sel & 4 || index < I386_TLS_ENTRY_MIN || index >= I386_TLS_ENTRY_MIN + I386_TLS_ENTRIES)
        return NULL;
    return &cpu->i386_tls[index - I386_TLS_ENTRY_MIN];
}

word_t i386_sreg_read(const struct cpu_state *cpu, unsigned sreg) {
    if (sreg == AMD64_SREG_CS)
        return I386_SEL_USER_CS;
    return cpu->i386_sreg[sreg];
}

bool i386_sreg_loadable(const struct cpu_state *cpu, unsigned sreg, word_t sel) {
    if (sel & 4)
        return false;
    const struct i386_tls_desc *tls = tls_desc(cpu, sel);
    if (sreg == AMD64_SREG_SS) {
        if ((sel & 3) != 3)
            return false;
        if (sel == I386_SEL_USER_DS)
            return true;
        return tls != NULL && tls->flags != 0 && !(tls->flags & USER_DESC_READ_EXEC_ONLY);
    }
    switch (sel >> 3) {
    case 0:
    case 4:
    case 5:
    case 6:
    case 15:
        return true;
    }
    return tls != NULL && tls->flags != 0;
}

static dword_t sel_base(const struct cpu_state *cpu, word_t sel) {
    const struct i386_tls_desc *tls = tls_desc(cpu, sel);
    return tls != NULL ? tls->base : 0;
}

void i386_sreg_load(struct cpu_state *cpu, unsigned sreg, word_t sel) {
    cpu->i386_sreg[sreg] = sel;
    if (sreg == AMD64_SREG_FS)
        cpu->i386_fs_base = sel_base(cpu, sel);
    else if (sreg == AMD64_SREG_GS)
        cpu->tls_ptr = sel_base(cpu, sel);
}

// start_thread and flush_thread. tls_ptr is amd64's FS base as well, which
// exec clears too.
void i386_sreg_exec_reset(struct cpu_state *cpu) {
    memset(cpu->i386_sreg, 0, sizeof(cpu->i386_sreg));
    memset(cpu->i386_tls, 0, sizeof(cpu->i386_tls));
    cpu->i386_sreg[AMD64_SREG_ES] = I386_SEL_USER_DS;
    cpu->i386_sreg[AMD64_SREG_SS] = I386_SEL_USER_DS;
    cpu->i386_sreg[AMD64_SREG_DS] = I386_SEL_USER_DS;
    cpu->i386_fs_base = 0;
    cpu->tls_ptr = 0;
}

// ia32_setup_rt_frame: loadsegment(ds, __USER_DS), the same for ES, and SS
// __USER_DS. FS and GS are left as they were.
void i386_sreg_signal_enter(struct cpu_state *cpu) {
    cpu->i386_sreg[AMD64_SREG_ES] = I386_SEL_USER_DS;
    cpu->i386_sreg[AMD64_SREG_SS] = I386_SEL_USER_DS;
    cpu->i386_sreg[AMD64_SREG_DS] = I386_SEL_USER_DS;
}

// reload_segments: the frame's selector with RPL 3, and one that faults when
// loaded becomes null (loadsegment's fixup). A null selector does not keep
// that RPL: Linux loads 3, and the IRET back to the task leaves 0 on the
// machine the test was measured on.
void i386_sreg_sigreturn(struct cpu_state *cpu, unsigned sreg, word_t frame_sel) {
    word_t sel = frame_sel | 3;
    if (sel >> 3 == 0 || !i386_sreg_loadable(cpu, sreg, sel))
        sel = 0;
    i386_sreg_load(cpu, sreg, sel);
}

// sigreturn takes CS and SS from the frame with RPL 3; the IRET to them #GPs
// unless CS is the 32-bit code segment and SS a stack segment. (CS 0x33 would
// switch Linux to 64-bit code, which a 32-bit task here cannot do.)
//
// The #GP's error code, measured on camd (-m32, and -m64 alike): CS's
// selector if CS is bad, else SS's (index and TI; 0 for a null one) -- but 0
// whenever SS names the LDT, whatever CS holds, because Linux returns to such
// an SS through its espfix stack and the fault there reports no selector.
int i386_sreg_sigreturn_cs_ss(struct cpu_state *cpu, word_t frame_cs, word_t frame_ss) {
    word_t cs = frame_cs | 3;
    word_t ss = frame_ss | 3;
    bool cs_ok = cs == I386_SEL_USER_CS;
    if (cs_ok && i386_sreg_loadable(cpu, AMD64_SREG_SS, ss)) {
        i386_sreg_load(cpu, AMD64_SREG_SS, ss);
        return -1;
    }
    if (ss & 4)
        return 0;
    return (cs_ok ? ss : cs) & 0xfffc;
}

// ---- TLS entries (arch/x86/kernel/tls.c) ----

// LDT_empty or LDT_zero: set_thread_area clears the entry. Bits 7 and up of
// the flags word (lm, then padding) are not looked at for a 32-bit task.
static bool desc_clears(dword_t base, dword_t limit, dword_t flags) {
    flags &= USER_DESC_FLAGS;
    return base == 0 && limit == 0 && (flags == 0 || flags == USER_DESC_EMPTY);
}

// tls_desc_okay: only present 32-bit data segments go in the TLS array, or
// a descriptor that clears the entry.
bool i386_tls_desc_okay(dword_t base, dword_t limit, dword_t flags) {
    flags &= USER_DESC_FLAGS;
    if (desc_clears(base, limit, flags))
        return true;
    return (flags & USER_DESC_SEG_32BIT) && (flags & USER_DESC_CONTENTS) <= 2 &&
           !(flags & USER_DESC_SEG_NOT_PRESENT);
}

void i386_tls_set(struct cpu_state *cpu, unsigned entry, dword_t base, dword_t limit,
        dword_t flags) {
    flags &= USER_DESC_FLAGS;
    struct i386_tls_desc *tls = &cpu->i386_tls[entry - I386_TLS_ENTRY_MIN];
    if (desc_clears(base, limit, flags))
        *tls = (struct i386_tls_desc) {0};
    else
        *tls = (struct i386_tls_desc) {.base = base, .limit = limit & 0xfffff, .flags = flags};

    // do_set_thread_area reloads DS, ES, FS and GS if one holds the entry's
    // selector with RPL 3, so a base set there takes effect at once, and a
    // cleared entry leaves the register null. SS is left alone.
    word_t sel = (word_t) (entry << 3 | 3);
    static const unsigned reloaded[] = {
        AMD64_SREG_ES, AMD64_SREG_DS, AMD64_SREG_FS, AMD64_SREG_GS,
    };
    for (unsigned i = 0; i < sizeof(reloaded) / sizeof(reloaded[0]); i++) {
        unsigned sreg = reloaded[i];
        if (cpu->i386_sreg[sreg] == sel)
            i386_sreg_load(cpu, sreg, i386_sreg_loadable(cpu, sreg, sel) ? sel : 0);
    }
}

// get_free_idx: the first empty entry, or -1.
int i386_tls_free_entry(const struct cpu_state *cpu) {
    for (unsigned i = 0; i < I386_TLS_ENTRIES; i++)
        if (cpu->i386_tls[i].flags == 0)
            return (int) (I386_TLS_ENTRY_MIN + i);
    return -1;
}

// fill_user_desc: an empty entry reads as not present and read-only.
void i386_tls_get(const struct cpu_state *cpu, unsigned entry, dword_t *base,
        dword_t *limit, dword_t *flags) {
    const struct i386_tls_desc *tls = &cpu->i386_tls[entry - I386_TLS_ENTRY_MIN];
    if (tls->flags == 0) {
        *base = 0;
        *limit = 0;
        *flags = USER_DESC_EMPTY;
        return;
    }
    *base = tls->base;
    *limit = tls->limit;
    *flags = tls->flags;
}

// ---- the JIT's helper ----

// A memory access that faulted: reported as the JIT's segfault_read and
// segfault_write gadgets report one, for the kernel's i386 fault routing.
static int mem_fault(struct cpu_state *cpu, struct tlb *tlb, bool write) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = write;
    cpu->segfault_reported = true;
    return INT_GPF;
}

// #GP: nothing reported, so the kernel delivers SIGSEGV with SI_KERNEL. The
// error code is the selector's index and TI bit, whatever made it fail --
// 0x13 gives 0x10, the LDT's 0x07 gives 0x04, a null SS 0 (camd, -m32).
static int gpf(struct cpu_state *cpu, word_t sel) {
    cpu->segfault_addr = 0;
    cpu->segfault_was_write = false;
    cpu->segfault_reported = false;
    return INT_GPF_CODE(sel & 0xfffc);
}

// The selector to load: two bytes of memory, or the low word of a register.
// A 32-bit POP reads four bytes and keeps the low word.
int i386_jit_sreg(struct cpu_state *cpu, struct tlb *tlb, unsigned long op,
        unsigned long addr) {
    unsigned sreg = I386_SREG_OP_SREG(op);
    unsigned bytes = op & I386_SREG_OP_16 ? 2 : 4;
    dword_t esp = cpu->esp;
    word_t sel;
    dword_t value;

    switch (op & I386_SREG_OP_KIND) {
    case I386_SREG_OP_READ:
        sel = i386_sreg_read(cpu, sreg);
        if (op & I386_SREG_OP_MEM) {
            // Two bytes whatever the operand size.
            if (!tlb_write(tlb, (addr_t) addr, &sel, sizeof(sel)))
                return mem_fault(cpu, tlb, true);
        } else if (bytes == 4) {
            cpu->regs[I386_SREG_OP_RM(op)] = sel;
        } else {
            memcpy(&cpu->regs[I386_SREG_OP_RM(op)], &sel, sizeof(sel));
        }
        return -1;

    case I386_SREG_OP_LOAD:
        if (op & I386_SREG_OP_MEM) {
            if (!tlb_read(tlb, (addr_t) addr, &sel, sizeof(sel)))
                return mem_fault(cpu, tlb, false);
        } else {
            sel = (word_t) cpu->regs[I386_SREG_OP_RM(op)];
        }
        if (!i386_sreg_loadable(cpu, sreg, sel))
            return gpf(cpu, sel);
        i386_sreg_load(cpu, sreg, sel);
        return -1;

    case I386_SREG_OP_PUSH:
        // Four bytes zero-extended, as AMD stores them; recent Intel parts
        // write the low two and leave the rest, and either is allowed.
        value = i386_sreg_read(cpu, sreg);
        if (!tlb_write(tlb, (addr_t) (dword_t) (esp - bytes), &value, bytes))
            return mem_fault(cpu, tlb, true);
        cpu->esp = esp - bytes;
        return -1;

    case I386_SREG_OP_POP:
        value = 0;
        if (!tlb_read(tlb, (addr_t) esp, &value, bytes))
            return mem_fault(cpu, tlb, false);
        sel = (word_t) value;
        // A POP that #GPs leaves ESP where it was.
        if (!i386_sreg_loadable(cpu, sreg, sel))
            return gpf(cpu, sel);
        cpu->esp = esp + bytes;
        i386_sreg_load(cpu, sreg, sel);
        return -1;
    }
    return gpf(cpu, 0);
}
