// Regression test for a fakefs metadata/host-filesystem entry-type race.
//
// fs/fake.c's mutators (fakefs_mkdir, fakefs_unlink, fakefs_rmdir,
// fakefs_symlink, fakefs_mknod, fakefs_rename, fakefs_link) each pair a real
// host filesystem operation with a SQLite ish_stat.mode metadata update
// inside one db_begin_write()/db_commit() transaction, so each pair is
// atomic on its own. But nothing used to serialize *between* these
// operations and open(O_CREAT) on fs/generic.c's side: generic_mkdirat,
// generic_unlinkat, generic_rmdirat, generic_symlinkat, generic_mknodat,
// generic_renameat, and generic_linkat called straight into the fakefs
// mutator with no lock at all, and fakefs_open()'s real host open() used to
// run before its own metadata write. A three-way interleaving of
// open(O_CREAT) vs. unlink()/rmdir() vs. mkdir()/symlink()/mknod() on the
// same path could leave the real host entry as one type (e.g. a directory)
// while the SQLite metadata still described a different type (e.g. a
// regular file), or vice versa -- exactly the mismatch that used to
// assert-crash the whole ish process in fs/real.c's realfs_opendir (fixed
// separately in commit 15ff5d8e; this test targets the underlying metadata
// corruption that produces that mismatch in the first place).
//
// Test shape: three racer threads continuously hammer
// open(O_CREAT)/unlink/mkdir/unlink on one shared path while the main thread
// polls it with lstat() + a follow-up opendir()/open()+fstat() consistency
// check. lstat() and that follow-up are two independent syscalls with no
// shared atomicity even on real Linux, so while the racers are still active
// a "candidate" inconsistency can be nothing more than ordinary TOCTOU (the
// path legitimately changed type in the gap between the two calls). To tell
// that apart from genuine, persistent metadata/host corruption, a candidate
// mismatch immediately stops and joins every racer thread, then re-checks
// the now-static entry: with nothing left to race against, any inconsistency
// that still reproduces is not timing-dependent -- the metadata and the real
// host filesystem simply disagree, which is exactly the bug this guards
// against.
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

#define TOTAL_CHECKS 20000

static char g_path[512];
static atomic_int g_stop;

static void *writer_thread(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        int fd = open(g_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            char byte = 'x';
            ssize_t res = write(fd, &byte, 1);
            (void) res;
            close(fd);
        }
    }
    return NULL;
}

static void *mkdir_thread(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        unlink(g_path);
        mkdir(g_path, 0700);
    }
    return NULL;
}

static void *unlink_thread(void *arg) {
    (void) arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        if (unlink(g_path) < 0)
            rmdir(g_path);
    }
    return NULL;
}

// Checks whether lstat()'s reported type is consistent with what a
// follow-up opendir()/open()+fstat() actually finds on the entry. Returns 1
// on inconsistency, 0 if consistent (or the path doesn't currently exist).
static int type_mismatch(void) {
    struct stat st;
    if (lstat(g_path, &st) < 0)
        return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(g_path);
        if (d == NULL)
            return errno == ENOTDIR;
        closedir(d);
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        int fd = open(g_path, O_RDONLY);
        if (fd < 0)
            return errno != ENOENT;
        // open(O_RDONLY) alone can't tell a regular file from a directory
        // (Linux allows opening a directory O_RDONLY), so cross-check with
        // fstat() to catch that direction too.
        struct stat fst;
        int mismatch = fstat(fd, &fst) == 0 && S_ISDIR(fst.st_mode);
        close(fd);
        return mismatch;
    }
    return 0;
}

