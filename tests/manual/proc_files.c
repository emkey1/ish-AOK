// procfs files that tools read, and two fields inside /proc/<pid>/status.
//
//   Every procfs inode reported nlink 0 -- what a DELETED inode looks like.
//   `find` and `du` read it, and so does anything checking "is this still
//   there". A file or symlink has one link; a directory has at least two.
//
//   /proc/<pid>/status had no Umask line at all, and it is the only place a
//   process's umask is observable from outside. SigIgn and SigCgt were
//   hardcoded zero, telling every reader the process ignored nothing and
//   caught nothing -- the two fields a debugger or supervisor reads to find
//   out which signals will actually reach it.
//
//   /proc/devices, /proc/partitions, /proc/swaps, /proc/modules,
//   /proc/cgroups, /proc/interrupts, /proc/thread-self, /proc/sysvipc/* and
//   /proc/sys/fs/inotify/* did not exist. Absence is what breaks a reader:
//   lsmod fails outright without /proc/modules, `ipcs -s` reports an empty
//   system without /proc/sysvipc/sem no matter how many semaphore sets there
//   are, and inotifywait sizes its watch set from max_user_watches. Where
//   there is genuinely nothing to report -- no modules, no swap, no block
//   devices, no interrupt controller behind a usermode kernel -- the file is
//   its header alone, which is exactly what Linux shows on a system with
//   none.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

// Whole file into buf; returns the byte count, or -1.
static long slurp(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t total = 0;
    for (;;) {
        ssize_t r = read(fd, buf + total, n - 1 - (size_t) total);
        if (r <= 0)
            break;
        total += r;
        if ((size_t) total >= n - 1)
            break;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

static void exists_and_reads(const char *path) {
    char buf[8192];
    char lab[128];
    snprintf(lab, sizeof lab, "%s exists", path);
    struct stat st;
    ck(lab, stat(path, &st) == 0 ? 1 : 0, 1);
    snprintf(lab, sizeof lab, "  and reads without error");
    ck(lab, slurp(path, buf, sizeof buf) >= 0 ? 1 : 0, 1);
}

static void status_field(const char *name, char *out, size_t n) {
    out[0] = '\0';
    char buf[8192];
    if (slurp("/proc/self/status", buf, sizeof buf) < 0)
        return;
    size_t namelen = strlen(name);
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, name, namelen) == 0) {
            const char *v = line + namelen;
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(out, n, "%s", v);
            return;
        }
    }
}

