// file_caps_exec.c -- file capabilities (security.capability) take effect at
// exec, the way Linux's cap_bprm_creds_from_file applies them.
//
// `setcap cap_net_bind_service+ep prog` is the standard way to let one program
// bind a low port without running it as root. With AOK's setxattr answering
// ENOTSUP the attribute could not even be stored, and nothing at exec would
// have looked at it anyway. The rules, for a caller that is not root:
//
//   pP' = (fP & bounding) | (fI & pI) | pA'        pA' = 0 if the file has caps
//   pE' = fE ? pP' : pA'                            pI' = pI
//
// and the exec fails with EPERM when fE is set but some fP bit could not be
// granted (it is outside the bounding set). AT_SECURE is set for any exec
// that gains capabilities or has fE set. A nosuid mount's file capabilities
// are ignored, as its set-id bits are. A #! script's own attribute is ignored:
// what counts is the interpreter's.
//
// Measured on Linux 6.12 (camd) without sudo: the fixture is made as root of
// a user namespace (unshare -Urm), and each exec is made from a nested user
// namespace where the caller is uid 1000 -- a real non-root uid, in a
// namespace whose root owns the file, which is the case these rules decide.
// Under AOK the test is root and simply drops to uid 1000.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include "test_common.h"

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef AT_SECURE
#define AT_SECURE 23
#endif

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW 13
#define BIT(c) (1ull << (c))

#define VFS_CAP_REVISION_2 0x02000000u
#define VFS_CAP_FLAGS_EFFECTIVE 0x000001u

struct caps_v2 {
    uint32_t magic_etc;
    struct {
        uint32_t permitted;
        uint32_t inheritable;
    } data[2];
};

struct cap_header {
    uint32_t version;
    int pid;
};
struct cap_data {
    uint32_t effective, permitted, inheritable;
};
#define LINUX_CAPABILITY_VERSION_3 0x20080522

static int is_root;
static int init_ns_root;
static char dir[200];
static char bin[260], plainbin[260], script[260], interp[260];

// What the exec'd program saw: its ids, its capability sets, AT_SECURE.
struct report {
    long uid, euid;
    unsigned long long inh, prm, eff, amb;
    unsigned long secure;
    int exec_errno;  // the exec failed with this, and nothing ran
};

static int in_initial_userns(void) {
    FILE *f = fopen("/proc/self/uid_map", "r");
    if (f == NULL)
        return 1;
    unsigned long inside = 1, outside = 1, count = 0;
    int n = fscanf(f, "%lu %lu %lu", &inside, &outside, &count);
    fclose(f);
    return n == 3 && inside == 0 && outside == 0 && count == 4294967295ul;
}

static unsigned long long status_caps(const char *field) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return ~0ull;
    char line[256];
    unsigned long long v = ~0ull;
    size_t n = strlen(field);
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, field, n) == 0 && line[n] == ':')
            v = strtoull(line + n + 1, NULL, 16);
    fclose(f);
    return v;
}

// The exec'd side: say what we are and exit.
static int report_main(void) {
    printf("REPORT %ld %ld %llx %llx %llx %llx %lu\n", (long) getuid(), (long) geteuid(),
           status_caps("CapInh"), status_caps("CapPrm"), status_caps("CapEff"),
           status_caps("CapAmb"), getauxval(AT_SECURE));
    return 0;
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t) len;
    close(fd);
    return ok ? 0 : -1;
}

static int copy_self(const char *dst) {
    int in = open("/proc/self/exe", O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t) n) != n)
            ok = 0;
    close(in);
    close(out);
    return ok && n == 0 && chmod(dst, 0755) == 0 ? 0 : -1;
}

static int set_file_caps(const char *path, uint32_t permitted, uint32_t inheritable, int effective) {
    struct caps_v2 caps = {.magic_etc = VFS_CAP_REVISION_2 | (effective ? VFS_CAP_FLAGS_EFFECTIVE : 0)};
    caps.data[0].permitted = permitted;
    caps.data[0].inheritable = inheritable;
    return setxattr(path, "security.capability", &caps, sizeof caps, 0) == 0 ? 0 : -errno;
}

static void write_map(const char *file, const char *text) {
    int fd = open(file, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, text, strlen(text)) < 0)
            perror(file);
        close(fd);
    }
}

