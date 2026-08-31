// Reading the mount table while the mount table changes.
//
//   /proc/mounts, /proc/self/mountinfo and contains_mount_point (the check
//   rename and rmdir make before touching a directory) all walked the global
//   mounts list with no lock, while fs/mount.c mutates that list under
//   mounts_lock. A concurrent mount or umount could free the entry the walk
//   was standing on: a use-after-free reachable from an ordinary `cat
//   /proc/mounts`, and systemd's libmount monitor re-reads mountinfo on every
//   single mount change, which is exactly when the list is being edited.
//
//   A race test is probabilistic by nature. This one is a stress loop: it is
//   here to crash a kernel that has the bug, not to prove one that does not.
//   What it can assert deterministically is that the output stays well-formed
//   -- a torn walk that survives still hands back a truncated or duplicated
//   line.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static volatile int stop_now;
static volatile unsigned reads_done, malformed;
static char mnt_dir[64];

// Every line of /proc/mounts is "source point type opts freq passno": six
// space-separated fields and nothing empty. A walk that ran off a freed entry
// and lived tends to produce a short line rather than a crash.
static int line_well_formed(const char *line) {
    int fields = 0;
    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        fields++;
        while (*p != '\0' && *p != ' ')
            p++;
    }
    return fields == 6;
}

static void *reader(void *arg) {
    const char *path = arg;
    char buf[16384];
    while (!stop_now) {
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        reads_done++;
        if (strcmp(path, "/proc/mounts") != 0)
            continue;
        char *save = NULL;
        for (char *l = strtok_r(buf, "\n", &save); l != NULL; l = strtok_r(NULL, "\n", &save))
            if (!line_well_formed(l))
                malformed++;
    }
    return NULL;
}

// rename() asks contains_mount_point about its target, which is the third
// unlocked walk.
static void *renamer(void *arg) {
    char a[96], b[96];
    snprintf(a, sizeof a, "%s-ra", mnt_dir);
    snprintf(b, sizeof b, "%s-rb", mnt_dir);
    mkdir(a, 0755);
    while (!stop_now) {
        if (rename(a, b) == 0)
            rename(b, a);
    }
    rmdir(a);
    rmdir(b);
    return arg;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (geteuid() != 0) {
        printf("mounts_list_race: SKIP (needs root to mount)\n");
        return 0;
    }

    snprintf(mnt_dir, sizeof mnt_dir, "/tmp/mntrace-%d", (int) getpid());
    // Tolerate a leftover from an earlier run that died: guest pids are small
    // and get reused, and the earlier run dying is exactly what this test
    // provokes on a kernel with the bug. A hard failure here would mask the
    // real result on the next attempt.
    ck("mount point exists", mkdir(mnt_dir, 0755) == 0 || errno == EEXIST, 1);

    pthread_t r1, r2, r3, rn;
    ck("reader on /proc/mounts", pthread_create(&r1, NULL, reader, (void *) "/proc/mounts"), 0);
    ck("reader on /proc/self/mountinfo",
       pthread_create(&r2, NULL, reader, (void *) "/proc/self/mountinfo"), 0);
    ck("second reader on /proc/mounts", pthread_create(&r3, NULL, reader, (void *) "/proc/mounts"), 0);
    ck("renamer", pthread_create(&rn, NULL, renamer, NULL), 0);

    // Churn the list under them.
    unsigned mounted = 0, failed = 0;
    for (int i = 0; i < 400; i++) {
        if (mount("none", mnt_dir, "tmpfs", 0, NULL) == 0) {
            mounted++;
            if (umount(mnt_dir) != 0)
                failed++;
        } else {
            failed++;
        }
    }
    stop_now = 1;
    pthread_join(r1, NULL);
    pthread_join(r2, NULL);
    pthread_join(r3, NULL);
    pthread_join(rn, NULL);

    test_logf("  %-56s %u\n", "mount/umount cycles completed", mounted);
    test_logf("  %-56s %u\n", "mount-table reads completed", reads_done);
    ck("every mount/umount succeeded", failed, 0);
    ck("the readers got somewhere", reads_done > 100 ? 1 : 0, 1);
    ck("no malformed /proc/mounts line", malformed, 0);
    // Surviving at all is most of the point: the bug's usual outcome is a
    // SIGSEGV in the emulator, which the harness reports as the test dying on
    // a signal rather than as a failure here.
    rmdir(mnt_dir);
    return finish_suite("mounts_list_race");
}
