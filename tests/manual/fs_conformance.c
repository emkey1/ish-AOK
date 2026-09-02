// fs_conformance.c — self-checking regression lock for the filesystem/VFS
// conformance fixes found by differential testing against real Linux (mint).
// Each check asserts the documented Linux behavior that AOK now matches:
//
//   - O_NOFOLLOW on a final symlink -> ELOOP
//   - rmdir of a symlink-to-dir -> ENOTDIR (final symlink not followed)
//   - mkdir over an existing/dangling symlink -> EEXIST
//   - unlink of a directory -> EISDIR (not the host's EPERM)
//   - "nonexistent/.." -> ENOENT (no lexical '..' collapse)
//   - open("name/", O_CREAT) -> EISDIR (no create through a trailing slash)
//   - getdents with a too-small buffer -> EINVAL (not a spurious EOF)
//   - fchdir on a non-directory (socket) -> ENOTDIR, cwd unchanged
//   - tmpfs pread past EOF -> 0; ftruncate shrink/grow underflow-safe, grow zero-fills
//   - renameat2(RENAME_NOREPLACE) onto an existing name -> EEXIST, else success
//   - fcntl(F_DUPFD) with a negative/over-limit arg -> EINVAL (no abort)
//   - readlink("/proc/self") is the bare numeric pid (no trailing slash)
//   - dup clears FD_CLOEXEC; dup3(O_CLOEXEC) sets it
//
// The same source is a Tier-0 functional gate (run under AOK, no oracle) and a
// portable check that also passes on a real Linux kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include "test_common.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted success\n", label, r, errno);
    failures_total++;
    return 0;
}
static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// open + immediately close; return the open() result (errno preserved on failure)
static long open_close(const char *path, int flags, mode_t mode) {
    int fd = open(path, flags, mode);
    if (fd >= 0) close(fd);
    return fd;
}
static void mkfile(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char base[200];
    snprintf(base, sizeof base, "/tmp/fsconf_XXXXXX");
    if (!mkdtemp(base) || chdir(base) != 0) { perror("setup"); return 2; }

    // ---- O_NOFOLLOW ----
    mkfile("target");
    symlink("target", "sl");
    mkdir("d", 0755);
    eq_errno("nofollow.symlink", open_close("sl", O_RDONLY | O_NOFOLLOW, 0), ELOOP);
    is_ok("nofollow.regfile", open_close("target", O_RDONLY | O_NOFOLLOW, 0));
    is_ok("nofollow.dir", open_close("d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0));

    // ---- '..'/'.' through a nonexistent component ----
    struct stat st;
    eq_errno("dotdot.nonexist.stat", stat("nope/..", &st), ENOENT);
    eq_errno("dotdot.nonexist.access", access("nope/..", F_OK), ENOENT);
    eq_errno("dotdot.nonexist.open", open_close("nope/../target", O_RDONLY, 0), ENOENT);

    // ---- trailing slash ----
    eq_errno("slash.creat", open_close("newf/", O_WRONLY | O_CREAT, 0644), EISDIR);
    eq_errno("slash.onfile", open_close("target/", O_RDONLY, 0), ENOTDIR);
    is_ok("slash.ondir", open_close("d/", O_RDONLY, 0));

    // ---- rmdir / mkdir / unlink on symlinks and dirs ----
    mkdir("rtgt", 0755);
    symlink("rtgt", "rsl");
    eq_errno("rmdir.symlink", rmdir("rsl"), ENOTDIR);
    is_true("rmdir.symtgt_kept", stat("rtgt", &st) == 0 && S_ISDIR(st.st_mode));

    symlink("d", "msl");
    eq_errno("mkdir.over_symlink", mkdir("msl", 0755), EEXIST);
    symlink("nowhere", "mdsl");
    eq_errno("mkdir.over_dangling", mkdir("mdsl", 0755), EEXIST);

    mkdir("ud", 0755);
    eq_errno("unlink.dir", unlink("ud"), EISDIR);

    // ---- getdents with a too-small buffer ----
    int dfd = open("d", O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        char tiny[1];
        eq_errno("getdents.tiny", syscall(SYS_getdents64, dfd, tiny, (unsigned) sizeof tiny), EINVAL);
        close(dfd);
    }

    // ---- fchdir requires a directory fd ----
    // fchdir on a non-directory (socket) must return ENOTDIR and leave the cwd
    // untouched. AOK used to skip the type check and pin the fd as cwd; a
    // listening socket pinned this way never released its host port, so
    // stress-ng --sockabuse spun on EADDRINUSE for minutes.
    int cdd = open("d", O_RDONLY | O_DIRECTORY);
    if (cdd >= 0) is_ok("fchdir.dir", fchdir(cdd));
    char cwd_before[1024];
    getcwd(cwd_before, sizeof cwd_before);
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd >= 0) {
        eq_errno("fchdir.socket", fchdir(sfd), ENOTDIR);
        char cwd_after[1024];
        getcwd(cwd_after, sizeof cwd_after);
        is_true("fchdir.socket.cwd_kept", strcmp(cwd_before, cwd_after) == 0);
        close(sfd);
    }
    if (cdd >= 0) { chdir(base); close(cdd); }

    // ---- tmpfs read/resize boundary conditions (unsigned-underflow crashes) ----
    // pread past EOF must return 0, not read out of bounds; ftruncate to a
    // smaller size (including 0) must not underflow the zero-fill; a re-grow
    // must read back as zeros. These crashed tmpfs (stress-ng copy-file,
    // fallocate, sync-file) before the size arithmetic was made underflow-safe.
    int tf = open("tmpf", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tf >= 0) {
        is_true("tmpfs.write5", write(tf, "hello", 5) == 5);
        char rb[16];
        is_true("tmpfs.pread_past_eof", pread(tf, rb, sizeof rb, 100) == 0);
        is_true("tmpfs.pread_clamped", pread(tf, rb, sizeof rb, 3) == 2);
        is_ok("tmpfs.ftruncate_zero", ftruncate(tf, 0));
        is_true("tmpfs.size_zero", fstat(tf, &st) == 0 && st.st_size == 0);
        is_ok("tmpfs.ftruncate_grow", ftruncate(tf, 4096));
        is_true("tmpfs.size_grow", fstat(tf, &st) == 0 && st.st_size == 4096);
        char zb[64]; memset(zb, 0xff, sizeof zb);
        ssize_t zn = pread(tf, zb, sizeof zb, 0);
        int allzero = zn == (ssize_t) sizeof zb;
        for (ssize_t i = 0; i < zn; i++) if (zb[i] != 0) allzero = 0;
        is_true("tmpfs.grow_reads_zero", allzero);
        close(tf);
    }

    // ---- renameat2(RENAME_NOREPLACE) ----
    mkfile("nr_a"); mkfile("nr_b");
    eq_errno("rename.noreplace.exist",
             syscall(SYS_renameat2, AT_FDCWD, "nr_a", AT_FDCWD, "nr_b", RENAME_NOREPLACE), EEXIST);
    is_ok("rename.noreplace.new",
          syscall(SYS_renameat2, AT_FDCWD, "nr_a", AT_FDCWD, "nr_new", RENAME_NOREPLACE));
    is_true("rename.noreplace.moved", stat("nr_new", &st) == 0);

    // ---- F_DUPFD bounds (must not abort) ----
    int fd = open("target", O_RDONLY);
    if (fd >= 0) {
        eq_errno("dupfd.neg", fcntl(fd, F_DUPFD, -1), EINVAL);
        struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
        long lim = (rl.rlim_cur < (rlim_t) 0x40000000) ? (long) rl.rlim_cur : 0x40000000;
        eq_errno("dupfd.at_limit", fcntl(fd, F_DUPFD, lim), EINVAL);
        // a valid F_DUPFD still works and clears cloexec
        int nf = fcntl(fd, F_DUPFD, 50);
        is_true("dupfd.valid", nf >= 50);
        if (nf >= 0) { is_true("dupfd.cloexec_clear", fcntl(nf, F_GETFD) == 0); close(nf); }
        close(fd);
    }

    // ---- dup cloexec semantics ----
    int ce = open("target", O_RDONLY | O_CLOEXEC);
    if (ce >= 0) {
        is_true("open.cloexec_set", fcntl(ce, F_GETFD) == FD_CLOEXEC);
        int d2 = dup(ce);
        if (d2 >= 0) { is_true("dup.cloexec_clear", fcntl(d2, F_GETFD) == 0); close(d2); }
        int d3 = dup3(ce, 60, O_CLOEXEC);
        if (d3 >= 0) { is_true("dup3.cloexec_set", fcntl(60, F_GETFD) == FD_CLOEXEC); close(60); }
        close(ce);
    }

    // ---- /proc/self is the bare numeric pid ----
    char link[64];
    ssize_t r = readlink("/proc/self", link, sizeof link - 1);
    if (r > 0) {
        link[r] = '\0';
        int numeric = 1;
        for (ssize_t i = 0; i < r; i++) if (link[i] < '0' || link[i] > '9') numeric = 0;
        is_true("proc.self.numeric", numeric);
    }

    // ---- readdir emits "." and ".." (tmpfs used to emit neither, so getdents
    // on an empty tmpfs dir wrongly reported EOF) and rewinddir restarts it
    // (reaches the kernel as lseek(SEEK_SET, 0)). ----
    mkdir("rd", 0755);
    mkfile("rd/f1");
    DIR *rdd = opendir("rd");
    if (rdd != NULL) {
        int dot = 0, dotdot = 0, f1 = 0;
        struct dirent *de;
        while ((de = readdir(rdd))) {
            if (strcmp(de->d_name, ".") == 0) dot++;
            else if (strcmp(de->d_name, "..") == 0) dotdot++;
            else if (strcmp(de->d_name, "f1") == 0) f1++;
        }
        is_true("readdir.dot", dot == 1);
        is_true("readdir.dotdot", dotdot == 1);
        is_true("readdir.entry", f1 == 1);
        rewinddir(rdd);
        is_true("rewinddir.restart", readdir(rdd) != NULL);
        closedir(rdd);
    }

    // ---- rename: cross-directory move, overwrite, and POSIX type/empty rules
    // (tmpfs had no ->rename, so every tmpfs rename returned EPERM). ----
    mkdir("rs", 0755); mkdir("rdst", 0755);
    mkfile("rs/mv");
    is_ok("rename.crossdir", rename("rs/mv", "rdst/mv"));
    is_true("rename.crossdir.moved", stat("rdst/mv", &st) == 0 && stat("rs/mv", &st) != 0);

    mkfile("ov_a"); mkfile("ov_b");
    int ova = open("ov_a", O_WRONLY);
    if (ova >= 0) { (void) !write(ova, "Z", 1); close(ova); }
    is_ok("rename.overwrite", rename("ov_a", "ov_b"));
    char ovc = 0;
    int ovb = open("ov_b", O_RDONLY);
    if (ovb >= 0) { (void) !read(ovb, &ovc, 1); close(ovb); }
    is_true("rename.overwrite.content", ovc == 'Z');

    mkfile("t_file"); mkdir("t_dir", 0755);
    eq_errno("rename.file_onto_dir", rename("t_file", "t_dir"), EISDIR);
    eq_errno("rename.dir_onto_file", rename("t_dir", "t_file"), ENOTDIR);

    mkdir("ne_src", 0755); mkdir("ne_dst", 0755); mkfile("ne_dst/x");
    eq_errno("rename.onto_nonempty_dir", rename("ne_src", "ne_dst"), ENOTEMPTY);

    mkdir("cyc", 0755); mkdir("cyc/sub", 0755);
    eq_errno("rename.into_descendant", rename("cyc", "cyc/sub/inner"), EINVAL);

    return finish_suite("fs_conformance");
}
