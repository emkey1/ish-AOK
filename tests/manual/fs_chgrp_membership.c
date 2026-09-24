// An unprivileged owner may give a file only a group it is itself in. AOK let
// the owner pick ANY group, and honoured the setgid bit on the result -- which
// is a privilege escalation with one extra step:
//
//     cp /usr/bin/id ~/id; chgrp shadow ~/id; chmod g+s ~/id; ~/id
//     uid=1000(mke) gid=1000(mke) egid=42(shadow)
//
// and egid shadow reads /etc/shadow. Linux refuses the chgrp (chgrp_ok() in
// fs/attr.c): the owner may move a file to its fsgid or a supplementary group,
// or leave the group where it is; anything else needs CAP_CHOWN.
//
// Two neighbouring rules close the same hole from other directions, and are
// asserted here too:
//
//   - chmod g+s by an owner who is NOT in the file's group succeeds, but the
//     S_ISGID bit is dropped (setattr_prepare's "Also check the setgid bit!").
//   - a new non-directory created with S_ISGID|S_IXGRP in a setgid directory
//     whose group the creator is not in loses S_ISGID (mode_strip_sgid()); a
//     new directory there inherits the group AND the bit, as always.
//
// The unprivileged legs need the caller to be an ordinary user. As root (the
// AOK CLI) this forks and drops to UNPRIV_* in-process, so the root-only gate
// catches the class; run unprivileged (camd, the suite's user leg) it tests
// itself directly, and the legs that need a root-made fixture are skipped.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// Any nonzero ids do; nothing below needs them in /etc/passwd or /etc/group.
#define UNPRIV_UID 2020
#define UNPRIV_GID 2121
#define UNPRIV_SUPP 3131
#define FOREIGN_GID 4242

static char base[160];

static const char *P(const char *sub) {
    static char buf[8][320];
    static int i;
    char *p = buf[i++ & 7];
    snprintf(p, 320, "%s/%s", base, sub);
    return p;
}

static int in_my_groups(gid_t g) {
    if (getegid() == g)
        return 1;
    gid_t list[256];
    int n = getgroups(256, list);
    for (int i = 0; i < n; i++)
        if (list[i] == g)
            return 1;
    return 0;
}

// A gid this process is not in. The fixed one when dropped by this test;
// otherwise the first candidate the caller's credentials do not include.
static gid_t foreign_gid(void) {
    static const gid_t cand[] = { FOREIGN_GID, 0, 42, 4243, 65000 };
    for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++)
        if (!in_my_groups(cand[i]))
            return cand[i];
    return (gid_t) -1;
}

// A supplementary group this process is in that is not its egid, or -1.
static gid_t supplementary_gid(void) {
    gid_t list[256];
    int n = getgroups(256, list);
    for (int i = 0; i < n; i++)
        if (list[i] != getegid())
            return list[i];
    return (gid_t) -1;
}

static gid_t gid_of(const char *path) {
    struct stat st;
    return lstat(path, &st) < 0 ? (gid_t) -1 : st.st_gid;
}

static mode_t mode_of(const char *path) {
    struct stat st;
    return lstat(path, &st) < 0 ? (mode_t) -1 : (st.st_mode & 07777);
}

static int mkfile(const char *path, mode_t mode) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        failf("mkfile", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        return -1;
    }
    close(fd);
    if (chmod(path, mode) < 0) {
        failf("mkfile chmod", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        return -1;
    }
    return 0;
}

// One call's result and the group it left behind.
static void expect(const char *label, int rc, int e, const char *path,
                   int want_errno, gid_t want_gid) {
    gid_t after = gid_of(path);
    if ((rc < 0 ? e : 0) != want_errno || (rc < 0) != (want_errno != 0) ||
            after != want_gid)
        failf(label, (uint64_t) rc, (uint64_t) (rc < 0 ? e : 0), (uint64_t) after,
              want_errno != 0 ? (uint64_t) -1 : 0, (uint64_t) want_errno,
              (uint64_t) want_gid);
    test_logf("  %-44s rc=%d errno=%-2d gid=%u (want errno %d gid %u)\n", label,
              rc, rc < 0 ? e : 0, (unsigned) after, want_errno, (unsigned) want_gid);
}

