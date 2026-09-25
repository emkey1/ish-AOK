// futex_shared_mapping: a futex in shared memory is one futex, whichever
// mapping, descriptor or process reaches the word.
//
// A FUTEX_WAIT on a MAP_SHARED file mapping was never woken by a FUTEX_WAKE
// from another process that had opened the same file itself. A shared futex
// was keyed by the struct fd its mapping came through, and by the word's
// offset in the HOST mapping rather than in the file. Two open()s are two
// struct fds, so the waiter and the waker named two different futexes; and
// even through one descriptor, a mapping that starts part way into the file
// has the word at a different offset of its host mapping than one starting at
// 0. Every process-shared pthread mutex, condvar and semaphore that two
// processes reach through mappings of their own -- a file both open, POSIX shm
// (shm_open is a file under /dev/shm), sem_open's named semaphores -- slept
// until its deadline, or for good without one. The key is now what the memory
// IS (mem_shared_page_id, emu/memory.c): a file's host device, inode and page,
// a SysV segment and page, or anonymous shared memory's own object and page.
//
// Each case is a waiter in one process and a waker in another, unless noted:
//   [1]  a file two processes opened themselves, both mapped from offset 0
//   [2]  the same, the waker mapping only the file's second page
//   [3]  ONE descriptor (inherited), the waker mapping from 16 KiB: the offset
//        half of the bug on its own
//   [4]  the key is the file AND the word: a wake at the same offset of
//        another file, or at the next word of this one, reaches nobody
//   [5]  a tmpfs file (a private mount when root, else /dev/shm if tmpfs)
//   [6]  a memfd, inherited, mapped from different offsets
//   [7]  POSIX shm: shm_open by name in an exec'd, unrelated process
//   [8]  SysV shm: a second shmat in the child, and shmat by id after exec
//   [9]  fork-shared MAP_SHARED|MAP_ANONYMOUS memory; two such mappings are
//        two objects
//   [10] MAP_SHARED /dev/zero, forked; two mappings through one descriptor
//        are two objects, not one
//   [11] FUTEX_CMP_REQUEUE and FUTEX_REQUEUE move a waiter by the file's key
//   [12] FUTEX_WAKE_OP reaches a waiter in another process through uaddr2
//   [13] FUTEX_WAKE_OP_PRIVATE on a MAP_SHARED page wakes a
//        FUTEX_WAIT_PRIVATE waiter (one process): the flag applies to both
//        words, and WAKE_OP ignored it
//   [14] a process-shared pthread mutex and condvar in a file
//   [15] a robust process-shared mutex: its holder dies, and the waiter in
//        the other process has EOWNERDEAD at once
//   [16] a sem_open named semaphore, posted to an exec'd process
//
// Measured before the fix on alpine-arm64-test, alpine-amd64-test,
// alpine-riscv64-test and devuan-amd64-test (glibc): the same 34 failures on
// each, in every case but [8] and [9] -- a SysV segment and anonymous shared
// memory were already keyed by what they are. Every waiter slept to its
// deadline; the pthread and semaphore cases came back only because they wait
// with one. And in [10] the wake on the other /dev/zero mapping woke the
// waiter. After it, 51 of 51 there and on alpine-i386-test.
//
// [11] found a crash on the way: FUTEX_CMP_REQUEUE freed the futex it moved a
// waiter to (futex_robust_requeue has it in one process). No requeue between
// processes had got that far before, because none found its waiter.
//
// Checked against Linux 6.12 (x86_64 glibc, 64-bit and -m32) as uid 1000.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

// linux/futex.h, which not every sysroot here has; stable kernel ABI.
#define FX_WAIT 0
#define FX_WAKE 1
#define FX_REQUEUE 3
#define FX_CMP_REQUEUE 4
#define FX_WAKE_OP 5
#define FX_PRIVATE 128
// FUTEX_OP(FUTEX_OP_SET, 1, FUTEX_OP_CMP_EQ, 0): store 1 in *uaddr2, and wake
// uaddr2's waiters too if it held 0.
#define FX_OP_SET1_IF_WAS0 ((0u << 28) | (0u << 24) | (1u << 12) | 0u)

#define TMPFS_MAGIC_ 0x01021994
static long pg;
// A waiter gives up after wait_secs, longer than a waker ever retries, so a
// waiter still asleep when the wakes stop is one no wake could reach. Both
// stretch with ISH_TEST_WATCHDOG_SCALE, for a heavily loaded run.
static unsigned wait_secs;
static int wake_retry_ms;
static size_t file_size;   // eight pages: room for a mapping from 16 KiB
static char self_exe[512];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-66s got=%-6ld want=%ld\n", label, got, want);
}

static void setup_fail(const char *what) {
    printf("FAIL setup: %s (errno=%d %s)\n", what, errno, strerror(errno));
    failures_total++;
}

