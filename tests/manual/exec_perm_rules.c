// What execve accepts, and what a program keeps across one.
//
//   execve did not check execute permission for the caller at all. It opened
//   the file O_RDONLY and let the ordinary read check decide, which is a
//   different question: a 0644 file the caller could read was executed, and a
//   0711 file -- execute granted, read denied -- was refused. Both answers are
//   backwards. Linux resolves execute permission against the caller's
//   credentials before it ever opens the file, and then opens it with the
//   permission decision already made.
//
//   Anything that was not a regular file took whatever path its type led to.
//   A directory reached the ELF loader and came back EIO; a FIFO reached
//   open(2), which BLOCKS until a writer arrives, so execing one hung the task
//   forever with no way to tell it apart from a slow program. Linux checks the
//   type first and answers EACCES for both.
//
//   MS_NOEXEC was recorded on the mount and never consulted, so a filesystem
//   mounted noexec ran programs.
//
//   Capabilities survived an ordinary exec. On Linux a process with no file
//   capabilities and no privilege gets an empty permitted and effective set
//   across execve -- that is what makes dropping privilege before exec mean
//   something. AOK carried the full set through, so a program that dropped to
//   an unprivileged uid and exec'd still handed its child every capability.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// The capability syscalls, spelled out: libcap is not on every root this runs
// on, and the ABI is one header plus three words per set.
struct cap_hdr { unsigned version; int pid; };
struct cap_data { unsigned effective, permitted, inheritable; };
#define CAP_VER 0x20080522

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static char base[128];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-6ld want=%ld\n", label, got, want);
}

// Run a block as an unprivileged user, folding its failures back into ours.
// Every permission answer below is uid-dependent -- as root the execute check
// short-circuits and the whole class under test is unreachable -- so the
// interesting arms all run here.
#define AS_USER(...) do {                                                      \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {          \
                printf("FAIL could not drop to uid %d: %s\n",                  \
                       UNPRIV_UID, strerror(errno));                           \
                fflush(NULL);                                                  \
                _exit(1);                                                      \
            }                                                                  \
            failures_total = 0;                                                \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

// execve in a child, and report the errno it failed with -- or 0 if it
// succeeded, which we learn from the exit status the exec'd program produces.
// A hang is the failure mode this is guarding against (execing a FIFO used to
// block forever), so the child is given its own alarm and a hang is reported
// as its own value rather than stalling the suite.
#define EXEC_HUNG (-999)
static int exec_errno(const char *path) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(pipefd[0]);
        alarm(10);
        // argv[0] is "true", not the path: the staged program is a copy of
        // /bin/true, which on a busybox root dispatches on argv[0] and would
        // otherwise print "applet not found" for every fixture name. The extra
        // arguments make the /bin/sh fallback exit instead of reading stdin.
        char *const av[] = { (char *) "true", (char *) "-c", (char *) "exit 0", NULL };
        char *const ev[] = { NULL };
        errno = 0;
        execve(path, av, ev);
        int er = errno;
        ssize_t ignored = write(pipefd[1], &er, sizeof er);
        (void) ignored;
        _exit(90);
    }
    close(pipefd[1]);
    int er = 0;
    ssize_t got = read(pipefd[0], &er, sizeof er);
    close(pipefd[0]);
    int st;
    if (waitpid(c, &st, 0) != c)
        return -1;
    if (WIFSIGNALED(st))
        return WTERMSIG(st) == SIGALRM ? EXEC_HUNG : -1;
    // Nothing came back down the pipe: the exec worked and the new program ran
    // to completion in place of the writer.
    return got == (ssize_t) sizeof er ? er : 0;
}

