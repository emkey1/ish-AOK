// Regression test for fakefs case sensitivity on case-insensitive hosts.
//
// fakefs stores guest files on the host filesystem (APFS on iOS/macOS, which
// is case-insensitive) and historically used the raw guest names as host
// names. Guest paths differing only in ASCII case then collided in the same
// host directory: in an Alpine guest, installing ncurses-terminfo lost
// /usr/share/terminfo/{a,e,l,m,n,p,q,x} because their uppercase twins
// {A,E,L,M,N,P,Q,X} already existed -- apk reported "failed to extract ...:
// No such file or directory", mkdir returned EEXIST for a directory that ls
// didn't show, and files silently shared content with their case twins.
//
// APFS also applies Unicode case folding and normalization insensitivity:
// "Ф" vs "ф", and NFC "é" (C3 A9) vs NFD "é" (65 CC 81), collide the same
// way.
//
// Fixed by escaping guest names into a collision-free on-disk form
// ('A' -> "%a", '%' -> "%%", non-ASCII bytes -> "%XY"; fs/fake-path.h). This
// test exercises the guest-visible contract: names that only a folding/
// normalizing filesystem would conflate are independent files, and names
// containing the escape character round-trip exactly.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "test_common.h"

#define FAKEFS_MAGIC 0x66616b65

static char g_dir[512];

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL fakefs_casefold: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static int write_file(const char *name, const char *content) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return n == (ssize_t) strlen(content) ? 0 : -1;
}

static int read_file(const char *name, char *buf, size_t size) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int file_content_is(const char *name, const char *expected) {
    char buf[64];
    if (read_file(name, buf, sizeof(buf)) < 0)
        return 0;
    return strcmp(buf, expected) == 0;
}

// Count entries of the test dir and check the exact-name presence of each
// expected entry (excluding "." and "..").
static int dir_matches(const char *const *names, unsigned count) {
    DIR *dir = opendir(g_dir);
    if (dir == NULL)
        return 0;
    unsigned seen = 0;
    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        seen++;
        int found = 0;
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(ent->d_name, names[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            test_log_if(1, "unexpected direntry: %s\n", ent->d_name);
            ok = 0;
        }
    }
    closedir(dir);
    return ok && seen == count;
}

