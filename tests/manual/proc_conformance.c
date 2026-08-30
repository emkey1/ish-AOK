// /proc facts that ordinary tools read.
//
// Five things AOK got wrong, all of them visible to something a user runs:
//
//   /proc/meminfo printed bytes/1000 as "kB". A kB there is 1024 bytes -- Linux
//   prints pages << (PAGE_SHIFT - 10) -- so every figure was overstated by 2.4%
//   and MemTotal contradicted the guest's own sysinfo(2).
//
//   /proc/stat's "processes" is a cumulative fork counter that only grows. AOK
//   reported the current live-task count, so anything deriving a fork rate from
//   it (vmstat, sar, monitoring agents) saw a flat line forever.
//
//   An unreaped zombie had no /proc entry at all: absent from readdir(/proc)
//   and ENOENT for /proc/<pid>/*. That is where ps gets the "Z" it shows, and
//   the only way a monitor notices something died and was never reaped.
//
//   /proc/self/oom_score_adj was mode 0444 though its write handler worked, so
//   a process that checked the mode before lowering its own OOM score gave up
//   without trying. /proc/sys/kernel/hostname was 0444 with no write handler.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-46s got=%ld want=%ld\n", label, got, want);
}
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
static long mode_of(const char *p) {
    struct stat st;
    return stat(p, &st) < 0 ? -1 : (long) (st.st_mode & 07777);
}
static long read_field(const char *file, const char *key) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return -1;
    char line[512];
    long v = -1;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, strlen(key)) == 0) {
            const char *p = line + strlen(key);
            while (*p == ' ' || *p == '\t' || *p == ':')
                p++;
            v = strtol(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    TEST_SKIP_IF_FOREIGN_PROC("proc_conformance");

    // A process can adjust its own OOM score.
    check("oom_score_adj is 0644", mode_of("/proc/self/oom_score_adj"), 0644);
    {
        int fd = open("/proc/self/oom_score_adj", O_WRONLY);
        check("oom_score_adj opens for write", fd >= 0, 1);
        if (fd >= 0) {
            check("oom_score_adj write accepted", write(fd, "100\n", 4) > 0, 1);
            close(fd);
            char buf[32] = {0};
            fd = open("/proc/self/oom_score_adj", O_RDONLY);
            if (fd >= 0) { if (read(fd, buf, sizeof buf - 1) < 0) {} close(fd); }
            check("oom_score_adj reads back", atol(buf), 100);
        }
    }

    check("kernel/hostname is 0644", mode_of("/proc/sys/kernel/hostname"), 0644);

    // meminfo's kB is 1024 bytes, and must agree with sysinfo(2).
    {
        struct sysinfo si;
        long meminfo_kb = read_field("/proc/meminfo", "MemTotal");
        if (sysinfo(&si) == 0 && meminfo_kb > 0) {
            unsigned long long sysinfo_kb =
                ((unsigned long long) si.totalram * si.mem_unit) / 1024ULL;
            double ratio = (double) meminfo_kb / (double) sysinfo_kb;
            test_logf("  MemTotal=%ldkB sysinfo=%llukB ratio=%.4f\n",
                      meminfo_kb, sysinfo_kb, ratio);
            check("MemTotal agrees with sysinfo within 1%",
                  ratio > 0.99 && ratio < 1.01, 1);
        } else {
            printf("  meminfo: SKIP (unreadable)\n");
        }
    }

    // "processes" counts forks and only grows.
    {
        long before = read_field("/proc/stat", "processes");
        for (int i = 0; i < 5; i++) {
            fflush(NULL);
            pid_t c = fork();
            if (c == 0) _exit(0);
            int st;
            while (waitpid(c, &st, 0) < 0 && errno == EINTR)
                continue;
        }
        long after = read_field("/proc/stat", "processes");
        test_logf("  /proc/stat processes: %ld -> %ld over 5 forks\n", before, after);
        check("processes grew by at least 5", after - before >= 5, 1);
    }

    // A zombie is a process: it has a /proc entry, reports Z, and is listed.
    {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) _exit(0);
        if (c < 0) {
            printf("  zombie: SKIP (fork failed)\n");
        } else {
            nap(400);                    // exited, deliberately not reaped yet
            char path[64];
            snprintf(path, sizeof path, "/proc/%d/stat", (int) c);
            struct stat st;
            check("zombie has a /proc entry", stat(path, &st) == 0, 1);

            char state = '?';
            FILE *f = fopen(path, "r");
            if (f != NULL) {
                char buf[512] = {0};
                if (fgets(buf, sizeof buf, f) != NULL) {
                    // comm is parenthesised and may contain spaces, so scan
                    // from the LAST ')': state is two characters after it.
                    char *rp = strrchr(buf, ')');
                    if (rp != NULL && rp[1] == ' ')
                        state = rp[2];
                }
                fclose(f);
            }
            check("zombie stat reports state Z", state == 'Z', 1);

            int listed = 0;
            DIR *d = opendir("/proc");
            if (d != NULL) {
                char want[16];
                snprintf(want, sizeof want, "%d", (int) c);
                struct dirent *e;
                while ((e = readdir(d)) != NULL)
                    if (strcmp(e->d_name, want) == 0) { listed = 1; break; }
                closedir(d);
            }
            check("zombie is listed in readdir(/proc)", listed, 1);

            int stt;
            while (waitpid(c, &stt, 0) < 0 && errno == EINTR)
                continue;
        }
    }

    return finish_suite("proc_conformance");
}
