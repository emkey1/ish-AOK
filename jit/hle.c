// High-level emulation (HLE) of hot, well-specified guest libc functions.
//
// When enabled (default OFF; CLI: ISH_HLE=1, app: the HLE toggle), block
// translation for the arm64 and riscv64 guests checks whether the block's
// start address is the entry point of a known libc function build --
// identified by an exact 64-byte prologue fingerprint, hashed against a
// table extracted from the libcs shipped in the bundled rootfs images. On a
// match, the whole block becomes one "giant gadget": a bridge into the C
// implementation below, which performs the function's entire contract
// against guest memory via tlb_read/tlb_write, writes the ABI return
// register, sets the guest pc to the return address, and exits the block.
//
// Correctness posture:
// - The ABI boundary is the contract: only architectural state at function
//   entry/exit has to be right. Caller-saved registers are legally dead, so
//   they are simply not touched (the values they held at entry persist,
//   which is a valid possible execution).
// - An updated/unknown libc simply never matches a fingerprint: execution
//   falls through to ordinary translation. HLE is a pure fast path; plain
//   emulation remains the always-correct fallback.
// - memcpy is implemented with memmove semantics (overlap-safe). For
//   overlapping buffers the guest program's behavior is undefined anyway;
//   a deterministic superset is the conservative choice.
// - memcmp returns the difference of the first differing bytes (exactly
//   musl's behavior; sign-compatible with any conforming caller of glibc's).
// - A fault mid-operation delivers a guest SIGSEGV with pc rewound to the
//   function entry, as if the first instruction had faulted. Partial writes
//   may have happened -- same as real hardware faulting mid-memcpy.
//
// The fingerprint table lives in hle-table.inc: glibc entries come from
// tools/hle_fingerprint_guest.c run inside the guest (dlsym resolves the
// ifunc to the variant actually selected under iSH's AT_HWCAP); musl
// entries from offline extraction of the .so's dynamic symbols (see that
// tool's header comment).

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "jit/gen.h"
#include "kernel/abi.h"
#include "kernel/task.h"
#include "debug.h"
#include "misc.h"

// Set from ISH_HLE (main.c) or the app preference observer (AppDelegate.m).
bool doEnableHLE = false;

enum hle_fn {
    HLE_MEMCPY,   // dst, src, n -> dst
    HLE_MEMMOVE,  // dst, src, n -> dst
    HLE_MEMSET,   // dst, c, n -> dst
    HLE_STRLEN,   // s -> len
    HLE_MEMCMP,   // a, b, n -> sign
    HLE_STRCMP,   // a, b -> sign
    HLE_STRNCMP,  // a, b, n -> sign
    HLE_MEMCHR,   // s, c, n -> ptr or NULL
    HLE_STRCHR,   // s, c -> ptr or NULL (incl. the NUL when c==0)
    HLE_STRCPY,   // dst, src -> dst
    HLE_STPCPY,   // dst, src -> dst + len (end)
    HLE_STRNCPY,  // dst, src, n -> dst (NUL-pad to n)
    HLE_STRCAT,   // dst, src -> dst
    HLE_STRRCHR,  // s, c -> last occurrence or NULL
    HLE_STRNLEN,  // s, n -> min(strlen, n)
    HLE_MEMRCHR,  // s, c, n -> last occurrence in n bytes or NULL
    HLE_STRNCAT,  // dst, src, n -> dst
    HLE_STPNCPY,  // dst, src, n -> dst + strnlen(src, n)
    HLE_RAWMEMCHR,// s, c -> first occurrence (c assumed present)
    HLE_STRSPN,   // s, accept -> len of initial all-in-accept run
    HLE_STRCSPN,  // s, reject -> len of initial none-in-reject run
    HLE_STRPBRK,  // s, accept -> first byte in accept, or NULL
};

#define HLE_PROLOGUE_LEN 64

struct hle_fingerprint {
    enum guest_abi abi;
    enum hle_fn fn;
    const char *name; // symbol + libc build, for tracing
    uint8_t bytes[HLE_PROLOGUE_LEN];
};

static const struct hle_fingerprint hle_table[] = {
#include "hle-table.inc"
};

static bool hle_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_HLE_TRACE") != NULL ? 1 : 0;
    return enabled == 1;
}

// ---- Optional call statistics (ISH_HLE_STATS=1) -------------------------
// Per-function call counts plus a size histogram for the length-bearing
// calls, dumped to stderr when the emulator exits. The point is to answer
// "how many HLE calls does workload X make, of what sizes" -- the sizing
// data that decides whether per-call overhead (bridge + block exit) can
// matter and where a small-n cutoff should sit.

static const char *const hle_fn_names[] = {
    "memcpy", "memmove", "memset", "strlen", "memcmp", "strcmp", "strncmp",
    "memchr", "strchr", "strcpy", "stpcpy", "strncpy", "strcat", "strrchr",
    "strnlen", "memrchr", "strncat", "stpncpy", "rawmemchr", "strspn",
    "strcspn", "strpbrk",
};
#define HLE_FN_COUNT (sizeof(hle_fn_names)/sizeof(hle_fn_names[0]))
static _Atomic uint64_t hle_stat_calls[HLE_FN_COUNT];
// Size buckets: 0-15, 16-63, 64-255, 256-1023, 1k-4k, 4k-64k, >=64k;
// [7] counts calls with no length argument (unbounded str functions).
static _Atomic uint64_t hle_stat_sizes[8];

// Dump target. cli_halt runs after do_exit has torn down the guest fd
// table, which (when stdio is piped) closes the underlying host fd 2 --
// so main.c saves a dup of stderr here at startup and the dump uses raw
// dprintf on it.
int hle_stats_fd = 2;

static void hle_nm_dump_final(void); // near-miss ranking, trace mode only

void hle_stats_dump(void) {
    uint64_t total = 0;
    for (unsigned i = 0; i < HLE_FN_COUNT; i++)
        total += atomic_load(&hle_stat_calls[i]);
    if (total == 0)
        return;
    dprintf(hle_stats_fd, "hle stats: %llu calls\n", (unsigned long long) total);
    for (unsigned i = 0; i < HLE_FN_COUNT; i++) {
        uint64_t c = atomic_load(&hle_stat_calls[i]);
        if (c != 0)
            dprintf(hle_stats_fd, "hle stats:   %-10s %llu\n", hle_fn_names[i],
                    (unsigned long long) c);
    }
    static const char *const bucket_names[8] = {
        "0-15", "16-63", "64-255", "256-1023", "1k-4k", "4k-64k", ">=64k",
        "unsized" };
    for (unsigned i = 0; i < 8; i++) {
        uint64_t c = atomic_load(&hle_stat_sizes[i]);
        if (c != 0)
            dprintf(hle_stats_fd, "hle stats:   size %-8s %llu\n",
                    bucket_names[i], (unsigned long long) c);
    }
    hle_nm_dump_final();
}

// Dumped from cli_halt (main.c) on emulator exit, same pattern as
// quiesce_stats_dump -- atexit is deliberately not used there.
static bool hle_stats_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_HLE_STATS") != NULL ? 1 : 0;
    return enabled == 1;
}

static void hle_stats_note(unsigned fn, qword_t a1, qword_t a2) {
    atomic_fetch_add(&hle_stat_calls[fn], 1);
    uint64_t n;
    switch ((enum hle_fn) fn) {
    case HLE_MEMCPY: case HLE_MEMMOVE: case HLE_MEMSET: case HLE_MEMCMP:
    case HLE_STRNCMP: case HLE_MEMCHR: case HLE_STRNCPY: case HLE_MEMRCHR:
    case HLE_STRNCAT: case HLE_STPNCPY:
        n = a2;
        break;
    case HLE_STRNLEN:
        n = a1;
        break;
    default:
        atomic_fetch_add(&hle_stat_sizes[7], 1);
        return;
    }
    unsigned b = n < 16 ? 0 : n < 64 ? 1 : n < 256 ? 2 : n < 1024 ? 3
               : n < 4096 ? 4 : n < 65536 ? 5 : 6;
    atomic_fetch_add(&hle_stat_sizes[b], 1);
}

#if defined(__aarch64__)

// Near-miss candidate table (trace mode only): recurring unmatched
// block-start prologues, so future fingerprints/functions come from data.
// File-scope so the exit-time stats dump can print the final ranking.
enum { NM_SLOTS = 4096 };
static struct hle_nm_slot { _Atomic uint64_t hash; _Atomic uint64_t count;
                            _Atomic uint64_t ip;
                            _Atomic(const char *) name; _Atomic bool ifunc;
                          } nm[NM_SLOTS];
static _Atomic uint64_t nm_total;

