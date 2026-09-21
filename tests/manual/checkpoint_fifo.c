// checkpoint_fifo.c -- named FIFOs across a checkpoint. Driven by
// checkpoint_fifo.sh, which runs it with a tmpfs on /run, as on device.
//
// A named FIFO had no restore rule. With no host descriptor behind it, it had
// real_fd 0 and came back as a copy of the standard input -- sysvinit's
// /run/initctl became /dev/null. Once the standard-stream rule was made exact,
// it was refused instead, and every suspend of a sysvinit guest failed.
//
// Checked after the restore:
//   INITCTL  an O_RDWR|O_NONBLOCK holder, the way init keeps /run/initctl:
//            same flags, and bytes still round-trip through it.
//   BUFFER   bytes written before the checkpoint and not yet read are there.
//   JOIN     the writer and the reader, separate opens, still share one FIFO.
//   EPIPE    the writer is restored BEFORE its reader, so its reopen had to be
//            deferred (O_WRONLY|O_NONBLOCK with no reader is ENXIO). Once the
//            reader has gone its next write must fail EPIPE -- if the deferral
//            left it attached read-write, its own phantom read end would
//            swallow the write instead.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(const char *what, int ok, const char *detail) {
    printf("%s %s: %s\n", ok ? "OK" : "FAIL", what, detail);
    fflush(stdout);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unlink("/run/ckinitctl");
    unlink("/run/ckfifo");
    mkfifo("/run/ckinitctl", 0600);
    mkfifo("/run/ckfifo", 0600);
    int ictl = open("/run/ckinitctl", O_RDWR | O_NONBLOCK);

    int ready[2];
    pipe(ready);
    pid_t child = fork();
    if (child == 0) {
        int rfd = open("/run/ckfifo", O_RDONLY);        // waits for the writer
        write(ready[1], "r", 1);
        for (int i = 0; i < 8; i++) sleep(1);           // checkpoint in here
        char buf[64] = {0};
        size_t got = 0;
        for (int i = 0; i < 40 && got < 13; i++) {       // "before|after|"
            ssize_t n = read(rfd, buf + got, sizeof(buf) - 1 - got);
            if (n > 0) got += (size_t) n;
            else usleep(50000);
        }
        printf("CHILD-READ=[%s]\n", buf);
        fflush(stdout);
        _exit(0);                                       // the reader goes
    }
    int wfd = open("/run/ckfifo", O_WRONLY);            // blocking, like most
    char c;
    read(ready[0], &c, 1);
    write(wfd, "before|", 7);
    for (int i = 0; i < 6; i++) sleep(1);               // checkpoint in here

    char d[128];
    // INITCTL
    int fl = fcntl(ictl, F_GETFL);
    int wr = (int) write(ictl, "ping", 4);
    char ib[8] = {0};
    int rd = (int) read(ictl, ib, sizeof(ib) - 1);
    snprintf(d, sizeof(d), "rdwr=%d nonblock=%d wrote=%d read=%d [%s]",
             (fl & O_ACCMODE) == O_RDWR, (fl & O_NONBLOCK) != 0, wr, rd, ib);
    check("INITCTL", (fl & O_ACCMODE) == O_RDWR && (fl & O_NONBLOCK) &&
                     wr == 4 && rd == 4 && strcmp(ib, "ping") == 0, d);

    // The writer's own flags: write-only and blocking, as it was opened.
    fl = fcntl(wfd, F_GETFL);
    snprintf(d, sizeof(d), "wronly=%d nonblock=%d", (fl & O_ACCMODE) == O_WRONLY,
             (fl & O_NONBLOCK) != 0);
    check("WRITER-FLAGS", (fl & O_ACCMODE) == O_WRONLY && !(fl & O_NONBLOCK), d);

    // JOIN: this reaches the child through the FIFO both reopened by path.
    write(wfd, "after|", 6);
    int st = 0;
    waitpid(child, &st, 0);

    // EPIPE: the reader is gone now.
    errno = 0;
    ssize_t n = write(wfd, "late", 4);
    snprintf(d, sizeof(d), "write=%zd errno=%d (EPIPE=%d)", n, errno, EPIPE);
    check("EPIPE", n == -1 && errno == EPIPE, d);
    return 0;
}
