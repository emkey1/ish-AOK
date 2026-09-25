// x86_smc_alias.c -- x86 code rewritten through a SECOND mapping of the same
// memory runs as rewritten.
//
// A W^X "dual mapping" JIT maps one shared object twice: read-write, where it
// writes code, and read-execute, where it runs it. .NET 7 and later do this by
// default (a memfd); libffi does it for closures when RWX mmap is refused (a
// memfd, else a temp file). On x86 nothing more is needed: the processor
// fetches from the physical page, so the next call through the executable
// alias sees what the writable alias stored.
//
// AOK keys translated blocks by guest virtual page, and each guest mmap of a
// shared file is its own host mapping. A store through the writable alias
// dropped that page's (empty) block list and never the executable page's, so
// the old translation ran on. Linux 6.12 passes every case below, 64- and
// 32-bit; AOK failed every one on the amd64 and i386 roots, 100 of 100 calls
// stale in the memfd case.
//
//   memfd      -- the .NET shape: memfd_create, one RW and one RX mapping of
//                 the same fd, patch through RW, call through RX
//   file       -- a regular file opened TWICE, each alias from its own open
//                 file description (libffi's temp-file fallback)
//   offset     -- the RX alias maps two pages from offset 0, the RW alias
//                 only the second page: aliases need not start together
//   span       -- an instruction whose immediate crosses the RX alias's page
//                 boundary, patched a byte at a time on each side
//   neighbour  -- two functions on one page; rewriting one leaves the other
//   anon       -- shared anonymous memory aliased by mremap(old_size = 0)
//   thread     -- another thread rewrites through RW and publishes a flag;
//                 this thread waits, serializes (SDM 8.1.3) and calls RX
//
// NOT covered, and AOK does not do it: a store from ANOTHER address space.
// `fork` -- a forked child rewrites through its inherited RW alias and the
// parent calls its own RX alias -- passes on Linux and is 100 of 100 stale on
// AOK, which tracks aliases per address space (jit/jit.c, jit_shared_code).
// Run it with X86_SMC_ALIAS_XPROC=1 in the environment.
//
// Every function is `mov $imm32, %eax; ret`, the same bytes in 32- and 64-bit
// mode, so a stale translation is a wrong return value.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../test_common.h"

#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#else
#define SYS_memfd_create 356
#endif
#endif

#define ROUNDS 100

typedef int (*fn_t)(void);

static void emit(unsigned char *p, uint32_t imm) {
    p[0] = 0xb8;
    memcpy(p + 1, &imm, 4);
    p[5] = 0xc3;
}

static void set_imm(unsigned char *p, uint32_t imm) {
    memcpy(p + 1, &imm, 4);
}

static void check(const char *label, uint32_t got, uint32_t want) {
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
}

static void serialize(void) {
    unsigned a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) : : "memory");
}

static int memfd(size_t len) {
    int fd = (int) syscall(SYS_memfd_create, "x86_smc_alias", 0);
    if (fd < 0) {
        perror("memfd_create");
        exit(1);
    }
    if (ftruncate(fd, (off_t) len) != 0) {
        perror("ftruncate");
        exit(1);
    }
    return fd;
}

static unsigned char *map(size_t len, int prot, int fd, off_t off) {
    void *p = mmap(NULL, len, prot, MAP_SHARED, fd, off);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

// Patch through w, call through x, ROUNDS times; returns the stale count.
static unsigned patch_loop(const char *label, unsigned char *w, unsigned char *x) {
    unsigned stale = 0;
    for (uint32_t i = 1; i <= ROUNDS; i++) {
        set_imm(w, i);
        uint32_t got = ((fn_t) x)();
        if (got != i && stale++ == 0)
            check(label, got, i);
    }
    test_logf("%s: %u of %u calls stale\n", label, stale, ROUNDS);
    return stale;
}

static void case_memfd(long pg) {
    int fd = memfd(pg);
    unsigned char *w = map(pg, PROT_READ | PROT_WRITE, fd, 0);
    unsigned char *x = map(pg, PROT_READ | PROT_EXEC, fd, 0);
    emit(w, 0);
    check("memfd first", ((fn_t) x)(), 0);
    patch_loop("memfd", w, x);
    munmap(w, pg);
    munmap(x, pg);
    close(fd);
}

static void case_file(long pg) {
    char path[] = "/tmp/x86_smc_alias.XXXXXX";
    int wfd = mkstemp(path);
    if (wfd < 0) {
        perror("mkstemp");
        failures_total++;
        return;
    }
    int xfd = open(path, O_RDONLY);
    unlink(path);
    if (xfd < 0 || ftruncate(wfd, pg) != 0) {
        perror("open/ftruncate");
        failures_total++;
        return;
    }
    unsigned char *w = map(pg, PROT_READ | PROT_WRITE, wfd, 0);
    unsigned char *x = map(pg, PROT_READ | PROT_EXEC, xfd, 0);
    emit(w, 0);
    check("file first", ((fn_t) x)(), 0);
    patch_loop("file", w, x);
    munmap(w, pg);
    munmap(x, pg);
    close(wfd);
    close(xfd);
}

static void case_offset(long pg) {
    int fd = memfd(2 * pg);
    unsigned char *x = map(2 * pg, PROT_READ | PROT_EXEC, fd, 0);
    unsigned char *w = map(pg, PROT_READ | PROT_WRITE, fd, pg);
    emit(w + 16, 0);
    check("offset first", ((fn_t) (x + pg + 16))(), 0);
    patch_loop("offset", w + 16, x + pg + 16);
    munmap(w, pg);
    munmap(x, 2 * pg);
    close(fd);
}

static void case_span(long pg) {
    int fd = memfd(2 * pg);
    unsigned char *w = map(2 * pg, PROT_READ | PROT_WRITE, fd, 0);
    unsigned char *x = map(2 * pg, PROT_READ | PROT_EXEC, fd, 0);
    // b8 and the immediate's first byte on the first page, the rest and the
    // ret on the second.
    emit(w + pg - 2, 0x11223344);
    fn_t f = (fn_t) (x + pg - 2);
    check("span before", f(), 0x11223344);
    w[pg + 1] = 0x77;   // byte 2 of the immediate, on the second page
    check("span after second page", f(), 0x11773344);
    w[pg - 1] = 0x55;   // byte 0, on the first page
    check("span after first page", f(), 0x11773355);
    munmap(w, 2 * pg);
    munmap(x, 2 * pg);
    close(fd);
}

static void case_neighbour(long pg) {
    int fd = memfd(pg);
    unsigned char *w = map(pg, PROT_READ | PROT_WRITE, fd, 0);
    unsigned char *x = map(pg, PROT_READ | PROT_EXEC, fd, 0);
    emit(w, 1);
    emit(w + 64, 2);
    unsigned wrong = 0;
    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint32_t got = ((fn_t) (x + 64))();
        if (got != 2 && wrong++ == 0)
            check("neighbour untouched", got, 2);
        set_imm(w, i);
        got = ((fn_t) x)();
        if (got != i && wrong++ == 0)
            check("neighbour patched", got, i);
    }
    test_logf("neighbour: %u of %u calls wrong\n", wrong, 2 * ROUNDS);
    munmap(w, pg);
    munmap(x, pg);
    close(fd);
}