// ---- Symbol-table-driven attach -----------------------------------------
// The fingerprint table only matches the exact libc builds it was extracted
// from; one `apk upgrade musl` and every fingerprint goes stale, silently
// disabling HLE. This second attach path is content-independent: when a
// block-start address misses the fingerprint table but lies inside a
// file-backed mapping whose name looks like a libc (musl's ld-musl-*.so.1 /
// libc.musl-*.so.1, glibc's libc.so.6 / libc-*.so), the ELF's dynamic
// symbol table is parsed (via the mapping's retained struct fd) and the
// address is matched against the known function names directly. Parsed
// results are cached globally, keyed by a content hash of the ELF header
// block, so identical libc files parse once and remaps can never leave the
// cache stale (it stores file-relative offsets, never guest addresses; the
// load bias is recomputed from the live page tables on every lookup).
//
// glibc's STT_GNU_IFUNC string functions are deliberately skipped -- their
// st_value is the resolver, not the implementation; the bundled-glibc
// fingerprints (taken via dlsym inside the guest) keep covering those.
// ISH_HLE_SYMTAB=0 turns this path off for debugging.

#include "emu/memory.h"

struct hle_load { uint64_t vaddr, offset, filesz; };
struct hle_module {
    uint64_t key;                  // FNV of ELF header block ^ file size
    bool usable;                   // parsed and contains at least one match
    unsigned nloads, nsyms;
    struct hle_load loads[8];
    struct { uint64_t vaddr; uint8_t fn; } syms[48];
    // Trace-only (ISH_HLE_TRACE): every defined function symbol, so the
    // near-miss tracker can print candidate NAMES instead of raw hashes.
    struct hle_allsym { uint64_t vaddr; uint32_t name_off; bool ifunc; } *all;
    unsigned nall;
    char *all_names;               // strtab copy the name_offs index into
};
static struct hle_module hle_modules[16];
static unsigned hle_module_count;
static pthread_mutex_t hle_modules_lock = PTHREAD_MUTEX_INITIALIZER;

