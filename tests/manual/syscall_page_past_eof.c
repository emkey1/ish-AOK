// A syscall that touches a file page past EOF fails the syscall; it does not
// take the host down.
//
// A MAP_PRIVATE file mapping may be longer than its file, and musl's dynamic
// linker makes such mappings routinely: it maps a library's whole span from
// the file, so on arm64 the gap between text and data is file offsets past
// EOF. A page wholly past EOF has nothing to page in. Linux answers a USER
// access to one with SIGBUS, and a KERNEL access -- a syscall copying to or
// from it -- with a failed copy: write(2) gives EFAULT, /proc/self/mem gives
// EIO, and so on.
//
// On iSH-AOK a guest page is a host mapping of the host file, so a page past
// EOF is a host page past EOF, and touching it from the host raises SIGBUS.
// The JIT turned a GUEST access into a guest SIGBUS, but kernel C code copies
// guest memory with memcpy, and a host fault there killed the app: rc=138 from
// every mode below. Each case here does one kernel access to a page wholly
// past EOF and checks the syscall's own answer, which is Linux's, measured on
// x86_64 Linux 6.12 (camd). Each is paired with the same access to a page
// INSIDE the file, which must succeed -- the positive control that shows the
// case reached the kernel path at all.
//
// The copy-on-write cases are a second route into the same fault: after a
// fork every private page is copy-on-write, and breaking that copies the old
// page -- which for a page past EOF is the same host fault, taken while the
// address space is write-locked.
//
// Where the page sits. 128 KiB into a mapping of a 5000-byte file is past EOF
// at every host page size up to 64 KiB. Guest pages 2 and 3 of the file (8192
// to 16383) are past EOF too, and Linux faults there, but on a 16 KiB-page host
// they share a host page with the file's last bytes and read as zeros: a
// granularity gap in the emulation that this test deliberately avoids.
#define _GNU_SOURCE
// /proc/self/mem is addressed by file offset, and a 32-bit address above 2 GiB
// is a negative 32-bit off_t: EINVAL before the kernel looks at memory.
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include "test_common.h"

_Static_assert(sizeof(off_t) == 8, "/proc/self/mem offsets need a 64-bit off_t");

#define FILE_BYTES 5000
#define MAP_BYTES (256 * 1024)
#define FAR_OFF (128 * 1024)

static void ck_ret(const char *what, long got, int got_errno, long want, int want_errno) {
    int ok = got == want && (want >= 0 || got_errno == want_errno);
    if (!ok) {
        printf("FAIL %s: got %ld (%s), want %ld (%s)\n", what, got,
               got < 0 ? strerror(got_errno) : "ok", want,
               want < 0 ? strerror(want_errno) : "ok");
        failures_total++;
    } else {
        test_logf("ok: %s -> %ld%s%s\n", what, got, got < 0 ? " " : "",
                  got < 0 ? strerror(got_errno) : "");
    }
}

static void ck(const char *what, int cond) {
    if (!cond) {
        printf("FAIL %s\n", what);
        failures_total++;
    } else {
        test_logf("ok: %s\n", what);
    }
}

// A private mapping of a fresh FILE_BYTES-long file of 'x', MAP_BYTES long.
// The file is unlinked at once; the mapping keeps it.
static char *map_short_file(int prot) {
    char path[] = "/tmp/syscall-page-past-eof-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        printf("FAIL mkstemp: %s\n", strerror(errno));
        failures_total++;
        return NULL;
    }
    unlink(path);
    char buf[FILE_BYTES];
    memset(buf, 'x', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t) sizeof(buf)) {
        printf("FAIL write file: %s\n", strerror(errno));
        failures_total++;
        close(fd);
        return NULL;
    }
    char *m = mmap(NULL, MAP_BYTES, prot, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        printf("FAIL mmap: %s\n", strerror(errno));
        failures_total++;
        return NULL;
    }
    return m;
}

