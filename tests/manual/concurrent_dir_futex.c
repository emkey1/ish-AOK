// Two read-modify-writes that were not atomic against other threads.
//
//   A directory read is a read-modify-write of the stream position: tell,
//   read, tell. Two threads sharing one directory fd interleaved it, so
//   entries came back twice and others were skipped entirely -- measured on a
//   400-entry directory read by four threads: 37 duplicates and one lost
//   entry. Linux holds f_pos_lock across the whole call, and delivers every
//   entry exactly once to exactly one caller. Anything walking a tree with a
//   worker pool -- find, rsync, a build system's scanner -- shares a dirfd
//   exactly this way, and a duplicated entry means duplicated work while a
//   lost one means a file that silently is not there.
//
//   FUTEX_WAKE_OP's operation on its second address is also a
//   read-modify-write, and the whole point of the call is to combine it with
//   a wake with no window in between. A plain load-compute-store lost updates
//   to any guest thread touching the same word -- 594 of 40000 with one
//   thread doing atomic adds and another doing WAKE_OP adds. Linux does it
//   with an arch cmpxchg loop and loses none. This is the primitive glibc's
//   condition variables are built on, so a lost update is a wakeup that never
//   happens.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#ifndef FUTEX_WAKE_OP
#define FUTEX_WAKE_OP 5
#endif
#define FUTEX_OP_ADD_ 1

struct ld64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define ENTRIES 400
#define READERS 4
#define ROUNDS 20000

static char base[80], dirpath[128];
static int shared_fd = -1;
static _Atomic int total_seen, duplicates;
static char seen[ENTRIES];
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static void *reader(void *arg) {
    (void) arg;
    char buf[512];
    for (;;) {
        long n = syscall(SYS_getdents64, shared_fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            struct ld64 *e = (struct ld64 *) (buf + off);
            if (e->d_reclen == 0)
                break;
            atomic_fetch_add(&total_seen, 1);
            if (e->d_name[0] == 'f') {
                int idx = atoi(e->d_name + 1);
                if (idx >= 0 && idx < ENTRIES) {
                    pthread_mutex_lock(&seen_lock);
                    if (seen[idx])
                        atomic_fetch_add(&duplicates, 1);
                    seen[idx] = 1;
                    pthread_mutex_unlock(&seen_lock);
                }
            }
            off += e->d_reclen;
        }
    }
    return NULL;
}

static _Atomic int counter;
static volatile int wake_word;

static void *atomic_adder(void *arg) {
    (void) arg;
    for (int i = 0; i < ROUNDS; i++)
        atomic_fetch_add(&counter, 1);
    return NULL;
}

static void *wakeop_adder(void *arg) {
    (void) arg;
    // FUTEX_OP_ADD 1 to `counter`, with a comparison that never wakes anyone:
    // the point is the arithmetic, not the wake.
    int encoded = (FUTEX_OP_ADD_ << 28) | (1 << 12) | 0;
    for (int i = 0; i < ROUNDS; i++)
        syscall(SYS_futex, &wake_word, FUTEX_WAKE_OP, 0, (void *) (long) 0,
                (int *) &counter, encoded);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(240));
    snprintf(base, sizeof base, "/tmp/concdf-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);
    snprintf(dirpath, sizeof dirpath, "%s/d", base);
    ck("mkdir the directory", mkdir(dirpath, 0755), 0);
    for (int i = 0; i < ENTRIES; i++) {
        char f[224];
        snprintf(f, sizeof f, "%s/f%03d", dirpath, i);
        int t = open(f, O_WRONLY | O_CREAT, 0644);
        if (t >= 0)
            close(t);
    }

    // ---- one directory fd, four readers -----------------------------------
    {
        shared_fd = open(dirpath, O_RDONLY | O_DIRECTORY);
        ck("open the directory once", shared_fd >= 0 ? 1 : 0, 1);
        if (shared_fd >= 0) {
            pthread_t th[READERS];
            int made = 0;
            for (; made < READERS; made++)
                if (pthread_create(&th[made], NULL, reader, NULL) != 0)
                    break;
            ck("start four readers on it", made, READERS);
            for (int i = 0; i < made; i++)
                pthread_join(th[i], NULL);
            int missing = 0;
            for (int i = 0; i < ENTRIES; i++)
                if (!seen[i])
                    missing++;
            // Every entry exactly once, to exactly one caller.
            ck("no entry was delivered twice", (long) atomic_load(&duplicates), 0);
            ck("no entry was lost", missing, 0);
            // ...and the total is the directory, plus . and ..
            ck("the total is the whole directory",
               (long) atomic_load(&total_seen), ENTRIES + 2);
            close(shared_fd);
        }
    }

    // ---- WAKE_OP's arithmetic against a guest atomic ----------------------
    {
        atomic_store(&counter, 0);
        pthread_t a, b;
        int ra = pthread_create(&a, NULL, atomic_adder, NULL);
        int rb = pthread_create(&b, NULL, wakeop_adder, NULL);
        ck("start the two adders", ra == 0 && rb == 0 ? 1 : 0, 1);
        if (ra == 0)
            pthread_join(a, NULL);
        if (rb == 0)
            pthread_join(b, NULL);
        // Both threads add ROUNDS times to the same word, one with a guest
        // atomic and one through WAKE_OP. Not one update may be lost.
        ck("every increment survived", (long) atomic_load(&counter), 2 * ROUNDS);
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("concurrent_dir_futex");
}