// Seconds of NO PROGRESS that counts as a deadlock. The check loop re-arms the
// alarm as it advances (see WATCHDOG_REARM_EVERY), so this is a stall detector,
// not a total-runtime budget.
//
// It used to be a flat alarm(120) covering the whole run, which made the test
// fail by construction on slow hardware: TOTAL_CHECKS is a FIXED count and every
// check is several fakefs operations, i.e. SQLite plus its per-transaction fcntl,
// so a device merely being slow blew the deadline with nothing wrong. That is
// exactly what happened on an A9 iPad (2 cores, 2GB): 3 for 3 "watchdog timeout
// (possible deadlock)" while the process was demonstrably in state R with CPU
// time still accumulating, i.e. grinding, not stuck. A regression gate that
// false-fails on the oldest supported hardware is worse than no gate, because
// the failure looks exactly like the deadlock it is meant to catch.
#define WATCHDOG_STALL_SECS 60
#define WATCHDOG_REARM_EVERY 64

static void on_alarm(int sig) {
    (void) sig;
    // Guard against a latent deadlock in the locking fix: fail loudly with a
    // clear marker instead of hanging the regression run forever. Reaching here
    // now means the check loop made NO progress for WATCHDOG_STALL_SECS.
    const char msg[] = "fakefs_type_race: FAIL watchdog timeout (possible deadlock)\n";
    ssize_t res = write(2, msg, sizeof(msg) - 1);
    (void) res;
    _exit(1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(WATCHDOG_STALL_SECS);

    snprintf(g_path, sizeof(g_path), "%s/fakefs_type_race.%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int) getpid());
    unlink(g_path);
    rmdir(g_path);

    unsigned mismatches = 0;
    unsigned restarts = 0;
    pthread_t writer, mkdirer, unlinker;
    atomic_store(&g_stop, 0);
    if (pthread_create(&writer, NULL, writer_thread, NULL) != 0 ||
        pthread_create(&mkdirer, NULL, mkdir_thread, NULL) != 0 ||
        pthread_create(&unlinker, NULL, unlink_thread, NULL) != 0) {
        perror("pthread_create");
        return 2;
    }

    // Race continuously (not in short bursts) to give the racer threads
    // maximum opportunity to hit the exact interleaving that corrupts
    // things, restarting them fresh only when a candidate mismatch needs to
    // be isolated and verified.
    for (int i = 0; i < TOTAL_CHECKS; i++) {
        // Re-arm the stall watchdog as we advance, so the deadline tracks
        // progress rather than total runtime. A genuine deadlock stops
        // advancing and the alarm then fires WATCHDOG_STALL_SECS later; a
        // merely slow device keeps pushing it out and still completes.
        if (i % WATCHDOG_REARM_EVERY == 0)
            alarm(WATCHDOG_STALL_SECS);
        if (!type_mismatch())
            continue;

        atomic_store(&g_stop, 1);
        pthread_join(writer, NULL);
        pthread_join(mkdirer, NULL);
        pthread_join(unlinker, NULL);

        // Every racer thread has now fully stopped: nothing can change
        // g_path underneath us any more. Re-check on the settled entry --
        // an inconsistency that still reproduces here can't be blamed on an
        // ordinary cross-syscall race window.
        if (type_mismatch()) {
            test_log_if(1, "MISMATCH check %d: metadata/host entry-type"
                        " disagreement persisted after all racers stopped\n", i);
            mismatches++;
        }

        unlink(g_path);
        rmdir(g_path);
        restarts++;

        atomic_store(&g_stop, 0);
        if (pthread_create(&writer, NULL, writer_thread, NULL) != 0 ||
            pthread_create(&mkdirer, NULL, mkdir_thread, NULL) != 0 ||
            pthread_create(&unlinker, NULL, unlink_thread, NULL) != 0) {
            perror("pthread_create");
            return 2;
        }
    }

    atomic_store(&g_stop, 1);
    pthread_join(writer, NULL);
    pthread_join(mkdirer, NULL);
    pthread_join(unlinker, NULL);
    unlink(g_path);
    rmdir(g_path);
    alarm(0);

    test_logf("fakefs_type_race: %u candidate restarts, %u persistent mismatches\n",
              restarts, mismatches);

    if (mismatches != 0) {
        printf("FAIL fakefs_type_race: %u persistent type mismatches observed"
               " (metadata/host filesystem entry-type race)\n", mismatches);
        failures_total++;
    }
    return finish_suite("fakefs_type_race");
}
