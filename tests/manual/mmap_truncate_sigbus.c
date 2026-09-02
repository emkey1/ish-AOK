// mmap-then-truncate-then-access => guest SIGBUS (not emulator crash).
//
// History: a file-backed guest mmap (tmpfs host_fd backing from fs/tmp.c, or
// realfs real_fd from fs/real.c) is a host mmap of a host file. If a racing
// thread ftruncate's the backing file smaller under a live mapping, a guest
// store/load to the now-past-EOF page raises a HOST SIGBUS. AOK's JIT does
// direct host memory accesses in its store/load gadgets and originally had no
// host-fault translation, so the whole emulator process died with
// "FS pagein error" instead of delivering SIGBUS to the guest program -- unlike
// real Linux, where accessing a truncated page of a file mapping raises
// SIGBUS (BUS_ADRERR) to the process. Found under stress-ng --filerace on the
// local realfs rig (project_stressng_sweep.md).
//
// Fix: a host SIGBUS handler (POSIX on the CLI, EXC_BAD_ACCESS via Mach on
// device) reverse-maps the faulting host address back to the guest address and
// delivers a guest SIGBUS via a synthetic INT_BUS interrupt.
//
// This test maps two pages of a file MAP_SHARED, ftruncate's the file down to
// one page, then accesses the (now past-EOF) second page and asserts the
// process receives SIGBUS -- with si_code BUS_ADRERR and si_addr inside the
// second page -- rather than dying. The still-backed first page must remain
// accessible. Exercised for reads and writes, on a tmpfs mount and on the
// current (realfs/fakefs) working directory. Ground-truthed against real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "test_common.h"

#define TMPFS_MAGIC_ 0x01021994
#define PGSZ 4096
// The mapping must be large enough that the page we fault on lives in a
// different HOST page than any page we pre-touched. iOS/Apple-silicon hosts use
// 16K host pages and page a file mapping in at host-page granularity, so a small
// mapping would make one touch resident the whole thing. 64K spans several host
// pages at any real host page size; we touch only the first page and fault on
// the last, guaranteeing the faulting page was never paged in.
#define MAP_BYTES (64 * 1024)
#define LAST_OFF  (MAP_BYTES - PGSZ)

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

// SIGBUS catcher: record the details and jump back out of the faulting access.
static sigjmp_buf bus_jmp;
static volatile sig_atomic_t bus_caught;
static volatile int bus_code;
static volatile uintptr_t bus_addr;

static void sigbus_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) uctx;
    bus_caught = 1;
    bus_code = info ? info->si_code : -1;
    bus_addr = info ? (uintptr_t) info->si_addr : 0;
    siglongjmp(bus_jmp, 1);
}

static void install_sigbus(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigbus_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
}

// Access byte at p[off]; if `write` also store to it. Returns:
//   0  -> access completed with no signal
//   1  -> SIGBUS caught (details in bus_* globals)
static int touch(volatile char *p, size_t off, int write) {
    bus_caught = 0;
    bus_code = 0;
    bus_addr = 0;
    if (sigsetjmp(bus_jmp, 1) == 0) {
        volatile char c = p[off];
        if (write)
            p[off] = (char) (c + 1);
        else
            (void) c;
        return 0;
    }
    return 1;
}