// Every way to ask for a chgrp, as an ordinary user, of a file it owns.
static void unprivileged_legs(void) {
    gid_t mine = getegid();
    gid_t foreign = foreign_gid();
    gid_t supp = supplementary_gid();
    test_logf("-- unprivileged: uid %u egid %u, foreign gid %u, supplementary %d --\n",
              (unsigned) geteuid(), (unsigned) mine, (unsigned) foreign, (int) supp);
    if (foreign == (gid_t) -1) {
        failf("no foreign gid to test with", 0, 0, 0, 0, 0, 0);
        return;
    }

    const char *f = P("own");
    if (mkfile(f, 0755) < 0)
        return;
    int rc;

    errno = 0; rc = chown(f, (uid_t) -1, foreign);
    expect("chown(-1, foreign)", rc, errno, f, EPERM, mine);
    errno = 0; rc = chown(f, geteuid(), foreign);
    expect("chown(me, foreign)", rc, errno, f, EPERM, mine);
    errno = 0; rc = lchown(f, (uid_t) -1, foreign);
    expect("lchown(-1, foreign)", rc, errno, f, EPERM, mine);
    errno = 0; rc = fchownat(AT_FDCWD, f, (uid_t) -1, foreign, 0);
    expect("fchownat(-1, foreign)", rc, errno, f, EPERM, mine);

    int fd = open(f, O_RDONLY);
    if (fd < 0) {
        failf("open own", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
    } else {
        errno = 0; rc = fchown(fd, (uid_t) -1, foreign);
        expect("fchown(fd, -1, foreign)", rc, errno, f, EPERM, mine);
        errno = 0; rc = fchownat(fd, "", (uid_t) -1, foreign, AT_EMPTY_PATH);
        expect("fchownat(fd, \"\", AT_EMPTY_PATH, foreign)", rc, errno, f, EPERM, mine);
        close(fd);
    }

    // A symlink is a file of its own for lchown.
    const char *l = P("own-link");
    unlink(l);
    if (symlink("own", l) == 0) {
        gid_t before = gid_of(l);
        errno = 0; rc = lchown(l, (uid_t) -1, foreign);
        expect("lchown(symlink, -1, foreign)", rc, errno, l, EPERM, before);
    }

    // The group it is already in, and the groups it belongs to, are fine.
    errno = 0; rc = chown(f, (uid_t) -1, mine);
    expect("chown(-1, own egid)", rc, errno, f, 0, mine);
    if (supp != (gid_t) -1) {
        errno = 0; rc = chown(f, (uid_t) -1, supp);
        expect("chown(-1, supplementary)", rc, errno, f, 0, supp);
        errno = 0; rc = chown(f, (uid_t) -1, mine);
        expect("chown(-1, back to egid)", rc, errno, f, 0, mine);
    } else {
        test_logf("  SKIP supplementary leg (no supplementary group)\n");
    }

    // The attack itself, end to end: the setgid bit is set on a file that
    // still has the caller's own group, because the chgrp was refused.
    if (mkfile(P("attack"), 0755) == 0) {
        rc = chown(P("attack"), (uid_t) -1, foreign);
        if (rc == 0 || gid_of(P("attack")) == foreign)
            failf("attack chgrp", (uint64_t) rc, (uint64_t) gid_of(P("attack")), 0,
                  (uint64_t) -1, (uint64_t) mine, 0);
        chmod(P("attack"), 02755);
        struct stat st;
        if (stat(P("attack"), &st) == 0 && (st.st_mode & S_ISGID) && st.st_gid == foreign)
            failf("attack setgid foreign", (uint64_t) st.st_mode, (uint64_t) st.st_gid,
                  0, 0, 0, 0);
    }
}

// Legs whose fixture only root can build: a file owned by the user but in a
// group the user is not in, and a setgid directory in such a group.
static void fixture_legs(void) {
    gid_t foreign = FOREIGN_GID;
    test_logf("-- root-made fixtures, checked as uid %u --\n", (unsigned) geteuid());

    // chmod g+s on an owned file in a foreign group: allowed, bit dropped.
    const char *f = P("theirs-group");
    errno = 0;
    int rc = chmod(f, 02755);
    int e = errno;
    mode_t m = mode_of(f);
    if (rc != 0 || m != 00755)
        failf("chmod 2755 on own file in foreign group", (uint64_t) rc, (uint64_t) e,
              (uint64_t) m, 0, 0, 00755);
    test_logf("  %-44s rc=%d errno=%-2d mode=%04o (want 0 0755)\n",
              "chmod(own, foreign group, 02755)", rc, rc < 0 ? e : 0, (unsigned) m);
    // A member keeps it: the same chmod on a file in its own group.
    if (mkfile(P("own-group"), 0755) == 0) {
        rc = chmod(P("own-group"), 02755);
        m = mode_of(P("own-group"));
        if (rc != 0 || m != 02755)
            failf("chmod 2755 on own file in own group", (uint64_t) rc, (uint64_t) errno,
                  (uint64_t) m, 0, 0, 02755);
    }
    // ...and moving it back to its own group is fine.
    errno = 0; rc = chown(f, (uid_t) -1, getegid());
    expect("chown(own file in foreign group, -1, egid)", rc, errno, f, 0, getegid());

    // Creating in a setgid directory of a foreign group.
    const char *d = P("sgid-dir");
    const char *c = P("sgid-dir/prog");
    unlink(c);
    int fd = open(c, O_WRONLY | O_CREAT | O_EXCL, 02755);
    if (fd < 0) {
        failf("create in setgid dir", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
    } else {
        struct stat st;
        fstat(fd, &st);
        close(fd);
        // The umask may take other bits; only the group and S_ISGID matter.
        if (st.st_gid != foreign || (st.st_mode & S_ISGID))
            failf("new file in foreign setgid dir", (uint64_t) st.st_gid,
                  (uint64_t) (st.st_mode & 07777), 0, (uint64_t) foreign, 0, 0);
        test_logf("  %-44s gid=%u mode=%04o (want gid %u, no S_ISGID)\n",
                  "open(O_CREAT, 02755) in foreign setgid dir", (unsigned) st.st_gid,
                  (unsigned) (st.st_mode & 07777), (unsigned) foreign);
    }
    const char *sd = P("sgid-dir/sub");
    rmdir(sd);
    if (mkdir(sd, 0755) < 0) {
        failf("mkdir in setgid dir", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
    } else {
        struct stat st;
        stat(sd, &st);
        if (st.st_gid != foreign || !(st.st_mode & S_ISGID))
            failf("new dir in foreign setgid dir", (uint64_t) st.st_gid,
                  (uint64_t) (st.st_mode & 07777), 0, (uint64_t) foreign, 02000, 0);
    }
    (void) d;
}

static int run_dropped(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        failf("fork", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        return 0;
    }
    if (pid == 0) {
        gid_t supp = UNPRIV_SUPP;
        if (setgroups(1, &supp) < 0 || setgid(UNPRIV_GID) < 0 || setuid(UNPRIV_UID) < 0) {
            printf("FAIL drop privilege errno=%d\n", errno);
            _exit(1);
        }
        umask(022);
        unprivileged_legs();
        fixture_legs();
        fflush(stdout);
        _exit(failures_total > 0 ? 1 : 0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures_total++;
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    snprintf(base, sizeof base, "/tmp/fs_chgrp_membership.%d", (int) getpid());
    mkdir(base, 0777);
    chmod(base, 0777);

    if (geteuid() == 0) {
        // Root may give its own file any group at all.
        if (mkfile(P("root-own"), 0755) == 0) {
            errno = 0;
            int rc = chown(P("root-own"), (uid_t) -1, FOREIGN_GID);
            expect("root chown(-1, foreign)", rc, errno, P("root-own"), 0, FOREIGN_GID);
        }
        // The fixtures the unprivileged child cannot make for itself.
        if (mkfile(P("theirs-group"), 0755) == 0 &&
                chown(P("theirs-group"), UNPRIV_UID, FOREIGN_GID) < 0)
            failf("fixture chown", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        const char *d = P("sgid-dir");
        rmdir(d);
        if (mkdir(d, 0777) < 0 || chown(d, 0, FOREIGN_GID) < 0 || chmod(d, 02777) < 0)
            failf("fixture setgid dir", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        run_dropped();
    } else {
        test_logf("not root: the root-made fixture legs are skipped\n");
        unprivileged_legs();
    }

    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (system(cmd) != 0)
        test_logf("cleanup failed\n");
    return finish_suite("fs_chgrp_membership");
}
