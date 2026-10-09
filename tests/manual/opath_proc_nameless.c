// opath_proc_nameless.c -- O_PATH through /proc/self/fd/N on what has no name
// to reopen by: a pipe, a socket, an eventfd, a directory removed while held.
// Linux's walk jumps to the file the descriptor holds, so each opens: an
// O_PATH handle whose fstat is the original's (type, inode, device), whose own
// /proc link reads the same, which reads nothing (EBADF), and which O_DIRECTORY
// refuses when it is not one. Opening the pipe again through the handle's link
// opens the pipe; a socket opens through /proc as nothing (ENXIO). AOK walked the link's text ("pipe:[N]"...): ENOENT. Checked on
// camd (Linux 6.12), 64- and 32-bit builds.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int opath_of(int fd, int extra) {
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    return open(link, O_PATH | extra);
}

static void link_text(int fd, char *buf, size_t size) {
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, size - 1);
    buf[n >= 0 ? n : 0] = '\0';
}

static void check_handle(const char *what, int fd, mode_t type) {
    int h = opath_of(fd, 0);
    CHECK(h >= 0, "%s: O_PATH through /proc/self/fd/%d: %s", what, fd, strerror(errno));
    if (h < 0)
        return;
    struct stat a, b;
    fstat(fd, &a);
    int r = fstat(h, &b);
    CHECK(r == 0 && (b.st_mode & S_IFMT) == type && b.st_ino == a.st_ino && b.st_dev == a.st_dev,
          "%s: fstat of the handle: mode %#o ino %llu dev %#llx (the original's %#o %llu %#llx)", what,
          (unsigned) b.st_mode, (unsigned long long) b.st_ino, (unsigned long long) b.st_dev,
          (unsigned) a.st_mode, (unsigned long long) a.st_ino, (unsigned long long) a.st_dev);
    char ta[256], tb[256];
    link_text(fd, ta, sizeof ta);
    link_text(h, tb, sizeof tb);
    CHECK(strcmp(ta, tb) == 0, "%s: the handle's link reads \"%s\", the original's \"%s\"", what, tb, ta);
    char c;
    errno = 0;
    CHECK(read(h, &c, 1) < 0 && errno == EBADF, "%s: read through the handle: errno %d (want EBADF)", what, errno);
    int fl = fcntl(h, F_GETFL);
    CHECK(fl >= 0 && (fl & O_PATH), "%s: F_GETFL %#x lacks O_PATH", what, fl);
    if (type != S_IFDIR) {
        errno = 0;
        int d = opath_of(fd, O_DIRECTORY);
        CHECK(d < 0 && errno == ENOTDIR, "%s: O_PATH|O_DIRECTORY: %d errno %d (want ENOTDIR)", what, d, errno);
        if (d >= 0)
            close(d);
    }
    close(h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int p[2];
    pipe(p);
    check_handle("pipe", p[0], S_IFIFO);
    // the pipe again, through the O_PATH handle's own link: its read end
    // (AOK cannot open a pipe for the other direction through /proc: its pipes
    // are host pipes, and a read end holds no write end -- docs/TODO.md)
    int h = opath_of(p[0], 0);
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", h);
    int rd = open(link, O_RDONLY);
    CHECK(rd >= 0, "the pipe reopened through the handle's link: %s", strerror(errno));
    if (rd >= 0) {
        char c = 'x', got = 0;
        write(p[1], &c, 1);
        read(rd, &got, 1);
        CHECK(got == 'x', "a byte written to the pipe arrived through the reopened end as %#x", got);
        close(rd);
    }
    close(h);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    check_handle("socket", sv[0], S_IFSOCK);
    // a socket opens through /proc as nothing: ENXIO
    errno = 0;
    snprintf(link, sizeof link, "/proc/self/fd/%d", sv[0]);
    int so = open(link, O_RDWR);
    CHECK(so < 0 && errno == ENXIO, "open(socket link, O_RDWR): %d errno %d (want ENXIO)", so, errno);

    int ev = eventfd(0, 0);
    struct stat es;
    fstat(ev, &es);
    check_handle("eventfd", ev, es.st_mode & S_IFMT);

    char dir[] = "/tmp/opn.XXXXXX";
    mkdtemp(dir);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    rmdir(dir);
    check_handle("a removed directory", dfd, S_IFDIR);

    printf("opath_proc_nameless: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