static long fx(uint32_t *uaddr, int op, uint32_t val, const struct timespec *t) {
#ifdef SYS_futex_time64
    if (t != NULL && sizeof t->tv_sec > sizeof(long))
        return syscall(SYS_futex_time64, uaddr, op, val, t, NULL, 0);
#endif
    return syscall(SYS_futex, uaddr, op, val, t, NULL, 0);
}

// The ops whose fourth argument is a count, not a timeout.
static long fx2(uint32_t *uaddr, int op, uint32_t val, unsigned long val2,
        uint32_t *uaddr2, uint32_t val3) {
    return syscall(SYS_futex, uaddr, op, val, val2, uaddr2, val3);
}

static char *map_at(int fd, off_t off, size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    return p == MAP_FAILED ? NULL : p;
}

static uint32_t *word_at(char *base, off_t base_off, off_t file_off) {
    return (uint32_t *) (base + (file_off - base_off));
}

// A waiter's side of a case: note on *marker that the next thing this process
// does is the wait, then FUTEX_WAIT while *word is 0. Exits 0 woken, 2 timed
// out, 3 the word was not 0, 4 any other error.
static void __attribute__((noreturn)) wait_then_exit(uint32_t *word, uint32_t *marker, int op) {
    struct timespec to = { (time_t) wait_secs, 0 };
    __atomic_store_n(marker, 1, __ATOMIC_SEQ_CST);
    long r = fx(word, op, 0, &to);
    _exit(r == 0 ? 0 : errno == ETIMEDOUT ? 2 : errno == EAGAIN ? 3 : 4);
}

// /proc/<pid>/stat's (or /proc/self/task/<tid>/stat's) state letter, or '?'.
static char proc_state(const char *path) {
    char buf[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    return p != NULL && p[1] == ' ' ? p[2] : '?';
}

// Wait for the waiter's marker, then for it to be asleep: after the marker the
// only thing it does is the FUTEX_WAIT, so asleep means queued. A wake that
// must reach nobody proves nothing unless the waiter was there to be reached,
// so the cases with one of those require this.
static int waiter_asleep(const char *stat_path, uint32_t *marker) {
    for (int i = 0; i < 500 && __atomic_load_n(marker, __ATOMIC_SEQ_CST) == 0; i++)
        usleep(10000);
    if (__atomic_load_n(marker, __ATOMIC_SEQ_CST) == 0)
        return 0;
    for (int i = 0; i < 500; i++) {
        if (proc_state(stat_path) == 'S')
            return 1;
        usleep(10000);
    }
    return 0;
}

static int child_asleep(pid_t pid, uint32_t *marker) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    return waiter_asleep(path, marker);
}

// FUTEX_WAKE one waiter, again every 20ms while it finds nobody: a waiter not
// queued yet is not a missed wake.
static long wake_retry(uint32_t *word, int op) {
    for (int waited = 0; ; waited += 20) {
        long woke = fx2(word, op, 1, 0, NULL, 0);
        if (woke != 0 || waited >= wake_retry_ms)
            return woke;
        usleep(20000);
    }
}

// A waiter's exit status as wait_then_exit set it, 100 + the signal that
// killed it, or -1.
static int reap(pid_t pid) {
    int st;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return WIFSIGNALED(st) ? 100 + WTERMSIG(st) : -1;
}

static void kill_and_reap(pid_t pid) {
    kill(pid, SIGKILL);
    reap(pid);
}

static double now_mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static struct timespec deadline_in(int secs) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += secs;
    return t;
}

static int make_file(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t) file_size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void expect_woken(const char *label, long woke, int status) {
    char what[160];
    snprintf(what, sizeof what, "%s: the wake found the waiter", label);
    ck(what, woke, 1);
    snprintf(what, sizeof what, "%s: the waiter was woken (0), not timed out (2)", label);
    ck(what, status, 0);
}

// [1] [2] [3] [5] [6]: a waiter that maps the whole file through `fd`, or
// through its own open() of `path` when fd is -1, and waits on the word at
// file offset word_off; and a waker that does the same, mapping `waker_len`
// bytes (0: to the end) from waker_off.
static void file_case(const char *label, const char *path, int fd,
        off_t waker_off, size_t waker_len, off_t word_off) {
    test_logf("%s\n", label);
    off_t mark_off = word_off + 64;
    pid_t pid = fork();
    if (pid < 0) {
        setup_fail("fork");
        return;
    }
    if (pid == 0) {
        int f = fd >= 0 ? fd : open(path, O_RDWR);
        char *m = f < 0 ? NULL : map_at(f, 0, file_size);
        if (m == NULL)
            _exit(5);
        wait_then_exit(word_at(m, 0, word_off), word_at(m, 0, mark_off), FX_WAIT);
    }
    int f = fd >= 0 ? fd : open(path, O_RDWR);
    size_t len = waker_len != 0 ? waker_len : file_size - (size_t) waker_off;
    char *m = f < 0 ? NULL : map_at(f, waker_off, len);
    if (m == NULL) {
        setup_fail("the waker's mapping");
        kill_and_reap(pid);
        if (fd < 0 && f >= 0)
            close(f);
        return;
    }
    child_asleep(pid, word_at(m, waker_off, mark_off));
    long woke = wake_retry(word_at(m, waker_off, word_off), FX_WAKE);
    expect_woken(label, woke, reap(pid));
    munmap(m, len);
    if (fd < 0)
        close(f);
}