// Every case runs in a child of its own, so each starts from a fresh mapping
// and a Linux-side crash in one case cannot hide the rest. (On a host that
// dies of the fault, the whole run stops at the case that did it.)
static int run_case(const char *name, void (*fn)(void)) {
    test_logf("-- %s\n", name);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fork for %s: %s\n", name, strerror(errno));
        failures_total++;
        return -1;
    }
    if (pid == 0) {
        alarm(test_watchdog_secs(30));
        failures_total = 0;
        fn();
        fflush(NULL);
        _exit(failures_total ? 1 : 0);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid) {
        printf("FAIL waitpid for %s: %s\n", name, strerror(errno));
        failures_total++;
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (WIFSIGNALED(st))
            printf("FAIL %s: case killed by signal %d\n", name, WTERMSIG(st));
        failures_total++;
    }
    return 0;
}

// ---- read direction: the kernel copies FROM the page ----------------------

static void case_write_from(void) {
    char *m = map_short_file(PROT_READ);
    if (m == NULL)
        return;
    int p[2];
    if (pipe(p) != 0)
        return;
    errno = 0;
    long n = write(p[1], m, 16);
    ck_ret("write(2) from a page inside the file", n, errno, 16, 0);
    errno = 0;
    n = write(p[1], m + FAR_OFF, 16);
    ck_ret("write(2) from a page past EOF", n, errno, -1, EFAULT);
    // The iovec path gathers each buffer on its own. (Past EOF first: what a
    // pipe does with a fault after some bytes is its own business.)
    struct iovec iov[2] = { { m + FAR_OFF, 16 }, { m, 16 } };
    errno = 0;
    n = writev(p[1], iov, 2);
    ck_ret("writev(2) whose first iovec is past EOF", n, errno, -1, EFAULT);
}

static void case_procmem_read(void) {
    char *m = map_short_file(PROT_READ);
    if (m == NULL)
        return;
    int mf = open("/proc/self/mem", O_RDONLY);
    if (mf < 0) {
        printf("FAIL open /proc/self/mem: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    char out[16];
    errno = 0;
    long n = pread(mf, out, 16, (off_t) (uintptr_t) m);
    ck_ret("/proc/self/mem pread inside the file", n, errno, 16, 0);
    ck("  and it read the file's bytes", n == 16 && out[0] == 'x' && out[15] == 'x');
    errno = 0;
    n = pread(mf, out, 16, (off_t) (uintptr_t) (m + FAR_OFF));
    ck_ret("/proc/self/mem pread past EOF", n, errno, -1, EIO);
}

static void case_vm_readv_remote(void) {
    char *m = map_short_file(PROT_READ);
    if (m == NULL)
        return;
    char out[16];
    struct iovec l = { out, 16 }, r = { m, 16 };
    errno = 0;
    long n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    ck_ret("process_vm_readv from a page inside the file", n, errno, 16, 0);
    r.iov_base = m + FAR_OFF;
    errno = 0;
    n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    ck_ret("process_vm_readv from a page past EOF", n, errno, -1, EFAULT);
}

// PTRACE_PEEKDATA and POKEDATA on a stopped child that inherited the mapping.
// POKE is the write direction, and on a private page it is a copy-on-write
// break of a page that cannot be read.
static void case_ptrace_peek_poke(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    pid_t child = fork();
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(0);
    }
    int st;
    if (waitpid(child, &st, WUNTRACED) != child || !WIFSTOPPED(st)) {
        printf("FAIL the tracee did not stop\n");
        failures_total++;
        kill(child, SIGKILL);
        return;
    }
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, child, m, NULL);
    ck_ret("PTRACE_PEEKDATA inside the file", errno ? -1 : 0, errno, 0, 0);
    ck("  and it read the file's bytes", (v & 0xff) == 'x');
    errno = 0;
    v = ptrace(PTRACE_PEEKDATA, child, m + FAR_OFF, NULL);
    ck_ret("PTRACE_PEEKDATA past EOF", v == -1 && errno ? -1 : 0, errno, -1, EIO);
    errno = 0;
    long r = ptrace(PTRACE_POKEDATA, child, m + 8, (void *) (uintptr_t) 0x41414141UL);
    ck_ret("PTRACE_POKEDATA inside the file", r, errno, 0, 0);
    errno = 0;
    r = ptrace(PTRACE_POKEDATA, child, m + FAR_OFF, (void *) (uintptr_t) 0x42424242UL);
    ck_ret("PTRACE_POKEDATA past EOF", r, errno, -1, EIO);
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
}

