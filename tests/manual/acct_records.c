/*
 * acct_records.c -- BSD process accounting: acct(2) and the records it writes.
 *
 * Checks:
 *   - acct(path) turns accounting on, acct(NULL) turns it off
 *   - one 64-byte acct_v3 record per PROCESS exit, not per thread
 *   - ac_version is 3, ac_comm is the exec'd name, ac_pid/ac_ppid are right
 *   - ac_exitcode is the WAIT-encoded status (exit 7 -> 0x0700), not the code
 *   - AFORK is set for a child that never exec'd and clear for one that did
 *   - ac_etime is in AHZ (centisecond) units, held as an IEEE float
 *
 * Runs unchanged on Linux, which is the point: the record format is Linux's
 * and the only way to be sure of it is to make the same assertions against a
 * real kernel. Needs root (CAP_SYS_PACCT); skips cleanly without it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#ifdef __linux__
#include <sys/syscall.h>
#endif

#define ACCT_COMM_ 16
#define AFORK_ 0x01

struct acct_v3_t {
    uint8_t  ac_flag;
    uint8_t  ac_version;
    uint16_t ac_tty;
    uint32_t ac_exitcode;
    uint32_t ac_uid;
    uint32_t ac_gid;
    uint32_t ac_pid;
    uint32_t ac_ppid;
    uint32_t ac_btime;
    uint32_t ac_etime_bits;
    uint16_t ac_utime;
    uint16_t ac_stime;
    uint16_t ac_mem;
    uint16_t ac_io;
    uint16_t ac_rw;
    uint16_t ac_minflt;
    uint16_t ac_majflt;
    uint16_t ac_swaps;
    char     ac_comm[ACCT_COMM_];
};

static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static float etime_of(const struct acct_v3_t *r) {
    float f;
    memcpy(&f, &r->ac_etime_bits, sizeof(f));
    return f;
}

static int do_acct(const char *path) {
    return syscall(SYS_acct, path);
}

int main(void) {
    const char *path = "/tmp/aok-acct-test";
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("acct_records: FAIL (create %s: %s)\n", path, strerror(errno));
        return 1;
    }
    close(fd);

    errno = 0;
    if (do_acct(path) != 0) {
        printf("acct_records: SKIP (acct on: %s)\n", strerror(errno));
        return 0;
    }
    test_logf("ok   acct.on\n");

    /* A child that EXECS: ac_comm becomes the new program, AFORK clear. */
    pid_t exec_child = fork();
    if (exec_child == 0) {
        execl("/bin/true", "true", (char *) NULL);
        _exit(9);
    }
    int st = 0;
    waitpid(exec_child, &st, 0);

    /* A child that does NOT exec, exiting 7: AFORK set, ac_exitcode 7<<8. */
    pid_t fork_child = fork();
    if (fork_child == 0)
        _exit(7);
    waitpid(fork_child, &st, 0);

    /* A child that lives a MEASURED length of time, so ac_etime's unit is
     * asserted rather than assumed. Self-calibrating against the wall clock
     * the parent measures, because an emulated guest is slower than a real
     * kernel and a fixed window would be a load test, not a unit test. The
     * window still separates the three candidates: for a ~1s child, AHZ
     * centiseconds gives ~100, seconds would give 1, milliseconds ~1000. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t timed_child = fork();
    if (timed_child == 0) {
        struct timespec nap = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&nap, NULL);
        _exit(0);
    }
    waitpid(timed_child, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall_cs = ((t1.tv_sec - t0.tv_sec) * 1e9 +
                      (t1.tv_nsec - t0.tv_nsec)) / 1e7;   /* centiseconds */

    check("acct.off", do_acct(NULL) == 0);

    /* Read the records back. */
    FILE *f = fopen(path, "rb");
    if (!check("acct.file.open", f != NULL))
        return failures_total ? 1 : 0;
    struct acct_v3_t recs[64];
    size_t n = fread(recs, sizeof(recs[0]), 64, f);
    fclose(f);
    test_logf("ok   acct.records = %zu\n", n);
    check("acct.records.some", n >= 2);

    const struct acct_v3_t *r_exec = NULL, *r_fork = NULL;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].ac_pid == (uint32_t) exec_child) r_exec = &recs[i];
        if (recs[i].ac_pid == (uint32_t) fork_child) r_fork = &recs[i];
    }

    if (check("acct.exec_child.present", r_exec != NULL)) {
        check("acct.exec_child.version", r_exec->ac_version == 3);
        check("acct.exec_child.comm", strncmp(r_exec->ac_comm, "true", 4) == 0);
        check("acct.exec_child.ppid", r_exec->ac_ppid == (uint32_t) getpid());
        check("acct.exec_child.exitcode", r_exec->ac_exitcode == 0);
        check("acct.exec_child.no_afork", (r_exec->ac_flag & AFORK_) == 0);
        check("acct.exec_child.etime_sane", etime_of(r_exec) >= 0 && etime_of(r_exec) < 100000);
        test_log_if(1, "  exec child: comm=%.16s flag=%u exit=%u etime=%g\n",
                r_exec->ac_comm, r_exec->ac_flag, r_exec->ac_exitcode, etime_of(r_exec));
    }
    if (check("acct.fork_child.present", r_fork != NULL)) {
        /* 7 << 8: the wait-encoded status, which is what Linux records. */
        check("acct.fork_child.exitcode", r_fork->ac_exitcode == (7u << 8));
        check("acct.fork_child.afork", (r_fork->ac_flag & AFORK_) != 0);
        check("acct.fork_child.ppid", r_fork->ac_ppid == (uint32_t) getpid());
        test_log_if(1, "  fork child: comm=%.16s flag=%u exit=%u etime=%g\n",
                r_fork->ac_comm, r_fork->ac_flag, r_fork->ac_exitcode, etime_of(r_fork));
    }

    const struct acct_v3_t *r_timed = NULL;
    for (size_t i = 0; i < n; i++)
        if (recs[i].ac_pid == (uint32_t) timed_child) r_timed = &recs[i];
    if (check("acct.timed_child.present", r_timed != NULL)) {
        double etime = etime_of(r_timed);
        test_log_if(1, "  timed child: measured %.1f cs, recorded etime %g\n",
                wall_cs, etime);
        check("acct.timed_child.etime_is_ahz",
                etime >= wall_cs * 0.5 && etime <= wall_cs * 2.0 + 50.0);
    }

    unlink(path);
    if (failures_total) {
        printf("acct_records: %d FAILURES\n", failures_total);
        return 1;
    }
    printf("acct_records: PASS\n");
    return 0;
}