// [4]: the waiter on file A's word; wakes at the same offset of file B, and at
// the next word of A, must reach nobody while it sleeps.
static void file_key_case(const char *path_a, const char *path_b) {
    const char *label = "[4] a file's key is the file and the word";
    test_logf("%s\n", label);
    off_t word_off = 5 * pg, mark_off = word_off + 64;
    pid_t pid = fork();
    if (pid < 0) {
        setup_fail("fork");
        return;
    }
    if (pid == 0) {
        int f = open(path_a, O_RDWR);
        char *m = f < 0 ? NULL : map_at(f, 0, file_size);
        if (m == NULL)
            _exit(5);
        wait_then_exit(word_at(m, 0, word_off), word_at(m, 0, mark_off), FX_WAIT);
    }
    int fa = open(path_a, O_RDWR), fb = open(path_b, O_RDWR);
    char *a = fa < 0 ? NULL : map_at(fa, 0, file_size);
    char *b = fb < 0 ? NULL : map_at(fb, 0, file_size);
    if (a == NULL || b == NULL) {
        setup_fail("the waker's mappings");
        kill_and_reap(pid);
    } else {
        ck("[4] setup: the waiter is asleep in its wait",
                child_asleep(pid, word_at(a, 0, mark_off)), 1);
        ck("[4] a wake at the same offset of ANOTHER file reaches nobody",
                fx2(word_at(b, 0, word_off), FX_WAKE, 1, 0, NULL, 0), 0);
        ck("[4] a wake at the next word of this file reaches nobody",
                fx2(word_at(a, 0, word_off + 4), FX_WAKE, 1, 0, NULL, 0), 0);
        long woke = wake_retry(word_at(a, 0, word_off), FX_WAKE);
        expect_woken("[4] and the word itself", woke, reap(pid));
    }
    if (a != NULL)
        munmap(a, file_size);
    if (b != NULL)
        munmap(b, file_size);
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
}

// [5]: a tmpfs to make a file in. A private mount needs privilege; without
// it, /dev/shm or /tmp if either already is one; else none, and the case is
// skipped. `mounted` says whether to unmount it afterwards.
static int pick_tmpfs(char *dir, size_t size, int *mounted) {
    struct statfs sfs;
    *mounted = 0;
    snprintf(dir, size, "/tmp/futex_shared_mapping.tmpfs.%d", (int) getpid());
    if (mkdir(dir, 0700) == 0) {
        if (mount("tmpfs", dir, "tmpfs", 0, NULL) == 0) {
            *mounted = 1;
            return 0;
        }
        rmdir(dir);
    }
    if (statfs("/dev/shm", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_) {
        snprintf(dir, size, "/dev/shm");
        return 0;
    }
    if (statfs("/tmp", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_) {
        snprintf(dir, size, "/tmp");
        return 0;
    }
    return -1;
}

static void tmpfs_case(void) {
    char dir[128], path[192];
    int mounted;
    if (pick_tmpfs(dir, sizeof dir, &mounted) != 0) {
        test_logf("[5] skipped: no tmpfs here, and no privilege to mount one\n");
        return;
    }
    snprintf(path, sizeof path, "%s/futex_shared_mapping.%d", dir, (int) getpid());
    int fd = make_file(path);
    if (fd < 0) {
        setup_fail("[5] the tmpfs file");
    } else {
        close(fd);
        file_case("[5] a tmpfs file, two opens, the waker mapping from 16 KiB",
                path, -1, 4 * pg, 0, 5 * pg);
        unlink(path);
    }
    if (mounted) {
        umount2(dir, MNT_DETACH);
        rmdir(dir);
    }
}

static void memfd_case(void) {
#ifdef SYS_memfd_create
    int fd = (int) syscall(SYS_memfd_create, "futex_shared_mapping", 0);
    if (fd < 0 || ftruncate(fd, (off_t) file_size) != 0) {
        setup_fail("[6] memfd_create");
        if (fd >= 0)
            close(fd);
        return;
    }
    file_case("[6] a memfd, inherited, the waker mapping from 16 KiB",
            NULL, fd, 4 * pg, 0, 5 * pg);
    close(fd);
#else
    test_logf("[6] skipped: no memfd_create in this sysroot\n");
#endif
}

// Start this binary again as an unrelated process image, running one of the
// waiter modes main() dispatches on.
static pid_t spawn_self(const char *mode, const char *arg1, const char *arg2) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { self_exe, (char *) mode, (char *) arg1, (char *) arg2, NULL };
        execv(self_exe, argv);
        _exit(6);
    }
    return pid;
}

