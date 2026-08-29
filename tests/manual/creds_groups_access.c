// Supplementary groups past the old 32 cap, and the one execute rule root
// does not get to bypass.
//
// The group array used to be a fixed 32-entry array inline in every task, so
// setgroups(33) was EINVAL where Linux takes up to NGROUPS_MAX. It is now
// heap-allocated, which means it is also an OWNED pointer crossing a fork that
// copies the task struct shallowly -- so the fork half of this test is a
// lifetime check, not a semantics one: a shared allocation would be freed
// twice and corrupt the heap.
//
// access(X_OK) as root: Linux's generic_permission does NOT let
// CAP_DAC_OVERRIDE conjure execute permission on a file with no execute bit
// set anywhere. Without that rule every file in the tree looks executable to
// root and `test -x` believes it. Measured as root on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

#define BIG 600

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-38s got=%ld want=%ld\n", label, got, want);
}

static void groups_beyond_32(void) {
    static gid_t set[BIG], back[BIG];
    for (int i = 0; i < BIG; i++)
        set[i] = 5000 + i;

    errno = 0;
    if (setgroups(8, set) < 0) {
        // Unprivileged: nothing here is testable.
        printf("  setgroups: SKIP (%s)\n", strerror(errno));
        return;
    }
    check("setgroups(8) then getgroups", getgroups(BIG, back), 8);

    errno = 0;
    int r = setgroups(33, set);
    check("setgroups(33) rc", r, 0);
    if (r == 0)
        check("getgroups after 33", getgroups(BIG, back), 33);

    errno = 0;
    r = setgroups(BIG, set);
    check("setgroups(600) rc", r, 0);
    if (r == 0) {
        check("getgroups after 600", getgroups(BIG, back), BIG);
        int mismatched = 0;
        for (int i = 0; i < BIG; i++)
            if (back[i] != set[i])
                mismatched++;
        check("all 600 gids round-tripped", mismatched, 0);
    }

    // A short buffer is EINVAL, but getgroups(0, NULL) still reports the count.
    check("getgroups(0, NULL)", getgroups(0, NULL), BIG);
    errno = 0;
    check("getgroups(BIG-1) rc", getgroups(BIG - 1, back), -1);
    check("getgroups(BIG-1) errno", errno, EINVAL);

    // The lifetime half: children inherit the array, and each must free its
    // own copy. A shared allocation double-frees as they exit.
    for (int i = 0; i < 8; i++) {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            static gid_t mine[BIG];
            int n = getgroups(BIG, mine);
            int bad = n != BIG;
            for (int j = 0; !bad && j < BIG; j++)
                if (mine[j] != (gid_t) (5000 + j))
                    bad = 1;
            // Replace our own set too, so the child frees the inherited one.
            setgroups(4, mine);
            _exit(bad);
        }
        int st = 0;
        while (waitpid(c, &st, 0) < 0 && errno == EINTR)
            continue;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            failf("forked child sees all 600 groups", (uint64_t) st, (uint64_t) i, 0, 0, 0, 0);
    }
    test_logf("  %-38s 8 children ok\n", "fork inherits the group array");

    setgroups(0, set);
    check("setgroups(0) then getgroups", getgroups(BIG, back), 0);
}

static void access_x_ok(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/creds_x.%d", (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("  access(X_OK): SKIP (cannot create %s)\n", path);
        return;
    }
    close(fd);

    // No execute bit anywhere: denied even for root.
    errno = 0;
    check("mode 0600 X_OK rc", access(path, X_OK), -1);
    check("mode 0600 X_OK errno", errno, EACCES);
    // R_OK/W_OK are unaffected -- root still overrides those.
    if (geteuid() == 0) {
        chmod(path, 0000);
        errno = 0;
        check("root: mode 0000 R_OK rc", access(path, R_OK), 0);
        errno = 0;
        check("root: mode 0000 W_OK rc", access(path, W_OK), 0);
        errno = 0;
        check("root: mode 0000 X_OK rc", access(path, X_OK), -1);
        check("root: mode 0000 X_OK errno", errno, EACCES);
        // Any single execute bit is enough for root.
        chmod(path, 0601);
        errno = 0;
        check("root: mode 0601 X_OK rc", access(path, X_OK), 0);
    }
    chmod(path, 0700);
    errno = 0;
    check("mode 0700 X_OK rc", access(path, X_OK), 0);

    // A directory keeps root's search permission regardless of its bits.
    char dir[64];
    snprintf(dir, sizeof dir, "/tmp/creds_d.%d", (int) getpid());
    if (mkdir(dir, 0700) == 0) {
        if (geteuid() == 0) {
            chmod(dir, 0000);
            errno = 0;
            check("root: mode 0000 dir X_OK rc", access(dir, X_OK), 0);
        }
        chmod(dir, 0700);
        rmdir(dir);
    }
    unlink(path);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    groups_beyond_32();
    access_x_ok();
    return finish_suite("creds_groups_access");
}
