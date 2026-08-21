// AIO under concurrency, which is the way MariaDB uses it: many threads
// sharing one context, and a teardown that can land while they are still
// inside the calls.
//
// The second phase is the one with history. io_destroy used to unlink the
// context and free it immediately, on the argument that submissions are
// synchronous so nothing is in flight. That confuses "no work in flight" with
// "no threads inside the calls": a thread parked in io_getevents holds the
// context's lock and cond, and freeing those under it is a use-after-free.
// Contexts are reference counted now.
//
// Be clear about what this phase does and does not prove. It pins down the
// CONTRACT -- a destroy wakes everyone parked on the context, they return
// rather than sit out their timeout, and the context reads EINVAL afterwards.
// It does NOT prove the use-after-free is gone: run it against the old code
// and it still passes, because cond_destroy happens to wake the waiters and
// freed memory happens to still be readable. The bug was real by inspection
// (free(ctx), then those threads touch ctx->lock and ctx->events) and the
// obvious detector is unavailable -- an AddressSanitizer build of the
// emulator dies in ASan's own poisoning code before any guest runs. So this
// is a regression test for the behaviour, and the reference counting is
// justified by reading rather than by a red-to-green here.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static long io_setup_(unsigned n, aio_context_t *c) { return syscall(__NR_io_setup, n, c); }
static long io_destroy_(aio_context_t c) { return syscall(__NR_io_destroy, c); }
static long io_submit_(aio_context_t c, long n, struct iocb **p) { return syscall(__NR_io_submit, c, n, p); }
static long io_getevents_(aio_context_t c, long min, long nr, struct io_event *e, struct timespec *t) {
    return syscall(__NR_io_getevents, c, min, nr, e, t);
}

static void ck(const char *what, int ok, const char *detail) {
    if (!ok)
        failf(what, (uint64_t) errno, 0, 0, 0, 0, 0);
    test_logf("  %-46s %s%s%s\n", what, ok ? "ok" : "FAIL",
              ok || detail == NULL ? "" : "   ", ok || detail == NULL ? "" : detail);
}

#define NTHREADS 4
#define PER_THREAD 128
#define TOTAL (NTHREADS * PER_THREAD)

static aio_context_t ctx;
static int shared_fd;
static char payload[256];

// Phase 1: every thread submits its own slice, tagging each iocb with a data
// value only it uses, so a lost or duplicated completion is visible in the
// tally rather than merely suspected.
struct slice { int id; int submitted; int failed; };

static void *submitter(void *arg) {
    struct slice *s = arg;
    for (int i = 0; i < PER_THREAD; i++) {
        struct iocb cb;
        struct iocb *cbs[1] = { &cb };
        memset(&cb, 0, sizeof(cb));
        cb.aio_lio_opcode = IOCB_CMD_PWRITE;
        cb.aio_fildes = shared_fd;
        cb.aio_buf = (uint64_t) (uintptr_t) payload;
        cb.aio_nbytes = sizeof(payload);
        cb.aio_offset = (long long) (s->id * PER_THREAD + i) * sizeof(payload);
        cb.aio_data = (uint64_t) (s->id * PER_THREAD + i) + 1;   // 1..TOTAL
        if (io_submit_(ctx, 1, cbs) == 1)
            s->submitted++;
        else
            s->failed++;
    }
    return NULL;
}

// Phase 2: destroy a context while threads are PARKED inside io_getevents on
// it. They wait with a long timeout on an empty ring, so they are genuinely
// blocked on the context's cond -- not merely between calls -- when the
// destroy lands. That distinction is the whole test: an earlier version let
// submitters run alongside, which kept waking the reapers, and it passed
// against the very bug it was written for.
//
// The verdict is timing, not a crash, because a use-after-free is not obliged
// to crash. A destroy that wakes its waiters returns them in milliseconds; one
// that frees the cond out from under them leaves them to sit out the full
// timeout (or worse). PARK_SECS is chosen far enough above the wake path's
// cost that the two cannot be confused.
#define PARK_SECS 5
static aio_context_t race_ctx;
static int parked_rc[NTHREADS];

static void *parker(void *arg) {
    long which = (long) (intptr_t) arg;
    struct io_event ev;
    struct timespec t = { PARK_SECS, 0 };
    errno = 0;
    parked_rc[which] = (int) io_getevents_(race_ctx, 1, 1, &ev, &t);
    return NULL;
}