// --shm-waiter NAME: map a POSIX shm object this process opened by name.
static int shm_waiter(const char *name) {
    int fd = shm_open(name, O_RDWR, 0);
    char *m = fd < 0 ? NULL : map_at(fd, 0, file_size);
    if (m == NULL)
        _exit(5);
    wait_then_exit(word_at(m, 0, 5 * pg), word_at(m, 0, 5 * pg + 64), FX_WAIT);
}

static void posix_shm_case(void) {
    const char *label = "[7] POSIX shm, opened by name in an exec'd process";
    test_logf("%s\n", label);
    char name[64];
    snprintf(name, sizeof name, "/futex_shared_mapping.%d", (int) getpid());
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || ftruncate(fd, (off_t) file_size) != 0) {
        setup_fail("[7] shm_open");
        if (fd >= 0)
            close(fd);
        shm_unlink(name);
        return;
    }
    // Only the page the word is on, from a host-page boundary on the waiter's
    // side of it.
    off_t off = 5 * pg;
    char *m = map_at(fd, off, (size_t) pg);
    pid_t pid = m == NULL ? -1 : spawn_self("--shm-waiter", name, NULL);
    if (pid < 0) {
        setup_fail("[7] the waker's mapping, or fork");
    } else {
        child_asleep(pid, word_at(m, off, 5 * pg + 64));
        long woke = wake_retry(word_at(m, off, 5 * pg), FX_WAKE);
        expect_woken(label, woke, reap(pid));
    }
    if (m != NULL)
        munmap(m, (size_t) pg);
    close(fd);
    shm_unlink(name);
}

// --sysv-waiter ID: attach a SysV segment by id, in a process image that
// never had it.
static int sysv_waiter(int id) {
    char *m = shmat(id, NULL, 0);
    if (m == (char *) -1)
        _exit(5);
    wait_then_exit(word_at(m, 0, pg), word_at(m, 0, pg + 64), FX_WAIT);
}

static void sysv_case(void) {
    test_logf("[8] SysV shm\n");
    int id = shmget(IPC_PRIVATE, (size_t) (2 * pg), IPC_CREAT | 0600);
    char *p = id < 0 ? (char *) -1 : shmat(id, NULL, 0);
    if (p == (char *) -1) {
        setup_fail("[8] shmget/shmat");
        if (id >= 0)
            shmctl(id, IPC_RMID, NULL);
        return;
    }
    // A second attach, made in the child, at an address of its own.
    pid_t pid = fork();
    if (pid == 0) {
        char *q = shmat(id, NULL, 0);
        if (q == (char *) -1)
            _exit(5);
        wait_then_exit(word_at(q, 0, pg), word_at(q, 0, pg + 64), FX_WAIT);
    }
    if (pid < 0) {
        setup_fail("fork");
    } else {
        child_asleep(pid, word_at(p, 0, pg + 64));
        long woke = wake_retry(word_at(p, 0, pg), FX_WAKE);
        expect_woken("[8] a second attach in a forked child", woke, reap(pid));
    }
    __atomic_store_n(word_at(p, 0, pg + 64), 0, __ATOMIC_SEQ_CST);
    char ids[32];
    snprintf(ids, sizeof ids, "%d", id);
    pid = spawn_self("--sysv-waiter", ids, NULL);
    if (pid < 0) {
        setup_fail("fork");
    } else {
        child_asleep(pid, word_at(p, 0, pg + 64));
        long woke = wake_retry(word_at(p, 0, pg), FX_WAKE);
        expect_woken("[8] an attach by id after exec", woke, reap(pid));
    }
    shmdt(p);
    shmctl(id, IPC_RMID, NULL);
}

// [9] [10]: the waiter on `a`'s word, inherited over fork; a wake at the same
// offset of `b`, a different object, must reach nobody.
static void fork_shared_case(const char *label, char *a, char *b) {
    test_logf("%s\n", label);
    off_t word_off = pg, mark_off = pg + 64;
    pid_t pid = fork();
    if (pid < 0) {
        setup_fail("fork");
        return;
    }
    if (pid == 0)
        wait_then_exit(word_at(a, 0, word_off), word_at(a, 0, mark_off), FX_WAIT);
    char what[160];
    snprintf(what, sizeof what, "%s: setup: the waiter is asleep in its wait", label);
    ck(what, child_asleep(pid, word_at(a, 0, mark_off)), 1);
    snprintf(what, sizeof what, "%s: a wake in the other mapping reaches nobody", label);
    ck(what, fx2(word_at(b, 0, word_off), FX_WAKE, 1, 0, NULL, 0), 0);
    long woke = wake_retry(word_at(a, 0, word_off), FX_WAKE);
    expect_woken(label, woke, reap(pid));
}

