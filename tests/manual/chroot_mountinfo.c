// /proc/{self,<pid>}/mountinfo and /proc/mounts are relative to the task's root
// (Linux semantics).
//
// Linux prints each mount point relative to the root of the task the listing
// belongs to and leaves out every mount outside that root (seq_path_root's
// SEQ_SKIP); a chroot to a plain directory therefore sees no "/" line at all.
// AOK printed every mount with its global path whatever the reader's root, so
// inside a chroot the listing named paths that did not exist there, and a bind
// made inside the chroot never matched its own path -- mount_bind_rbind's
// rbind.self_mounted failed on every device leg run through mount-root.sh's
// chroot (555, 556) while passing on a booted root.
//
// Requires privilege (mount, chroot): run as root, or on a Linux oracle under
// `unshare -rm`.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static int fails;
#define CHECK(cond, ...) do { if (cond) { test_logf("ok: " __VA_ARGS__); test_logf("\n"); } \
    else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// Field `field` (0-based, space-separated) of every line of `path`, into a
// newline-separated list.
static void fields(const char *path, int field, char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *save = NULL, *tok = strtok_r(line, " \n", &save);
        for (int i = 0; i < field && tok != NULL; i++)
            tok = strtok_r(NULL, " \n", &save);
        if (tok != NULL && strlen(out) + strlen(tok) + 2 < cap) {
            strcat(out, tok);
            strcat(out, "\n");
        }
    }
    fclose(f);
}

static int has_line(const char *list, const char *want) {
    size_t n = strlen(want);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        if (len == n && strncmp(p, want, n) == 0)
            return 1;
        p += len + (e != NULL);
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (geteuid() != 0) {
        printf("chroot_mountinfo: SKIP (needs root)\n");
        return 0;
    }
    char base[64] = "/tmp/chrmi_XXXXXX", jail[128], mnt[128], src[128], proc[128];
    if (mkdtemp(base) == NULL) { perror("mkdtemp"); return 2; }
    snprintf(jail, sizeof jail, "%s/jail", base);
    snprintf(mnt, sizeof mnt, "%s/jail/mnt", base);
    snprintf(proc, sizeof proc, "%s/jail/proc", base);
    snprintf(src, sizeof src, "%s/src", base);
    mkdir(jail, 0755); mkdir(mnt, 0755); mkdir(proc, 0755); mkdir(src, 0755);
    if (mount(src, mnt, NULL, MS_BIND, NULL) != 0 || mount("/proc", proc, NULL, MS_BIND | MS_REC, NULL) != 0) {
        printf("FAIL: setup mounts: %s\n", strerror(errno));
        return 1;
    }

    int to_child[2], to_parent[2];
    if (pipe(to_child) != 0 || pipe(to_parent) != 0) { perror("pipe"); return 2; }
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        if (chroot(jail) != 0 || chdir("/") != 0)
            _exit(3);
        static char mi[16384], mounts[16384];
        fields("/proc/self/mountinfo", 4, mi, sizeof mi);
        fields("/proc/mounts", 1, mounts, sizeof mounts);
        int bad = 0;
#define CCHECK(cond, ...) do { if (!(cond)) { printf("FAIL: chrooted: " __VA_ARGS__); printf("\n"); bad++; } } while (0)
        CCHECK(has_line(mi, "/mnt"), "mountinfo lists the bind at /mnt");
        CCHECK(has_line(mi, "/proc"), "mountinfo lists /proc");
        CCHECK(strstr(mi, base) == NULL, "mountinfo names no path outside the root (%s)", base);
        CCHECK(!has_line(mi, "/"), "mountinfo has no \"/\": the root is a plain directory");
        CCHECK(has_line(mounts, "/mnt"), "/proc/mounts lists /mnt");
        CCHECK(strstr(mounts, base) == NULL, "/proc/mounts names no path outside the root");
        // A bind made inside the chroot is listed under its chrooted path.
        mkdir("/mnt/inner", 0755);
        if (mount("/mnt", "/mnt/inner", NULL, MS_BIND | MS_REC, NULL) == 0) {
            fields("/proc/self/mountinfo", 4, mi, sizeof mi);
            CCHECK(has_line(mi, "/mnt/inner"), "a bind made in the chroot is listed as /mnt/inner");
            umount("/mnt/inner");
        } else {
            CCHECK(0, "bind inside the chroot: %s", strerror(errno));
        }
        // Let the parent read /proc/<pid>/mountinfo while we are in here.
        if (write(to_parent[1], "r", 1) != 1 || read(to_child[0], &c, 1) != 1)
            bad++;
        fflush(stdout);
        _exit(bad);
    }
    char c;
    if (read(to_parent[0], &c, 1) != 1) {
        printf("FAIL: child did not reach the chroot\n");
        fails++;
    } else {
        char path[64], mi[16384], own[16384];
        snprintf(path, sizeof path, "/proc/%d/mountinfo", (int) pid);
        fields(path, 4, mi, sizeof mi);
        fields("/proc/self/mountinfo", 4, own, sizeof own);
        CHECK(has_line(mi, "/mnt") && strstr(mi, base) == NULL,
              "/proc/<chrooted pid>/mountinfo read from outside is relative to that task's root");
        CHECK(has_line(own, mnt), "the reader's own mountinfo still has the global path %s", mnt);
        if (write(to_child[1], "x", 1) != 1)
            fails++;
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: chrooted checks: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        fails++;
    }
    umount2(proc, MNT_DETACH);
    umount(mnt);
    rmdir(mnt); rmdir(proc); rmdir(jail); rmdir(src); rmdir(base);
    printf("chroot_mountinfo: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
