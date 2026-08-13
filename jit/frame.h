#include <stdatomic.h>
#include "emu/cpu.h"

// keep in sync with asm
//
// NOT freely adjustable: the hash width is hardcoded in the gadgets as a
// 12-bit bitfield extract (`ubfx ..., 12` in gadgets-aarch64/control.S and
// guest-riscv64/alu.S, `andl $0xfff` in gadgets-x86_64/control.S). Shrinking
// this array without editing every one of them indexes past the end of the
// frame; that was tried at 64 entries and the guest silently produced nothing.
#define JIT_RETURN_CACHE_SIZE 4096
#define JIT_RETURN_CACHE_HASH(x) ((x) & 0xFFF0) >> 4)

// Key derivation for the LINK-REGISTER engines (riscv64 jalr, arm64 br/blr/ret)
// which key this array by guest address -- see the ret_cache member below for
// what each engine stores. Mixing in bits 12+ rather than taking a bare
// bitfield the way i386 does, because this key is a guest CODE address and
// these instructions are 2- and 4-byte aligned: i386's bits 4..15 would put
// every call site inside one 16-byte window in the same bucket, which in these
// guests is a run of consecutive instructions.
//
// The `>> 1` costs nothing on arm64's 4-byte alignment: hash bit k is
// ip[k+1] ^ ip[k+13], so with ip[1] always 0 bit 0 becomes ip[13] and adjacent
// 4-byte call sites still separate on bit 1 = ip[2] ^ ip[14].
//
// MUST match the gadgets bit for bit -- gadget_riscv64_jalr_cached
// (jit/guest-riscv64/alu.S) and the ret_cache_dispatch macro
// (jit/guest-arm64/control.S) both spell it:
//   eor xN, target, target, lsr #12   ->   (ip ^ (ip >> 12))
//   ubfx xN, xN, #1, #12              ->   (... >> 1) & 0xfff
// A one-bit disagreement is not a correctness bug (entries self-validate), it
// is a silent 0% hit rate -- which is indistinguishable from "the lever did
// not pay off", and every correctness test still passes.
#define LINKREG_RET_CACHE_HASH(ip) \
    ((size_t) ((((ip) ^ ((ip) >> 12)) >> 1) & (JIT_RETURN_CACHE_SIZE - 1)))

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
    // Guest-address-keyed dispatch cache, shared by two engines that store
    // DIFFERENT things in it. Do not unify them without reading both.
    //
    //   i386  (gadgets-{aarch64,x86_64}/control.S): a map of return address to
    //         pointer-to-call-gadget-arguments. `call` records it; `ret`
    //         validates by re-reading the recorded call's return address and
    //         then dispatches through that call's frontend-patched
    //         return-continuation slot -- so invalidation is handled for it by
    //         the ordinary jump_ip unpatch in jit_block_disconnect.
    //   riscv64 (guest-riscv64/alu.S jalr_cached): a map of guest address to
    //         that address's block->code. The FRONTEND records it at dispatch
    //         (cpu_step_to_interrupt_riscv64), nothing is recorded at the call,
    //         and the entry self-validates by reading block->addr back at a
    //         negative offset. It has no jump_ip to unpatch, so the gadget must
    //         also reject an is_jetsam block itself.
    //
    // Either way an entry names memory inside a jit_block, so every path that
    // frees or invalidates blocks must clear this: jit_entry_scratch_get on a
    // jit switch or a block free, and the per-frontend cleanup_seq purge.
    long ret_cache[JIT_RETURN_CACHE_SIZE];
};