static void anon_case(void) {
    size_t len = (size_t) (2 * pg);
    char *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    char *b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) {
        setup_fail("[9] MAP_SHARED|MAP_ANONYMOUS");
    } else {
        fork_shared_case("[9] MAP_SHARED|MAP_ANONYMOUS, forked", a, b);
    }
    if (a != MAP_FAILED)
        munmap(a, len);
    if (b != MAP_FAILED)
        munmap(b, len);
}

static void dev_zero_case(void) {
    size_t len = (size_t) (2 * pg);
    int fd = open("/dev/zero", O_RDWR);
    char *a = fd < 0 ? NULL : map_at(fd, 0, len);
    char *b = fd < 0 ? NULL : map_at(fd, 0, len);
    if (a == NULL || b == NULL)
        setup_fail("[10] MAP_SHARED /dev/zero");
    else
        fork_shared_case("[10] MAP_SHARED /dev/zero, two mappings of one descriptor", a, b);
    if (a != NULL)
        munmap(a, len);
    if (b != NULL)
        munmap(b, len);
    if (fd >= 0)
        close(fd);
}

// [11] [12]: the waiter maps the whole file and waits on word A (or B); the
// waker opened the file itself, and reaches the words through a mapping of
// all of it and one of only their page.
static pid_t file_waiter(const char *path, off_t word_off, off_t mark_off) {
    pid_t pid = fork();
    if (pid == 0) {
        int f = open(path, O_RDWR);
        char *m = f < 0 ? NULL : map_at(f, 0, file_size);
        if (m == NULL)
            _exit(5);
        wait_then_exit(word_at(m, 0, word_off), word_at(m, 0, mark_off), FX_WAIT);
    }
    return pid;
}

static void requeue_cases(char *whole, char *page, const char *path) {
    off_t a_off = 5 * pg, b_off = 5 * pg + 16, mark_off = 5 * pg + 64;
    for (int cmp = 1; cmp >= 0; cmp--) {
        const char *label = cmp ? "[11] FUTEX_CMP_REQUEUE between processes"
                                : "[11] FUTEX_REQUEUE between processes";
        test_logf("%s\n", label);
        __atomic_store_n(word_at(whole, 0, mark_off), 0, __ATOMIC_SEQ_CST);
        pid_t pid = file_waiter(path, a_off, mark_off);
        if (pid < 0) {
            setup_fail("fork");
            continue;
        }
        child_asleep(pid, word_at(whole, 0, mark_off));
        long moved = 0;
        for (int waited = 0; moved == 0 && waited <= wake_retry_ms; waited += 20) {
            moved = cmp ? fx2(word_at(whole, 0, a_off), FX_CMP_REQUEUE, 0, 1,
                              word_at(whole, 0, b_off), 0)
                        : fx2(word_at(whole, 0, a_off), FX_REQUEUE, 0, 1,
                              word_at(whole, 0, b_off), 0);
            if (moved == 0)
                usleep(20000);
        }
        char what[160];
        snprintf(what, sizeof what, "%s: the waiter was requeued (none woken, 1 moved)", label);
        ck(what, moved, 1);
        long woke = fx2(word_at(page, 5 * pg, b_off), FX_WAKE, 1, 0, NULL, 0);
        snprintf(what, sizeof what, "%s, then woken at the target through another mapping", label);
        expect_woken(what, woke, reap(pid));
    }
}

static void wake_op_case(char *whole, char *page, const char *path) {
    off_t a_off = 5 * pg, b_off = 5 * pg + 16, mark_off = 5 * pg + 64;
    const char *label = "[12] FUTEX_WAKE_OP reaches a waiter on uaddr2 in another process";
    test_logf("%s\n", label);
    __atomic_store_n(word_at(whole, 0, mark_off), 0, __ATOMIC_SEQ_CST);
    pid_t pid = file_waiter(path, b_off, mark_off);
    if (pid < 0) {
        setup_fail("fork");
        return;
    }
    child_asleep(pid, word_at(page, 5 * pg, mark_off));
    long woke = 0;
    for (int waited = 0; woke == 0 && waited <= wake_retry_ms; waited += 20) {
        __atomic_store_n(word_at(page, 5 * pg, b_off), 0, __ATOMIC_SEQ_CST);
        woke = fx2(word_at(page, 5 * pg, a_off), FX_WAKE_OP, 1, 1,
                   word_at(page, 5 * pg, b_off), FX_OP_SET1_IF_WAS0);
        if (woke == 0)
            usleep(20000);
    }
    expect_woken(label, woke, reap(pid));
    ck("[12] and the op was applied to uaddr2",
            __atomic_load_n(word_at(whole, 0, b_off), __ATOMIC_SEQ_CST), 1);
}

