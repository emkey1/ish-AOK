// ISH_JIT_PROFILE=<file>: the dynamic instruction mix of arm64 and riscv64
// guest code, for deciding what to fuse or translate natively.
//
// Every guest instruction costs about the same under the gadget JIT (~1.5 ns
// on an M5, whatever it does), so what a workload spends is set by how many
// of each it runs. Guest PC sampling cannot say: the PC is published only when
// execution returns to the dispatcher, and chained blocks never do. So each
// translated block starts with a gadget that bumps its own counter, and keeps
// the guest instruction words it was made from. At exit the file gets one
// line per block that ran: "<abi> <count> <addr> <insn> <insn> ...", and
// tools/jitprof-report.py turns that into the mix, the run lengths of vector
// instructions, and the commonest adjacent pairs.
//
// Blocks are never freed (a translated block that is thrown away and made
// again gets a second record); the counters race between threads. Both are
// fine for a measurement and would not be for anything else.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit/jitprof.h"

struct jitprof_block {
    uint64_t count;               // bumped by the block_count gadget
    uint64_t addr;
    struct jitprof_block *next;
    uint32_t *insns;
    uint32_t n, cap;
    uint8_t abi;
};

static pthread_once_t jitprof_once = PTHREAD_ONCE_INIT;
static const char *jitprof_path;
static pthread_mutex_t jitprof_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jitprof_block *jitprof_blocks;

// Called from main.c's cli_halt, as the other instruments' dumps are (it
// exits with _exit, so atexit would never run). No-op unless ISH_JIT_PROFILE.
void jitprof_dump(void) {
    if (jitprof_path == NULL)
        return;
    FILE *f = fopen(jitprof_path, "w");
    if (f == NULL)
        return;
    pthread_mutex_lock(&jitprof_lock);
    for (struct jitprof_block *b = jitprof_blocks; b != NULL; b = b->next) {
        uint64_t count = __atomic_load_n(&b->count, __ATOMIC_RELAXED);
        if (count == 0 || b->n == 0)
            continue;
        fprintf(f, "%s %llu %#llx", b->abi == JITPROF_RISCV64 ? "riscv64" : "arm64",
                (unsigned long long) count, (unsigned long long) b->addr);
        for (uint32_t i = 0; i < b->n; i++)
            fprintf(f, " %08x", b->insns[i]);
        fputc('\n', f);
    }
    pthread_mutex_unlock(&jitprof_lock);
    fclose(f);
}

static void jitprof_init(void) {
    const char *path = getenv("ISH_JIT_PROFILE");
    if (path == NULL || *path == '\0')
        return;
    jitprof_path = strdup(path);
}

struct jitprof_block *jitprof_block_new(uint64_t addr, enum jitprof_abi abi) {
    pthread_once(&jitprof_once, jitprof_init);
    if (jitprof_path == NULL)
        return NULL;
    struct jitprof_block *b = calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->addr = addr;
    b->abi = (uint8_t) abi;
    pthread_mutex_lock(&jitprof_lock);
    b->next = jitprof_blocks;
    jitprof_blocks = b;
    pthread_mutex_unlock(&jitprof_lock);
    return b;
}

uint64_t *jitprof_counter(struct jitprof_block *block) {
    return &block->count;
}

void jitprof_note(struct jitprof_block *block, uint32_t insn) {
    if (block->n == block->cap) {
        uint32_t cap = block->cap ? block->cap * 2 : 16;
        uint32_t *insns = realloc(block->insns, cap * sizeof(*insns));
        if (insns == NULL)
            return;
        block->insns = insns;
        block->cap = cap;
    }
    block->insns[block->n++] = insn;
}
