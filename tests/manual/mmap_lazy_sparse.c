// A sparse use of a huge reservation must cost what it uses, not what it
// reserves.
//
// Bun's JavaScriptCore reserves 64 GiB of address space (MAP_NORESERVE, read
// and write), mprotects 622 MiB at its top PROT_NONE, and allocates from pages
// anywhere inside. AOK records an anonymous mapping of 64 MiB or more as a
// lazy reservation with no page-table entries (emu/memory.h, struct
// mem_lazy_map), and two things turned that back into the whole 64 GiB of
// entries: an mprotect of any part of a reservation materialised all of it,
// and a first touch materialised everything from the reservation's start to
// the page touched. Either built 16 million entries -- 1.1 GB of host page
// tables, two seconds with every guest thread stopped -- where Linux touches a
// few page-table pages. OpenCode's server (a Bun binary) sat at a 2.2 GB host
// footprint for it, and on a loaded Mac stalled for minutes at a time.
//
// The witness is VmPTE in /proc/self/status, which Linux reports as the memory
// in the process's page tables and AOK as the host memory in its page-table
// chunks and leaves. A positive control first: touching every page of a 32 MiB
// mapping must move it on both kernels. Then each sparse operation must move it
// by less than LIMIT_KB. Each is checked for its semantics too, since a cheap
// wrong answer would pass the cost check.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_common.h"

#define MB (1024UL * 1024)
#define GB (1024UL * MB)
#define RESERVE (32 * GB)
// Linux: a handful of 4 KiB page-table pages. AOK before the fix: 500 MB and
// more for each of the operations below. Well clear of both.
#define LIMIT_KB (8 * 1024L)

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static long vm_pte_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "VmPTE: %ld kB", &kb) == 1)
            break;
    fclose(f);
    return kb;
}

static sigjmp_buf fault_jb;
static void on_segv(int sig) {
    (void) sig;
    siglongjmp(fault_jb, 1);
}

// 1 if reading *p faults, 0 if not.
static int read_faults(volatile char *p) {
    if (sigsetjmp(fault_jb, 1))
        return 1;
    (void) *p;
    return 0;
}

static char *reserve(void) {
    char *p = mmap(NULL, RESERVE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    struct sigaction sa = {.sa_handler = on_segv};
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    long before = vm_pte_kb();
    if (before < 0) {
        printf("FAIL no VmPTE line in /proc/self/status\n");
        return 1;
    }

    // Positive control: page tables for 32 MiB of touched pages.
    size_t ctl_len = 32 * MB;
    char *ctl = mmap(NULL, ctl_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(ctl != MAP_FAILED, "control mmap");
    if (ctl != MAP_FAILED) {
        for (size_t off = 0; off < ctl_len; off += 4096)
            ctl[off] = 1;
        long after = vm_pte_kb();
        check(after > before, "control: touching 32 MiB moves VmPTE (%ld -> %ld kB)",
              before, after);
        munmap(ctl, ctl_len);
    }

    // 1. A first touch deep inside a reservation.
    char *a = reserve();
    if (a == NULL) {
        printf("mmap_lazy_sparse: SKIP (cannot reserve %lu GiB: %s)\n",
               RESERVE / GB, strerror(errno));
        return failures_total ? 1 : 0;
    }
    long base = vm_pte_kb();
    a[20 * GB] = 42;
    a[20 * GB + 3 * MB] = 43;
    long used = vm_pte_kb() - base;
    check(used < LIMIT_KB, "touch 20 GiB in costs %ld kB of page tables (limit %ld)",
          used, LIMIT_KB);
    check(a[20 * GB] == 42 && a[20 * GB + 3 * MB] == 43, "the touched bytes read back");
    check(a[10 * GB] == 0 && a[RESERVE - 1] == 0, "untouched pages below and above read zero");

    // 2. mprotect near the top, as JavaScriptCore does, and one strictly
    // inside. Neither may build entries for the reservation.
    char *b = reserve();
    check(b != NULL, "second reservation");
    if (b != NULL) {
        base = vm_pte_kb();
        char *top = b + RESERVE - 64 * MB;
        int r = mprotect(top, 64 * MB, PROT_NONE);
        check(r == 0, "mprotect top 64 MiB PROT_NONE (%s)", r == 0 ? "ok" : strerror(errno));
        char *mid = b + 7 * GB;
        r = mprotect(mid, 1 * MB, PROT_READ);
        check(r == 0, "mprotect 1 MiB inside PROT_READ (%s)", r == 0 ? "ok" : strerror(errno));
        used = vm_pte_kb() - base;
        check(used < LIMIT_KB, "two mprotects cost %ld kB of page tables (limit %ld)",
              used, LIMIT_KB);

        check(read_faults(top) == 1, "a PROT_NONE page faults");
        check(read_faults(top + 64 * MB - 1) == 1, "the last PROT_NONE page faults");
        check(read_faults(top - 1) == 0 && top[-1] == 0, "the page below it reads zero");
        check(read_faults(mid) == 0 && mid[0] == 0, "a PROT_READ page reads zero");
        b[3 * GB] = 7;
        check(b[3 * GB] == 7, "a write below the PROT_READ piece sticks");
        (mid + 1 * MB)[0] = 9;
        check((mid + 1 * MB)[0] == 9, "a write just above the PROT_READ piece sticks");

        // And back: made writable again, the pages are usable and still zero.
        check(mprotect(top, 64 * MB, PROT_READ | PROT_WRITE) == 0, "mprotect top back RW");
        check(read_faults(top) == 0 && top[0] == 0, "reopened page reads zero");
        top[4096] = 5;
        check(top[4096] == 5, "reopened page is writable");
        used = vm_pte_kb() - base;
        check(used < LIMIT_KB, "after reopening: %ld kB of page tables (limit %ld)",
              used, LIMIT_KB);

        // A hint over the whole reservation is not a reason to build entries,
        // and a hole in its range is still ENOMEM.
        check(madvise(b, RESERVE, MADV_DONTDUMP) == 0, "MADV_DONTDUMP over the reservation");
        check(madvise(b, RESERVE, MADV_DODUMP) == 0, "MADV_DODUMP over the reservation");
        used = vm_pte_kb() - base;
        check(used < LIMIT_KB, "after madvise hints: %ld kB of page tables", used);
        check(munmap(b + 5 * GB, 4096) == 0, "punch a one-page hole");
        errno = 0;
        r = madvise(b + 5 * GB - 4096, 3 * 4096, MADV_DONTDUMP);
        int e = errno;
        check(r == -1 && e == ENOMEM, "a hint over a hole is ENOMEM (got %d, errno %d)", r, e);
        munmap(b, RESERVE);
    }
    munmap(a, RESERVE);

    if (failures_total == 0)
        printf("mmap_lazy_sparse: PASS\n");
    return failures_total ? 1 : 0;
}
