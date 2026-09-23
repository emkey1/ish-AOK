/*
 * exec_cred_ids.c -- what an exec leaves in the saved and filesystem ids, and
 * in AT_SECURE, set-id file or not.
 *
 * Two defects in kernel/exec.c.
 *
 * Every exec leaves the saved and filesystem ids equal to the effective ones:
 * POSIX's "the effective user ID of the new process image shall be saved (as
 * the saved set-user-ID)", and Linux's cap_bprm_creds_from_file, "new->suid =
 * new->fsuid = new->euid" and the same for the gids. AOK did it only for a
 * set-id file. So a process that had lowered its effective uid while keeping
 * root as its saved one -- what a setuid-root program does with
 * seteuid(getuid()) before running a helper -- handed the saved root to the
 * program it exec'd, which could take root back with seteuid(0). And an id
 * set with setfsuid or setfsgid outlived the program that set it.
 *
 * AT_SECURE is Linux's secureexec: set when the exec leaves the effective ids
 * other than the real ones, however they got that way. AOK set it when the file
 * had a set-id bit, which is wrong both ways: a root running a setuid-root
 * program, or anyone running a set-id program of their own, was told it had
 * gained privilege, and a process already running with an effective uid other
 * than its real one -- a setuid program's child -- was told it had not, so
 * its loader honoured LD_PRELOAD from an environment its real user controls.
 *
 * Each case runs in a child of its own, which changes its credentials and then
 * execs this program, which reports what it runs with through a pipe: all six
 * ids, AT_SECURE, and whether seteuid(0) works. The cases that change
 * credentials first need root, and are skipped without it; the set-id file
 * cases run as anyone (the one of another group needs a supplementary group
 * to give the file, as an unprivileged caller).
 *
 * Measured before the fix on alpine-amd64-test and devuan-amd64-test, as
 * root: after setresuid(1000, 1000, 0) the exec'd program had saved uid 0 and
 * seteuid(0) succeeded; after setfsuid(1000) its filesystem uid was 1000; and
 * AT_SECURE was 0 after setresuid(-1, 1000, -1) and 1 for root's own
 * setuid-root binary. Linux 6.12: saved uid 1000 and EPERM, filesystem uid 0,
 * AT_SECURE 1 and 0.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, as uid 1000 and as
 * root).
 *
 * Exits 0 and prints "exec_cred_ids: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

/* Ids for the privileged cases: no /etc/passwd entry needed. */
#define OTHER_UID 1000
#define OTHER_GID 1234

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok || test_verbose) {
        printf("%s ", ok ? "ok" : "FAIL");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
    if (!ok)
        failures_total++;
}

/* What the exec'd program runs with. */
struct ids {
    unsigned ruid, euid, suid, fsuid;
    unsigned rgid, egid, sgid, fsgid;
    unsigned long secure;
    int seteuid_root;       /* 0 if seteuid(0) worked, else its errno */
};

/* --image FD: report this image's credentials on FD. */
static int image(char **argv) {
    struct ids now;
    uid_t r, e, s;
    gid_t rg, eg, sg;
    getresuid(&r, &e, &s);
    getresgid(&rg, &eg, &sg);
    now.ruid = r;
    now.euid = e;
    now.suid = s;
    now.fsuid = (unsigned) setfsuid((uid_t) -1);
    now.rgid = rg;
    now.egid = eg;
    now.sgid = sg;
    now.fsgid = (unsigned) setfsgid((gid_t) -1);
    now.secure = getauxval(AT_SECURE);
    /* Asked last: it changes what the fields above describe. */
    now.seteuid_root = seteuid(0) == 0 ? 0 : errno;
    int fd = atoi(argv[2]);
    return write(fd, &now, sizeof(now)) == (ssize_t) sizeof(now) ? 0 : 1;
}

static char self_path[64];

/* ---- the cases ----------------------------------------------------------- */

enum file_kind {
    PLAIN,          /* this binary as it is */
    SETUID_OWN,     /* a copy, set-user-ID, owned by us */
    SETUID_OTHER,   /* a copy, set-user-ID, owned by OTHER_UID */
    SETGID_OTHER,   /* a copy, set-group-ID, of a group that is not our egid */
};

