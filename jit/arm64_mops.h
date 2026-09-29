#ifndef JIT_ARM64_MOPS_H
#define JIT_ARM64_MOPS_H

#include <stdbool.h>
#include <stdint.h>

struct cpu_state;
struct tlb;

// One FEAT_MOPS instruction (CPY*, CPYF*, SET*; not SETG*), against the
// registers in cpu. 0, or INT_PF with the registers describing what is left.
// See jit/arm64_mops.c.
int arm64_mops(struct cpu_state *cpu, struct tlb *tlb, uint32_t insn);
// Whether the guest is told about MOPS (ISH_MOPS=0 or /proc/ish/arm64_mops
// says no), and the /proc/ish/arm64_mops switch.
bool arm64_mops_advertised(void);
void arm64_mops_set_advertised(bool advertised);

#endif
