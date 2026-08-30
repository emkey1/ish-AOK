// POSIX byte-range locks: what the range means, who holds it, and what
// happens when two holders want each other's.
//
// Three things AOK got wrong:
//
//   There was no range validation at all. A negative l_start, a negative
//   l_len reaching below zero, a SEEK_END offset that underflows, and an
//   l_start+l_len that overflows off_t were all accepted and quietly turned
//   into a lock over some other range -- so a caller took a lock it had not
//   asked for, and a later F_GETLK answered about a range nobody enquired
//   about. (That cascade is how this was found: an F_GETLK looked broken
//   because an earlier invalid request had already locked the region.)
//
//   l_pid reported the holding THREAD's id. fcntl(2) says it is "the PID of
//   the process holding the lock", so a lock taken by a worker thread was
//   attributed to a pid the reader could not kill, wait for, or find in ps.
//
//   F_SETLKW had no deadlock detection: two processes each holding what the
//   other wants waited forever. Linux returns EDEADLK, and this test hung
//   until it did.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12,
// including EOVERFLOW (not EINVAL) for the arithmetic overflow.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

static char path_a[64], path_b[64];

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-44s got=%ld want=%ld\n", label, got, want);
}
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
static int setlk(int fd, int type, off_t start, off_t len, int wait) {
    struct flock l;
    memset(&l, 0, sizeof l);
    l.l_type = type; l.l_whence = SEEK_SET; l.l_start = start; l.l_len = len;
    errno = 0;
    return fcntl(fd, wait ? F_SETLKW : F_SETLK, &l);
}
static void check_rejected(const char *label, int fd, int whence,
        off_t start, off_t len, int want_errno) {
    struct flock l;
    memset(&l, 0, sizeof l);
    l.l_type = F_WRLCK; l.l_whence = whence; l.l_start = start; l.l_len = len;
    errno = 0;
    int r = fcntl(fd, F_SETLK, &l);
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-44s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

// A non-leader thread holds the lock, so l_pid can tell tid from pid apart.
static volatile int held;
static void *holder(void *arg) {
    (void) arg;
    int fd = open(path_a, O_RDWR);
    if (fd >= 0 && setlk(fd, F_WRLCK, 0, 100, 0) == 0)
        held = 1;
    while (held == 1)
        nap(50);
    if (fd >= 0) close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    snprintf(path_a, sizeof path_a, "/tmp/lockval_a.%d", (int) getpid());
    snprintf(path_b, sizeof path_b, "/tmp/lockval_b.%d", (int) getpid());

    int fa = open(path_a, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int fb = open(path_b, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fa < 0 || fb < 0) {
        printf("fcntl_lock_validation: SKIP (cannot create temp files)\n");
        return 0;
    }
    if (ftruncate(fa, 4096) < 0 || ftruncate(fb, 4096) < 0) {}

    // Range validation. None of these may be accepted.
    check_rejected("negative l_start", fa, SEEK_SET, -1, 10, EINVAL);
    check_rejected("l_len reaching below zero", fa, SEEK_SET, 0, -100, EINVAL);
    check_rejected("l_start+l_len overflow", fa, SEEK_SET,
                   0x7ffffffffffffff0LL, 0x100, EOVERFLOW);
    check_rejected("SEEK_END underflow", fa, SEEK_END, -100000, 10, EINVAL);
    {   // and a bad l_type, which was already right
        struct flock l;
        memset(&l, 0, sizeof l);
        l.l_type = 99; l.l_whence = SEEK_SET;
        errno = 0;
        int r = fcntl(fa, F_SETLK, &l);
        check("bad l_type rejected", r, -1);
        check("bad l_type errno", r < 0 ? errno : 0, EINVAL);
    }
    // Nothing above may have left a lock behind: a valid request over the same
    // region must still succeed.
    check("region is still lockable afterwards", setlk(fa, F_WRLCK, 0, 100, 0), 0);
    setlk(fa, F_UNLCK, 0, 100, 0);

    // l_pid names the process, even when a thread took the lock.
    {
        held = 0;
        pthread_t t;
        if (pthread_create(&t, NULL, holder, NULL) != 0) {
            printf("  l_pid: SKIP (pthread_create)\n");
        } else {
            for (int i = 0; i < 200 && !held; i++)
                nap(20);
            if (!held) {
                printf("  l_pid: SKIP (holder thread never got the lock)\n");
            } else {
                fflush(NULL);
                pid_t c = fork();
                if (c == 0) {
                    int b = open(path_a, O_RDWR);
                    struct flock l;
                    memset(&l, 0, sizeof l);
                    l.l_type = F_WRLCK; l.l_whence = SEEK_SET;
                    l.l_start = 0; l.l_len = 100;
                    int ok = 0;
                    if (b >= 0 && fcntl(b, F_GETLK, &l) == 0)
                        ok = (l.l_type != F_UNLCK) && (l.l_pid == getppid());
                    _exit(ok ? 0 : 1);
                }
                int st = 0;
                while (waitpid(c, &st, 0) < 0 && errno == EINTR)
                    continue;
                check("F_GETLK reports the holding PROCESS",
                      WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
            }
            held = 2;
            pthread_join(t, NULL);
        }
    }

    // Deadlock: we hold A and want B; the child holds B and wants A.
    {
        int sync[2];
        if (pipe(sync) < 0) {
            printf("  deadlock: SKIP (pipe)\n");
        } else {
            fflush(NULL);
            pid_t c = fork();
            if (c == 0) {
                close(sync[0]);
                int a2 = open(path_a, O_RDWR), b2 = open(path_b, O_RDWR);
                setlk(b2, F_WRLCK, 0, 10, 0);          // child holds B
                if (write(sync[1], "r", 1) < 0) {}
                nap(300);
                setlk(a2, F_WRLCK, 0, 10, 1);          // and wants A
                _exit(0);
            }
            close(sync[1]);
            char r;
            if (read(sync[0], &r, 1) != 1) {}
            check("we take A", setlk(fa, F_WRLCK, 0, 10, 0), 0);
            nap(600);                                   // let the child block
            int rr = setlk(fb, F_WRLCK, 0, 10, 1);      // and we want B
            int e = rr < 0 ? errno : 0;
            check("F_SETLKW returns EDEADLK, not a hang", rr, -1);
            check("  errno", e, EDEADLK);
            kill(c, SIGKILL);
            int st;
            while (waitpid(c, &st, 0) < 0 && errno == EINTR)
                continue;
            close(sync[0]);
        }
    }

    close(fa); close(fb);
    unlink(path_a); unlink(path_b);
    return finish_suite("fcntl_lock_validation");
}
