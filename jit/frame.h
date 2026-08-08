#include <stdatomic.h>
#include "emu/cpu.h"

// keep in sync with asm
#define JIT_RETURN_CACHE_SIZE 4096
#define JIT_RETURN_CACHE_HASH(x) ((x) & 0xFFF0) >> 4)

struct jit_frame {
    struct cpu_state cpu;
    void *bp;
    // 64-bit, not addr_t (32-bit): arm64 guest gadgets stash a full
    // 64-bit guest address here for the crosspage-write flush
    // (jit/guest-arm64/memory.S). i386's gadgets store/load only the low
    // 32 bits (str w/ldr w against the same symbolic LOCAL_value_addr
    // offset), which stays correct on a little-endian 64-bit field.
    uint64_t value_addr;
    // 32 bytes: sized for the largest crosspage access any gadget can
    // produce — an arm64 STP/LDP of two 128-bit Q registers
    // (jit/guest-arm64/simd.S). i386/amd64 use at most 16 of it.
    uint64_t value[4]; // buffer for crosspage crap
    struct jit_block *last_block;
    // backward-chaining budget: decremented by arm64_chain
    // (jit/guest-arm64/control.S), riscv64_chain and amd64_chain
    // (jit/gadgets-aarch64/control.S) on every chained block-to-block
    // dispatch; on expiry the chain exits to C so the cycle counter,
    // poke checks, and jetsam writers all get their turn. This is what
    // lets those engines chain BACKWARD edges (loops) safely. The i386
    // loop has no such budget, so it still forbids backward links
    // outright and pays an exit-to-C round trip per loop iteration.
    long chain_budget;
    long ret_cache[JIT_RETURN_CACHE_SIZE]; // a map of ip to pointer-to-call-gadget-arguments
};
