// /proc/ish/arch: which architecture every process runs, readable by anyone.
//
// ktop's ARCH column read the ELF header behind /proc/<pid>/exe. Since the
// 2026-09-24 hardening another user's exe is refused, as on Linux, so a normal
// user saw "?" beside every root process. iSH-AOK now publishes the answer in
// its own /proc/ish/arch: "PID ARCH", then "<pid> <machine>" per live process,
// machine being what uname(2) says inside it, or "native" for host code.
//
// Asserted here:
//   - the header, this process listed with its own uname machine, pid 1 listed;
//   - an unprivileged reader sees a root process listed, while that process's
//     exe stays EACCES to it (the reason the table exists);
//   - a program running natively from /AOK/native is listed as "native".
//
// iSH-AOK only: with no /proc/ish at all (real Linux) it skips; with /proc/ish
// but no arch file (an iSH-AOK from before the table) it FAILS.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 2020
#define UNPRIV_GID 2121

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

// The machine the table lists for `pid`, or NULL when it is not listed.
static const char *listed_arch(pid_t pid) {
    static char arch[64];
    FILE *f = fopen("/proc/ish/arch", "r");
    if (f == NULL)
        return NULL;
    char line[128];
    const char *found = NULL;
    while (fgets(line, sizeof(line), f) != NULL) {
        long p;
        if (sscanf(line, "%ld %63s", &p, arch) == 2 && p == (long) pid) {
            found = arch;
            break;
        }
    }
    fclose(f);
    return found;
}

static int header_ok(void) {
    FILE *f = fopen("/proc/ish/arch", "r");
    if (f == NULL)
        return 0;
    char line[64] = {0};
    int ok = fgets(line, sizeof(line), f) != NULL && strcmp(line, "PID ARCH\n") == 0;
    fclose(f);
    return ok;
}

static int drop_privileges(void) {
    gid_t g = UNPRIV_GID;
    return setgroups(1, &g) == 0 && setresgid(UNPRIV_GID, UNPRIV_GID, UNPRIV_GID) == 0 &&
           setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID) == 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    struct stat st;
    if (stat("/proc/ish", &st) != 0) {
        printf("proc_ish_arch: SKIP (no /proc/ish, not iSH-AOK)\n");
        return 0;
    }
    check("/proc/ish/arch exists", stat("/proc/ish/arch", &st) == 0, errno, 0);
    check("it is readable by everyone (0444)",
          (st.st_mode & 0444) == 0444, (long) (st.st_mode & 0777), 0444);
    check("header is \"PID ARCH\"", header_ok(), 0, 1);

    struct utsname u;
    uname(&u);
    const char *mine = listed_arch(getpid());
    check("this process is listed", mine != NULL, 0, 1);
    if (mine != NULL) {
        int same = strcmp(mine, u.machine) == 0;
        if (!same)
            test_logf("    listed \"%s\", uname says \"%s\"\n", mine, u.machine);
        check("...with its own uname machine", same, 0, 1);
    }
    check("pid 1 is listed", listed_arch(1) != NULL, 0, 1);

    // An unprivileged reader and a root process. Root sets both up; otherwise
    // pid 1 is the root process and this process is already unprivileged.
    pid_t victim = 1;
    int as_root = geteuid() == 0;
    if (as_root) {
        victim = fork();
        if (victim == 0) {
            pause();
            _exit(0);
        }
    }
    int pfd[2];
    if (pipe(pfd) != 0)
        failf("pipe", errno, 0, 0, 0, 0, 0);
    pid_t prober = fork();
    if (prober == 0) {
        close(pfd[0]);
        int result[3] = {0, 0, 0};   // dropped, listed, exe errno
        result[0] = !as_root || drop_privileges();
        const char *arch = listed_arch(victim);
        result[1] = arch != NULL && strcmp(arch, u.machine) == 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/exe", (int) victim);
        int fd = open(path, O_RDONLY);
        result[2] = fd < 0 ? errno : 0;
        if (fd >= 0)
            close(fd);
        if (write(pfd[1], result, sizeof(result)) != (ssize_t) sizeof(result))
            _exit(2);
        _exit(0);
    }
    close(pfd[1]);
    int result[3] = {0, 0, -1};
    ssize_t n = read(pfd[0], result, sizeof(result));
    close(pfd[0]);
    waitpid(prober, NULL, 0);
    check("prober ran unprivileged", n == (ssize_t) sizeof(result) && result[0], result[0], 1);
    check("unprivileged reader sees the root process listed", result[1], result[1], 1);
    // Positive control for WHY: the ELF header route is closed to this reader.
    check("...while its exe is EACCES to that reader", result[2] == EACCES, result[2], EACCES);
    if (as_root) {
        kill(victim, SIGKILL);
        waitpid(victim, NULL, 0);
    }

    // A program running as host code.
    if (access("/AOK/native/dash", X_OK) == 0) {
        pid_t nat = fork();
        if (nat == 0) {
            execl("/AOK/native/dash", "dash", "-c", "sleep 3; :", (char *) NULL);
            _exit(127);
        }
        const char *arch = NULL;
        for (int i = 0; i < 40; i++) {   // up to 2 s for the exec to land
            usleep(50000);
            arch = listed_arch(nat);
            if (arch != NULL && strcmp(arch, "native") == 0)
                break;
        }
        check("a native program is listed as \"native\"",
              arch != NULL && strcmp(arch, "native") == 0, 0, 1);
        kill(nat, SIGKILL);
        waitpid(nat, NULL, 0);
    } else {
        test_logf("  (no /AOK/native/dash; native leg skipped)\n");
    }
    return finish_suite("proc_ish_arch");
}
