// The native ping and wget take the options Linux scripts pass them.
//
// A root with native links runs SmallCLUE's ping and wget (/usr/local/bin is
// first on PATH). Both had only their own few options: `ping -c 2 -W 2 host`
// was "illegal option -- W", `wget -q -T 6 -O file URL` "illegal option -- T",
// and wget saved a server's 404 page under the file's name and exited 0. A
// script that probes the network with either saw every probe fail, or took
// an error page for a download (TestFlight feedback on build 555).
//
// Nothing here leaves the machine: ping goes to 127.0.0.1, wget to a server
// this test runs on loopback, which answers by path --
//
//   /ok       200 and a body
//   /missing  404 and an error page
//   /stall    accepts the request and never answers
//
// Checked, each with the exit status Linux's ping or GNU wget gives:
//
//   ping -c 2 -W 1 -i 0.2    two replies, "0% packet loss", status 0, and the
//                            interval honoured (well under the 1 s default)
//   ping -q -c 1             the summary without the reply line
//   ping -w 1 -i 0.2         no count: runs to the deadline, about a second,
//                            three echoes at least
//   ping -c 1 -s 1000        "1008 bytes from"
//   ping -c 0                status 2
//   wget -q -T 5 -O f /ok    status 0 and the body in f
//   wget -q /missing         status 8, and no file named for the page
//   wget -q --spider         0 for /ok, 8 for /missing, nothing saved
//   wget -q -T 1 /stall      status 4 after about a second -- the control for
//                            the timeout: /ok with the same -T is status 0
//   wget -t 2 -T 1 /stall    asked twice
//   ssh to a closed port     "Connection refused", status 255. A non-blocking
//                            connect's result is read back with
//                            getsockopt(SO_ERROR), whose value came through as
//                            the guest's Linux number: 111, for which Darwin's
//                            strerror has no text, so ssh said
//                            "Unknown error: 111".
//
// SKIPs where there is no native smallclue.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SMALLCLUE "/AOK/native/smallclue"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("native_net_options: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int listener;
static int stalled;   // requests for /stall so far

static void *serve(void *unused) {
    (void) unused;
    for (;;) {
        int c = accept(listener, NULL, NULL);
        if (c < 0)
            return NULL;
        char req[2048];
        size_t used = 0;
        ssize_t n;
        while (used < sizeof(req) - 1 && (n = read(c, req + used, sizeof(req) - 1 - used)) > 0) {
            used += (size_t) n;
            req[used] = '\0';
            if (strstr(req, "\r\n\r\n") != NULL)
                break;
        }
        req[used] = '\0';
        if (strstr(req, " /stall ") != NULL) {
            // Kept open and never answered; the descriptor is left to the
            // end of the test.
            __sync_fetch_and_add(&stalled, 1);
            continue;
        }
        const char *reply = strstr(req, " /ok ") != NULL
            ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: close\r\n\r\nhello\n"
            : "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 10\r\nConnection: close\r\n\r\nnot here.\n";
        // HEAD (--spider) gets the same headers and no body.
        size_t len = strlen(reply);
        if (strncmp(req, "HEAD ", 5) == 0)
            len = (size_t) (strstr(reply, "\r\n\r\n") + 4 - reply);
        (void) !write(c, reply, len);
        close(c);
    }
}

// Runs an applet; stdout into `out`. Returns the exit status, or -1 if it
// did not exit; *ms is how long it took.
static bool want_stderr;   // run() collects stderr with stdout when set