static bool hle_symtab_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("ISH_HLE_SYMTAB");
        enabled = (v == NULL || strcmp(v, "0") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

// ISH_HLE_FP=0 disables the fingerprint table (debug/testing: forces every
// attach through the symbol-table path so it can be exercised against libcs
// whose prologues happen to still match the shipped fingerprints).
static bool hle_fingerprints_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("ISH_HLE_FP");
        enabled = (v == NULL || strcmp(v, "0") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

static bool hle_pread_all(struct fd *fd, void *buf, size_t size, uint64_t off) {
    if (fd == NULL || fd->ops == NULL || fd->ops->pread == NULL)
        return false;
    size_t done = 0;
    while (done < size) {
        ssize_t n = fd->ops->pread(fd, (char *) buf + done, size - done,
                (off_t) (off + done));
        if (n <= 0)
            return false;
        done += (size_t) n;
    }
    return true;
}

// Does this /proc/maps-style mapping name look like a libc?
static bool hle_libc_name(const char *name) {
    if (name == NULL)
        return false;
    const char *base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    return strncmp(base, "ld-musl-", 8) == 0
        || strncmp(base, "libc.musl-", 10) == 0
        || strcmp(base, "libc.so.6") == 0
        || strncmp(base, "libc-", 5) == 0;
}

// vaddr -> file offset via the module's PT_LOADs; ~0 if outside any.
static uint64_t hle_vaddr_to_off(const struct hle_module *m, uint64_t vaddr) {
    for (unsigned i = 0; i < m->nloads; i++) {
        const struct hle_load *l = &m->loads[i];
        if (vaddr >= l->vaddr && vaddr < l->vaddr + l->filesz)
            return vaddr - l->vaddr + l->offset;
    }
    return ~0ull;
}

// ELF64 constants, local to avoid host <elf.h> availability questions.
#define HLE_EM_AARCH64 183
#define HLE_EM_RISCV 243
#define HLE_PT_LOAD 1
#define HLE_PT_DYNAMIC 2
#define HLE_DT_HASH 4
#define HLE_DT_STRTAB 5
#define HLE_DT_SYMTAB 6
#define HLE_DT_STRSZ 10
#define HLE_DT_GNU_HASH 0x6ffffef5ull
#define HLE_STT_FUNC 2
#define HLE_STT_GNU_IFUNC 10

struct hle_elf_sym {           // Elf64_Sym, packed exactly
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};

// Parse the ELF at `fd` into `m`. Returns with m->usable set iff the file is
// a loadable little-endian ELF64 shared object for `abi` whose dynsym
// contains at least one function we implement. Bounded work: header block,
// phdrs (<=64), dynamic (<=1024 entries), strtab <=1MB, dynsym <=64k syms.
static void hle_module_parse(struct hle_module *m, struct fd *fd,
        enum guest_abi abi) {
    uint8_t ehdr[64];
    if (!hle_pread_all(fd, ehdr, sizeof(ehdr), 0))
        return;
    if (memcmp(ehdr, "\x7f""ELF", 4) != 0 || ehdr[4] != 2 /*ELFCLASS64*/ ||
            ehdr[5] != 1 /*little-endian*/)
        return;
    uint16_t machine; memcpy(&machine, ehdr + 18, 2);
    if (machine != (abi == GUEST_ABI_RISCV64 ? HLE_EM_RISCV : HLE_EM_AARCH64))
        return;
    uint64_t phoff; memcpy(&phoff, ehdr + 32, 8);
    uint16_t phnum; memcpy(&phnum, ehdr + 56, 2);
    if (phnum == 0 || phnum > 64)
        return;

    uint64_t dyn_off = 0, dyn_size = 0;
    for (unsigned i = 0; i < phnum; i++) {
        uint8_t ph[56];
        if (!hle_pread_all(fd, ph, sizeof(ph), phoff + i * 56ull))
            return;
        uint32_t type; memcpy(&type, ph, 4);
        uint64_t off, vaddr, filesz;
        memcpy(&off, ph + 8, 8);
        memcpy(&vaddr, ph + 16, 8);
        memcpy(&filesz, ph + 32, 8);
        if (type == HLE_PT_LOAD && m->nloads < 8) {
            m->loads[m->nloads].vaddr = vaddr;
            m->loads[m->nloads].offset = off;
            m->loads[m->nloads].filesz = filesz;
            m->nloads++;
        } else if (type == HLE_PT_DYNAMIC) {
            dyn_off = off;
            dyn_size = filesz;
        }
    }
    if (m->nloads == 0 || dyn_off == 0 || dyn_size < 16)
        return;

    uint64_t symtab_va = 0, strtab_va = 0, hash_va = 0, gnu_hash_va = 0,
             strsz = 0;
    unsigned ndyn = (unsigned) (dyn_size / 16);
    if (ndyn > 1024)
        ndyn = 1024;
    for (unsigned i = 0; i < ndyn; i++) {
        uint64_t tag, val;
        uint8_t d[16];
        if (!hle_pread_all(fd, d, sizeof(d), dyn_off + i * 16ull))
            return;
        memcpy(&tag, d, 8);
        memcpy(&val, d + 8, 8);
        if (tag == 0)
            break;
        else if (tag == HLE_DT_SYMTAB) symtab_va = val;
        else if (tag == HLE_DT_STRTAB) strtab_va = val;
        else if (tag == HLE_DT_HASH) hash_va = val;
        else if (tag == HLE_DT_GNU_HASH) gnu_hash_va = val;
        else if (tag == HLE_DT_STRSZ) strsz = val;
    }
    if (symtab_va == 0 || strtab_va == 0 || strsz == 0 || strsz > (1u << 20))
        return;
    uint64_t symtab_off = hle_vaddr_to_off(m, symtab_va);
    uint64_t strtab_off = hle_vaddr_to_off(m, strtab_va);
    if (symtab_off == ~0ull || strtab_off == ~0ull)
        return;

    // Symbol count: DT_HASH's nchain when present, else walk DT_GNU_HASH.
    uint64_t nsyms = 0;
    if (hash_va != 0) {
        uint64_t off = hle_vaddr_to_off(m, hash_va);
        uint32_t words[2];
        if (off == ~0ull || !hle_pread_all(fd, words, sizeof(words), off))
            return;
        nsyms = words[1];
    } else if (gnu_hash_va != 0) {
        uint64_t off = hle_vaddr_to_off(m, gnu_hash_va);
        uint32_t h[4];
        if (off == ~0ull || !hle_pread_all(fd, h, sizeof(h), off))
            return;
        uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
        if (nbuckets == 0 || nbuckets > (1u << 20))
            return;
        uint64_t buckets_off = off + 16 + (uint64_t) bloom_size * 8;
        uint32_t maxsym = 0;
        for (uint32_t i = 0; i < nbuckets; i++) {
            uint32_t b;
            if (!hle_pread_all(fd, &b, 4, buckets_off + i * 4ull))
                return;
            if (b > maxsym)
                maxsym = b;
        }
        if (maxsym < symoffset)
            return;
        uint64_t chain_off = buckets_off + (uint64_t) nbuckets * 4;
        for (;;) {
            uint32_t c;
            if (!hle_pread_all(fd, &c, 4,
                        chain_off + (uint64_t) (maxsym - symoffset) * 4))
                return;
            if (c & 1)
                break;
            maxsym++;
            if (maxsym - symoffset > (1u << 20))
                return;
        }
        nsyms = (uint64_t) maxsym + 1;
    } else {
        return;
    }
    if (nsyms < 2 || nsyms > (1u << 16))
        return;

    char *strtab = malloc(strsz);
    if (strtab == NULL)
        return;
    if (!hle_pread_all(fd, strtab, strsz, strtab_off)) {
        free(strtab);
        return;
    }
    strtab[strsz - 1] = '\0';

    if (hle_trace_enabled()) {
        m->all = calloc(4096, sizeof(*m->all));
        m->all_names = malloc(strsz);
        if (m->all_names != NULL)
            memcpy(m->all_names, strtab, strsz);
    }
    for (uint64_t i = 1; i < nsyms; i++) {
        struct hle_elf_sym sym;
        if (!hle_pread_all(fd, &sym, 24, symtab_off + i * 24))
            break;
        unsigned type = sym.st_info & 0xf;
        if (type != HLE_STT_FUNC && type != HLE_STT_GNU_IFUNC)
            continue;
        if (sym.st_shndx == 0 || sym.st_value == 0)
            continue;
        if (sym.st_name >= strsz)
            continue;
        if (m->all != NULL && m->all_names != NULL && m->nall < 4096) {
            m->all[m->nall].vaddr = sym.st_value;
            m->all[m->nall].name_off = sym.st_name;
            m->all[m->nall].ifunc = type == HLE_STT_GNU_IFUNC;
            m->nall++;
        }
        if (type == HLE_STT_GNU_IFUNC)  // st_value is the resolver, not code
            continue;
        const char *name = strtab + sym.st_name;
        for (unsigned f = 0; f < HLE_FN_COUNT && m->nsyms < 48; f++) {
            if (strcmp(name, hle_fn_names[f]) == 0) {
                m->syms[m->nsyms].vaddr = sym.st_value;
                m->syms[m->nsyms].fn = (uint8_t) f;
                m->nsyms++;
                break;
            }
        }
    }
    free(strtab);
    m->usable = m->nsyms > 0;
}

// Find (or parse) the module for this fd. Returns NULL when the file can't
// be identified/parsed; a cached not-usable entry also returns NULL.
static struct hle_module *hle_module_get(struct fd *fd, enum guest_abi abi) {
    uint8_t head[256];
    if (!hle_pread_all(fd, head, sizeof(head), 0))
        return NULL;
    uint64_t key = 0xcbf29ce484222325ull ^ (uint64_t) abi;
    for (unsigned i = 0; i < sizeof(head); i++)
        key = (key ^ head[i]) * 0x100000001b3ull;
    if (key == 0)
        key = 1;

    pthread_mutex_lock(&hle_modules_lock);
    struct hle_module *m = NULL;
    for (unsigned i = 0; i < hle_module_count; i++) {
        if (hle_modules[i].key == key) {
            m = &hle_modules[i];
            break;
        }
    }
    if (m == NULL && hle_module_count < 16) {
        m = &hle_modules[hle_module_count++];
        memset(m, 0, sizeof(*m));
        m->key = key;
        hle_module_parse(m, fd, abi);
        if (hle_trace_enabled())
            fprintf(stderr, "hle: symtab parse %s: %u loads, %u known syms\n",
                    m->usable ? "usable" : "unusable", m->nloads, m->nsyms);
    }
    pthread_mutex_unlock(&hle_modules_lock);
    return m;
}

// data->hle_memo encoding. 0 means "not resolved yet"; any other value is a
// resolved answer for the abi recorded in it:
//   bit 0     always 1, so a resolved answer is never confusable with 0
//   bits 1-3  the guest abi the answer was resolved under
//   bits 4+   hle_modules slot + 1, or 0 for "this mapping is not a libc"
// Module slots stay valid forever: hle_modules entries are never freed/moved.
#define HLE_MEMO_ABI_SHIFT 1
#define HLE_MEMO_ABI_MASK 0x7u
#define HLE_MEMO_SLOT_SHIFT 4

static unsigned hle_memo_pack(enum guest_abi abi, int slot) {
    return 1u | (((unsigned) abi & HLE_MEMO_ABI_MASK) << HLE_MEMO_ABI_SHIFT)
        | ((unsigned) (slot + 1) << HLE_MEMO_SLOT_SHIFT);
}
static enum guest_abi hle_memo_abi(unsigned memo) {
    return (enum guest_abi) ((memo >> HLE_MEMO_ABI_SHIFT) & HLE_MEMO_ABI_MASK);
}
static struct hle_module *hle_memo_module(unsigned memo) {
    unsigned slot = memo >> HLE_MEMO_SLOT_SHIFT;
    return slot == 0 ? NULL : &hle_modules[slot - 1];
}

// The parsed libc module this mapping belongs to, or NULL if it isn't one.
//
// Memoized on the mapping because everything below is expensive and the
// answer can't change for a given `struct data`: the name comes from the
// retained fd, which the mapping holds for its whole life. Block translation
// calls this on every fingerprint-table miss, so without the memo every
// translated block paid a full path resolution -- on a fakefs root that is
// fakefs_getpath, i.e. SQLite queries -- plus a pread of the ELF header.
// The negative answer is the one that matters most: non-libc mappings are
// the common case and were paying the whole path resolution only to be
// rejected by hle_libc_name() right after.
static struct hle_module *hle_mapping_module(struct data *data,
        enum guest_abi abi) {
    unsigned memo = atomic_load_explicit(&data->hle_memo, memory_order_acquire);
    if (memo != 0 && hle_memo_abi(memo) == abi)
        return hle_memo_module(memo);

    // Mapping name: exec-loader/mmap mappings usually leave data->name NULL
    // and /proc/maps derives the path from the retained fd; do the same. A
    // path that won't resolve memoizes as "not a libc" rather than retrying
    // the failing lookup on every subsequent block.
    const char *name = data->name;
    char pathbuf[MAX_PATH];
    if (name == NULL) {
        extern int generic_getpath(struct fd *fd, char *buf);
        if (generic_getpath(data->fd, pathbuf) == 0)
            name = pathbuf;
    }
    struct hle_module *m = NULL;
    if (name != NULL && hle_libc_name(name))
        m = hle_module_get(data->fd, abi);
    // Release so a reader that takes the memo fast path (and therefore never
    // takes hle_modules_lock) sees the module hle_module_get finished parsing.
    atomic_store_explicit(&data->hle_memo,
            hle_memo_pack(abi, m == NULL ? -1 : (int) (m - hle_modules)),
            memory_order_release);
    return m;
}

// Resolve ip to its libc module and load bias, or return false. The bias is
// recomputed from the live page tables on every call, so a remap of the
// library can never act on stale addresses.
static bool hle_module_and_bias(guest_addr_t ip, enum guest_abi abi,
        struct hle_module **m_out, uint64_t *bias_out) {
    if (!hle_symtab_enabled() || current == NULL || current->mem == NULL)
        return false;
    struct pt_entry *pt = mem_pt(current->mem, PAGE(ip));
    if (pt == NULL || pt->data == NULL)
        return false;
    struct data *data = pt->data;
    if (data->fd == NULL)
        return false;
    struct hle_module *m = hle_mapping_module(data, abi);
    if (m == NULL)
        return false;
    // Bias from the live mapping: this page's guest address minus the vaddr
    // its file offset corresponds to.
    uint64_t page_guest = (uint64_t) PAGE(ip) << PAGE_BITS;
    // pt->offset is the byte offset of this page within data's allocation;
    // data->file_offset is the file offset the allocation starts at.
    uint64_t page_file_off = (uint64_t) data->file_offset + pt->offset;
    uint64_t page_vaddr = ~0ull;
    for (unsigned i = 0; i < m->nloads; i++) {
        const struct hle_load *l = &m->loads[i];
        if (page_file_off >= l->offset &&
                page_file_off < l->offset + l->filesz) {
            page_vaddr = page_file_off - l->offset + l->vaddr;
            break;
        }
    }
    if (page_vaddr == ~0ull || page_guest < page_vaddr)
        return false;
    *m_out = m;
    *bias_out = page_guest - page_vaddr;
    return true;
}

// Fingerprint-table miss fallback: is `ip` the entry of a known function per
// the dynamic symbol table of the libc mapped at this address?
static bool hle_symtab_lookup(guest_addr_t ip, enum guest_abi abi,
        enum hle_fn *fn_out, const char **name_out) {
    struct hle_module *m;
    uint64_t bias;
    if (!hle_module_and_bias(ip, abi, &m, &bias) || !m->usable)
        return false;
    for (unsigned i = 0; i < m->nsyms; i++) {
        if (bias + m->syms[i].vaddr == ip) {
            *fn_out = (enum hle_fn) m->syms[i].fn;
            *name_out = hle_fn_names[m->syms[i].fn];
            return true;
        }
    }
    return false;
}

// Trace-only: name of the libc function whose entry is exactly ip, or NULL.
// Feeds the near-miss tracker so its candidate list is a ranked list of
// function NAMES -- the data that decides what to implement next.
static const char *hle_symtab_resolve_name(guest_addr_t ip, enum guest_abi abi,
        bool *ifunc_out) {
    struct hle_module *m;
    uint64_t bias;
    if (!hle_module_and_bias(ip, abi, &m, &bias))
        return NULL;
    if (m->all == NULL || m->all_names == NULL)
        return NULL;
    for (unsigned i = 0; i < m->nall; i++) {
        if (bias + m->all[i].vaddr == ip) {
            *ifunc_out = m->all[i].ifunc;
            return m->all_names + m->all[i].name_off;
        }
    }
    return NULL;
}

// ---- Emission (called from jit.c at block-translation start) ------------

extern void gadget_arm64_hle_call(void);
#if ISH_GUEST_RISCV64
extern void gadget_riscv64_hle_call(void);
#else
// The riscv64 gadgets are only compiled when that guest is selected
// (meson.build gates jit/guest-riscv64/*.S on guest_archs), but this file is
// built unconditionally -- so a build with a SUBSET of guest_archs that omits
// riscv64 failed to link on an undefined _gadget_riscv64_hle_call. Nothing
// reaches the riscv64 branch in such a build, because the `riscv64' flag is
// only ever set by the riscv64 decoder, so a never-called stub is honest and
// keeps hle_emit_word's shape identical across configurations.
static void gadget_riscv64_hle_call(void) { __builtin_unreachable(); }
#endif

// Emit the whole-block hle_call gadget with code-stream word `word` at guest
// address `ip`, claiming `len` guest bytes so gen_end's end_addr covers them
// and the block invalidates if those bytes are ever rewritten/unmapped.
static void hle_emit_word(struct gen_state *state, unsigned long word,
        guest_addr_t ip, unsigned len, bool riscv64) {
    gen_raw(state, (unsigned long) (riscv64
                ? (void (*)(void)) gadget_riscv64_hle_call
                : (void (*)(void)) gadget_arm64_hle_call));
    gen_raw(state, word);
    gen_raw(state, (unsigned long) ip);
    if (riscv64)
        state->riscv64_ip = ip + len;
    else
        state->arm64_ip = ip + len;
}

// Emit for a plain fingerprinted/symtab libc function.
static void hle_emit(struct gen_state *state, enum hle_fn fn, guest_addr_t ip,
        bool riscv64) {
    // Bit 8 of the word tells hle_call which guest ABI's registers to use
    // (cpu_state itself carries no abi field).
    hle_emit_word(state, (unsigned long) fn | (riscv64 ? 0x100ul : 0), ip,
            HLE_PROLOGUE_LEN, riscv64);
}

// ---- Copy/set loop idiom recognition (arm64 guest) ----------------------
// A fingerprint or symbol can only catch a libc CALL; an open-coded copy
// loop (inlined memcpy at -O0/-O1, LZMA's match-copy, hand-rolled byte
// loops) never has an entry to attach to. But such a loop IS a block: the
// backward conditional branch makes its head a block start, so the same
// translation hook can recognize the whole block as one bulk operation.
//
// Recognized shapes (all post-index, forward, unit stride of the element
// size; the trailing branch must target the block start exactly):
//   copy: LDR{B,H,W,X} Rt,[Rs],#k ; STR Rt,[Rd],#k ; <counter> ; <branch>
//   fill:                          STR Rt,[Rd],#k ; <counter> ; <branch>
//   counter/branch pairs: SUBS Rc,Rc,#1 + B.NE  |  SUB Rc,Rc,#1 + CBNZ Rc
//                       | CMP Ra,Re + B.NE  (Ra = the incremented Rs or Rd)
//
// The executor performs the loop's exact architectural contract: registers
// advance per iteration, Rt holds the last element loaded, NZCV reflects
// the last executed SUBS/CMP, and a fault leaves state at the precise
// iteration boundary (or mid-iteration for a store fault, pc on the store)
// with pc rewound so ordinary translation retakes over. Work is chunked so
// no single call runs unbounded; an unfinished chunk re-enters this same
// block via pc = loop head. Guest-overlapping forward copies (LZ77 match
// copies with distance < length) reproduce byte-forward propagation --
// periodic tiling of the leading [src,dst) bytes -- never memmove.
//
// ISH_HLE_LOOPS=0 turns the recognizer off for debugging.

#define HLE_LOOP_FN 0x40u   // word low byte marking a loop spec
// spec word layout (above the low byte):
//   bit  8      riscv64 (always 0: arm64-only recognizer for now)
//   bits 12-16  Rt (data reg; 31 = WZR for fill)
//   bits 17-21  Rs (src addr reg; 31 = none, fill)
//   bits 22-26  Rd (dst addr reg)
//   bits 27-31  Rc (count reg, or end reg for CMP loops)
//   bits 32-35  log2 element size
//   bit  36     fill (store-only loop)
//   bits 37-38  counter kind: 0 SUB+CBNZ, 1 SUBS+B.NE, 2 CMP+B.NE
//   bit  39     CMP compares the src reg (else the dst reg)
//   bits 40-47  loop length in bytes

static bool hle_loops_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("ISH_HLE_LOOPS");
        enabled = (v == NULL || strcmp(v, "0") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

// arm64 decode helpers (small, mask-based; only the shapes above).
struct hle_a64_ldst { unsigned rt, rn, k; bool load; };
static bool hle_a64_ldst_post(uint32_t insn, struct hle_a64_ldst *out) {
    // LDR/STR (immediate, post-index): size:2 111 0 00 opc:2 0 imm9 01 Rn Rt
    if ((insn & 0x3F200C00) != 0x38000400)
        return false;
    unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    if (opc > 1)
        return false; // signed loads etc.: not these loops
    int imm9 = (int) ((insn >> 12) & 0x1FF);
    if (imm9 & 0x100)
        return false; // negative stride: not recognized
    unsigned k = 1u << size;
    if ((unsigned) imm9 != k)
        return false; // stride must equal the element size
    out->rt = insn & 31;
    out->rn = (insn >> 5) & 31;
    out->k = k;
    out->load = opc == 1;
    return true;
}
static bool hle_a64_dec1(uint32_t insn, bool *flags, unsigned *rc) {
    if ((insn & 0xFFFFFC00) == 0xF1000400 && (insn & 31) == ((insn >> 5) & 31)) {
        *flags = true;  // SUBS Xc, Xc, #1
        *rc = insn & 31;
        return true;
    }
    if ((insn & 0xFFFFFC00) == 0xD1000400 && (insn & 31) == ((insn >> 5) & 31)) {
        *flags = false; // SUB Xc, Xc, #1
        *rc = insn & 31;
        return true;
    }
    return false;
}
static bool hle_a64_cmp_rr(uint32_t insn, unsigned *rn, unsigned *rm) {
    // CMP Xn, Xm == SUBS XZR, Xn, Xm (shifted reg, shift 0)
    if ((insn & 0xFFE0FC1F) != 0xEB00001F)
        return false;
    *rn = (insn >> 5) & 31;
    *rm = (insn >> 16) & 31;
    return *rn != 31 && *rm != 31;
}
static bool hle_a64_bne_to(uint32_t insn, guest_addr_t addr, guest_addr_t target) {
    if ((insn & 0xFF00001F) != 0x54000001)
        return false; // B.NE
    int64_t off = ((int64_t) (int32_t) (insn << 8) >> 13) * 4;
    return addr + (uint64_t) off == target;
}
static bool hle_a64_cbnz_to(uint32_t insn, unsigned rc, guest_addr_t addr,
        guest_addr_t target) {
    if ((insn & 0xFF000000) != 0xB5000000)
        return false; // CBNZ Xt (64-bit)
    if ((insn & 31) != rc)
        return false;
    int64_t off = ((int64_t) (int32_t) (insn << 8) >> 13) * 4;
    return addr + (uint64_t) off == target;
}

static bool hle_try_loop_arm64(struct gen_state *state, struct tlb *tlb,
        guest_addr_t ip) {
    if (!hle_loops_enabled())
        return false;
    uint32_t insn[4];
    if (!tlb_read(tlb, ip, insn, sizeof(insn)))
        return false;

    struct hle_a64_ldst ld, st;
    unsigned base; // index of the counter instruction
    bool fill;
    if (hle_a64_ldst_post(insn[0], &ld) && ld.load &&
            hle_a64_ldst_post(insn[1], &st) && !st.load &&
            st.rt == ld.rt && st.k == ld.k) {
        fill = false;
        base = 2;
    } else if (hle_a64_ldst_post(insn[0], &st) && !st.load) {
        fill = true;
        base = 1;
    } else {
        return false;
    }

    unsigned rt = st.rt, rd = st.rn, k = st.k;
    unsigned rs = fill ? 31 : ld.rn;
    if (rd == 31 || (!fill && rs == 31))
        return false; // reg 31 here is SP; keep out of these loops
    if (!fill && (rt == 31 || rt == rs || rt == rd || rs == rd))
        return false;

    unsigned ckind, rc;
    bool cmp_on_src = false;
    bool subs_flags;
    unsigned crc;
    guest_addr_t baddr = ip + (base + 1) * 4;
    if (hle_a64_dec1(insn[base], &subs_flags, &crc)) {
        ckind = subs_flags ? 1 : 0;
        rc = crc;
        if (subs_flags ? !hle_a64_bne_to(insn[base + 1], baddr, ip)
                       : !hle_a64_cbnz_to(insn[base + 1], rc, baddr, ip))
            return false;
    } else {
        unsigned cn, cm;
        if (!hle_a64_cmp_rr(insn[base], &cn, &cm))
            return false;
        if (cn == rs)
            cmp_on_src = true;
        else if (cn != rd)
            return false;
        ckind = 2;
        rc = cm; // the end register
        if (!hle_a64_bne_to(insn[base + 1], baddr, ip))
            return false;
    }
    // The counter/end register must be none of the loop's other registers.
    if (rc == 31 || rc == rd || (!fill && (rc == rs || rc == rt)))
        return false;
    if (fill && rc == rt)
        return false;

    unsigned len = (base + 2) * 4;
    unsigned long spec = HLE_LOOP_FN
        | ((unsigned long) rt << 12)
        | ((unsigned long) rs << 17)
        | ((unsigned long) rd << 22)
        | ((unsigned long) rc << 27)
        | ((unsigned long) __builtin_ctz(k) << 32)
        | ((unsigned long) (fill ? 1 : 0) << 36)
        | ((unsigned long) ckind << 37)
        | ((unsigned long) (cmp_on_src ? 1 : 0) << 39)
        | ((unsigned long) len << 40);
    if (hle_trace_enabled())
        fprintf(stderr, "hle: attach %s-loop k=%u ckind=%u at %#llx (pid %d)\n",
                fill ? "fill" : "copy", k, ckind, (unsigned long long) ip,
                current != NULL ? current->pid : -1);
    hle_emit_word(state, spec, ip, len, false);
    return true;
}

bool hle_try_emit(struct gen_state *state, struct tlb *tlb, guest_addr_t ip,
        bool riscv64) {
    if (hle_trace_enabled()) {
        static _Atomic bool announced = false;
        if (!atomic_exchange(&announced, true))
            fprintf(stderr, "hle: hook reached, enabled=%d riscv64=%d ip=%#llx\n",
                    doEnableHLE, riscv64, (unsigned long long) ip);
    }
    if (!doEnableHLE)
        return false;
    // Skip addresses that cannot be a function entry: arm64 instructions are
    // 4-aligned, but riscv64 with RVC has 2-aligned entries -- musl-riscv64
    // really does place memcpy/strcmp/memchr/strnlen at 2-mod-4 addresses,
    // and an `ip & 3` test here silently excluded them from HLE entirely.
    if (ip & (riscv64 ? 1 : 3))
        return false;
    uint8_t code[HLE_PROLOGUE_LEN];
    if (!tlb_read(tlb, ip, code, sizeof(code)))
        return false;
    enum guest_abi abi = riscv64 ? GUEST_ABI_RISCV64 : GUEST_ABI_ARM64;
    for (size_t i = hle_fingerprints_enabled() ? 0
                : sizeof(hle_table)/sizeof(hle_table[0]);
            i < sizeof(hle_table)/sizeof(hle_table[0]); i++) {
        const struct hle_fingerprint *fp = &hle_table[i];
        if (fp->abi != abi)
            continue;
        if (memcmp(fp->bytes, code, HLE_PROLOGUE_LEN) != 0)
            continue;
        if (hle_trace_enabled())
            fprintf(stderr, "hle: attach %s at %#llx (pid %d)\n", fp->name,
                    (unsigned long long) ip, current != NULL ? current->pid : -1);
        hle_emit(state, fp->fn, ip, riscv64);
        return true;
    }
    // Fingerprint miss: try the dynamic symbol table of the mapped libc
    // (survives libc upgrades the fingerprint table has never seen).
    {
        enum hle_fn sfn;
        const char *sname;
        if (hle_symtab_lookup(ip, abi, &sfn, &sname)) {
            if (hle_trace_enabled())
                fprintf(stderr, "hle: attach %s:dynsym at %#llx (pid %d)\n",
                        sname, (unsigned long long) ip,
                        current != NULL ? current->pid : -1);
            hle_emit(state, sfn, ip, riscv64);
            return true;
        }
    }
    // Not a known function entry: maybe an open-coded copy/fill loop.
    if (!riscv64 && hle_try_loop_arm64(state, tlb, ip))
        return true;
    // Near-miss candidate discovery (trace mode only): count recurring
    // unmatched block-start prologues so future fingerprints come from data.
    // Tiny open-addressed table keyed by a 64-bit FNV of the prologue; the
    // top recurrers are dumped at powers-of-two totals. Zero cost when
    // ISH_HLE_TRACE is off.
    if (hle_trace_enabled()) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned b = 0; b < HLE_PROLOGUE_LEN; b++)
            h = (h ^ code[b]) * 0x100000001b3ull;
        // If this block start is exactly a libc dynsym function entry,
        // record its name -- candidates then dump ranked BY NAME.
        bool nm_ifunc = false;
        const char *nm_name = hle_symtab_resolve_name(ip, abi, &nm_ifunc);
        unsigned slot = (unsigned) (h % NM_SLOTS);
        for (unsigned probe = 0; probe < 8; probe++, slot = (slot + 1) % NM_SLOTS) {
            uint64_t expect = 0;
            if (atomic_load(&nm[slot].hash) == h ||
                    atomic_compare_exchange_strong(&nm[slot].hash, &expect, h)) {
                atomic_store(&nm[slot].ip, ip);
                if (nm_name != NULL) {
                    atomic_store(&nm[slot].name, nm_name);
                    atomic_store(&nm[slot].ifunc, nm_ifunc);
                }
                atomic_fetch_add(&nm[slot].count, 1);
                break;
            }
        }
        uint64_t total = atomic_fetch_add(&nm_total, 1) + 1;
        if (total >= 4096 && (total & (total - 1)) == 0) {
            // A block translates roughly once (then it's cached), so a given
            // prologue's count is "how many distinct translation events hit
            // this exact function" -- a function used across many processes
            // (each fork/exec re-translates its libc) recurs; a one-off does
            // not. Print the recurring ones (count >= 2), highest first, so
            // the output is a ranked candidate list, not 4096 singletons.
            // Selection sort over the small hot subset, capped at 24 lines.
            fprintf(stderr, "hle: near-miss candidates at %llu unmatched blocks (count ip hash):\n",
                    (unsigned long long) total);
            unsigned lines = 0;
            for (unsigned i2 = 0; i2 < NM_SLOTS && lines < 24; i2++) {
                uint64_t c = atomic_load(&nm[i2].count);
                if (c >= 2) {
                    const char *nom = atomic_load(&nm[i2].name);
                    fprintf(stderr, "hle:   %8llu %#llx %016llx %s%s\n",
                            (unsigned long long) c,
                            (unsigned long long) atomic_load(&nm[i2].ip),
                            (unsigned long long) atomic_load(&nm[i2].hash),
                            nom != NULL ? nom : "",
                            nom != NULL && atomic_load(&nm[i2].ifunc)
                                ? " (ifunc)" : "");
                    lines++;
                }
            }
        }
    }
    return false;
}

// Exit-time near-miss ranking (needs both trace and stats mode: trace to
// have collected, stats to have a live dump fd). Sorted by count, so the
// top lines are the next functions worth implementing; entries with names
// are libc dynsym entries, the strongest candidates.
static void hle_nm_dump_final(void) {
    uint64_t total = atomic_load(&nm_total);
    if (total == 0)
        return;
    dprintf(hle_stats_fd, "hle stats: %llu unmatched block starts; "
            "recurring candidates (count ip name):\n",
            (unsigned long long) total);
    // Selection-sort the top 40 by count (small table, exit path).
    bool used[NM_SLOTS] = { false };
    for (unsigned line = 0; line < 40; line++) {
        uint64_t best = 0; unsigned bi = NM_SLOTS;
        for (unsigned i = 0; i < NM_SLOTS; i++) {
            uint64_t c = atomic_load(&nm[i].count);
            if (!used[i] && c > best) { best = c; bi = i; }
        }
        if (bi == NM_SLOTS || best < 2)
            break;
        used[bi] = true;
        const char *nom = atomic_load(&nm[bi].name);
        dprintf(hle_stats_fd, "hle stats:   %8llu %#llx %s%s\n",
                (unsigned long long) best,
                (unsigned long long) atomic_load(&nm[bi].ip),
                nom != NULL ? nom : "-",
                nom != NULL && atomic_load(&nm[bi].ifunc) ? " (ifunc)" : "");
    }
}

#else // !__aarch64__

static void hle_nm_dump_final(void) {}

// The arm64/riscv64 guest JITs only exist on aarch64 hosts; elsewhere HLE
// never applies (jit.c still calls this, so keep the symbol).
bool hle_try_emit(struct gen_state *UNUSED(state), struct tlb *UNUSED(tlb),
        guest_addr_t UNUSED(ip), bool UNUSED(riscv64)) {
    return false;
}

#endif // __aarch64__

// ---- Runtime implementations (called from the hle_call gadgets) ---------

// These resolve a DIRECT host pointer to guest memory, one page at a time,
// and run a single native host memcpy/memset/memcmp over each in-page span
// -- the same direct-host-access the JIT's TLB fast path uses, but as one
// fully-vectorized libc call per page instead of a per-16-byte gadget loop.
// (An earlier revision bounced every 256 bytes through a stack buffer via
// tlb_read/tlb_write: 2x memory traffic plus a TLB lookup per chunk, which
// benchmarked ~2x SLOWER than the plain translated glibc memcpy. Direct
// pointers remove both costs.) __tlb_{read,write}_ptr handle TLB misses and
// write-revalidation and return NULL on a genuine fault.
//
// Note: unlike tlb_write, the direct write path does not invoke
// arm64_watch_scan_value (the ISH_ARM64_WATCH_LO16 debug store watchpoint).
// That instrumentation is a debug-only diagnostic, not correctness, so HLE
// stores are simply invisible to it -- acceptable, and HLE is off by default
// anyway.

// The fault path needs to know which guest pc to rewind; threaded through
// from hle_call's fn word rather than sniffing cpu state.
static __thread bool hle_is_riscv64;

static bool hle_fault(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t entry_ip, bool was_write) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = was_write;
    // Rewind to the function entry: the fault is reported as if the first
    // instruction of the function had taken it.
    if (hle_is_riscv64)
        cpu->riscv64_pc = entry_ip;
    else
        cpu->arm64_pc = entry_ip;
    return false;
}