// Become uid 1000 while still holding every capability, so a case can shape
// its capability sets before the exec that is being measured. Under AOK that
// is setuid with PR_SET_KEEPCAPS and the effective set raised back from the
// permitted one; on Linux without sudo it is a nested user namespace in which
// uid 1000 is the parent namespace's root -- the owner of the fixture.
static int become_user(void) {
    if (init_ns_root) {
        if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0)
            return -1;
        if (setgroups(0, NULL) != 0 || setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            return -1;
        struct cap_header h = {LINUX_CAPABILITY_VERSION_3, 0};
        struct cap_data d[2];
        if (syscall(SYS_capget, &h, d) != 0)
            return -1;
        d[0].effective = d[0].permitted;
        d[1].effective = d[1].permitted;
        return syscall(SYS_capset, &h, d) == 0 ? 0 : -1;
    }
    if (unshare(CLONE_NEWUSER) != 0)
        return -1;
    write_map("/proc/self/setgroups", "deny");
    write_map("/proc/self/uid_map", "1000 0 1");
    write_map("/proc/self/gid_map", "1000 0 1");
    return getuid() == UNPRIV_UID ? 0 : -1;
}

// Raise `cap` in the inheritable set (and optionally the ambient one).
static int raise_inheritable(int cap, int ambient) {
    struct cap_header h = {LINUX_CAPABILITY_VERSION_3, 0};
    struct cap_data d[2];
    if (syscall(SYS_capget, &h, d) != 0)
        return -1;
    d[cap / 32].inheritable |= 1u << (cap % 32);
    if (syscall(SYS_capset, &h, d) != 0)
        return -1;
    if (ambient && prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) != 0)
        return -1;
    return 0;
}

// Set all three capability sets at once.
static int set_caps(uint32_t eff, uint32_t prm, uint32_t inh) {
    struct cap_header h = {LINUX_CAPABILITY_VERSION_3, 0};
    struct cap_data d[2] = {{eff, prm, inh}, {0, 0, 0}};
    return syscall(SYS_capset, &h, d) == 0 ? 0 : -1;
}

// Every case but the last two runs as an ordinary user holding no
// capabilities at all -- a login shell's state -- so anything the program has
// came from the exec being measured.
enum setup {
    SETUP_NONE,
    SETUP_DROP_BOUNDING,     // CAP_NET_BIND_SERVICE out of the bounding set
    SETUP_INHERITABLE,       // CAP_NET_BIND_SERVICE in pI, nothing else
    SETUP_AMBIENT_RAW,       // CAP_NET_RAW in pP, pI and pA
    SETUP_NO_NEW_PRIVS,
    SETUP_NO_NEW_PRIVS_KEEP, // no_new_privs, still holding a full pP
    SETUP_STAY_ROOT,         // no uid change at all
};

// Run `path` as uid 1000 after `setup`, and read back its report.
static struct report run_case(const char *path, enum setup setup) {
    struct report r = {.uid = -1};
    int p[2];
    if (pipe(p) != 0) {
        r.exec_errno = -1;
        return r;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (setup != SETUP_STAY_ROOT && become_user() != 0) {
            dprintf(p[1], "SETUPFAIL become_user %d\n", errno);
            _exit(1);
        }
        int err = 0;
        switch (setup) {
            case SETUP_NONE:
                err = set_caps(0, 0, 0);
                break;
            case SETUP_DROP_BOUNDING:
                err = prctl(PR_CAPBSET_DROP, CAP_NET_BIND_SERVICE, 0, 0, 0);
                if (err == 0)
                    err = set_caps(0, 0, 0);
                break;
            case SETUP_INHERITABLE:
                err = set_caps(0, 0, 1u << CAP_NET_BIND_SERVICE);
                break;
            case SETUP_AMBIENT_RAW:
                err = raise_inheritable(CAP_NET_RAW, 1);
                if (err == 0)
                    err = set_caps(0, 1u << CAP_NET_RAW, 1u << CAP_NET_RAW);
                break;
            case SETUP_NO_NEW_PRIVS:
                err = set_caps(0, 0, 0);
                if (err == 0)
                    err = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
                break;
            case SETUP_NO_NEW_PRIVS_KEEP:
                err = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
                break;
            default:
                break;
        }
        if (err != 0) {
            dprintf(p[1], "SETUPFAIL case %d\n", errno);
            _exit(1);
        }
        dup2(p[1], 1);
        execl(path, path, "--report", (char *) NULL);
        dprintf(p[1], "EXECFAIL %d\n", errno);
        _exit(1);
    }
    close(p[1]);
    char buf[512] = {0};
    size_t got = 0;
    ssize_t n;
    while (got < sizeof buf - 1 && (n = read(p[0], buf + got, sizeof buf - 1 - got)) > 0)
        got += (size_t) n;
    close(p[0]);
    waitpid(pid, NULL, 0);
    int e;
    if (sscanf(buf, "EXECFAIL %d", &e) == 1) {
        r.exec_errno = e;
        return r;
    }
    if (sscanf(buf, "REPORT %ld %ld %llx %llx %llx %llx %lu", &r.uid, &r.euid, &r.inh, &r.prm,
               &r.eff, &r.amb, &r.secure) != 7) {
        printf("FAIL no report: %s\n", buf);
        failures_total++;
        r.exec_errno = -1;
    }
    return r;
}