// The waker's side of [11] and [12]: its own open() of the file, a mapping of
// all of it, and one of only the page the words are on.
static void requeue_wake_op_cases(const char *path) {
    int f = open(path, O_RDWR);
    char *whole = f < 0 ? NULL : map_at(f, 0, file_size);
    char *page = f < 0 ? NULL : map_at(f, 5 * pg, (size_t) pg);
    if (whole == NULL || page == NULL) {
        setup_fail("[11] the waker's mappings");
    } else {
        requeue_cases(whole, page, path);
        wake_op_case(whole, page, path);
    }
    if (whole != NULL)
        munmap(whole, file_size);
    if (page != NULL)
        munmap(page, (size_t) pg);
    if (f >= 0)
        close(f);
}

static uint32_t *wop_word;
static uint32_t wop_marker;
static volatile pid_t wop_tid;
static volatile long wop_ret;
static volatile int wop_errno;

static void *wop_waiter(void *arg) {
    (void) arg;
    struct timespec to = { (time_t) wait_secs, 0 };
    wop_tid = (pid_t) syscall(SYS_gettid);
    __atomic_store_n(&wop_marker, 1, __ATOMIC_SEQ_CST);
    errno = 0;
    wop_ret = fx(wop_word, FX_WAIT | FX_PRIVATE, 0, &to);
    wop_errno = errno;
    return NULL;
}

static void wake_op_private_case(void) {
    const char *label = "[13] FUTEX_WAKE_OP_PRIVATE on a MAP_SHARED page";
    test_logf("%s\n", label);
    size_t len = (size_t) (2 * pg);
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        setup_fail("[13] MAP_SHARED|MAP_ANONYMOUS");
        return;
    }
    wop_word = word_at(m, 0, pg);
    wop_marker = 0;
    wop_tid = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, wop_waiter, NULL) != 0) {
        setup_fail("[13] pthread_create");
        munmap(m, len);
        return;
    }
    while (__atomic_load_n(&wop_marker, __ATOMIC_SEQ_CST) == 0)
        usleep(1000);
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int) wop_tid);
    waiter_asleep(path, &wop_marker);
    long woke = 0;
    for (int waited = 0; woke == 0 && waited <= wake_retry_ms; waited += 20) {
        woke = fx2(wop_word, FX_WAKE_OP | FX_PRIVATE, 1, 0, word_at(m, 0, pg + 16),
                   FX_OP_SET1_IF_WAS0);
        if (woke == 0)
            usleep(20000);
    }
    pthread_join(t, NULL);
    ck("[13] the wake found the FUTEX_WAIT_PRIVATE waiter", woke, 1);
    ck("[13] the waiter was woken, not timed out", wop_ret == 0 ? 0 : wop_errno, 0);
    munmap(m, len);
}

// [14] [15]: pthread objects at the start of the file's second page. The
// parent reaches them through a mapping of that page alone, the child through
// its own open() and a mapping of the whole file.
struct pshared_block {
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t ready, locking, flag;
};

static struct pshared_block *block_in_child(const char *path) {
    int f = open(path, O_RDWR);
    char *m = f < 0 ? NULL : map_at(f, 0, file_size);
    return m == NULL ? NULL : (struct pshared_block *) (m + pg);
}

static int wait_flag(uint32_t *flag) {
    for (int i = 0; i < 500; i++) {
        if (__atomic_load_n(flag, __ATOMIC_SEQ_CST) != 0)
            return 1;
        usleep(10000);
    }
    return 0;
}