// Bytes from addr to the end of its page.
static inline uint64_t hle_to_page_end(guest_addr_t addr) {
    return PAGE_SIZE - (addr & (PAGE_SIZE - 1));
}

static bool hle_copy(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, guest_addr_t src, uint64_t n) {
    if (dst == src || n == 0)
        return true;
    if (dst < src || src + n <= dst) {
        // Forward: safe when dst is below src or the ranges don't overlap.
        while (n > 0) {
            uint64_t span = n;
            uint64_t sp = hle_to_page_end(src), dp = hle_to_page_end(dst);
            if (span > sp) span = sp;
            if (span > dp) span = dp;
            void *sh = __tlb_read_ptr(tlb, src);
            if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
            void *dh = __tlb_write_ptr(tlb, dst);
            if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
            memcpy(dh, sh, span);
            src += span; dst += span; n -= span;
        }
    } else {
        // Overlapping with dst above src: copy back-to-front, still one
        // native memcpy per (min-aligned) span.
        guest_addr_t s = src + n, d = dst + n;
        while (n > 0) {
            // Span backward is bounded by each address's offset within its
            // page (distance to page START), min 1.
            uint64_t so = (s & (PAGE_SIZE - 1)); if (so == 0) so = PAGE_SIZE;
            uint64_t doff = (d & (PAGE_SIZE - 1)); if (doff == 0) doff = PAGE_SIZE;
            uint64_t span = n;
            if (span > so) span = so;
            if (span > doff) span = doff;
            s -= span; d -= span; n -= span;
            void *sh = __tlb_read_ptr(tlb, s);
            if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
            void *dh = __tlb_write_ptr(tlb, d);
            if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
            memmove(dh, sh, span);
        }
    }
    return true;
}

