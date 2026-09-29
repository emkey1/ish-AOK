#ifndef JIT_JITPROF_H
#define JIT_JITPROF_H

// ISH_JIT_PROFILE=<file>: count how often each translated arm64/riscv64 block
// is entered, and remember the guest instructions it was made from, so the
// dynamic instruction mix can be worked out afterwards (tools/jitprof-report.py).
// A measurement instrument, off by default; see jit/jitprof.c.

#include <stdbool.h>
#include <stdint.h>

enum jitprof_abi { JITPROF_ARM64 = 1, JITPROF_RISCV64 = 2 };

struct jitprof_block;

// NULL when profiling is off (the common case, one load and a branch).
struct jitprof_block *jitprof_block_new(uint64_t addr, enum jitprof_abi abi);
// The block's execution counter, for the block_count gadget.
uint64_t *jitprof_counter(struct jitprof_block *block);
// One guest instruction of the block, in order (RVC expanded to 32 bits).
void jitprof_note(struct jitprof_block *block, uint32_t insn);
// Write the file (main.c's cli_halt); no-op unless ISH_JIT_PROFILE.
void jitprof_dump(void);

#endif