static int run(char *out, size_t out_size, long *ms, const char *applet, ...) {
    char *argv[24];
    int argc = 0;
    argv[argc++] = (char *) applet;
    va_list ap;
    va_start(ap, applet);
    const char *arg;
    while (argc < 23 && (arg = va_arg(ap, const char *)) != NULL)
        argv[argc++] = (char *) arg;
    va_end(ap);
    argv[argc] = NULL;

    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fds[1], 1);
        dup2(want_stderr ? fds[1] : null, 2);
        close(fds[0]);
        close(fds[1]);
        // The applet is chosen by the name it is run under.
        execv(SMALLCLUE, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t used = 0;
    ssize_t n;
    while (used < out_size - 1 && (n = read(fds[0], out + used, out_size - 1 - used)) > 0)
        used += (size_t) n;
    out[used] = '\0';
    close(fds[0]);
    int status = -1;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (ms != NULL)
        *ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

// ssh to a port that was just bound and closed, so nothing listens on it.
static void check_ssh_refused(char *out, size_t out_size) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in closed = {.sin_family = AF_INET};
    inet_pton(AF_INET, "127.0.0.1", &closed.sin_addr);
    socklen_t len = sizeof(closed);
    if (probe < 0 || bind(probe, (struct sockaddr *) &closed, sizeof(closed)) != 0 ||
        getsockname(probe, (struct sockaddr *) &closed, &len) != 0) {
        CHECK(0, "setup: no closed port to try ssh on");
        return;
    }
    close(probe);
    char port[16];
    snprintf(port, sizeof(port), "%d", ntohs(closed.sin_port));
    want_stderr = true;
    long ms;
    int rc = run(out, out_size, &ms, "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                 "-p", port, "nobody@127.0.0.1", "true", (char *) NULL);
    want_stderr = false;
    CHECK(rc == 255, "ssh to a closed port: status %d, wanted 255 (\"%.300s\")", rc, out);
    CHECK(strstr(out, "Connection refused") != NULL,
          "ssh to a closed port: wanted \"Connection refused\" (\"%.300s\")", out);
}

int main(void) {
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_net_options: SKIP (no " SMALLCLUE ")\n");
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    char out[8192];
    long ms = 0;
    int rc;

    check_ssh_refused(out, sizeof(out));

    // ---- ping -----------------------------------------------------------
    rc = run(out, sizeof(out), &ms, "ping", "-c", "1", "127.0.0.1", (char *) NULL);
    if (rc != 0 || strstr(out, "bytes from 127.0.0.1") == NULL) {
        // No ICMP to loopback at all here: nothing about the options can be
        // told from that.
        if (failures != 0) {
            printf("native_net_options: FAIL failures=%d\n", failures);
            return 1;
        }
        printf("native_net_options: SKIP (ping cannot reach 127.0.0.1: status %d)\n", rc);
        return 0;
    }

    rc = run(out, sizeof(out), &ms, "ping", "-c", "2", "-W", "1", "-i", "0.2", "127.0.0.1", (char *) NULL);
    CHECK(rc == 0, "ping -c 2 -W 1 -i 0.2: status %d, wanted 0 (\"%.200s\")", rc, out);
    CHECK(count_of(out, "bytes from 127.0.0.1") == 2, "ping -c 2: wanted two replies (\"%.300s\")", out);
    CHECK(strstr(out, "2 packets transmitted, 2 packets received, 0% packet loss") != NULL,
          "ping -c 2: the summary is not Linux's (\"%.300s\")", out);
    CHECK(ms < 900, "ping -c 2 -i 0.2 took %ld ms: the interval was not honoured", ms);

    rc = run(out, sizeof(out), &ms, "ping", "-q", "-c", "1", "127.0.0.1", (char *) NULL);
    CHECK(rc == 0 && strstr(out, "bytes from") == NULL && strstr(out, "1 packets transmitted") != NULL,
          "ping -q: status %d, wanted the summary alone (\"%.300s\")", rc, out);

    rc = run(out, sizeof(out), &ms, "ping", "-w", "1", "-i", "0.2", "127.0.0.1", (char *) NULL);
    CHECK(rc == 0, "ping -w 1: status %d, wanted 0", rc);
    CHECK(ms >= 900 && ms < 2500, "ping -w 1 took %ld ms, wanted about 1000", ms);
    CHECK(count_of(out, "bytes from 127.0.0.1") >= 3, "ping -w 1 -i 0.2: wanted three replies or more (\"%.300s\")", out);

    rc = run(out, sizeof(out), &ms, "ping", "-c", "1", "-s", "1000", "127.0.0.1", (char *) NULL);
    CHECK(rc == 0 && strstr(out, "1008 bytes from 127.0.0.1") != NULL, "ping -s 1000: status %d (\"%.300s\")", rc, out);

    rc = run(out, sizeof(out), &ms, "ping", "-c", "0", "127.0.0.1", (char *) NULL);
    CHECK(rc == 2, "ping -c 0: status %d, wanted 2", rc);

    // ---- wget -----------------------------------------------------------
    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET};
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    socklen_t len = sizeof(addr);
    if (listener < 0 || bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, (struct sockaddr *) &addr, &len) != 0) {
        printf("native_net_options: FAIL setup: listen\n");
        return 1;
    }
    pthread_t server;
    pthread_create(&server, NULL, serve, NULL);

    char dir[] = "/tmp/native_net_options.XXXXXX";
    if (mkdtemp(dir) == NULL || chdir(dir) != 0) {
        printf("native_net_options: FAIL setup: scratch directory\n");
        return 1;
    }
    char ok[96], missing[96], stall[96];
    snprintf(ok, sizeof(ok), "http://127.0.0.1:%d/ok", ntohs(addr.sin_port));
    snprintf(missing, sizeof(missing), "http://127.0.0.1:%d/missing", ntohs(addr.sin_port));
    snprintf(stall, sizeof(stall), "http://127.0.0.1:%d/stall", ntohs(addr.sin_port));
    struct stat st;

    rc = run(out, sizeof(out), &ms, "wget", "-q", "-T", "5", "-O", "got", ok, (char *) NULL);
    CHECK(rc == 0, "wget -q -T 5 -O: status %d, wanted 0", rc);
    char body[32] = "";
    int fd = open("got", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, body, sizeof(body) - 1);
        body[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    CHECK(strcmp(body, "hello\n") == 0, "wget -O: the file holds \"%s\", wanted the body", body);

    rc = run(out, sizeof(out), &ms, "wget", "-q", "--timeout=5", "--tries=2", missing, (char *) NULL);
    CHECK(rc == 8, "wget of a 404: status %d, wanted 8", rc);
    CHECK(stat("missing", &st) != 0, "wget of a 404 left a file named for the page (%lld bytes)", (long long) st.st_size);

    rc = run(out, sizeof(out), &ms, "wget", "-q", "--spider", ok, (char *) NULL);
    CHECK(rc == 0, "wget --spider of a page that is there: status %d, wanted 0", rc);
    CHECK(stat("ok", &st) != 0, "wget --spider saved the page");
    rc = run(out, sizeof(out), &ms, "wget", "-q", "--spider", missing, (char *) NULL);
    CHECK(rc == 8, "wget --spider of a 404: status %d, wanted 8", rc);

    rc = run(out, sizeof(out), &ms, "wget", "-q", "-T", "1", "-O", "stalled", stall, (char *) NULL);
    CHECK(rc == 4, "wget -T 1 of a server that never answers: status %d, wanted 4", rc);
    CHECK(ms >= 900 && ms < 5000, "wget -T 1 gave up after %ld ms, wanted about 1000", ms);
    CHECK(stalled == 1, "wget -T 1: the server saw %d requests, wanted 1", stalled);

    rc = run(out, sizeof(out), &ms, "wget", "-q", "-T", "1", "-t", "2", "--waitretry=0", "-O", "stalled", stall, (char *) NULL);
    CHECK(rc == 4, "wget -T 1 -t 2: status %d, wanted 4", rc);
    CHECK(stalled == 3, "wget -t 2: the server saw %d requests in all, wanted 3 (one, then two)", stalled);

    unlink("got");
    unlink("stalled");
    chdir("/");
    rmdir(dir);

    if (failures != 0) {
        printf("native_net_options: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("native_net_options: PASS\n");
    return 0;
}