static bool hle_set(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, uint8_t c, uint64_t n) {
    while (n > 0) {
        uint64_t span = n, dp = hle_to_page_end(dst);
        if (span > dp) span = dp;
        void *dh = __tlb_write_ptr(tlb, dst);
        if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
        memset(dh, c, span);
        dst += span; n -= span;
    }
    return true;
}

static bool hle_strlen(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint64_t *len_out) {
    uint64_t len = 0;
    for (;;) {
        // Scan one page at a time (never touching the next page until the
        // current one has no NUL -- matches what a page-aligned vector strlen
        // is allowed to read, so a string ending just before an unmapped page
        // doesn't spuriously fault).
        uint64_t span = hle_to_page_end(s + len);
        const uint8_t *p = __tlb_read_ptr(tlb, s + len);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *nul = memchr(p, 0, span);
        if (nul != NULL) {
            *len_out = len + (uint64_t) (nul - p);
            return true;
        }
        len += span;
    }
}

static bool hle_cmp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t a, guest_addr_t b, uint64_t n, int64_t *res_out) {
    while (n > 0) {
        uint64_t span = n, ap = hle_to_page_end(a), bp = hle_to_page_end(b);
        if (span > ap) span = ap;
        if (span > bp) span = bp;
        const uint8_t *ah = __tlb_read_ptr(tlb, a);
        if (ah == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *bh = __tlb_read_ptr(tlb, b);
        if (bh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        if (memcmp(ah, bh, span) != 0) {
            for (uint64_t i = 0; i < span; i++) {
                if (ah[i] != bh[i]) {
                    *res_out = (int64_t) ah[i] - (int64_t) bh[i];
                    return true;
                }
            }
        }
        a += span; b += span; n -= span;
    }
    *res_out = 0;
    return true;
}

// strcmp / strncmp: compare byte-by-byte within each page span, stopping at
// the first difference or at a shared NUL. The result is the unsigned-char
// difference of the first differing bytes (musl's exact behavior; POSIX
// only guarantees the sign, which every conforming caller relies on). For
// strncmp, `limit` bounds the comparison; strcmp passes ~0.
static bool hle_strcmp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t a, guest_addr_t b, uint64_t limit, int64_t *res_out) {
    uint64_t done = 0;
    while (done < limit) {
        uint64_t span = limit - done;
        uint64_t ap = hle_to_page_end(a), bp = hle_to_page_end(b);
        if (span > ap) span = ap;
        if (span > bp) span = bp;
        const uint8_t *ah = __tlb_read_ptr(tlb, a);
        if (ah == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *bh = __tlb_read_ptr(tlb, b);
        if (bh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (ah[i] != bh[i]) {
                *res_out = (int64_t) ah[i] - (int64_t) bh[i];
                return true;
            }
            if (ah[i] == 0) { // shared terminator: strings equal up to here
                *res_out = 0;
                return true;
            }
        }
        a += span; b += span; done += span;
    }
    *res_out = 0; // strncmp hit its limit with no difference
    return true;
}

// memchr / strchr: scan for byte `c`. memchr is bounded by `n`; strchr is
// unbounded and also matches the terminating NUL when c==0 (its documented
// behavior). Returns the guest address of the match, or 0 for "not found".
// strchr never reads past the NUL (matches a page-safe vector strchr).
static bool hle_chr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, uint64_t n, bool is_str, guest_addr_t *res_out) {
    uint64_t done = 0;
    for (;;) {
        if (!is_str && done >= n) {
            *res_out = 0;
            return true;
        }
        uint64_t span = hle_to_page_end(s);
        if (!is_str && span > n - done)
            span = n - done;
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == c) {
                *res_out = s + i;
                return true;
            }
            if (is_str && p[i] == 0) { // reached end of string without a match
                *res_out = 0;          // (c==0 matched above, so c!=0 here)
                return true;
            }
        }
        s += span; done += span;
    }
}