static void case_anon(long pg) {
    unsigned char *w = mmap(NULL, pg, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (w == MAP_FAILED) {
        perror("mmap anon");
        failures_total++;
        return;
    }
    unsigned char *x = mremap(w, 0, pg, MREMAP_MAYMOVE);
    if (x == MAP_FAILED) {
        perror("mremap alias");
        failures_total++;
        return;
    }
    if (mprotect(x, pg, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect alias");
        failures_total++;
        return;
    }
    emit(w, 0);
    check("anon first", ((fn_t) x)(), 0);
    patch_loop("anon", w, x);
    munmap(w, pg);
    munmap(x, pg);
}

static unsigned char *thread_w;
static atomic_uint published, acked;
#define THREAD_ROUNDS 2000

static void *patcher(void *arg) {
    (void) arg;
    for (unsigned k = 1; k <= THREAD_ROUNDS; k++) {
        set_imm(thread_w, k);
        atomic_store_explicit(&published, k, memory_order_release);
        while (atomic_load_explicit(&acked, memory_order_acquire) != k)
            ;
    }
    return NULL;
}

static void case_thread(long pg) {
    int fd = memfd(pg);
    thread_w = map(pg, PROT_READ | PROT_WRITE, fd, 0);
    unsigned char *x = map(pg, PROT_READ | PROT_EXEC, fd, 0);
    emit(thread_w, 0);
    check("thread before", ((fn_t) x)(), 0);
    pthread_t t;
    if (pthread_create(&t, NULL, patcher, NULL) != 0) {
        perror("pthread_create");
        failures_total++;
        return;
    }
    unsigned stale = 0;
    for (unsigned k = 1; k <= THREAD_ROUNDS; k++) {
        while (atomic_load_explicit(&published, memory_order_acquire) != k)
            ;
        serialize();
        uint32_t got = ((fn_t) x)();
        if (got != k && stale++ == 0)
            check("thread patched", got, k);
        atomic_store_explicit(&acked, k, memory_order_release);
    }
    pthread_join(t, NULL);
    test_logf("thread: %u of %u calls stale\n", stale, THREAD_ROUNDS);
    munmap(thread_w, pg);
    munmap(x, pg);
    close(fd);
}

static void case_fork(long pg) {
    int fd = memfd(pg);
    unsigned char *w = map(pg, PROT_READ | PROT_WRITE, fd, 0);
    unsigned char *x = map(pg, PROT_READ | PROT_EXEC, fd, 0);
    emit(w, 0);
    check("fork first", ((fn_t) x)(), 0);
    int go[2], done[2];
    if (pipe(go) != 0 || pipe(done) != 0) {
        perror("pipe");
        failures_total++;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        failures_total++;
        return;
    }
    if (pid == 0) {
        close(go[1]);
        close(done[0]);
        uint32_t k;
        while (read(go[0], &k, sizeof(k)) == sizeof(k)) {
            set_imm(w, k);
            if (write(done[1], &k, sizeof(k)) != sizeof(k))
                _exit(1);
        }
        _exit(0);
    }
    close(go[0]);
    close(done[1]);
    unsigned stale = 0;
    for (uint32_t k = 1; k <= ROUNDS; k++) {
        uint32_t ack;
        if (write(go[1], &k, sizeof(k)) != sizeof(k) ||
                read(done[0], &ack, sizeof(ack)) != sizeof(ack)) {
            perror("fork handshake");
            failures_total++;
            break;
        }
        uint32_t got = ((fn_t) x)();
        if (got != k && stale++ == 0)
            check("fork patched", got, k);
    }
    close(go[1]);
    int status;
    waitpid(pid, &status, 0);
    close(done[0]);
    test_logf("fork: %u of %u calls stale\n", stale, ROUNDS);
    munmap(w, pg);
    munmap(x, pg);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    long pg = sysconf(_SC_PAGESIZE);
    case_memfd(pg);
    case_file(pg);
    case_offset(pg);
    case_span(pg);
    case_neighbour(pg);
    case_anon(pg);
    case_thread(pg);
    if (getenv("X86_SMC_ALIAS_XPROC") != NULL)
        case_fork(pg);
    return finish_suite("x86_smc_alias");
}
