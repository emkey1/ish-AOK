#ifndef EMU_I386_SREG_H
#define EMU_I386_SREG_H

#include "emu/cpu.h"
#include "emu/tlb.h"

// i386 segment registers and the TLS descriptors behind them. See
// emu/i386_sreg.c for the model and the Linux rules it follows.

// The selectors a 32-bit task sees on a 64-bit Linux kernel, whose GDT is the
// one presented here: __USER32_CS and __USER_DS.
#define I386_SEL_USER_CS 0x23
#define I386_SEL_USER_DS 0x2b

// GDT entries 12-14 hold the TLS descriptors (GDT_ENTRY_TLS_MIN on x86_64).
#define I386_TLS_ENTRY_MIN 12
#define I386_TLS_ENTRIES 3

// struct user_desc's bit-fields, as the one word they share.
#define USER_DESC_SEG_32BIT 0x01
#define USER_DESC_CONTENTS 0x06
#define USER_DESC_READ_EXEC_ONLY 0x08
#define USER_DESC_LIMIT_IN_PAGES 0x10
#define USER_DESC_SEG_NOT_PRESENT 0x20
#define USER_DESC_USEABLE 0x40
#define USER_DESC_FLAGS 0x7f
// What get_thread_area reports for an empty entry.
#define USER_DESC_EMPTY (USER_DESC_READ_EXEC_ONLY | USER_DESC_SEG_NOT_PRESENT)

word_t i386_sreg_read(const struct cpu_state *cpu, unsigned sreg);
bool i386_sreg_loadable(const struct cpu_state *cpu, unsigned sreg, word_t sel);
// Loads a selector that i386_sreg_loadable accepted, and for FS and GS the
// base of the descriptor it names.
void i386_sreg_load(struct cpu_state *cpu, unsigned sreg, word_t sel);

// What exec leaves: ES, DS and SS 0x2b, FS and GS null, no TLS entries.
void i386_sreg_exec_reset(struct cpu_state *cpu);
// A signal handler starts with DS and ES (and SS) 0x2b.
void i386_sreg_signal_enter(struct cpu_state *cpu);
// sigreturn's reload of GS, FS, DS or ES from the frame.
void i386_sreg_sigreturn(struct cpu_state *cpu, unsigned sreg, word_t frame_sel);
// Whether sigreturn can resume with this CS and SS; if so, SS is loaded.
bool i386_sreg_sigreturn_cs_ss(struct cpu_state *cpu, word_t frame_cs, word_t frame_ss);

// set_thread_area and get_thread_area on a TLS entry (12-14), as struct
// user_desc words. i386_tls_set takes a descriptor i386_tls_desc_okay
// accepted, and reloads any segment register that selects the entry.
bool i386_tls_desc_okay(dword_t base, dword_t limit, dword_t flags);
void i386_tls_set(struct cpu_state *cpu, unsigned entry, dword_t base, dword_t limit,
        dword_t flags);
int i386_tls_free_entry(const struct cpu_state *cpu);
void i386_tls_get(const struct cpu_state *cpu, unsigned entry, dword_t *base,
        dword_t *limit, dword_t *flags);

// The JIT's helper for 8C, 8E and the PUSH and POP of a segment register.
// `op` is built from the I386_SREG_OP_* fields below; `addr` is the memory
// operand's address, from the JIT's own address gadgets, segment base
// included. Returns -1 to continue the block, or the interrupt to raise at
// the instruction: INT_UNDEFINED is decided at compile time, so this is
// INT_GPF, for a selector that cannot be loaded or for a memory access that
// faulted (with segfault_reported set, as the JIT's memory gadgets do).
int i386_jit_sreg(struct cpu_state *cpu, struct tlb *tlb, unsigned long op,
        unsigned long addr);
#define I386_SREG_OP_READ 0     // 8C: MOV r/m, Sreg
#define I386_SREG_OP_LOAD 1     // 8E: MOV Sreg, r/m
#define I386_SREG_OP_PUSH 2
#define I386_SREG_OP_POP 3
#define I386_SREG_OP_KIND 0x3
#define I386_SREG_OP_SREG(op) (((op) >> 4) & 7)
#define I386_SREG_OP_16 0x100   // operand size 16
#define I386_SREG_OP_MEM 0x200  // memory operand at addr
#define I386_SREG_OP_RM(op) (((op) >> 12) & 7)  // register operand

#endif