static void pthread_cases(const char *path) {
    int f = make_file(path);
    struct pshared_block *b = f < 0 ? NULL : (struct pshared_block *) map_at(f, pg, (size_t) pg);
    if (b == NULL) {
        setup_fail("[14] the pthread file");
        if (f >= 0)
            close(f);
        unlink(path);
        return;
    }
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);

    const char *label = "[14] pthread condvar, process-shared, in a file";
    test_logf("%s\n", label);
    if (pthread_mutex_init(&b->m, &ma) != 0 || pthread_cond_init(&b->c, &ca) != 0) {
        setup_fail("[14] pthread_mutex_init/pthread_cond_init");
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            struct pshared_block *c = block_in_child(path);
            if (c == NULL)
                _exit(5);
            struct timespec at = deadline_in((int) wait_secs);
            if (pthread_mutex_timedlock(&c->m, &at) != 0)
                _exit(5);
            __atomic_store_n(&c->ready, 1, __ATOMIC_SEQ_CST);
            int r = 0;
            while (r == 0 && __atomic_load_n(&c->flag, __ATOMIC_SEQ_CST) == 0)
                r = pthread_cond_timedwait(&c->c, &c->m, &at);
            pthread_mutex_unlock(&c->m);
            _exit(r == 0 ? 0 : r == ETIMEDOUT ? 2 : 4);
        }
        if (pid < 0) {
            setup_fail("fork");
        } else if (!wait_flag(&b->ready)) {
            setup_fail("[14] the child never locked the mutex");
            kill_and_reap(pid);
        } else {
            // The child holds the mutex until its cond wait lets go of it, so
            // this also takes the mutex's wake across the two mappings.
            struct timespec at = deadline_in((int) wait_secs);
            int lr = pthread_mutex_timedlock(&b->m, &at);
            ck("[14] the parent took the mutex the child's cond wait released", lr, 0);
            __atomic_store_n(&b->flag, 1, __ATOMIC_SEQ_CST);
            double t0 = now_mono();
            pthread_cond_signal(&b->c);
            if (lr == 0)
                pthread_mutex_unlock(&b->m);
            int st = reap(pid);
            double ms = (now_mono() - t0) * 1000;
            test_logf("    the child returned %.0fms after the signal\n", ms);
            ck("[14] pthread_cond_signal woke the child (0), not its timeout (2)", st, 0);
            ck("[14] promptly", ms < wait_secs * 1000 / 3, 1);
        }
        pthread_cond_destroy(&b->c);
        pthread_mutex_destroy(&b->m);
    }

    label = "[14] pthread mutex, process-shared: an unlock wakes the other process";
    test_logf("%s\n", label);
    memset(b, 0, sizeof *b);
    if (pthread_mutex_init(&b->m, &ma) != 0) {
        setup_fail("[14] pthread_mutex_init");
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            struct pshared_block *c = block_in_child(path);
            if (c == NULL)
                _exit(5);
            struct timespec at = deadline_in((int) wait_secs);
            if (pthread_mutex_timedlock(&c->m, &at) != 0)
                _exit(5);
            __atomic_store_n(&c->ready, 1, __ATOMIC_SEQ_CST);
            wait_flag(&c->locking);
            usleep(300000);
            pthread_mutex_unlock(&c->m);
            _exit(0);
        }
        if (pid < 0) {
            setup_fail("fork");
        } else if (!wait_flag(&b->ready)) {
            setup_fail("[14] the child never locked the mutex");
            kill_and_reap(pid);
        } else {
            __atomic_store_n(&b->locking, 1, __ATOMIC_SEQ_CST);
            struct timespec at = deadline_in((int) wait_secs);
            double t0 = now_mono();
            int lr = pthread_mutex_timedlock(&b->m, &at);
            double ms = (now_mono() - t0) * 1000;
            test_logf("    timedlock returned %d after %.0fms\n", lr, ms);
            ck("[14] the blocked pthread_mutex_timedlock took the lock", lr, 0);
            ck("[14] when the child let go, not at the deadline", ms < wait_secs * 1000 / 2, 1);
            if (lr == 0)
                pthread_mutex_unlock(&b->m);
            ck("[14] the holder exited cleanly", reap(pid), 0);
        }
        pthread_mutex_destroy(&b->m);
    }

    label = "[15] robust process-shared mutex: the holder dies";
    test_logf("%s\n", label);
    memset(b, 0, sizeof *b);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    if (pthread_mutex_init(&b->m, &ma) != 0) {
        setup_fail("[15] pthread_mutex_init (robust)");
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            struct pshared_block *c = block_in_child(path);
            if (c == NULL)
                _exit(5);
            struct timespec at = deadline_in((int) wait_secs);
            if (pthread_mutex_timedlock(&c->m, &at) != 0)
                _exit(5);
            __atomic_store_n(&c->ready, 1, __ATOMIC_SEQ_CST);
            wait_flag(&c->locking);
            usleep(300000);
            _exit(0);                // still holding it
        }
        if (pid < 0) {
            setup_fail("fork");
        } else if (!wait_flag(&b->ready)) {
            setup_fail("[15] the child never locked the mutex");
            kill_and_reap(pid);
        } else {
            __atomic_store_n(&b->locking, 1, __ATOMIC_SEQ_CST);
            struct timespec at = deadline_in((int) wait_secs);
            double t0 = now_mono();
            int lr = pthread_mutex_timedlock(&b->m, &at);
            double ms = (now_mono() - t0) * 1000;
            test_logf("    timedlock returned %d (%s) after %.0fms\n", lr, strerror(lr), ms);
            ck("[15] the waiter in the other process has EOWNERDEAD", lr, EOWNERDEAD);
            ck("[15] when the holder died, not at the deadline", ms < wait_secs * 1000 / 2, 1);
            if (lr == EOWNERDEAD) {
                pthread_mutex_consistent(&b->m);
                pthread_mutex_unlock(&b->m);
            }
            ck("[15] the holder exited cleanly", reap(pid), 0);
        }
        pthread_mutex_destroy(&b->m);
    }
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);
    munmap(b, (size_t) pg);
    close(f);
    unlink(path);
}