// Run the full mmap+truncate+access sequence in `dir`. Returns 1 if the
// scenario ran (dir usable), 0 if it had to be skipped (dir not writable / no
// file-backed shared mmap support). Populates *ran_write on success.
static int run_in_dir(const char *label, const char *dir, int do_write) {
    char path[256];
    snprintf(path, sizeof(path), "%s/mmap-trunc-sigbus.%d", dir, (int) getpid());

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        test_logf("skip %s: open(%s) failed: %s\n", label, path, strerror(errno));
        return 0;
    }
    if (ftruncate(fd, MAP_BYTES) != 0) {
        test_logf("skip %s: ftruncate grow failed: %s\n", label, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    char *p = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        test_logf("skip %s: mmap MAP_SHARED failed: %s\n", label, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    // Touch only the FIRST page. Every other page is deliberately left
    // un-faulted: the host SIGBUS this test targets fires when the JIT pages a
    // not-yet-resident page IN from a file that has since been truncated (the
    // "FS pagein error" crash). A page already faulted in and dirtied stays
    // resident and accessible after a truncate on macOS, so pre-touching the
    // fault page would hide the fault (real Linux faults either way).
    check(touch(p, 0, 1) == 0, "pre-truncate: first page writable");

    // Drop everything but the first page out of the backing file.
    check(ftruncate(fd, PGSZ) == 0, "ftruncate shrink to one page");

    // First page is still backed -> still accessible, no SIGBUS.
    check(touch(p, 0, 1) == 0, "post-truncate: first page still accessible");

    // First access to the last page: it is now far past EOF (and in a different
    // host page than the one we touched), so paging it in fails -> the guest
    // must receive SIGBUS (Linux BUS_ADRERR), NOT have the emulator die.
    int faulted = touch(p, LAST_OFF, do_write);
    check(faulted == 1, do_write ? "post-truncate: write to truncated page -> SIGBUS"
                                 : "post-truncate: read of truncated page -> SIGBUS");
    if (faulted) {
        check(bus_code == BUS_ADRERR, "SIGBUS si_code is BUS_ADRERR");
        // si_addr must fall inside the mapping, at or past the surviving first
        // page (i.e. in the truncated region). Exact-byte on real Linux; on
        // AOK the host reports the pagein fault at host-page (16K) granularity,
        // so si_addr is rounded down to a host page within the truncated
        // region -- still inside the mapping, which is what callers key off.
        test_logf("  si_addr offset in mapping = %#lx (faulted at %#x)\n",
                  (unsigned long) (bus_addr - (uintptr_t) p), LAST_OFF);
        check(bus_addr >= (uintptr_t) (p + PGSZ) &&
              bus_addr < (uintptr_t) (p + MAP_BYTES),
              "SIGBUS si_addr points into the truncated region");
    }

    munmap(p, MAP_BYTES);
    close(fd);
    unlink(path);
    return 1;
}

// A tmpfs dir to test in. Prefer mounting a private tmpfs (needs root, which
// the suite has on-device and under the CLI); fall back to /dev/shm or /tmp if
// either is already tmpfs (the usual case on real Linux).
static char tmpfs_buf[128];
static const char *pick_tmpfs_dir(void) {
    struct statfs sfs;
    snprintf(tmpfs_buf, sizeof(tmpfs_buf), "/tmp/mmap-trunc-tmpfs.%d", (int) getpid());
    if (mkdir(tmpfs_buf, 0700) == 0 &&
            mount("tmpfs", tmpfs_buf, "tmpfs", 0, NULL) == 0)
        return tmpfs_buf;
    rmdir(tmpfs_buf);
    if (statfs("/dev/shm", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_)
        return "/dev/shm";
    if (statfs("/tmp", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_)
        return "/tmp";
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    install_sigbus();

    int ran = 0;

    const char *tmpfs = pick_tmpfs_dir();
    if (tmpfs != NULL) {
        test_logf("== tmpfs dir: %s ==\n", tmpfs);
        ran += run_in_dir("tmpfs write", tmpfs, 1);
        ran += run_in_dir("tmpfs read", tmpfs, 0);
    } else {
        test_logf("skip tmpfs: no tmpfs mount found\n");
    }

    // The current working directory stands in for realfs (local realfs rig, or
    // /AOK realfs on device) / fakefs. "." is always writable in the harness.
    test_logf("== cwd dir: . ==\n");
    ran += run_in_dir("cwd write", ".", 1);
    ran += run_in_dir("cwd read", ".", 0);

    check(ran > 0, "at least one filesystem exercised the truncate-mmap fault");

    return finish_suite("mmap_truncate_sigbus");
}
