// native_stty.c -- SmallCLUE's stty against GNU coreutils 9's answers.
//
// SmallCLUE's stty knew `reset`, `sane` and `ixon` and nothing else: no -g,
// so aok-sdl-game's keystroke pause had to call the distro's /bin/stty, and
// its `sane` never touched termios at all. The rewrite matches GNU stty on
// Linux; this checks it on a pty of its own. The -a and short listings are
// GNU's output for the same states, recorded in build/devuan-amd64-test.
//
// The Linux-only flags (xcase, iuclc, olcuc) are the witness that stty reaches
// the guest's termios directly: through Darwin's struct termios they cannot
// be set at all, and stty reports "unable to perform all requested operations".
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define DIR "/tmp/native_stty_test"
#define STTY DIR "/stty"

static const char gold_all[] =
    "speed 38400 baud; rows 24; columns 80; line = 0;\n"
    "intr = ^C; quit = ^\\; erase = ^?; kill = ^U; eof = ^D; eol = <undef>;\n"
    "eol2 = <undef>; swtch = <undef>; start = ^Q; stop = ^S; susp = ^Z; rprnt = ^R;\n"
    "werase = ^W; lnext = ^V; discard = ^O; min = 1; time = 0;\n"
    "-parenb -parodd -cmspar cs8 -hupcl -cstopb cread -clocal -crtscts\n"
    "-ignbrk brkint -ignpar -parmrk -inpck -istrip -inlcr -igncr icrnl ixon -ixoff\n"
    "-iuclc -ixany imaxbel -iutf8\n"
    "opost -olcuc -ocrnl onlcr -onocr -onlret -ofill -ofdel nl0 cr0 tab0 bs0 vt0 ff0\n"
    "isig icanon iexten echo echoe echok -echonl -noflsh -xcase -tostop -echoprt\n"
    "echoctl echoke -flusho -extproc\n";
static const char gold_short_sane[] = "speed 38400 baud; line = 0;\n";
static const char gold_short_raw[] =
    "speed 38400 baud; line = 0;\n"
    "min = 1; time = 0;\n"
    "-brkint -icrnl -imaxbel\n"
    "-opost\n"
    "-isig -icanon -echo\n";

static int slave_fd = -1;

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