// Remove the tree with syscalls, not `rm -rf` through system().
//
// The tier0 root has no /bin/sh at all, so system() failed silently and the
// directory survived every run. That alone would be harmless -- except the
// name is keyed on getpid(), and in a root whose only process IS the test the
// pid is always 1, so the next run collided with its own debris and reported
// "mkdir /tmp/fakefs_casefold.1: File exists". A test that cannot be run twice
// looks like a product failure the second time.
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir != NULL) {
        int dfd = dirfd(dir);
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            // unlink first and only recurse if it was a directory: one syscall
            // in the common case, and no reliance on d_type, which fakefs is
            // entitled not to fill in.
            if (unlinkat(dfd, ent->d_name, 0) == 0)
                continue;
            if (unlinkat(dfd, ent->d_name, AT_REMOVEDIR) == 0)
                continue;
            char child[sizeof(g_dir) + 256];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_tree(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

static void cleanup_tree(void) {
    remove_tree(g_dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/fakefs_casefold.%d", base, (int) getpid());

    struct statfs sfs;
    if (statfs(base, &sfs) == 0 && sfs.f_type != FAKEFS_MAGIC) {
        // e.g. tmpfs mounted on /tmp: nothing here would touch fakefs
        printf("fakefs_casefold: SKIP (%s is not on fakefs)\n", base);
        return 0;
    }

    // A previous run that died mid-test (or one whose cleanup could not run)
    // leaves this directory behind, and the name repeats -- see remove_tree.
    remove_tree(g_dir);
    if (mkdir(g_dir, 0755) < 0) {
        printf("FAIL fakefs_casefold: mkdir %s: %s\n", g_dir, strerror(errno));
        return 1;
    }

    // Case-distinct regular files are independent.
    check(write_file("x", "lower") == 0, "create file 'x'");
    check(write_file("X", "upper") == 0, "create case-twin file 'X' (O_EXCL)");
    check(file_content_is("x", "lower"), "'x' keeps its own content");
    check(file_content_is("X", "upper"), "'X' keeps its own content");

    struct stat st_lower, st_upper;
    char path_lower[600], path_upper[600];
    snprintf(path_lower, sizeof(path_lower), "%s/x", g_dir);
    snprintf(path_upper, sizeof(path_upper), "%s/X", g_dir);
    check(stat(path_lower, &st_lower) == 0 && stat(path_upper, &st_upper) == 0 &&
          st_lower.st_ino != st_upper.st_ino, "'x' and 'X' are distinct inodes");

    // Case-distinct directories are independent (the terminfo failure mode:
    // mkdir of the lowercase twin returned EEXIST).
    char path_a[600], path_A[600], path_af[600];
    snprintf(path_a, sizeof(path_a), "%s/a", g_dir);
    snprintf(path_A, sizeof(path_A), "%s/A", g_dir);
    check(mkdir(path_A, 0755) == 0, "mkdir 'A'");
    check(mkdir(path_a, 0755) == 0, "mkdir case-twin 'a'");
    snprintf(path_af, sizeof(path_af), "%s/a/inner", g_dir);
    check(write_file("a/inner", "in-a") == 0, "create file inside 'a'");
    struct stat st_dir;
    snprintf(path_af, sizeof(path_af), "%s/A/inner", g_dir);
    check(stat(path_af, &st_dir) < 0 && errno == ENOENT,
          "'A' does not see the file created inside 'a'");

    // Escape-character robustness: literal '%' names round-trip and don't
    // collide with the escaped forms of other names.
    check(write_file("%x", "pct-x") == 0, "create file '%x'");
    check(write_file("%%", "pct-pct") == 0, "create file '%%'");
    check(write_file("%X", "pct-upper") == 0, "create file '%X'");
    check(file_content_is("%x", "pct-x"), "'%x' keeps its own content");
    check(file_content_is("%%", "pct-pct"), "'%%' keeps its own content");
    check(file_content_is("%X", "pct-upper"), "'%X' keeps its own content");
    check(file_content_is("X", "upper"), "'X' unaffected by '%x' twin games");

    // Symlink under an uppercase name.
    char path_link[600];
    snprintf(path_link, sizeof(path_link), "%s/LINK", g_dir);
    check(symlink("x", path_link) == 0, "symlink 'LINK' -> 'x'");
    char target[64];
    ssize_t tlen = readlink(path_link, target, sizeof(target) - 1);
    if (tlen >= 0)
        target[tlen] = '\0';
    check(tlen == 1 && strcmp(target, "x") == 0, "readlink 'LINK'");

    // readdir returns the exact guest names, nothing more or less.
    const char *const expected[] = {"x", "X", "a", "A", "%x", "%%", "%X", "LINK"};
    check(dir_matches(expected, sizeof(expected)/sizeof(expected[0])),
          "readdir lists exact case-distinct names");

    // Unicode: normalization twins (NFC vs NFD "e-acute") are independent.
    static const char nfc[] = "caf\xc3\xa9";        // U+00E9 precomposed
    static const char nfd[] = "cafe\xcc\x81";       // 'e' + U+0301 combining
    check(write_file(nfc, "nfc") == 0, "create NFC 'cafe-acute'");
    check(write_file(nfd, "nfd") == 0, "create NFD twin (O_EXCL)");
    check(file_content_is(nfc, "nfc"), "NFC name keeps its own content");
    check(file_content_is(nfd, "nfd"), "NFD name keeps its own content");

    // Unicode: case-folding twins (Cyrillic upper/lower) are independent.
    static const char cyr_upper[] = "\xd0\xa4";     // U+0424 Ф
    static const char cyr_lower[] = "\xd1\x84";     // U+0444 ф
    check(write_file(cyr_upper, "cyr-up") == 0, "create Cyrillic uppercase");
    check(write_file(cyr_lower, "cyr-low") == 0, "create Cyrillic lowercase twin (O_EXCL)");
    check(file_content_is(cyr_upper, "cyr-up"), "uppercase Cyrillic keeps its own content");
    check(file_content_is(cyr_lower, "cyr-low"), "lowercase Cyrillic keeps its own content");

    // Unicode names round-trip byte-exactly through readdir.
    const char *const expected_uni[] = {"x", "X", "a", "A", "%x", "%%", "%X", "LINK",
                                        nfc, nfd, cyr_upper, cyr_lower};
    check(dir_matches(expected_uni, sizeof(expected_uni)/sizeof(expected_uni[0])),
          "readdir lists exact Unicode-twin names");

    char path_uni[600];
    snprintf(path_uni, sizeof(path_uni), "%s/%s", g_dir, nfd);
    check(unlink(path_uni) == 0, "unlink NFD twin");
    check(file_content_is(nfc, "nfc"), "NFC twin survives NFD unlink");
    snprintf(path_uni, sizeof(path_uni), "%s/%s", g_dir, cyr_upper);
    check(unlink(path_uni) == 0, "unlink uppercase Cyrillic");
    check(file_content_is(cyr_lower, "cyr-low"), "lowercase Cyrillic survives");
    snprintf(path_uni, sizeof(path_uni), "%s/%s", g_dir, cyr_lower);
    check(unlink(path_uni) == 0, "unlink lowercase Cyrillic");

    // Case-only rename leaves exactly one entry under the new name.
    char path_case[600], path_CASE[600];
    snprintf(path_case, sizeof(path_case), "%s/case", g_dir);
    snprintf(path_CASE, sizeof(path_CASE), "%s/CASE", g_dir);
    check(write_file("case", "renamed") == 0, "create file 'case'");
    check(rename(path_case, path_CASE) == 0, "case-only rename 'case' -> 'CASE'");
    check(stat(path_case, &st_dir) < 0 && errno == ENOENT, "'case' gone after rename");
    check(file_content_is("CASE", "renamed"), "'CASE' present after rename");

    // Unlinking one case twin leaves the other.
    check(unlink(path_upper) == 0, "unlink 'X'");
    check(file_content_is("x", "lower"), "'x' survives unlink of 'X'");
    check(stat(path_upper, &st_dir) < 0 && errno == ENOENT, "'X' really gone");

    cleanup_tree();
    return finish_suite("fakefs_casefold");
}
