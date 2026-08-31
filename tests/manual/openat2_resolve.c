// openat2's RESOLVE_* constraints.
//
//   Every RESOLVE_* bit was rejected with EINVAL, so the whole reason openat2
//   exists over openat -- constraining how the path is allowed to resolve --
//   was unusable. RESOLVE_NO_SYMLINKS is the one that matters most in
//   practice: it is how a program opens a path it does not control without a
//   symlink somewhere in it redirecting the open elsewhere.
//
//   AOK implements the two it can enforce exactly. RESOLVE_BENEATH,
//   RESOLVE_IN_ROOT, RESOLVE_NO_XDEV and RESOLVE_NO_MAGICLINKS stay refused
//   with EINVAL, which is what a kernel without openat2 support says and what
//   every caller already handles -- openat2 is Linux 5.6+, so nothing may
//   assume it. They are sandboxes, and the property they promise is that no
//   intermediate step of the resolution escaped; AOK resolves and then opens
//   in a second pass, so a check in between is a check against a path that
//   could have moved -- the exact race RESOLVE_BENEATH exists to close. This
//   test therefore accepts EITHER a working implementation or EINVAL for those
//   four, and pins the two that are implemented.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#define RESOLVE_NO_XDEV_       0x01
#define RESOLVE_NO_MAGICLINKS_ 0x02
#define RESOLVE_NO_SYMLINKS_   0x04
#define RESOLVE_BENEATH_       0x08
#define RESOLVE_IN_ROOT_       0x10
#define RESOLVE_CACHED_        0x20

struct open_how_ {
    unsigned long long flags, mode, resolve;
};

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// The fd on success, or -errno.
static long open2(int dfd, const char *path, unsigned long long resolve) {
    struct open_how_ how;
    memset(&how, 0, sizeof how);
    how.flags = O_RDONLY;
    how.resolve = resolve;
    errno = 0;
    long r = syscall(SYS_openat2, dfd, path, &how, sizeof how);
    return r < 0 ? -errno : r;
}

static void close_if_fd(long r) {
    if (r >= 0)
        close((int) r);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    snprintf(base, sizeof base, "/tmp/openat2-resolve-%d", (int) getpid());
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    char sub[160], file[160], link[160], vialink[220], dirlink[160];
    snprintf(sub, sizeof sub, "%s/sub", base);
    snprintf(file, sizeof file, "%s/sub/f", base);
    snprintf(link, sizeof link, "%s/link", base);
    snprintf(dirlink, sizeof dirlink, "%s/dirlink", base);
    snprintf(vialink, sizeof vialink, "%s/dirlink/f", base);
    ck("mkdir sub", mkdir(sub, 0755), 0);
    {
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("create the target file", fd >= 0, 1);
        if (fd >= 0)
            close(fd);
    }
    ck("symlink to the file", symlink(file, link), 0);
    ck("symlink to the directory", symlink(sub, dirlink), 0);

    int dfd = open(base, O_RDONLY | O_DIRECTORY);
    ck("open the base directory", dfd >= 0, 1);
    if (dfd < 0)
        return finish_suite("openat2_resolve");

    // ---- openat2 works at all ---------------------------------------------
    {
        long r = open2(dfd, "sub/f", 0);
        ck("openat2 with no constraints opens the file", r >= 0, 1);
        close_if_fd(r);
    }

    // ---- RESOLVE_NO_SYMLINKS ----------------------------------------------
    {
        // A path with no symlink in it is unaffected -- the constraint must not
        // simply fail everything.
        long r = open2(dfd, "sub/f", RESOLVE_NO_SYMLINKS_);
        ck("NO_SYMLINKS on a symlink-free path still opens", r >= 0, 1);
        close_if_fd(r);

        // The final component is a symlink.
        ck("NO_SYMLINKS on a symlink is ELOOP",
           open2(dfd, "link", RESOLVE_NO_SYMLINKS_), -ELOOP);
        // ...and so is one in the MIDDLE, which is the case that matters: the
        // caller controls the last name and not the directories above it.
        ck("NO_SYMLINKS through a symlinked directory is ELOOP",
           open2(dfd, "dirlink/f", RESOLVE_NO_SYMLINKS_), -ELOOP);

        // Without the flag both still resolve, so the failures above are the
        // constraint and not a broken path.
        long a = open2(dfd, "link", 0);
        ck("...but without the flag the symlink opens", a >= 0, 1);
        close_if_fd(a);
        long b = open2(dfd, "dirlink/f", 0);
        ck("...and so does the symlinked directory", b >= 0, 1);
        close_if_fd(b);
    }

    // ---- RESOLVE_CACHED ----------------------------------------------------
    // "Only if this is already cached, else EAGAIN." On Linux the answer
    // depends on what happens to be in the dcache, so a caller cannot rely on
    // either -- which is exactly why declining is honest. Accept either.
    {
        long r = open2(dfd, "sub/f", RESOLVE_CACHED_);
        test_logf("  %-56s got=%ld\n", "CACHED answers", r);
        ck("CACHED either opens or is EAGAIN", r >= 0 || r == -EAGAIN, 1);
        close_if_fd(r);
    }

    // ---- the sandbox flags: implemented, or honestly refused ---------------
    {
        struct { const char *name; unsigned long long bit; } sandbox[] = {
            { "BENEATH", RESOLVE_BENEATH_ },
            { "IN_ROOT", RESOLVE_IN_ROOT_ },
            { "NO_XDEV", RESOLVE_NO_XDEV_ },
            { "NO_MAGICLINKS", RESOLVE_NO_MAGICLINKS_ },
        };
        for (unsigned i = 0; i < sizeof sandbox / sizeof sandbox[0]; i++) {
            long r = open2(dfd, "sub/f", sandbox[i].bit);
            char label[96];
            snprintf(label, sizeof label, "%s works or is EINVAL", sandbox[i].name);
            ck(label, r >= 0 || r == -EINVAL, 1);
            // What must not happen is succeeding without enforcing: if it
            // opened, escaping must be refused.
            if (r >= 0 && sandbox[i].bit == RESOLVE_BENEATH_) {
                close((int) r);
                long esc = open2(dfd, "../", RESOLVE_BENEATH_);
                ck("  ...and if BENEATH works, escaping is refused", esc < 0, 1);
                close_if_fd(esc);
            } else {
                close_if_fd(r);
            }
        }
    }

    // ---- an unknown resolve bit is EINVAL ---------------------------------
    ck("an unknown RESOLVE bit is EINVAL", open2(dfd, "sub/f", 0x8000), -EINVAL);

    close(dfd);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("openat2_resolve");
}