struct exec_case {
    const char *label;
    bool needs_root;
    int (*change)(void);    /* before the exec, or NULL */
    enum file_kind file;
    /* What the new image runs with, as offsets from the caller's starting
     * ids: 'o' OTHER_UID/OTHER_GID, 's' the start value, 'g' the other group. */
    char ruid, euid, suid, fsuid, rgid, egid, sgid, fsgid;
    unsigned long secure;
    bool root_regained;     /* whether seteuid(0) is expected to work */
};

static int fsuid_other(void) {
    setfsuid(OTHER_UID);
    return setfsuid((uid_t) -1) == OTHER_UID ? 0 : -1;
}
static int fsgid_other(void) {
    setfsgid(OTHER_GID);
    return setfsgid((gid_t) -1) == OTHER_GID ? 0 : -1;
}
static int euid_other(void) { return setresuid((uid_t) -1, OTHER_UID, (uid_t) -1); }
static int suid_other(void) { return setresuid((uid_t) -1, (uid_t) -1, OTHER_UID); }
static int egid_other(void) { return setresgid((gid_t) -1, OTHER_GID, (gid_t) -1); }
static int sgid_other(void) { return setresgid((gid_t) -1, (gid_t) -1, OTHER_GID); }
/* A setuid-root program's temporary drop: real and effective lowered, the
 * saved id still root. */
static int drop_keep_saved(void) { return setresuid(OTHER_UID, OTHER_UID, 0); }

static const struct exec_case cases[] = {
    {"a plain exec", false, NULL, PLAIN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"a set-user-ID binary it owns", false, NULL, SETUID_OWN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"a set-group-ID binary of another group", false, NULL, SETGID_OTHER,
     's', 's', 's', 's', 's', 'g', 'g', 'g', 1, true},
    {"a set-user-ID binary of another user", true, NULL, SETUID_OTHER,
     's', 'o', 'o', 'o', 's', 's', 's', 's', 1, true},
    {"after setfsuid", true, fsuid_other, PLAIN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"after an effective uid change", true, euid_other, PLAIN,
     's', 'o', 'o', 'o', 's', 's', 's', 's', 1, true},
    {"after a saved uid change", true, suid_other, PLAIN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"after setfsgid", true, fsgid_other, PLAIN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"after an effective gid change", true, egid_other, PLAIN,
     's', 's', 's', 's', 's', 'o', 'o', 'o', 1, true},
    {"after a saved gid change", true, sgid_other, PLAIN,
     's', 's', 's', 's', 's', 's', 's', 's', 0, true},
    {"after dropping to uid 1000 with root saved", true, drop_keep_saved, PLAIN,
     'o', 'o', 'o', 'o', 's', 's', 's', 's', 0, false},
};

/* A group we may give a file that is not our effective one, or -1. */
static gid_t other_group(void) {
    if (geteuid() == 0)
        return OTHER_GID;
    gid_t groups[64];
    int n = getgroups(64, groups);
    for (int i = 0; i < n; i++) {
        if (groups[i] != getegid())
            return groups[i];
    }
    return (gid_t) -1;
}

/* A copy of this binary owned by uid:gid with `mode`, at `path`. */
static bool copy_self(const char *path, uid_t uid, gid_t gid, mode_t mode) {
    unlink(path);
    int in = open(self_path, O_RDONLY);
    int out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    bool ok = in >= 0 && out >= 0;
    char buf[65536];
    ssize_t n;
    while (ok && (n = read(in, buf, sizeof(buf))) != 0) {
        if (n < 0 || write(out, buf, (size_t) n) != n)
            ok = false;
    }
    if (in >= 0)
        close(in);
    if (out >= 0 && close(out) != 0)
        ok = false;
    /* chown first: it clears the set-id bits. */
    if (ok && (chown(path, uid, gid) != 0 || chmod(path, mode) != 0))
        ok = false;
    if (!ok) {
        check(0, "copying %s to %s: %s", self_path, path, strerror(errno));
        unlink(path);
    }
    return ok;
}

