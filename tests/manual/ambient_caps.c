// Ambient capabilities + SECBIT_KEEP_CAPS across a root-to-nonroot uid
// transition -- the exact sequence systemd's executor runs for every
// service with User= plus AmbientCapabilities= (and, with SystemCallFilter=,
// its post-setuid keep_capability() re-assert):
//
//   capset(add caps to inheritable) ->
//   prctl(PR_CAP_AMBIENT_RAISE, cap) ->
//   prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS) ->
//   setresuid(uid, uid, uid) ->
//   capset(re-assert cap in effective+permitted+inheritable)
//
// Was, in AOK: PR_CAP_AMBIENT_RAISE was accepted but recorded nothing,
// PR_SET_SECUREBITS was accepted but never armed keepcaps, and capset's
// inheritable check demanded pI' be a subset of pI alone (Linux allows
// pI | pP). Net effect: the setresuid wiped the permitted set, the
// re-assert capset failed EPERM, and systemd-resolved died at step USER
// ("Failed to keep CAP_SYS_ADMIN: Operation not permitted") on every
// start. Each scenario runs in a fork()ed child (the uid drop is
// irreversible). Needs root; skips otherwise. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#endif
#ifndef PR_GET_SECUREBITS
#define PR_GET_SECUREBITS 27
#define PR_SET_SECUREBITS 28
#endif
#define SECBIT_KEEP_CAPS_ (1 << 4)

#define CAP_NET_RAW_ 13
#define CAP_SYS_ADMIN_ 21

#define LINUX_CAPABILITY_VERSION_3_ 0x20080522

struct cap_hdr { unsigned version; int pid; };
struct cap_data { unsigned effective, permitted, inheritable; };

static int capget_(struct cap_hdr *h, struct cap_data *d) {
    return syscall(SYS_capget, h, d);
}
static int capset_(struct cap_hdr *h, struct cap_data *d) {
    return syscall(SYS_capset, h, d);
}

#define TEST_UID 12345

// The systemd executor sequence, verbatim. Returns 0 on full success, a
// step number on failure (reported via the child's exit status).
static int child_systemd_sequence(void) {
    struct cap_hdr h = {.version = LINUX_CAPABILITY_VERSION_3_, .pid = getpid()};
    struct cap_data d[2];

    // Add our caps to the inheritable set (required to raise ambient).
    if (capget_(&h, d) != 0) return 10;
    d[0].inheritable |= (1u << CAP_NET_RAW_) | (1u << CAP_SYS_ADMIN_);
    if (capset_(&h, d) != 0) return 11;

    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW_, 0, 0) != 0) return 12;
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_ADMIN_, 0, 0) != 0) return 13;
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW_, 0, 0) != 1) return 14;

    // enforce_user: arm keep-caps via SECUREBITS (not PR_SET_KEEPCAPS!).
    if (prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS_, 0, 0, 0) != 0) return 15;
    if ((prctl(PR_GET_SECUREBITS, 0, 0, 0, 0) & SECBIT_KEEP_CAPS_) == 0) return 16;

    if (setresuid(TEST_UID, TEST_UID, TEST_UID) != 0) return 17;

    // Permitted must have survived; ambient bits must be in effective.
    if (capget_(&h, d) != 0) return 18;
    if (!(d[0].permitted & (1u << CAP_SYS_ADMIN_))) return 19;
    if (!(d[0].effective & (1u << CAP_NET_RAW_))) return 20;

    // keep_capability(CAP_SYS_ADMIN): re-assert in all three sets. The
    // inheritable bit comes from PERMITTED here (pI' subset of pI|pP).
    d[0].effective |= 1u << CAP_SYS_ADMIN_;
    d[0].permitted |= 1u << CAP_SYS_ADMIN_;
    d[0].inheritable |= 1u << CAP_SYS_ADMIN_;
    if (capset_(&h, d) != 0) return 21;

    return 0;
}

// Without keep-caps, the transition must still keep the AMBIENT bits (in
// both permitted and effective) but drop everything else from permitted.
static int child_ambient_only(void) {
    struct cap_hdr h = {.version = LINUX_CAPABILITY_VERSION_3_, .pid = getpid()};
    struct cap_data d[2];

    if (capget_(&h, d) != 0) return 30;
    d[0].inheritable |= 1u << CAP_NET_RAW_;
    if (capset_(&h, d) != 0) return 31;
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW_, 0, 0) != 0) return 32;

    if (setresuid(TEST_UID, TEST_UID, TEST_UID) != 0) return 33;

    if (capget_(&h, d) != 0) return 34;
    if (!(d[0].permitted & (1u << CAP_NET_RAW_))) return 35;
    if (!(d[0].effective & (1u << CAP_NET_RAW_))) return 36;
    // Non-ambient caps must be gone from permitted (no keepcaps).
    if (d[0].permitted & (1u << CAP_SYS_ADMIN_)) return 37;

    return 0;
}

static void run_case(const char *name, int (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); failures_total++; return; }
    if (pid == 0)
        _exit(fn());
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    test_log_if(code == 0, "%s: ok\n", name);
    if (code != 0) {
        printf("FAIL: %s failed at step %d\n", name, code);
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    if (geteuid() != 0) {
        printf("ambient_caps: SKIP (needs root)\n");
        return 0;
    }

    run_case("systemd executor sequence", child_systemd_sequence);
    run_case("ambient without keepcaps", child_ambient_only);

    return finish_suite("ambient_caps");
}