// strcpy / stpcpy: copy src (including its NUL) to dst. Single pass -- scan
// each src page span for the NUL, copy up to it (or the page/dst-page limit)
// with a native memcpy. stpcpy returns dst+len (the NUL); strcpy returns dst.
// `limit` bounds it for strncpy (~0 for strcpy/stpcpy); `pad` requests the
// strncpy NUL-fill of the tail. Result (end address) is returned via end_out.
static bool hle_stpcpy(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, guest_addr_t src, uint64_t limit, bool pad,
        guest_addr_t *end_out) {
    guest_addr_t d = dst, s = src;
    uint64_t copied = 0;
    for (;;) {
        if (copied == limit) { // strncpy hit n with no NUL: no terminator written
            *end_out = d;
            return true;
        }
        uint64_t span = hle_to_page_end(s);
        uint64_t dspan = hle_to_page_end(d);
        if (span > dspan) span = dspan;
        if (span > limit - copied) span = limit - copied;
        const uint8_t *sh = __tlb_read_ptr(tlb, s);
        if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        uint8_t *dh = __tlb_write_ptr(tlb, d);
        if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
        const uint8_t *nul = memchr(sh, 0, span);
        uint64_t take = nul != NULL ? (uint64_t) (nul - sh) + 1 : span;
        memcpy(dh, sh, take);
        if (nul != NULL) {
            *end_out = d + (uint64_t) (nul - sh);   // address of the written NUL
            if (pad) {                              // strncpy: zero-fill to limit
                guest_addr_t pd = d + take;
                uint64_t rem = limit - (copied + take);
                if (!hle_set(cpu, tlb, entry_ip, pd, 0, rem))
                    return false;
            }
            return true;
        }
        s += span; d += span; copied += span;
    }
}