static void expect(const char *label, struct report r, int exec_errno,
                   unsigned long long prm, unsigned long long eff, unsigned long long amb,
                   unsigned long secure) {
    char l[160];
    snprintf(l, sizeof l, "%s: exec", label);
    if (r.exec_errno != exec_errno)
        failf(l, (uint64_t) r.exec_errno, 0, 0, (uint64_t) exec_errno, 0, 0);
    test_logf("%-44s errno=%d uid=%ld euid=%ld inh=%llx prm=%llx eff=%llx amb=%llx secure=%lu\n",
              label, r.exec_errno, r.uid, r.euid, r.inh, r.prm, r.eff, r.amb, r.secure);
    if (r.exec_errno != 0 || exec_errno != 0)
        return;
    snprintf(l, sizeof l, "%s: permitted/effective/ambient", label);
    if (r.prm != prm || r.eff != eff || r.amb != amb)
        failf(l, r.prm, r.eff, r.amb, prm, eff, amb);
    snprintf(l, sizeof l, "%s: AT_SECURE", label);
    if (r.secure != secure)
        failf(l, r.secure, 0, 0, secure, 0, 0);
}

static void rm_rf(const char *path) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) < 0)
        return;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--report") == 0)
        return report_main();
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    is_root = geteuid() == 0;
    init_ns_root = is_root && in_initial_userns();
    if (!is_root) {
        printf("file_caps_exec: SKIP (needs root, or unshare -Urm on Linux)\n");
        return 0;
    }
    umask(022);

    snprintf(dir, sizeof dir, "/tmp/file_caps_exec.%d", (int) getpid());
    rm_rf(dir);
    if (mkdir(dir, 0755) != 0 || chmod(dir, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", dir, strerror(errno));
        return finish_suite("file_caps_exec");
    }
    snprintf(bin, sizeof bin, "%s/capbin", dir);
    snprintf(plainbin, sizeof plainbin, "%s/plain", dir);
    if (copy_self(bin) != 0 || copy_self(plainbin) != 0) {
        printf("FAIL copying the test binary: %s\n", strerror(errno));
        return finish_suite("file_caps_exec");
    }
    const unsigned long long NBS = BIT(CAP_NET_BIND_SERVICE), RAW = BIT(CAP_NET_RAW);

    // The control: nothing on the file, nothing gained.
    expect("no file capabilities", run_case(plainbin, SETUP_NONE), 0, 0, 0, 0, 0);

    int err = set_file_caps(bin, NBS, 0, 1);
    if (err != 0) {
        printf("FAIL setting security.capability: %s\n", strerror(-err));
        failures_total++;
        rm_rf(dir);
        return finish_suite("file_caps_exec");
    }
    // +ep: permitted and effective, and a secure exec.
    expect("+ep", run_case(bin, SETUP_NONE), 0, NBS, NBS, 0, 1);
    // A root caller gets everything whatever the file says, and is not marked
    // secure for it.
    {
        struct report r = run_case(bin, SETUP_STAY_ROOT);
        expect("+ep, run by root", r, 0, r.prm, r.eff, 0, 0);
        if (r.exec_errno == 0 && (r.prm & NBS) == 0)
            failf("+ep, run by root: has the file's capability", r.prm, 0, 0, NBS, 0, 0);
    }
    // Out of the bounding set: fE asks for a capability the exec cannot give,
    // and the exec fails rather than run the program without it.
    expect("+ep, bounding set dropped", run_case(bin, SETUP_DROP_BOUNDING), EPERM, 0, 0, 0, 0);
    // no_new_privs: no capability is gained, yet fE still makes it secure.
    expect("+ep, no_new_privs", run_case(bin, SETUP_NO_NEW_PRIVS), 0, 0, 0, 0, 1);
    // ...but "gained" is measured against what the caller already held, so a
    // caller whose permitted set has the capability keeps it.
    expect("+ep, no_new_privs, full pP", run_case(bin, SETUP_NO_NEW_PRIVS_KEEP), 0, NBS, NBS, 0, 1);
    // File capabilities cancel the ambient set.
    expect("+ep, ambient CAP_NET_RAW", run_case(bin, SETUP_AMBIENT_RAW), 0, NBS, NBS, 0, 1);
    // ...which an exec without them carries through, into both sets.
    expect("plain, ambient CAP_NET_RAW", run_case(plainbin, SETUP_AMBIENT_RAW), 0, RAW, RAW, RAW, 0);

    // +p: permitted only, still a secure exec (the permitted set grew).
    set_file_caps(bin, NBS, 0, 0);
    expect("+p", run_case(bin, SETUP_NONE), 0, NBS, 0, 0, 1);
    // Out of the bounding set without fE: the program runs without it.
    expect("+p, bounding set dropped", run_case(bin, SETUP_DROP_BOUNDING), 0, 0, 0, 0, 0);

    // +ei: the inheritable half needs the caller's pI to match.
    set_file_caps(bin, 0, (uint32_t) NBS, 1);
    expect("+ei, empty pI", run_case(bin, SETUP_NONE), 0, 0, 0, 0, 1);
    expect("+ei, pI has it", run_case(bin, SETUP_INHERITABLE), 0, NBS, NBS, 0, 1);

    // A #! script: its own attribute counts for nothing, its interpreter's for
    // everything.
    snprintf(interp, sizeof interp, "%s/interp", dir);
    snprintf(script, sizeof script, "%s/script", dir);
    char text[400];
    snprintf(text, sizeof text, "#!%s --report\n", plainbin);
    if (write_file(script, text) == 0 && set_file_caps(script, NBS, 0, 1) == 0)
        expect("script with caps, plain interpreter", run_case(script, SETUP_NONE), 0, 0, 0, 0, 0);
    if (copy_self(interp) == 0 && set_file_caps(interp, NBS, 0, 1) == 0) {
        snprintf(text, sizeof text, "#!%s --report\n", interp);
        if (write_file(script, text) == 0)
            expect("plain script, interpreter with caps", run_case(script, SETUP_NONE), 0, NBS, NBS, 0, 1);
    }

    // nosuid: the mount's file capabilities are ignored.
    char mnt[260];
    snprintf(mnt, sizeof mnt, "%s/nosuid", dir);
    if (mkdir(mnt, 0755) == 0 && mount("file_caps_exec", mnt, "tmpfs", MS_NOSUID, "mode=0755") == 0) {
        char nbin[300];
        snprintf(nbin, sizeof nbin, "%s/capbin", mnt);
        if (copy_self(nbin) == 0 && set_file_caps(nbin, NBS, 0, 1) == 0)
            expect("+ep on a nosuid mount", run_case(nbin, SETUP_NONE), 0, 0, 0, 0, 0);
        else
            printf("FAIL nosuid fixture: %s\n", strerror(errno)), failures_total++;
        // The same file on a mount without nosuid: the control.
        if (mount(NULL, mnt, NULL, MS_REMOUNT, "mode=0755") == 0)
            expect("+ep, remounted suid", run_case(nbin, SETUP_NONE), 0, NBS, NBS, 0, 1);
        umount(mnt);
    } else {
        test_logf("nosuid leg skipped: %s\n", strerror(errno));
    }

    rm_rf(dir);
    return finish_suite("file_caps_exec");
}
