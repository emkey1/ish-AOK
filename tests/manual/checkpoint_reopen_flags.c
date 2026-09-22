// checkpoint_reopen_flags.c -- a restore reopens descriptors; it must not
// replay the side effects of the open that made them. Driven by
// checkpoint_reopen_flags.sh.
//
// fd->flags keeps everything open() was given, and the restore used to hand
// it back verbatim:
//   O_TRUNC        -- the file was emptied. What had been written before the
//                     checkpoint became NULs up to the restored offset.
//   O_CREAT|O_EXCL -- mkstemp's flags. The file exists, so the reopen was
//                     EEXIST, and the degrade gave the process /dev/null: every
//                     later write to its temp file went nowhere.
//
// And the flags a descriptor must KEEP. A pipe is rebuilt rather than
// reopened, and it came back blocking whatever it had been: a restored tmux
// server, whose event loop drains its non-blocking signal pipe until EAGAIN,
// sat in read() for good on the first signal after a resume.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void show(const char *tag, const char *path) {
    char buf[256]; int f = open(path, O_RDONLY); ssize_t n = f >= 0 ? read(f, buf, sizeof(buf) - 1) : -1;
    if (f >= 0) close(f);
    printf("%s=[", tag);
    for (ssize_t i = 0; i < n; i++) putchar(buf[i] == 0 ? '@' : buf[i] == '\n' ? '|' : buf[i]);
    printf("]\n"); fflush(stdout);
}
int main(void) {
    unlink("/tmp/ckrf-trunc");
    int t = open("/tmp/ckrf-trunc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char tmpl[] = "/tmp/ckrf-excl-XXXXXX";
    int e = mkstemp(tmpl);                       // O_RDWR|O_CREAT|O_EXCL
    if (t < 0 || e < 0) { perror("open"); return 1; }
    write(t, "before\n", 7);
    write(e, "before\n", 7);
    int nb[2], blk[2];
    if (pipe2(nb, O_NONBLOCK) < 0 || pipe(blk) < 0) { perror("pipe"); return 1; }
    // The checkpoint lands in here, and the script kills this run as soon as
    // the image exists -- the way iOS kills the app. A saving run that went
    // on to write "after" itself would make a broken restore look fine.
    for (int i = 0; i < 8; i++) sleep(1);
    write(t, "after\n", 6);
    write(e, "after\n", 6);
    show("TRUNC", "/tmp/ckrf-trunc");
    show("EXCL", tmpl);
    int nbr = !!(fcntl(nb[0], F_GETFL) & O_NONBLOCK);
    int nbw = !!(fcntl(nb[1], F_GETFL) & O_NONBLOCK);
    int bl = !!(fcntl(blk[0], F_GETFL) & O_NONBLOCK);
    alarm(4);                       // blocking where it should not is the bug
    char c;
    ssize_t r = read(nb[0], &c, 1);
    int re = r < 0 ? errno : 0;
    alarm(0);
    printf("PIPE=[nonblock %d/%d blocking %d empty-read %zd/%s]\n", nbr, nbw, bl, r,
           re == EAGAIN ? "EAGAIN" : "other");
    fflush(stdout);
    return 0;
}