// Runs stty with stdin on the pty; returns its exit status, stdout in `out`.
static int run_stty(char *out, size_t out_size, const char *a0, ...) {
    const char *args[24] = {"stty"};
    int n = 1;
    va_list ap;
    va_start(ap, a0);
    for (const char *a = a0; a && n < 23; a = va_arg(ap, const char *))
        args[n++] = a;
    va_end(ap);
    args[n] = NULL;
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(slave_fd, 0);
        dup2(pipefd[1], 1);
        dup2(null, 2);
        close(pipefd[0]);
        unsetenv("COLUMNS");
        execv(STTY, (char *const *)args);
        _exit(127);
    }
    close(pipefd[1]);
    size_t len = 0;
    for (;;) {
        if (len + 1 >= out_size) break;
        ssize_t r = read(pipefd[0], out + len, out_size - 1 - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    out[len] = '\0';
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void show_diff(const char *want, const char *got) {
    printf("  want:\n%s  got:\n%s", want, got);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_stty: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(120));
    mkdir(DIR, 0755);
    unlink(STTY);
    if (symlink(SMALLCLUE, STTY) != 0) {
        printf("FAIL symlink %s: %s\n", STTY, strerror(errno));
        return 1;
    }
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        printf("FAIL posix_openpt: %s\n", strerror(errno));
        return 1;
    }
    slave_fd = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        printf("FAIL open %s: %s\n", ptsname(master), strerror(errno));
        return 1;
    }
    struct winsize ws = {.ws_row = 24, .ws_col = 80};
    ioctl(slave_fd, TIOCSWINSZ, &ws);

    char out[8192];
    struct termios t;

    // A sane terminal, listed in full and in short, exactly as GNU does.
    check("sane.status", run_stty(out, sizeof(out), "sane", NULL) == 0);
    run_stty(out, sizeof(out), "-a", NULL);
    check("all.gnu_text", strcmp(out, gold_all) == 0);
    if (strcmp(out, gold_all) != 0) show_diff(gold_all, out);
    run_stty(out, sizeof(out), NULL);
    check("short.sane_gnu_text", strcmp(out, gold_short_sane) == 0);
    if (strcmp(out, gold_short_sane) != 0) show_diff(gold_short_sane, out);

    // -g is GNU's format and agrees with the guest's own tcgetattr.
    tcgetattr(slave_fd, &t);
    char want_g[512];
    int off = snprintf(want_g, sizeof(want_g), "%lx:%lx:%lx:%lx", (unsigned long)t.c_iflag,
                       (unsigned long)t.c_oflag, (unsigned long)t.c_cflag, (unsigned long)t.c_lflag);
    for (int i = 0; i < 32; i++)
        off += snprintf(want_g + off, sizeof(want_g) - (size_t)off, ":%lx",
                        i < NCCS ? (unsigned long)t.c_cc[i] : 0UL);
    snprintf(want_g + off, sizeof(want_g) - (size_t)off, "\n");
    char saved[512];
    run_stty(saved, sizeof(saved), "-g", NULL);
    check("save.matches_tcgetattr", strcmp(saved, want_g) == 0);
    if (strcmp(saved, want_g) != 0) show_diff(want_g, saved);
    saved[strcspn(saved, "\n")] = '\0';

    // raw, and its short listing.
    check("raw.status", run_stty(out, sizeof(out), "raw", "-echo", NULL) == 0);
    tcgetattr(slave_fd, &t);
    check("raw.icanon_off", !(t.c_lflag & ICANON));
    check("raw.echo_off", !(t.c_lflag & ECHO));
    check("raw.isig_off", !(t.c_lflag & ISIG));
    check("raw.opost_off", !(t.c_oflag & OPOST));
    check("raw.iflag_clear", t.c_iflag == 0);
    run_stty(out, sizeof(out), NULL);
    check("short.raw_gnu_text", strcmp(out, gold_short_raw) == 0);
    if (strcmp(out, gold_short_raw) != 0) show_diff(gold_short_raw, out);

    // Control characters in each spelling, and min/time.
    check("cc.status", run_stty(out, sizeof(out), "intr", "^X", "erase", "^H", "kill", "undef",
                                "eof", "0x5", "min", "0", "time", "3", NULL) == 0);
    tcgetattr(slave_fd, &t);
    check("cc.intr", t.c_cc[VINTR] == 0x18);
    check("cc.erase", t.c_cc[VERASE] == 0x08);
    check("cc.kill_undef", t.c_cc[VKILL] == 0);
    check("cc.eof_hex", t.c_cc[VEOF] == 5);
    check("cc.min", t.c_cc[VMIN] == 0);
    check("cc.time", t.c_cc[VTIME] == 3);

    // Linux-only flags: only reachable through the guest's own termios.
    check("linux_only.status", run_stty(out, sizeof(out), "xcase", "iuclc", "olcuc", NULL) == 0);
    tcgetattr(slave_fd, &t);
    check("linux_only.xcase", (t.c_lflag & XCASE) != 0);
    check("linux_only.iuclc", (t.c_iflag & IUCLC) != 0);
    check("linux_only.olcuc", (t.c_oflag & OLCUC) != 0);

    // Restoring the saved string puts every word back.
    check("restore.status", run_stty(out, sizeof(out), saved, NULL) == 0);
    char again[512];
    run_stty(again, sizeof(again), "-g", NULL);
    again[strcspn(again, "\n")] = '\0';
    check("restore.round_trip", strcmp(again, saved) == 0);
    if (strcmp(again, saved) != 0) printf("  want %s\n  got  %s\n", saved, again);

    // Window size.
    run_stty(out, sizeof(out), "size", NULL);
    check("size.24x80", strcmp(out, "24 80\n") == 0);
    check("size.set", run_stty(out, sizeof(out), "rows", "30", "cols", "100", NULL) == 0);
    run_stty(out, sizeof(out), "size", NULL);
    check("size.30x100", strcmp(out, "30 100\n") == 0);

    // GNU's exit statuses for what it refuses.
    check("error.invalid", run_stty(out, sizeof(out), "bogus", NULL) == 1);
    check("error.styles", run_stty(out, sizeof(out), "-g", "-a", NULL) == 1);
    check("error.missing", run_stty(out, sizeof(out), "intr", NULL) == 1);
    check("error.range", run_stty(out, sizeof(out), "min", "300", NULL) == 1);

    close(slave_fd);
    close(master);
    unlink(STTY);
    return finish_suite("native_stty");
}
