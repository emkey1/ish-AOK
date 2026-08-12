#ifndef JIT_H
#define JIT_H
#define ENGINE_JIT 1
#include <stdatomic.h>
#include "misc.h"
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"

#if ENGINE_JIT

#define JIT_INITIAL_HASH_SIZE (1 << 10)
#define JIT_CACHE_SIZE (1 << 10)
#define JIT_PAGE_HASH_SIZE (1 << 10)

struct jit {
    // there is one jit per address space
    struct mmu *mmu;
    size_t mem_used;
    size_t num_blocks;

    struct list *hash;
    size_t hash_size;

    // list of jit_blocks that should be freed soon (at the next RCU grace
    // period, if we had such a thing)
    struct list jetsam;

    // A way to look up blocks in a page
    struct {
        struct list blocks[2];
    } *page_hash;
    
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;

    lock_t lock;
    wrlock_t jetsam_lock;
    // Set once jit_teardown_lock has acquired jetsam_lock for write, ahead
    // of mem_destroy's own pt_unmap_always call -- see jit_invalidate_lock's
    // comment in jit.c for why pt_unmap_always must skip re-acquiring in
    // that case (pthread_rwlock write locks are not recursive).
    bool teardown_locked;
    // Incremented after each jit_free_jetsam in OOM paths; frames compare
    // against last_block_cleanup_seq to detect stale last_block pointers.
    atomic_uint cleanup_seq;
    // Set to true before acquiring jetsam_lock for write, cleared after. All
    // goroutines point their cpu->poked_ptr here, so jit_ret_chain sees this
    // flag on every block boundary and exits jit_enter, releasing the read
    // lock promptly — even if a linked-block cycle somehow forms.
    uint8_t write_wanted;
};

// this is roughly the average number of instructions in a basic block according to anonymous sources
// times 4, roughly the average number of gadgets/parameters in an instruction, according to anonymous sources
#define JIT_BLOCK_INITIAL_CAPACITY 16

struct jit_block {
    guest_addr_t addr;
    guest_addr_t end_addr;
    size_t used;

    // pointers to the ip values in the last gadget
    unsigned long *jump_ip[2];
    // original values of *jump_ip[]
    unsigned long old_jump_ip[2];
    // blocks that jump to this block
    struct list jumps_from[2];

    // hashtable bucket links
    struct list chain;
    // list of blocks in a page
    struct list page[2];
    // links for jumps_from
    struct list jumps_from_links[2];
    // links for free list
    struct list jetsam;
    bool is_jetsam;

    unsigned long code[];
};

// Create a new jit
struct jit *jit_new(struct mmu *mmu);
void jit_free(struct jit *jit);

// Exclude any CLONE_VM sibling thread still executing chained JIT code in
// this jit before a full-address-space teardown (mem_destroy) invalidates
// and frees every block. Returns true if the exclusion was actually
// acquired; false on timeout, in which case the caller must proceed
// without it (the teardown cannot be abandoned). The lock is NEVER
// released: jit_free frees the struct the lock lives inside, so the only
// safe shapes are "free while holding" or "never acquired" -- an unlock
// after jit_free is a use-after-free (this happened; see mem_destroy's
// comment). By mem_destroy time the mm refcount is zero, so no thread can
// legitimately be left waiting on it. Mirrors the write_wanted + jetsam
// write-lock protocol every other block-freeing path already uses.
bool jit_teardown_lock(struct jit *jit);

// Same exclusion as jit_teardown_lock, for a live invalidation sweep that
// must release the lock afterward instead of freeing the jit while holding
// it. See jit_invalidate_lock's comment in jit.c for the race this closes.
// Returns false on timeout (caller proceeds unprotected); jit_invalidate_unlock
// is a safe no-op to call even when the paired lock call returned false.
bool jit_invalidate_lock(struct jit *jit);
void jit_invalidate_unlock(struct jit *jit);

// Invalidate all jit blocks in pages start (inclusive) to end (exclusive).
// Locks the jit. Should only be called by memory.c in conjunction with
// mem_changed.
void jit_invalidate_range(struct jit *jit, page_t start, page_t end);
void jit_invalidate_page(struct jit *jit, page_t page);
void jit_invalidate_all(struct jit *jit);