static unsigned want_id(char kind, unsigned start, unsigned other, unsigned group) {
    return kind == 'o' ? other : kind == 'g' ? group : start;
}

static void run_case(const struct exec_case *ec) {
    if (ec->needs_root && geteuid() != 0) {
        test_logf("SKIP %s: needs root\n", ec->label);
        return;
    }
    gid_t group = ec->file == SETGID_OTHER ? other_group() : getegid();
    if (group == (gid_t) -1) {
        test_logf("SKIP %s: no other group to give the file\n", ec->label);
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/tmp/exec_cred_ids.%d.bin", (int) getpid());
    const char *file = self_path;
    if (ec->file != PLAIN) {
        uid_t owner = ec->file == SETUID_OTHER ? OTHER_UID : geteuid();
        mode_t mode = ec->file == SETGID_OTHER ? 02755 : 04755;
        if (!copy_self(path, owner, group, mode))
            return;
        file = path;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        check(0, "%s: pipe: %s", ec->label, strerror(errno));
        return;
    }
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        if (ec->change != NULL && ec->change() != 0)
            _exit(3);
        char fd[16];
        snprintf(fd, sizeof(fd), "%d", fds[1]);
        execl(file, file, "--image", fd, (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    struct ids got;
    ssize_t n = read(fds[0], &got, sizeof(got));
    close(fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        continue;
    if (ec->file != PLAIN)
        unlink(path);
    if (n != (ssize_t) sizeof(got)) {
        check(0, "%s: no report (child status %#x)", ec->label, status);
        return;
    }

    unsigned uid = getuid(), gid = getgid();
    struct ids want = {
        .ruid = want_id(ec->ruid, uid, OTHER_UID, group),
        .euid = want_id(ec->euid, uid, OTHER_UID, group),
        .suid = want_id(ec->suid, uid, OTHER_UID, group),
        .fsuid = want_id(ec->fsuid, uid, OTHER_UID, group),
        .rgid = want_id(ec->rgid, gid, OTHER_GID, group),
        .egid = want_id(ec->egid, gid, OTHER_GID, group),
        .sgid = want_id(ec->sgid, gid, OTHER_GID, group),
        .fsgid = want_id(ec->fsgid, gid, OTHER_GID, group),
        .secure = ec->secure,
    };
    check(got.ruid == want.ruid && got.euid == want.euid && got.suid == want.suid &&
                  got.fsuid == want.fsuid,
          "%s: uids %u/%u/%u fs %u, want %u/%u/%u fs %u", ec->label,
          got.ruid, got.euid, got.suid, got.fsuid, want.ruid, want.euid, want.suid, want.fsuid);
    check(got.rgid == want.rgid && got.egid == want.egid && got.sgid == want.sgid &&
                  got.fsgid == want.fsgid,
          "%s: gids %u/%u/%u fs %u, want %u/%u/%u fs %u", ec->label,
          got.rgid, got.egid, got.sgid, got.fsgid, want.rgid, want.egid, want.sgid, want.fsgid);
    check(got.secure == want.secure, "%s: AT_SECURE %lu, want %lu", ec->label,
          got.secure, want.secure);
    /* Only asked where the answer does not follow from the ids above: an
     * unprivileged caller's own seteuid(0) is refused, and root's allowed. */
    if (ec->needs_root)
        check((got.seteuid_root == 0) == ec->root_regained,
              "%s: seteuid(0) in the new image %s, want it %s", ec->label,
              got.seteuid_root == 0 ? "works" : strerror(got.seteuid_root),
              ec->root_regained ? "to work" : "refused");
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--image") == 0)
        return image(argv);
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(60));

    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0 || n >= (ssize_t) sizeof(self_path) - 1) {
        printf("exec_cred_ids: FAIL cannot read /proc/self/exe\n");
        return 1;
    }
    self_path[n] = '\0';

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    return finish_suite("exec_cred_ids");
}