static void handler(int s) { (void) s; }

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- files that must exist -------------------------------------------
    exists_and_reads("/proc/devices");
    exists_and_reads("/proc/partitions");
    exists_and_reads("/proc/swaps");
    exists_and_reads("/proc/modules");
    exists_and_reads("/proc/cgroups");
    exists_and_reads("/proc/interrupts");
    exists_and_reads("/proc/sysvipc/sem");
    exists_and_reads("/proc/sysvipc/msg");
    exists_and_reads("/proc/sysvipc/shm");
    exists_and_reads("/proc/sys/fs/inotify/max_queued_events");
    exists_and_reads("/proc/sys/fs/inotify/max_user_instances");
    exists_and_reads("/proc/sys/fs/inotify/max_user_watches");

    // ...with content a reader can use, not just an empty file.
    {
        char buf[8192];
        ck("/proc/devices names the mem major",
           slurp("/proc/devices", buf, sizeof buf) > 0 && strstr(buf, "mem") != NULL ? 1 : 0, 1);
        ck("  and has a Block devices section",
           strstr(buf, "Block devices:") != NULL ? 1 : 0, 1);
        ck("/proc/swaps has its header",
           slurp("/proc/swaps", buf, sizeof buf) > 0 && strstr(buf, "Filename") != NULL ? 1 : 0, 1);
        ck("/proc/partitions has its header",
           slurp("/proc/partitions", buf, sizeof buf) > 0 && strstr(buf, "#blocks") != NULL ? 1 : 0, 1);
        ck("/proc/sysvipc/sem has its header",
           slurp("/proc/sysvipc/sem", buf, sizeof buf) > 0 && strstr(buf, "semid") != NULL ? 1 : 0, 1);
        ck("max_queued_events is a positive number",
           slurp("/proc/sys/fs/inotify/max_queued_events", buf, sizeof buf) > 0
               && atol(buf) > 0 ? 1 : 0, 1);
    }

    // ---- a live semaphore set shows up ------------------------------------
    {
        int id = semget(IPC_PRIVATE, 3, 0600 | IPC_CREAT);
        ck("create a semaphore set", id >= 0 ? 1 : 0, 1);
        if (id >= 0) {
            char buf[16384];
            long n = slurp("/proc/sysvipc/sem", buf, sizeof buf);
            // The set's id appears on a line of its own; a header-only file
            // would pass every check above and still tell ipcs nothing.
            char needle[32];
            snprintf(needle, sizeof needle, " %d ", id);
            char padded[16400];
            snprintf(padded, sizeof padded, "%s ", buf);
            ck("  and it is listed in /proc/sysvipc/sem",
               n > 0 && strstr(padded, needle) != NULL ? 1 : 0, 1);
            semctl(id, 0, IPC_RMID);
            n = slurp("/proc/sysvipc/sem", buf, sizeof buf);
            snprintf(padded, sizeof padded, "%s ", buf);
            ck("  and gone once removed",
               n > 0 && strstr(padded, needle) == NULL ? 1 : 0, 1);
        }
    }

    // ---- /proc/thread-self -------------------------------------------------
    {
        struct stat a, b;
        ck("/proc/thread-self resolves", stat("/proc/thread-self", &a) == 0 ? 1 : 0, 1);
        // Single-threaded, so it names the same process as /proc/self; the
        // point is that it names SOMETHING, and the same something.
        char mine[128], theirs[128];
        ck("read comm through /proc/self",
           slurp("/proc/self/comm", mine, sizeof mine) > 0 ? 1 : 0, 1);
        ck("read comm through /proc/thread-self",
           slurp("/proc/thread-self/comm", theirs, sizeof theirs) > 0 ? 1 : 0, 1);
        ck("  and they agree", strcmp(mine, theirs) == 0 ? 1 : 0, 1);
        (void) b;
    }

    // ---- link counts --------------------------------------------------------
    {
        struct stat st;
        ck("stat /proc", stat("/proc", &st), 0);
        // Never 0, which is what a deleted inode looks like. A directory has
        // at least . and .. -- the exact count depends on how many
        // subdirectories the kernel counts, which differs legitimately.
        ck("  /proc has at least 2 links", st.st_nlink >= 2 ? 1 : 0, 1);
        ck("stat /proc/self/stat", stat("/proc/self/stat", &st), 0);
        ck("  a procfs file has exactly 1 link", (long) st.st_nlink, 1);
        ck("stat /proc/uptime", stat("/proc/uptime", &st), 0);
        ck("  so does another", (long) st.st_nlink, 1);
        ck("stat /proc/self", stat("/proc/self", &st), 0);
        ck("  /proc/<pid> has at least 2", st.st_nlink >= 2 ? 1 : 0, 1);
    }

    // ---- status: Umask, SigIgn, SigCgt --------------------------------------
    {
        char val[64];
        mode_t old = umask(027);
        status_field("Umask:", val, sizeof val);
        ck("status has a Umask line", val[0] != '\0' ? 1 : 0, 1);
        ck("  reporting the real umask", (long) strtol(val, NULL, 8), 027);
        umask(old);

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = handler;
        ck("catch SIGUSR1", sigaction(SIGUSR1, &sa, NULL), 0);
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        ck("ignore SIGUSR2", sigaction(SIGUSR2, &sa, NULL), 0);

        // Linux's render_sigset_t prints bit (sig - 1) for signal sig.
        unsigned long long caught_bit = 1ULL << (SIGUSR1 - 1);
        unsigned long long ignored_bit = 1ULL << (SIGUSR2 - 1);
        status_field("SigCgt:", val, sizeof val);
        unsigned long long cgt = strtoull(val, NULL, 16);
        ck("SigCgt has the caught signal", (cgt & caught_bit) ? 1 : 0, 1);
        ck("  and not the ignored one", (cgt & ignored_bit) ? 1 : 0, 0);
        status_field("SigIgn:", val, sizeof val);
        unsigned long long ign = strtoull(val, NULL, 16);
        ck("SigIgn has the ignored signal", (ign & ignored_bit) ? 1 : 0, 1);
        ck("  and not the caught one", (ign & caught_bit) ? 1 : 0, 0);
        // ...and they change back when the dispositions do, so these are read
        // from the real state rather than printed once.
        signal(SIGUSR1, SIG_DFL);
        signal(SIGUSR2, SIG_DFL);
        status_field("SigCgt:", val, sizeof val);
        ck("SigCgt drops it once reset",
           (strtoull(val, NULL, 16) & caught_bit) ? 1 : 0, 0);
        status_field("SigIgn:", val, sizeof val);
        ck("SigIgn drops it too",
           (strtoull(val, NULL, 16) & ignored_bit) ? 1 : 0, 0);
    }

    // ---- /proc/<pid>/stat starttime (field 22) -----------------------------
    {
        char buf[4096];
        ck("read /proc/self/stat", slurp("/proc/self/stat", buf, sizeof buf) > 0 ? 1 : 0, 1);
        // The comm field is parenthesised and may contain spaces, so the
        // fields are counted from the LAST ')'.
        char *after = strrchr(buf, ')');
        ck("  it has a comm field", after != NULL ? 1 : 0, 1);
        unsigned long long starttime = 0;
        if (after != NULL) {
            int i = 0;
            for (char *t = strtok(after + 2, " "); t != NULL; t = strtok(NULL, " ")) {
                if (++i == 20) {   // field 3 is the first token here, so 22 is the 20th
                    starttime = strtoull(t, NULL, 10);
                    break;
                }
            }
        }
        // Hardcoded 0 before, which made every process look as old as the
        // kernel: ps -o etime, ps -o lstart and top's age column all derive
        // from this one number.
        //
        // It cannot be tested on OURSELVES. Under the CLI this suite is often
        // the first thing the guest runs, so it genuinely starts inside the
        // kernel's first clock tick and 0 is then the correct answer -- Linux
        // reports 0 for a process started at boot too, which is why pid 1
        // reads 0 there. Asserting nonzero here made the test fail whenever it
        // ran early enough, and which of those it got depended on how much
        // work the harness happened to do first.
        //
        // So wait for the clock to move and test a CHILD, which is the real
        // property anyway: a process started later must report a later start.
        char up[128];
        double uptime = 0;
        for (int i = 0; i < 300 && uptime <= 0; i++) {
            if (slurp("/proc/uptime", up, sizeof up) > 0)
                uptime = atof(up);
            if (uptime <= 0)
                usleep(10000);
        }
        ck("uptime advances past zero", uptime > 0 ? 1 : 0, 1);

        pid_t kid = fork();
        if (kid == 0) {
            sleep(3);          // stay alive long enough to be read
            _exit(0);
        }
        ck("  forked a child to time", kid > 0 ? 1 : 0, 1);
        unsigned long long kid_start = 0;
        if (kid > 0) {
            char path[64], kbuf[4096];
            snprintf(path, sizeof path, "/proc/%d/stat", (int) kid);
            if (slurp(path, kbuf, sizeof kbuf) > 0) {
                char *kafter = strrchr(kbuf, ')');
                if (kafter != NULL) {
                    int i = 0;
                    for (char *t = strtok(kafter + 2, " "); t != NULL; t = strtok(NULL, " ")) {
                        if (++i == 20) { kid_start = strtoull(t, NULL, 10); break; }
                    }
                }
            }
            kill(kid, SIGKILL);
            waitpid(kid, NULL, 0);
        }
        // The child started after the clock had moved, so its starttime must
        // have moved with it. This is what catches a hardcoded 0.
        ck("  a child started later has a nonzero starttime", kid_start > 0 ? 1 : 0, 1);
        // ...and it is a time BEFORE now, in the same ticks-since-boot space
        // /proc/uptime reports. Re-read uptime: it advanced while we forked.
        double now_up = uptime;
        if (slurp("/proc/uptime", up, sizeof up) > 0)
            now_up = atof(up);
        ck("  and is not in the future",
           (double) kid_start <= now_up * 100 + 100 ? 1 : 0, 1);
        // The parent's own starttime stays a valid reading, just possibly 0.
        ck("  self starttime is not in the future",
           (double) starttime <= now_up * 100 + 100 ? 1 : 0, 1);
    }

    return finish_suite("proc_files");
}