// FUTEX_WAIT has to load the word before it can sleep. With a value that does
// not match, the control returns EAGAIN at once rather than sleeping.
static void case_futex_wait(void) {
    char *m = map_short_file(PROT_READ);
    if (m == NULL)
        return;
    struct timespec ts = { 0, 1000000 };
    errno = 0;
    long n = syscall(SYS_futex, (int *) m, FUTEX_WAIT, 0, &ts, NULL, 0);
    ck_ret("FUTEX_WAIT on a page inside the file (value differs)", n, errno, -1, EAGAIN);
    errno = 0;
    n = syscall(SYS_futex, (int *) (m + FAR_OFF), FUTEX_WAIT, 0, &ts, NULL, 0);
    ck_ret("FUTEX_WAIT on a page past EOF", n, errno, -1, EFAULT);
}

// A path is copied as a string: up to its NUL and no further. A path in a page
// past EOF is EFAULT; a string of the file's own bytes is read through the file
// mapping to the zeroes past EOF, 5000 bytes, longer than PATH_MAX. And the
// page edges the string copy has to get right, beside an unmapped page.
static void case_path_strings(void) {
    char *m = map_short_file(PROT_READ);
    if (m == NULL)
        return;
    errno = 0;
    long n = access(m + FAR_OFF, F_OK);
    ck_ret("access(2) of a path in a page past EOF", n, errno, -1, EFAULT);
    errno = 0;
    n = access(m, F_OK);
    ck_ret("access(2) of a 5000-byte path inside the file", n, errno, -1, ENAMETOOLONG);

    long pg = sysconf(_SC_PAGESIZE);
    char *two = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (two == MAP_FAILED) {
        printf("FAIL mmap two pages: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    // "/tmp" split across the page boundary, NUL on the second page.
    memcpy(two + pg - 3, "/tm", 3);
    memcpy(two + pg, "p", 2);
    errno = 0;
    n = access(two + pg - 3, F_OK);
    ck_ret("access(2) of a path that crosses a page boundary", n, errno, 0, 0);
    // Now the second page goes: a path that ends exactly at the end of the
    // first still reads, and one that runs into the hole does not.
    munmap(two + pg, pg);
    memcpy(two + pg - 5, "/tmp", 5);
    errno = 0;
    n = access(two + pg - 5, F_OK);
    ck_ret("access(2) of a path ending at the last byte before a hole", n, errno, 0, 0);
    memcpy(two + pg - 4, "/tmp", 4);
    errno = 0;
    n = access(two + pg - 4, F_OK);
    ck_ret("access(2) of a path running into a hole", n, errno, -1, EFAULT);
}

// ---- write direction: the kernel copies INTO the page ---------------------

static void case_read_into(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    int p[2];
    if (pipe(p) != 0)
        return;
    char src[64];
    memset(src, 'r', sizeof(src));
    if (write(p[1], src, sizeof(src)) != (ssize_t) sizeof(src))
        return;
    errno = 0;
    long n = read(p[0], m, 16);
    ck_ret("read(2) into a page inside the file", n, errno, 16, 0);
    ck("  and the bytes landed", n == 16 && m[0] == 'r' && m[15] == 'r');
    errno = 0;
    n = read(p[0], m + FAR_OFF, 16);
    ck_ret("read(2) into a page past EOF", n, errno, -1, EFAULT);
}

static void case_vm_readv_local(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    static char src[] = "0123456789abcdef";
    struct iovec l = { m, 16 }, r = { src, 16 };
    errno = 0;
    long n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    ck_ret("process_vm_readv into a page inside the file", n, errno, 16, 0);
    l.iov_base = m + FAR_OFF;
    errno = 0;
    n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    ck_ret("process_vm_readv into a page past EOF", n, errno, -1, EFAULT);
}

static void case_procmem_write(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    int mf = open("/proc/self/mem", O_RDWR);
    if (mf < 0) {
        printf("FAIL open /proc/self/mem O_RDWR: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    errno = 0;
    long n = pwrite(mf, "WWWWWWWWWWWWWWWW", 16, (off_t) (uintptr_t) m);
    ck_ret("/proc/self/mem pwrite inside the file", n, errno, 16, 0);
    ck("  and the bytes landed", m[0] == 'W' && m[15] == 'W');
    errno = 0;
    n = pwrite(mf, "WWWWWWWWWWWWWWWW", 16, (off_t) (uintptr_t) (m + FAR_OFF));
    ck_ret("/proc/self/mem pwrite past EOF", n, errno, -1, EIO);
}

// ---- after fork: every private page is copy-on-write ----------------------

// A syscall writing into an inherited page breaks copy-on-write, which copies
// the page first.
static void case_fork_read_into(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    int p[2];
    if (pipe(p) != 0)
        return;
    char src[64];
    memset(src, 'c', sizeof(src));
    if (write(p[1], src, sizeof(src)) != (ssize_t) sizeof(src))
        return;
    pid_t child = fork();
    if (child == 0) {
        failures_total = 0;
        errno = 0;
        long n = read(p[0], m, 16);
        ck_ret("after fork: read(2) into a page inside the file", n, errno, 16, 0);
        errno = 0;
        n = read(p[0], m + FAR_OFF, 16);
        ck_ret("after fork: read(2) into a page past EOF", n, errno, -1, EFAULT);
        fflush(NULL);
        _exit(failures_total ? 1 : 0);
    }
    int st;
    waitpid(child, &st, 0);
    ck("after fork: the child's reads were answered", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// The guest's own store: SIGBUS, BUS_ADRERR, at the address.
static sigjmp_buf bus_jmp;
static volatile sig_atomic_t bus_code;
static volatile uintptr_t bus_addr;

static void on_sigbus(int sig, siginfo_t *info, void *uctx) {
    (void) sig;
    (void) uctx;
    bus_code = info->si_code;
    bus_addr = (uintptr_t) info->si_addr;
    siglongjmp(bus_jmp, 1);
}

static int store_faults(volatile char *p) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigbus;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
    bus_code = 0;
    bus_addr = 0;
    if (sigsetjmp(bus_jmp, 1) == 0) {
        *p = 's';
        return 0;
    }
    return 1;
}

static void case_store(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    ck("a store inside the file succeeds", store_faults(m) == 0);
    ck("a store past EOF raises SIGBUS", store_faults(m + FAR_OFF) == 1);
    ck("  with si_code BUS_ADRERR", bus_code == BUS_ADRERR);
    ck("  in the page stored to",
       bus_addr >= (uintptr_t) (m + FAR_OFF) - 65536 && bus_addr < (uintptr_t) (m + FAR_OFF) + 4096);
}

static void case_fork_store(void) {
    char *m = map_short_file(PROT_READ | PROT_WRITE);
    if (m == NULL)
        return;
    pid_t child = fork();
    if (child == 0) {
        failures_total = 0;
        ck("after fork: a store inside the file succeeds", store_faults(m) == 0);
        ck("after fork: a store past EOF raises SIGBUS", store_faults(m + FAR_OFF) == 1);
        ck("  with si_code BUS_ADRERR", bus_code == BUS_ADRERR);
        fflush(NULL);
        _exit(failures_total ? 1 : 0);
    }
    int st;
    waitpid(child, &st, 0);
    ck("after fork: the child's stores were answered", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    run_case("write_from", case_write_from);
    run_case("procmem_read", case_procmem_read);
    run_case("vm_readv_remote", case_vm_readv_remote);
    run_case("ptrace_peek_poke", case_ptrace_peek_poke);
    run_case("futex_wait", case_futex_wait);
    run_case("path_strings", case_path_strings);
    run_case("read_into", case_read_into);
    run_case("vm_readv_local", case_vm_readv_local);
    run_case("procmem_write", case_procmem_write);
    run_case("fork_read_into", case_fork_read_into);
    run_case("store", case_store);
    run_case("fork_store", case_fork_store);
    return finish_suite("syscall_page_past_eof");
}