static bool hle_strrchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, guest_addr_t *res_out) {
    guest_addr_t last = 0; bool found = false;
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == c) { last = s + i; found = true; }
            if (p[i] == 0) { // include the NUL as a candidate when c==0
                if (c == 0) { last = s + i; found = true; }
                *res_out = found ? last : 0;
                return true;
            }
        }
        s += span;
    }
}

static bool hle_strnlen(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint64_t maxlen, uint64_t *len_out) {
    uint64_t len = 0;
    while (len < maxlen) {
        uint64_t span = hle_to_page_end(s + len);
        if (span > maxlen - len) span = maxlen - len;
        const uint8_t *p = __tlb_read_ptr(tlb, s + len);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *nul = memchr(p, 0, span);
        if (nul != NULL) { *len_out = len + (uint64_t) (nul - p); return true; }
        len += span;
    }
    *len_out = maxlen;
    return true;
}

// memrchr: last occurrence of c in n bytes. Must examine all n bytes; scan
// each page span with the native memrchr-free approach (walk forward tracking
// the last match -- one native pass per span, cache-friendly).
static bool hle_memrchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, uint64_t n, guest_addr_t *res_out) {
    guest_addr_t last = 0; bool found = false;
    uint64_t done = 0;
    while (done < n) {
        uint64_t span = hle_to_page_end(s);
        if (span > n - done) span = n - done;
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++)
            if (p[i] == c) { last = s + i; found = true; }
        s += span; done += span;
    }
    *res_out = found ? last : 0;
    return true;
}

// rawmemchr: like memchr but unbounded -- the caller guarantees c is present,
// so the scan finds it before running off into unmapped memory (a fault here
// means the guarantee was violated, i.e. a genuine guest bug -> guest SIGSEGV).
static bool hle_rawmemchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, guest_addr_t *res_out) {
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *hit = memchr(p, c, span);
        if (hit != NULL) { *res_out = s + (uint64_t) (hit - p); return true; }
        s += span;
    }
}

// Read a guest C string into a 256-entry membership set (for strspn/cspn/pbrk).
// Bounded work: stops at the NUL or once all 256 byte values are marked.
static bool hle_build_set(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t set_str, bool set[256]) {
    memset(set, 0, 256 * sizeof(bool));
    guest_addr_t s = set_str;
    unsigned distinct = 0;
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == 0)
                return true;
            if (!set[p[i]]) { set[p[i]] = true; if (++distinct == 255) { /* all non-NUL seen */ } }
        }
        s += span;
    }
}

// strspn/strcspn: length of the initial run of s whose bytes are (in_set ?
// members : non-members) of the accept/reject set. strpbrk (via ptr_out !=
// NULL) instead returns the address of the first set member, or 0.
static bool hle_spn(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, guest_addr_t set_str, bool want_in_set,
        uint64_t *len_out, guest_addr_t *ptr_out) {
    bool set[256];
    if (!hle_build_set(cpu, tlb, entry_ip, set_str, set))
        return false;
    guest_addr_t cur = s;
    for (;;) {
        uint64_t span = hle_to_page_end(cur);
        const uint8_t *p = __tlb_read_ptr(tlb, cur);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            bool member = set[p[i]];
            // strspn stops at first non-member (also at NUL, which is a
            // non-member unless '\0' is in accept -- but strspn's accept never
            // meaningfully contains NUL since build_set stops at it, so NUL
            // always terminates the run). strcspn/strpbrk stop at first member
            // or the NUL.
            bool stop = want_in_set ? !member : member;
            if (p[i] == 0) stop = true;
            if (stop) {
                if (ptr_out != NULL)
                    *ptr_out = (p[i] != 0 && member) ? cur + i : 0; // strpbrk
                if (len_out != NULL)
                    *len_out = (uint64_t) (cur + i - s);
                return true;
            }
        }
        cur += span;
    }
}