static int write_file(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (content != NULL && write(fd, content, strlen(content)) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return chmod(path, mode);
}

static void p(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", base, leaf);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (geteuid() != 0) {
        printf("exec_perm_rules: SKIP (needs root to build the fixtures and to "
               "drop privilege)\n");
        return 0;
    }

    snprintf(base, sizeof base, "/tmp/execperm-%d", (int) getpid());
    if (mkdir(base, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", base, strerror(errno));
        return finish_suite("exec_perm_rules");
    }

    // A real program to copy around. /bin/sh is on every root this runs on and
    // is what the shell would have exec'd anyway.
    char runnable[192], noexec[192], execonly[192], fifo[192], dir[192], script[192];
    p(runnable, sizeof runnable, "runnable");
    p(noexec, sizeof noexec, "noexec");
    p(execonly, sizeof execonly, "execonly");
    p(fifo, sizeof fifo, "fifo");
    p(dir, sizeof dir, "dir");
    p(script, sizeof script, "script.sh");
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cp /bin/true '%s' 2>/dev/null || cp /bin/sh '%s'",
                 runnable, runnable);
        if (system(cmd) != 0) {
            printf("FAIL could not stage a runnable program\n");
            return finish_suite("exec_perm_rules");
        }
    }
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cp '%s' '%s' && cp '%s' '%s'",
                 runnable, noexec, runnable, execonly);
        if (system(cmd) != 0)
            failures_total++;
    }
    ck("staged program is executable", chmod(runnable, 0755), 0);
    // Readable by everyone, executable by nobody.
    ck("noexec fixture is 0644", chmod(noexec, 0644), 0);
    // Executable by everyone, readable by nobody: the case an implementation
    // that opens O_RDONLY and lets the read check decide gets exactly wrong.
    ck("execonly fixture is 0111", chmod(execonly, 0111), 0);
    ck("fifo", mkfifo(fifo, 0777), 0);
    ck("directory", mkdir(dir, 0755), 0);
    ck("shell script", write_file(script, "#!/bin/sh\nexit 0\n", 0755), 0);

    AS_USER({
        // ---- the type check comes first ----------------------------------
        ck("exec of a directory is EACCES", exec_errno(dir), EACCES);
        // The one that used to hang rather than answer.
        ck("exec of a FIFO is EACCES, not a hang", exec_errno(fifo), EACCES);

        // ---- and then the caller's execute permission ---------------------
        ck("exec of a 0644 file is EACCES", exec_errno(noexec), EACCES);
        ck("exec of a 0755 file works", exec_errno(runnable), 0);
        // Execute without read: the kernel may read it even though we cannot.
        ck("exec of a 0111 file works", exec_errno(execonly), 0);
        ck("exec of a #! script works", exec_errno(script), 0);

        // ...and the missing-file answer stays distinct from the refusals.
        char gone[192];
        p(gone, sizeof gone, "nothing-here");
        ck("exec of a missing file is ENOENT", exec_errno(gone), ENOENT);
    });

    // ---- an interpreter that is not executable ---------------------------
    // The #! line names a program the caller cannot execute. Linux reports
    // that as EACCES too, from the interpreter's own permission check.
    {
        char badinterp[192], interp[192];
        p(interp, sizeof interp, "interp");
        p(badinterp, sizeof badinterp, "badinterp.sh");
        char line[300];
        {
            char cmd[512];
            snprintf(cmd, sizeof cmd, "cp '%s' '%s'", runnable, interp);
            if (system(cmd) != 0)
                failures_total++;
        }
        ck("interpreter is 0644", chmod(interp, 0644), 0);
        snprintf(line, sizeof line, "#!%s\nexit 0\n", interp);
        ck("script naming it", write_file(badinterp, line, 0755), 0);
        AS_USER({
            ck("exec of a script with an unexecutable interpreter is EACCES",
               exec_errno(badinterp), EACCES);
        });
    }

    // ---- capabilities do not survive an ordinary exec ---------------------
    // The distinguishing case is the one a setuid-root helper actually
    // performs: keep capabilities across the drop to an unprivileged uid, so
    // the process is uid 1000 but still fully capable, and then exec. An
    // ordinary unprivileged process has empty sets on Linux and on AOK alike,
    // so checking one of those proves nothing either way.
    {
        char capreport[192];
        snprintf(capreport, sizeof capreport, "/tmp/execperm-caps-%d", (int) getpid());
        ck("capability report file", write_file(capreport, "", 0666), 0);
        char cmd[640];
        snprintf(cmd, sizeof cmd,
                 "grep -E '^CapPrm|^CapEff' /proc/self/status > '%s' 2>/dev/null",
                 capreport);

        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            failures_total = 0;
            struct cap_hdr h = { CAP_VER, 0 };
            struct cap_data d[2] = {{0}};
            if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0 ||
                    setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID) != 0) {
                printf("FAIL could not keep caps across the uid drop: %s\n",
                       strerror(errno));
                fflush(NULL);
                _exit(1);
            }
            // Raise effective back to permitted, which KEEPCAPS left intact --
            // exactly what a setuid-root helper does before it needs one more
            // privileged call.
            if (syscall(SYS_capget, &h, d) == 0) {
                d[0].effective = d[0].permitted;
                d[1].effective = d[1].permitted;
                if (syscall(SYS_capset, &h, d) != 0)
                    d[0].permitted = d[1].permitted = 0;
            }
            memset(d, 0, sizeof d);
            if (syscall(SYS_capget, &h, d) != 0) {
                printf("FAIL capget: %s\n", strerror(errno));
                fflush(NULL);
                _exit(1);
            }
            // The precondition. Without it the check below cannot tell a fixed
            // kernel from one that simply had nothing to drop.
            ck("unprivileged but still capable before exec",
               (d[0].permitted | d[1].permitted) != 0, 1);
            ck("  and uid really is unprivileged", getuid() == UNPRIV_UID, 1);
            if (failures_total != 0) {
                fflush(NULL);
                _exit(failures_total > 250 ? 250 : (int) failures_total);
            }
            // execv discards buffered stdout, taking the two checks above
            // with it. Flush before the image is replaced.
            fflush(NULL);
            char *const av[] = { (char *) "/bin/sh", (char *) "-c", cmd, NULL };
            execv("/bin/sh", av);
            printf("FAIL exec of /bin/sh: %s\n", strerror(errno));
            fflush(NULL);
            _exit(1);
        }
        int st;
        ck("capability reporter ran", waitpid(c, &st, 0) == c && WIFEXITED(st), 1);
        if (WIFEXITED(st))
            failures_total += (unsigned) WEXITSTATUS(st);

        int fd = open(capreport, O_RDONLY);
        if (fd < 0) {
            printf("FAIL capability report unreadable: %s\n", strerror(errno));
            failures_total++;
        } else {
            char buf[512] = {0};
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0) {
                // No Cap lines at all means procfs does not publish them here.
                // Say so rather than counting the absence as a pass.
                test_logf("  %-56s (procfs has no Cap lines)\n", "capability check");
            } else {
                // Every hex digit after the colon must be 0 on both lines.
                int nonzero = 0;
                for (char *t = buf; *t != '\0'; t++) {
                    if (*t != ':')
                        continue;
                    for (t++; *t != '\0' && *t != '\n'; t++) {
                        if (*t != ' ' && *t != '\t' && *t != '0')
                            nonzero = 1;
                    }
                    if (*t == '\0')
                        break;
                }
                ck("permitted and effective caps are empty after exec", nonzero, 0);
            }
        }
        unlink(capreport);
    }

    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
        if (system(cmd) < 0)
            failures_total++;
    }
    return finish_suite("exec_perm_rules");
}
