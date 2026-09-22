// checkpoint_tty_descriptions.c -- descriptors on a terminal, across a
// checkpoint. Driven by checkpoint_tty_descriptions.sh.
//
// The restore used to rebuild ONE descriptor per terminal and hand it to every
// record that named a terminal, whatever the record's own description was:
//
//   - the flags were never put back, so a description a program had made
//     non-blocking came back blocking. tmux does exactly that to the terminal
//     of every client it serves, and a server blocked writing to a terminal
//     nobody is draining stops serving every pane;
//   - a program's SEPARATE open of its terminal (less, vi and ssh open
//     /dev/tty) was merged into the session's shared one, flags and all;
//   - a descriptor on a terminal that is not the holder's own -- a daemon
//     holding a session's terminal, the way the tmux server holds each
//     attached client's -- was handed the holder's OWN standard streams: a
//     different terminal, or none.
//
// Three descriptions of one terminal are set up here, each with its own flags:
//   D1  the session child's standard streams (fds 0-2, one description),
//       made non-blocking -- and shared, so fd 1 reports it too;
//   D3  the same child's separate open of /dev/tty, left blocking;
//   D2  a daemon's open of the terminal, non-blocking, in a process with no
//       terminal of its own. Run twice by the script -- the daemon created
//       before the session child and after it -- because the image orders
//       siblings by age, so in one of the two runs D2 is named before the
//       terminal it is on has been rebuilt at all.
// After the restore all three must be the same terminal as each other, and
// each must have its own flags.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : !!(fl & O_NONBLOCK);
}

static unsigned long long rdev(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? (unsigned long long) st.st_rdev : 0;
}

static void to_null(void) {
    int n = open("/dev/null", O_RDWR);
    dup2(n, 0);
    dup2(n, 1);
    dup2(n, 2);
    if (n > 2)
        close(n);
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static pid_t start_daemon(const char *tpath) {
    pid_t daemon = fork();
    if (daemon == 0) {
        setsid();
        int t = open(tpath, O_RDWR | O_NOCTTY | O_NONBLOCK);
        dup2(t, 5);
        if (t != 5)
            close(t);
        to_null();
        while (!exists("/tmp/ckt.go"))
            usleep(50000);
        FILE *f = fopen("/tmp/ckt.daemon.tmp", "w");
        fprintf(f, "%d %d %llu\n", isatty(5), nonblock(5), rdev(5));
        fclose(f);
        rename("/tmp/ckt.daemon.tmp", "/tmp/ckt.daemon");
        _exit(0);
    }
    return daemon;
}

int main(int argc, char **argv) {
    int late = argc > 1 && strcmp(argv[1], "late") == 0;
    char tpath[64];
    if (ttyname_r(0, tpath, sizeof(tpath)) != 0) {
        printf("NO-TERMINAL\n");
        return 1;
    }
    unlink("/tmp/ckt.go");
    unlink("/tmp/ckt.daemon");

    // D2: the daemon. Its own session, no terminal of its own.
    pid_t daemon = late ? 0 : start_daemon(tpath);
    usleep(300000);
    pid_t child = fork();
    if (child == 0) {
        // D1 non-blocking, shared by fds 0-2; D3 a separate, blocking open.
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        int d3 = open("/dev/tty", O_RDWR);
        dup2(d3, 6);
        if (d3 != 6)
            close(d3);
        usleep(late ? 900000 : 300000);
        int c = open("/proc/ish/checkpoint", O_WRONLY);
        if (c < 0 || write(c, "suspend", 7) != 7) {
            printf("SUSPEND-REFUSED errno=%d\n", errno);
            return 1;
        }
        close(c);
        // Only a restored run gets here: the suspend halted the one that saved.
        int fd = open("/tmp/ckt.go", O_WRONLY | O_CREAT, 0644);
        close(fd);
        for (int i = 0; i < 100 && !exists("/tmp/ckt.daemon"); i++)
            usleep(50000);
        int d_tty = -1, d_nb = -1;
        unsigned long long d_rdev = 0;
        FILE *f = fopen("/tmp/ckt.daemon", "r");
        if (f != NULL) {
            if (fscanf(f, "%d %d %llu", &d_tty, &d_nb, &d_rdev) != 3)
                d_tty = -2;
            fclose(f);
        }
        unsigned long long r0 = rdev(0);
        int nb0 = nonblock(0), nb1 = nonblock(1), nb6 = nonblock(6);
        // Leave the session's description as it was found: blocking.
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
        printf("D1 tty=%d nonblock=%d fd1-shares=%d\n", isatty(0), nb0, nb1);
        printf("D3 tty=%d nonblock=%d same-terminal=%d\n", isatty(6), nb6,
               rdev(6) == r0);
        printf("D2 tty=%d nonblock=%d same-terminal=%d\n", d_tty, d_nb,
               d_rdev == r0 && r0 != 0);
        printf("DESCRIPTIONS-DONE\n");
        fflush(stdout);
        return 0;
    }

    if (late)
        daemon = start_daemon(tpath);
    // The leader keeps no terminal of its own, so the image's first process
    // on this terminal is the child.
    to_null();
    int st;
    waitpid(child, &st, 0);
    waitpid(daemon, &st, 0);
    return 0;
}
