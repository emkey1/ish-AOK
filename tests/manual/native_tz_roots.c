/*
 * native_tz_roots.c -- native programs read their own time zone, whatever
 * another native program read a moment before.
 *
 * Native programs are threads of one host process, and kernel/native_tz.c
 * converts local time for all of them. It kept one zone, and for a second
 * after any program looked, every other program got that zone without
 * looking: a native `date` in a chroot whose /etc/localtime is BBB printed the
 * zone of a chroot whose /etc/localtime is AAA, 150 times in 150, and a date
 * run straight after /etc/localtime was replaced printed the old zone. Each
 * program's zone is its own now (its TZ, or its own root's /etc/localtime),
 * and a replaced file is seen at once.
 *
 * Two chroots, each with /AOK bound in and its own /etc/localtime (a one-type
 * TZif written here: AAA at +3, BBB at -5), run native dash loops of native
 * `date +%Z` at the same time; then one chroot's file is replaced, back and
 * forth, with a date straight after each. Needs root for chroot and mount.
 *
 * Exits 0 and prints "native_tz_roots: PASS" on success.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define LOOPS 150

static char base[64];

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures_total++;
    }
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

// A version 1 TZif with no transitions and one type.
static int write_zone(const char *path, int32_t off, const char *abbr) {
    unsigned char b[64];
    memset(b, 0, sizeof(b));
    memcpy(b, "TZif", 4);
    size_t alen = strlen(abbr) + 1;
    put_be32(b + 20 + 4 * 4, 1);            // typecnt
    put_be32(b + 20 + 5 * 4, (uint32_t) alen);  // charcnt
    put_be32(b + 44, (uint32_t) off);
    b[48] = 0;                              // isdst
    b[49] = 0;                              // abbreviation index
    memcpy(b + 50, abbr, alen);
    size_t len = 50 + alen;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int ok = write(fd, b, len) == (ssize_t) len;
    close(fd);
    // A replacement, as a package manager or ln -sf makes one: a new file.
    return ok ? rename(tmp, path) : -1;
}

// Runs `script` under native dash chrooted in `root`, stdout to root/out.
static pid_t spawn_in(const char *root, const char *script) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    if (chroot(root) != 0 || chdir("/") != 0)
        _exit(120);
    int fd = open("/out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        _exit(121);
    dup2(fd, 1);
    close(fd);
    unsetenv("TZ");
    execl("/AOK/native/dash", "dash", "-c", script, (char *) NULL);
    _exit(122);
}

static int wait_ok(pid_t pid) {
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Lines of root/out equal to `want`, and lines in all.
static void count_out(const char *root, const char *want, int *match, int *total) {
    char path[128], line[64];
    snprintf(path, sizeof(path), "%s/out", root);
    *match = *total = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        (*total)++;
        if (!strcmp(line, want))
            (*match)++;
    }
    fclose(f);
}

static char *one_date(const char *root, char *buf, size_t size) {
    buf[0] = '\0';
    if (!wait_ok(spawn_in(root, "/AOK/native/smallclue date +%Z")))
        return buf;
    char path[128];
    snprintf(path, sizeof(path), "%s/out", root);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        if (fgets(buf, (int) size, f) != NULL)
            buf[strcspn(buf, "\n")] = '\0';
        fclose(f);
    }
    return buf;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(240));
    if (access("/AOK/native/smallclue", X_OK) != 0 || access("/AOK/native/dash", X_OK) != 0) {
        printf("native_tz_roots: SKIP (no native smallclue or dash)\n");
        return 0;
    }

    snprintf(base, sizeof(base), "/tmp/native_tz_roots.XXXXXX");
    if (mkdtemp(base) == NULL) {
        check(0, "mkdtemp");
        return finish_suite("native_tz_roots");
    }
    char root[2][96], dir[160];
    const char *zone[2] = {"AAA", "BBB"};
    const int32_t off[2] = {3 * 3600, -5 * 3600};
    int mounted[2] = {0, 0};
    int skipped = 0;
    for (int i = 0; i < 2; i++) {
        snprintf(root[i], sizeof(root[i]), "%s/%c", base, 'a' + i);
        mkdir(root[i], 0755);
        snprintf(dir, sizeof(dir), "%s/etc", root[i]);
        mkdir(dir, 0755);
        snprintf(dir, sizeof(dir), "%s/AOK", root[i]);
        mkdir(dir, 0755);
        if (mount("/AOK", dir, NULL, MS_BIND, NULL) != 0) {
            printf("native_tz_roots: SKIP (bind mount: %s; needs root)\n", strerror(errno));
            skipped = 1;
            goto out;
        }
        mounted[i] = 1;
        snprintf(dir, sizeof(dir), "%s/etc/localtime", root[i]);
        if (write_zone(dir, off[i], zone[i]) != 0) {
            check(0, "write a zone file");
            goto out;
        }
    }

    // Each root alone first: the zone files are read at all.
    char got[64];
    check(!strcmp(one_date(root[0], got, sizeof(got)), "AAA"), "chroot a alone: AAA");
    check(!strcmp(one_date(root[1], got, sizeof(got)), "BBB"), "chroot b, straight after a: BBB");

    // Both at once.
    char script[160];
    snprintf(script, sizeof(script),
             "i=0; while [ $i -lt %d ]; do /AOK/native/smallclue date +%%Z; i=$((i+1)); done", LOOPS);
    pid_t pa = spawn_in(root[0], script);
    pid_t pb = spawn_in(root[1], script);
    int ok_a = wait_ok(pa), ok_b = wait_ok(pb);
    check(ok_a && ok_b, "both loops ran");
    for (int i = 0; i < 2; i++) {
        int match, total;
        count_out(root[i], zone[i], &match, &total);
        char what[96];
        snprintf(what, sizeof(what), "chroot %c: %d of %d dates say %s", 'a' + i, match, total, zone[i]);
        check(total == LOOPS && match == LOOPS, what);
    }

    // A replaced /etc/localtime is seen by the next program, at once.
    snprintf(dir, sizeof(dir), "%s/etc/localtime", root[0]);
    for (int round = 0; round < 4; round++) {
        int j = (round + 1) % 2;
        write_zone(dir, off[j], zone[j]);
        char what[96];
        snprintf(what, sizeof(what), "after replacing a's file with %s, a date says it (%s)",
                 zone[j], one_date(root[0], got, sizeof(got)));
        check(!strcmp(got, zone[j]), what);
    }

out:
    for (int i = 0; i < 2; i++) {
        snprintf(dir, sizeof(dir), "%s/AOK", root[i]);
        if (mounted[i])
            umount(dir);
        rmdir(dir);
        static const char *const files[] = {"out", "etc/localtime", "etc/localtime.new"};
        for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
            snprintf(dir, sizeof(dir), "%s/%s", root[i], files[f]);
            unlink(dir);
        }
        snprintf(dir, sizeof(dir), "%s/etc", root[i]);
        rmdir(dir);
        rmdir(root[i]);
    }
    rmdir(base);
    if (skipped)
        return 0;
    return finish_suite("native_tz_roots");
}