// i386 gadget-fusion switches, readable and writable at RUNTIME via
// /proc/ish/i386_jit_fuse. Each bit enables one fused gadget family in
// jit/gen.c; clearing it makes gen fall back to the unfused multi-dispatch
// expansion, so both sides of a fusion live in one binary.
//
// The point of making these live rather than build-time or env-only is
// MEASUREMENT. An env var can only be changed by relaunching, which on a device
// means killing the app (and with it sshd, and the guest /tmp), and it cannot be
// read back to prove it took effect -- two failure modes that silently produced
// meaningless "flat" A/B results before this existed. Here the value that gen.c
// consults and the value `cat` prints are the SAME variable, so a read-back is
// proof of plumbing by construction, and flipping arms costs a file write
// instead of a launch cycle. That makes rep-level interleaving possible, which
// is the only way to keep thermal drift and host load out of a 1-2% result.
//
// Affects NEWLY COMPILED blocks only: already-translated blocks keep whatever
// they were compiled with. A freshly exec'd guest process gets a fresh mm and
// therefore a fresh jit, so running each benchmark rep as its own process is
// enough to pick up a change; nothing invalidates live blocks here on purpose,
// since sweeping another thread's cache from a proc write buys nothing for
// measurement and is a concurrency hazard.
#define JIT_FUSE_ADDR    (1u << 0)  // addr_* folded into the mem load/store gadget
#define JIT_FUSE_MOVMR   (1u << 1)  // mov reg<->[base+disp] in one gadget
#define JIT_FUSE_LEA     (1u << 2)  // lea reg,[base+disp] in one gadget
#define JIT_FUSE_ALU     (1u << 3)  // ALU reg,imm (32-bit) in one gadget
#define JIT_FUSE_PUSHPOP (1u << 4)  // push/pop of a register in one gadget
#define JIT_FUSE_ALL (JIT_FUSE_ADDR | JIT_FUSE_MOVMR | JIT_FUSE_LEA | \
                      JIT_FUSE_ALU | JIT_FUSE_PUSHPOP)

// The arm64 and riscv64 guests get the same treatment, with their own bit
// namespaces (the values overlap; the arch selects which table applies).
//
// arm64's three lookahead passes shared ONE env var, ISH_ARM64_NO_FUSE, so they
// could only be turned off together. Each has its own bit here so a lever can be
// sized on its own; the env var still clears all three, preserving its meaning.
#define JIT_FUSE_A64_BCOND (1u << 0)  // gen_arm64_peek_bcond: compare + branch
#define JIT_FUSE_A64_LDST  (1u << 1)  // gen_arm64_try_ldst_fusion: load/store RMW
#define JIT_FUSE_A64_LDCMP (1u << 2)  // gen_arm64_try_ld_cmp_fusion: load + compare
#define JIT_FUSE_A64_ALL (JIT_FUSE_A64_BCOND | JIT_FUSE_A64_LDST | JIT_FUSE_A64_LDCMP)

// riscv64 had NO switch at all for either of its fusions, so neither could be
// A/B'd without rebuilding. ISH_RISCV64_NO_FUSE now clears both.
#define JIT_FUSE_RV_FOLD (1u << 0)  // gen_riscv64_fold_const: lui/auipc + addi/load
#define JIT_FUSE_RV_JAL  (1u << 1)  // jal_link: call = link write + branch in one
#define JIT_FUSE_RV_ALL (JIT_FUSE_RV_FOLD | JIT_FUSE_RV_JAL)

// Live masks. Seeded on first use from the ISH_NO_*_FUSE / ISH_*_NO_FUSE
// environment variables, whose existing semantics are unchanged (set to ANY value
// = that family off; ISH_NO_MOVMR_FUSE still covers LEA, which shared its gate
// before LEA got its own bit).
unsigned i386_jit_fuse_mask(void);
unsigned arm64_jit_fuse_mask(void);
unsigned riscv64_jit_fuse_mask(void);

// Generic access for the /proc/ish/<arch>_jit_fuse nodes. One implementation
// serves all three; the arch picks the mask and the name table.
enum jit_fuse_arch { JIT_FUSE_ARCH_I386, JIT_FUSE_ARCH_ARM64, JIT_FUSE_ARCH_RISCV64 };
unsigned jit_fuse_mask_get(enum jit_fuse_arch arch);
void jit_fuse_mask_set(enum jit_fuse_arch arch, unsigned mask);
// Walks the arch's table for the show handler; returns NULL past the end.
const char *jit_fuse_name(enum jit_fuse_arch arch, unsigned index, unsigned *bit_out);
// Returns false for a name this arch does not have ("all" is accepted).
bool jit_fuse_set_by_name(enum jit_fuse_arch arch, const char *name, bool on);

bool amd64_jit_is_enabled(void);
void amd64_jit_set_enabled(bool enabled);
bool i386_single_step_comm_matches(const char *comm);
void i386_single_step_comm_set(const char *comm);
void i386_single_step_comm_get(char *buf, size_t bufsize);
bool i386_no_cache_comm_matches(const char *comm);
void i386_no_cache_comm_set(const char *comm);
void i386_no_cache_comm_get(char *buf, size_t bufsize);
void i386_special_trace_reset(pid_t_ tgid, const char *comm);
void i386_trace_special_op(const char *op, addr_t ip);
void i386_trace_special_reg_op(const char *op, addr_t ip, int reg);
struct cpu_state;
void dump_amd64_cc1_jit_trace(const struct cpu_state *cpu);
void jit_cleanup_jetsam_after_interrupt(struct cpu_state *cpu);

#endif

#endif