#if defined(__aarch64__)
// Executor for recognized copy/fill loops (see the recognizer above for the
// contract). Chunked: at most CHUNK iterations per gadget entry; if the loop
// isn't finished, pc is left at the loop head so the dispatcher re-enters
// this same block. All state written back is exactly what the interpreted
// loop would have produced at an iteration boundary -- except a store fault
// in a copy loop, which architecturally happens mid-iteration (the load's
// post-increment already retired): there Rs/Rt reflect the completed load
// and pc lands on the store instruction itself.
static int hle_loop_exec(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long spec, unsigned long entry_ip) {
    qword_t *regs = cpu->arm64_regs;
    unsigned rt = (spec >> 12) & 31, rs = (spec >> 17) & 31,
             rd = (spec >> 22) & 31, rc = (spec >> 27) & 31;
    unsigned k = 1u << ((spec >> 32) & 15);
    bool fill = (spec >> 36) & 1;
    unsigned ckind = (spec >> 37) & 3;
    bool cmp_on_src = (spec >> 39) & 1;
    unsigned looplen = (spec >> 40) & 0xff;

    uint64_t s0 = fill ? 0 : regs[rs], d0 = regs[rd], c0 = regs[rc];
    // Iterations until the loop's own exit condition. These are do-while
    // loops: a zero count (SUBS) or already-equal pointers (CMP) still run
    // one iteration and then wrap the full 2^64 -- represent as "huge" and
    // let chunking reproduce it faithfully.
    uint64_t remaining;
    if (ckind == 2) {
        uint64_t delta = c0 - (cmp_on_src ? s0 : d0);
        remaining = (delta == 0 || delta % k != 0) ? UINT64_MAX : delta / k;
    } else {
        remaining = c0 == 0 ? UINT64_MAX : c0;
    }
    enum { CHUNK = 1u << 20 };
    uint64_t todo = remaining < CHUNK ? remaining : CHUNK;

    uint8_t pat[8] = { 0 };
    if (fill && rt != 31)
        memcpy(pat, &regs[rt], 8);

    uint64_t done = 0;
    int fault = 0; // 0 none, 1 read, 2 write mid-iteration, 3 write at boundary
    while (done < todo) {
        uint64_t s = s0 + done * k, d = d0 + done * k;
        uint64_t elems = todo - done;
        uint64_t cap = hle_to_page_end(d) / k;
        if (!fill) {
            uint64_t scap = hle_to_page_end(s) / k;
            if (scap < cap)
                cap = scap;
        }
        if (cap == 0) {
            // Element straddles a page boundary: do this one via the
            // bounce-buffer tlb ops (correct, rare).
            uint8_t tmp[8];
            if (!fill) {
                if (!tlb_read(tlb, s, tmp, k)) { fault = 1; break; }
            } else {
                memcpy(tmp, pat, k);
            }
            if (!tlb_write(tlb, d, tmp, k)) { fault = fill ? 3 : 2; break; }
            done++;
            continue;
        }
        if (elems > cap)
            elems = cap;
        uint64_t bytes = elems * k;
        const uint8_t *sh = NULL;
        if (!fill) {
            sh = __tlb_read_ptr(tlb, s);
            if (sh == NULL) { fault = 1; break; }
        }
        uint8_t *dh = __tlb_write_ptr(tlb, d);
        if (dh == NULL) { fault = fill ? 3 : 2; break; }

        if (fill) {
            if (k == 1) {
                memset(dh, pat[0], bytes);
            } else {
                for (uint64_t i = 0; i < bytes; i += k)
                    memcpy(dh + i, pat, k);
            }
        } else if (d == s) {
            // copying a range onto itself: no visible change
        } else if (d > s && d - s < bytes) {
            // Forward-overlapping copy (LZ77 match): byte-forward semantics
            // duplicate the leading [s,d) bytes periodically. Only byte
            // loops get the fast tiling; k>1 with sub-element overlap keeps
            // exact element load/store order via the aliased host pointers.
            uint64_t p = d - s;
            if (k == 1) {
                for (uint64_t i = 0; i < bytes; i++)
                    dh[i] = i < p ? sh[i] : dh[i - p];
            } else {
                for (uint64_t i = 0; i < bytes; i += k) {
                    uint8_t tmp[8];
                    memcpy(tmp, sh + i, k);
                    memcpy(dh + i, tmp, k);
                }
            }
        } else {
            // No architectural read-after-write hazard; memmove for the
            // host-aliasing case, same result as the element loop.
            memmove(dh, sh, bytes);
        }
        done += elems;
    }

    // Rt: the last element the loop loaded (copy loops only). On a store
    // fault that's the faulting iteration's element; otherwise the last
    // completed one. Loads zero-extend into the 64-bit reg, matching LDR*.
    if (!fill && (done > 0 || fault == 2)) {
        uint64_t eaddr = s0 + (fault == 2 ? done : done - 1) * k;
        uint64_t v = 0;
        if (tlb_read(tlb, eaddr, &v, k))
            regs[rt] = v;
    }
    regs[rd] = d0 + done * k;
    if (!fill)
        regs[rs] = s0 + (done + (fault == 2 ? 1 : 0)) * k;
    if (ckind != 2)
        regs[rc] = c0 - done;
    // NZCV from the last executed SUBS/CMP (none executed -> flags keep
    // their entry values, which is architecturally exact).
    if (ckind == 1 && done > 0) {
        uint64_t a = c0 - done + 1, res = c0 - done;
        uint32_t nzcv = 0;
        if (res >> 63) nzcv |= 0x80000000u;
        if (res == 0) nzcv |= 0x40000000u;
        if (a >= 1) nzcv |= 0x20000000u;
        if (((a ^ 1ull) & (a ^ res)) >> 63) nzcv |= 0x10000000u;
        cpu->arm64_nzcv = nzcv;
    } else if (ckind == 2 && done > 0) {
        uint64_t a = (cmp_on_src ? s0 : d0) + done * k, b = c0;
        uint64_t res = a - b;
        uint32_t nzcv = 0;
        if (res >> 63) nzcv |= 0x80000000u;
        if (res == 0) nzcv |= 0x40000000u;
        if (a >= b) nzcv |= 0x20000000u;
        if (((a ^ b) & (a ^ res)) >> 63) nzcv |= 0x10000000u;
        cpu->arm64_nzcv = nzcv;
    }

    if (fault != 0) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = fault != 1;
        cpu->arm64_pc = entry_ip + (fault == 2 ? 4 : 0);
        return INT_PF;
    }
    cpu->arm64_pc = done < remaining ? entry_ip : entry_ip + looplen;
    return INT_NONE;
}
#endif // __aarch64__

// Gadget entry point. Reads args from the guest ABI registers, performs the
// operation, writes the return register, and sets the guest pc to the return
// address. Returns INT_NONE on success (the gadget then exits the block via
// jit_ret) or INT_PF on a guest memory fault (the gadget exits via jit_exit).
int hle_call(struct cpu_state *cpu, struct tlb *tlb, unsigned long fn,
        unsigned long entry_ip) {
#if defined(__aarch64__)
    if ((fn & 0xff) == HLE_LOOP_FN)
        return hle_loop_exec(cpu, tlb, fn, entry_ip);
#endif
    bool rv = (fn & 0x100) != 0;
    fn &= 0xff;
    hle_is_riscv64 = rv;
    qword_t *regs = rv ? cpu->riscv64_regs : cpu->arm64_regs;
    qword_t a0 = regs[rv ? (int) riscv64_a0 : 0];
    qword_t a1 = regs[rv ? (int) riscv64_a1 : 1];
    qword_t a2 = regs[rv ? (int) riscv64_a2 : 2];
    qword_t ret_addr = regs[rv ? (int) riscv64_ra : (int) arm64_x30];
    qword_t result = a0;
    bool ok;

    if (hle_stats_enabled())
        hle_stats_note(fn, a1, a2);

    switch ((enum hle_fn) fn) {
    case HLE_MEMCPY:
    case HLE_MEMMOVE:
        ok = hle_copy(cpu, tlb, entry_ip, a0, a1, a2);
        break;
    case HLE_MEMSET:
        ok = hle_set(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2);
        break;
    case HLE_STRLEN: {
        uint64_t len = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &len);
        result = len;
        break;
    }
    case HLE_MEMCMP: {
        int64_t res = 0;
        ok = hle_cmp(cpu, tlb, entry_ip, a0, a1, a2, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_STRCMP: {
        int64_t res = 0;
        ok = hle_strcmp(cpu, tlb, entry_ip, a0, a1, ~0ull, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_STRNCMP: {
        int64_t res = 0;
        ok = hle_strcmp(cpu, tlb, entry_ip, a0, a1, a2, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_MEMCHR: {
        guest_addr_t p = 0;
        ok = hle_chr(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2, false, &p);
        result = p;
        break;
    }
    case HLE_STRCHR: {
        guest_addr_t p = 0;
        ok = hle_chr(cpu, tlb, entry_ip, a0, (uint8_t) a1, 0, true, &p);
        result = p;
        break;
    }
    case HLE_STRCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, ~0ull, false, &end);
        result = a0; // strcpy returns dst
        break;
    }
    case HLE_STPCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, ~0ull, false, &end);
        result = end; // stpcpy returns the address of the written NUL
        break;
    }
    case HLE_STRNCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, a2, true, &end);
        result = a0;
        break;
    }
    case HLE_STRCAT: {
        // strcat = strcpy(dst + strlen(dst), src); returns dst.
        uint64_t dlen = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &dlen);
        if (ok) {
            guest_addr_t end = 0;
            ok = hle_stpcpy(cpu, tlb, entry_ip, a0 + dlen, a1, ~0ull, false, &end);
        }
        result = a0;
        break;
    }
    case HLE_STRRCHR: {
        guest_addr_t p = 0;
        ok = hle_strrchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, &p);
        result = p;
        break;
    }
    case HLE_STRNLEN: {
        uint64_t len = 0;
        ok = hle_strnlen(cpu, tlb, entry_ip, a0, a1, &len);
        result = len;
        break;
    }
    case HLE_MEMRCHR: {
        guest_addr_t p = 0;
        ok = hle_memrchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2, &p);
        result = p;
        break;
    }
    case HLE_STRNCAT: {
        // strncat = copy min(strlen(src), n) bytes after dst's NUL, then
        // always terminate. Returns dst.
        uint64_t dlen = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &dlen);
        if (ok) {
            guest_addr_t end = 0;
            ok = hle_stpcpy(cpu, tlb, entry_ip, a0 + dlen, a1, a2, false, &end);
            if (ok)
                ok = hle_set(cpu, tlb, entry_ip, end, 0, 1); // terminate (no-op if already NUL)
        }
        result = a0;
        break;
    }
    case HLE_STPNCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, a2, true, &end);
        result = end; // dst + strnlen(src, n)
        break;
    }
    case HLE_RAWMEMCHR: {
        guest_addr_t p = 0;
        ok = hle_rawmemchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, &p);
        result = p;
        break;
    }
    case HLE_STRSPN: {
        uint64_t len = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, true, &len, NULL);
        result = len;
        break;
    }
    case HLE_STRCSPN: {
        uint64_t len = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, false, &len, NULL);
        result = len;
        break;
    }
    case HLE_STRPBRK: {
        guest_addr_t p = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, false, NULL, &p);
        result = p;
        break;
    }
    default:
        ok = false; // unreachable with a well-formed table
        cpu->segfault_addr = entry_ip;
        cpu->segfault_was_write = false;
        break;
    }

    if (!ok)
        return INT_PF;
    // x0/a0 gets the return value; pc jumps to the caller. riscv64's x0 slot
    // is the hardwired zero -- writing regs[10] (a0) is correct there.
    regs[rv ? riscv64_a0 : 0] = result;
    if (rv)
        cpu->riscv64_pc = ret_addr;
    else
        cpu->arm64_pc = ret_addr;
    return INT_NONE;
}