// --sem-waiter NAME FD: open the named semaphore, say so on FD, and wait.
static int sem_waiter(const char *name, int fd) {
    sem_t *s = sem_open(name, 0);
    if (s == SEM_FAILED)
        _exit(5);
    struct timespec at = deadline_in((int) wait_secs);
    if (write(fd, "w", 1) != 1)
        _exit(5);
    close(fd);
    int r = sem_timedwait(s, &at);
    _exit(r == 0 ? 0 : errno == ETIMEDOUT ? 2 : 4);
}

static void named_sem_case(void) {
    const char *label = "[16] sem_open, posted to an exec'd process";
    test_logf("%s\n", label);
    char name[64];
    snprintf(name, sizeof name, "/futex_shared_mapping.%d", (int) getpid());
    sem_unlink(name);
    sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    int p[2];
    if (s == SEM_FAILED || pipe(p) != 0) {
        setup_fail("[16] sem_open/pipe");
        if (s != SEM_FAILED)
            sem_close(s);
        sem_unlink(name);
        return;
    }
    char fds[16];
    snprintf(fds, sizeof fds, "%d", p[1]);
    pid_t pid = spawn_self("--sem-waiter", name, fds);
    close(p[1]);
    char c;
    if (pid < 0) {
        setup_fail("fork");
    } else if (read(p[0], &c, 1) != 1) {
        setup_fail("[16] the child never opened the semaphore");
        kill_and_reap(pid);
    } else {
        // Asleep in sem_timedwait: after its write, it does nothing else.
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
        for (int i = 0; i < 500 && proc_state(path) != 'S'; i++)
            usleep(10000);
        usleep(100000);
        double t0 = now_mono();
        sem_post(s);
        int st = reap(pid);
        double ms = (now_mono() - t0) * 1000;
        test_logf("    the child returned %.0fms after the post\n", ms);
        ck("[16] sem_post woke the child (0), not its timeout (2)", st, 0);
        ck("[16] promptly", ms < wait_secs * 1000 / 3, 1);
    }
    close(p[0]);
    sem_close(s);
    sem_unlink(name);
}

int main(int argc, char **argv) {
    pg = sysconf(_SC_PAGESIZE);
    file_size = (size_t) (8 * pg);
    wait_secs = test_watchdog_secs(6);
    wake_retry_ms = (int) test_watchdog_secs(3) * 1000;
    if (argc >= 3 && strcmp(argv[1], "--shm-waiter") == 0)
        return shm_waiter(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--sysv-waiter") == 0)
        return sysv_waiter(atoi(argv[2]));
    if (argc >= 4 && strcmp(argv[1], "--sem-waiter") == 0)
        return sem_waiter(argv[2], atoi(argv[3]));
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    if (n > 0)
        self_exe[n] = '\0';
    else
        snprintf(self_exe, sizeof self_exe, "%s", argv[0]);

    char path_a[128], path_b[128], path_p[128];
    snprintf(path_a, sizeof path_a, "/tmp/futex_shared_mapping.%d.a", (int) getpid());
    snprintf(path_b, sizeof path_b, "/tmp/futex_shared_mapping.%d.b", (int) getpid());
    snprintf(path_p, sizeof path_p, "/tmp/futex_shared_mapping.%d.p", (int) getpid());
    int fa = make_file(path_a), fb = make_file(path_b);
    if (fa < 0 || fb < 0) {
        setup_fail("the test files in /tmp");
        return finish_suite("futex_shared_mapping");
    }

    // The two shapes first measured: the word at file offset 4096.
    file_case("[1] a file two processes opened, both mapping from 0",
            path_a, -1, 0, 0, pg);
    file_case("[2] a file two processes opened, the waker mapping only page 2",
            path_a, -1, pg, (size_t) pg, pg);
    file_case("[3] one inherited descriptor, the waker mapping from 16 KiB",
            NULL, fa, 4 * pg, 0, 5 * pg);
    file_key_case(path_a, path_b);
    tmpfs_case();
    memfd_case();
    posix_shm_case();
    sysv_case();
    anon_case();
    dev_zero_case();
    requeue_wake_op_cases(path_a);
    wake_op_private_case();
    pthread_cases(path_p);
    named_sem_case();

    close(fa);
    close(fb);
    unlink(path_a);
    unlink(path_b);
    return finish_suite("futex_shared_mapping");
}