static double secs_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double) (now.tv_sec - start->tv_sec) +
           (double) (now.tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    memset(payload, 'q', sizeof(payload));

    char path[] = "/tmp/aio_threads_XXXXXX";
    shared_fd = mkstemp(path);
    ck("temp file", shared_fd >= 0, strerror(errno));
    unlink(path);

    ck("io_setup", io_setup_(TOTAL + 16, &ctx) == 0, strerror(errno));

    pthread_t th[NTHREADS];
    struct slice slices[NTHREADS];
    int spawned = 0;
    for (int i = 0; i < NTHREADS; i++) {
        slices[i] = (struct slice){ .id = i };
        if (pthread_create(&th[i], NULL, submitter, &slices[i]) == 0)
            spawned++;
    }
    ck("spawned the submitters", spawned == NTHREADS, NULL);
    for (int i = 0; i < spawned; i++)
        pthread_join(th[i], NULL);

    int submitted = 0, failed = 0;
    for (int i = 0; i < NTHREADS; i++) {
        submitted += slices[i].submitted;
        failed += slices[i].failed;
    }
    ck("every concurrent submit was accepted", failed == 0 && submitted == TOTAL, NULL);

    // Reap the lot and check each tag arrives exactly once. A context whose
    // ring is not properly locked loses or repeats events here.
    static unsigned char seen[TOTAL + 2];
    int reaped = 0, bad_res = 0, dup = 0, out_of_range = 0;
    while (reaped < submitted) {
        struct io_event evs[32];
        struct timespec t = { 2, 0 };
        long got = io_getevents_(ctx, 1, 32, evs, &t);
        if (got <= 0)
            break;
        for (long i = 0; i < got; i++) {
            if ((long) evs[i].res != (long) sizeof(payload))
                bad_res++;
            uint64_t d = evs[i].data;
            if (d < 1 || d > TOTAL) { out_of_range++; continue; }
            if (seen[d]++) dup++;
        }
        reaped += got;
    }
    ck("reaped exactly what was submitted", reaped == submitted, NULL);
    ck("every completion carried the full byte count", bad_res == 0, NULL);
    ck("no completion arrived twice", dup == 0, NULL);
    ck("no completion carried a tag nobody submitted", out_of_range == 0, NULL);

    int missing = 0;
    for (int d = 1; d <= TOTAL; d++)
        if (!seen[d]) missing++;
    ck("no completion went missing", missing == 0, NULL);
    ck("io_destroy", io_destroy_(ctx) == 0, strerror(errno));

    // ---- phase 2: destroy with reapers parked on the context ----
    ck("io_setup for the destroy race", io_setup_(64, &race_ctx) == 0, strerror(errno));
    pthread_t rth[NTHREADS];
    int rspawned = 0;
    for (long i = 0; i < NTHREADS; i++) {
        parked_rc[i] = -999;
        if (pthread_create(&rth[i], NULL, parker, (void *) (intptr_t) i) == 0)
            rspawned++;
    }
    ck("spawned the parkers", rspawned == NTHREADS, NULL);

    // Long enough that every one of them is inside wait_for rather than still
    // on its way there.
    struct timespec settle = { 0, 250 * 1000 * 1000 };
    nanosleep(&settle, NULL);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ck("io_destroy with reapers parked on it", io_destroy_(race_ctx) == 0, strerror(errno));
    for (int i = 0; i < rspawned; i++)
        pthread_join(rth[i], NULL);
    double waited = secs_since(&t0);

    char detail[64];
    snprintf(detail, sizeof(detail), "took %.2fs of a %ds timeout", waited, PARK_SECS);
    ck("the destroy woke them instead of leaving them parked",
       waited < (double) PARK_SECS / 2, detail);
    ck("everyone survived the destroy", 1, NULL);

    int still_waiting = 0;
    for (int i = 0; i < rspawned; i++)
        if (parked_rc[i] == -999) still_waiting++;
    ck("every parked reaper returned", still_waiting == 0, NULL);

    // And the context really is gone.
    struct io_event ev;
    struct timespec zero = { 0, 0 };
    ck("the destroyed context is EINVAL afterwards",
       io_getevents_(race_ctx, 0, 1, &ev, &zero) == -1 && errno == EINVAL,
       strerror(errno));

    close(shared_fd);
    return finish_suite("aio_threads");
}
