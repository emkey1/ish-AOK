// prctl(PR_CAPBSET_DROP) was entirely unimplemented -- fell through to the
// default EINVAL case. Real-world trigger: systemd's per-unit
// CapabilityBoundingSet= enforcement (exec_context_apply, run for every
// service including systemd-logind) calls PR_CAPBSET_DROP once per
// capability it wants removed from the bounding set, before ever calling
// capset(). The unconditional EINVAL aborted the whole spawn ("Failed to
// drop capabilities: Invalid argument" / "Failed at step CAPABILITIES"),
// so systemd-logind (and anything else declaring CapabilityBoundingSet=)
// could never start during Arch aarch64 boot -- symptomatically, agetty's
// spawned login process ran fine (plain fork+exec, unaffected) but hung
// forever waiting on a PAM session registration with a logind that had
// never come up, so no login prompt ever appeared despite systemd itself
// reaching "Login Prompts".
//
// It is a real bounding set now (struct task's cap_bounding), which exec
// consults, so the call does what it says. Measured on Linux 6.12, as root of
// a user namespace: every capability up to cap_last_cap drops, one past it is
// EINVAL -- 63 included, which this test once expected to succeed and which
// no kernel with fewer than 64 capabilities accepts -- and a dropped one reads
// back as gone. Without CAP_SETPCAP every call is EPERM, checked before the
// number is.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static long cap_last_cap(void) {
    FILE *f = fopen("/proc/sys/kernel/cap_last_cap", "r");
    long v = -1;
    if (f != NULL) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    long last = cap_last_cap();
    check(last >= 37 && last < 63, "cap_last_cap is a real kernel's");

    if (geteuid() != 0) {
        errno = 0;
        int r = prctl(PR_CAPBSET_DROP, 21, 0, 0, 0);
        check(r < 0 && errno == EPERM, "PR_CAPBSET_DROP without CAP_SETPCAP is EPERM");
        errno = 0;
        r = prctl(PR_CAPBSET_DROP, 999, 0, 0, 0);
        check(r < 0 && errno == EPERM, "...checked before the number");
        return finish_suite("prctl_capbset_drop");
    }

    // A representative sample of real capability numbers a
    // CapabilityBoundingSet= line drops (CAP_SYS_ADMIN=21, CAP_NET_ADMIN=12,
    // CAP_SYS_MODULE=16, plus the boundaries 0 and cap_last_cap).
    int caps[] = {0, 12, 16, 21, (int) last};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        errno = 0;
        int r = prctl(PR_CAPBSET_DROP, caps[i], 0, 0, 0);
        char what[64];
        snprintf(what, sizeof(what), "PR_CAPBSET_DROP(%d) succeeds", caps[i]);
        check(r == 0, what);
        snprintf(what, sizeof(what), "PR_CAPBSET_READ(%d) is 0 after the drop", caps[i]);
        errno = 0;
        check(prctl(PR_CAPBSET_READ, caps[i], 0, 0, 0) == 0, what);
    }

    errno = 0;
    int r = prctl(PR_CAPBSET_DROP, (int) last + 1, 0, 0, 0);
    check(r < 0 && errno == EINVAL, "PR_CAPBSET_DROP(cap_last_cap + 1) is EINVAL");
    errno = 0;
    r = prctl(PR_CAPBSET_DROP, 63, 0, 0, 0);
    check(r < 0 && errno == EINVAL, "PR_CAPBSET_DROP(63) is EINVAL");
    errno = 0;
    r = prctl(PR_CAPBSET_DROP, 999, 0, 0, 0);
    check(r < 0 && errno == EINVAL, "PR_CAPBSET_DROP(999) (out of range) is EINVAL");

    // PR_CAPBSET_READ answers for what was not dropped, and refuses past the
    // last capability the same way.
    errno = 0;
    r = prctl(PR_CAPBSET_READ, 1, 0, 0, 0);
    check(r == 1, "PR_CAPBSET_READ(1) is 1");
    errno = 0;
    r = prctl(PR_CAPBSET_READ, (int) last + 1, 0, 0, 0);
    check(r < 0 && errno == EINVAL, "PR_CAPBSET_READ(cap_last_cap + 1) is EINVAL");

    return finish_suite("prctl_capbset_drop");
}
