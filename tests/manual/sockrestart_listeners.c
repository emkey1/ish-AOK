// Guest half of sockrestart_listeners.sh: listeners across the app's
// suspend/resume socket cycle (fs/sockrestart.c).
//
// The host script runs this with ISH_SOCKRESTART_AFTER=1, so the save and the
// rebuild happen inside the three-second sleep below, and with each of the
// ISH_SOCKRESTART_TEST_DESTROY modes. One mode per run:
//
//   survive      a unix listener at a path; afterwards a client connects and
//                the accepted connection carries its byte
//   abstract     the same at an abstract name
//   pending      a client connects and writes BEFORE the cycle; afterwards
//                accept must hand back that same connection, and a new client
//                must still get through. Only meaningful when the listener
//                was NOT destroyed: a dead listener's queue dies with it.
//   accept       the server sits in a blocking accept() across the cycle, and
//                a child connects after it
//   poll         the server sits in poll() across the cycle
//   relisten     the guest calls listen() twice on the one socket
//   mixed        a unix and a TCP listener at once, both serving afterwards
//   tcp, tcp-pending, tcp-accept
//                survive, pending and accept over 127.0.0.1
//
// Prints "PASS <mode>" or "FAIL <mode>: ..." and exits 0 or 1.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *mode;
static struct sockaddr_storage addr;
static socklen_t addr_len;

static void fail(const char *what) {
    printf("FAIL %s: %s: %s\n", mode, what, strerror(errno));
    exit(1);
}

// Listen at a fresh name, and leave that name in addr for client().
static int make_listener(int family, int abstract) {
    int s = socket(family, SOCK_STREAM, 0);
    if (s < 0)
        fail("socket");
    memset(&addr, 0, sizeof(addr));
    if (family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *) &addr;
        un->sun_family = AF_UNIX;
        if (abstract) {
            const char *name = "sockrestart-listeners";
            strcpy(un->sun_path + 1, name);
            addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
        } else {
            const char *name = "/tmp/sockrestart-listeners.sock";
            unlink(name);
            strcpy(un->sun_path, name);
            addr_len = sizeof(*un);
        }
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *) &addr;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr_len = sizeof(*in);
    }
    if (bind(s, (struct sockaddr *) &addr, addr_len) < 0)
        fail("bind");
    if (listen(s, 8) < 0)
        fail("listen");
    if (family == AF_INET) {
        addr_len = sizeof(addr);
        if (getsockname(s, (struct sockaddr *) &addr, &addr_len) < 0)
            fail("getsockname");
    }
    return s;
}

static int client(char byte) {
    int c = socket(addr.ss_family, SOCK_STREAM, 0);
    if (c < 0)
        fail("client socket");
    if (connect(c, (struct sockaddr *) &addr, addr_len) < 0)
        fail("connect");
    if (write(c, &byte, 1) != 1)
        fail("client write");
    return c;
}

// Accept one connection and check that it carries the byte `want`.
static void serve(int s, char want) {
    int a = accept(s, NULL, NULL);
    if (a < 0)
        fail("accept");
    char got = 0;
    if (read(a, &got, 1) != 1)
        fail("read");
    if (got != want) {
        printf("FAIL %s: accepted a connection carrying '%c', wanted '%c'\n", mode, got, want);
        exit(1);
    }
    close(a);
}

static int is(const char *m) {
    return strcmp(mode, m) == 0;
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "survive";
    alarm(20);
    setvbuf(stdout, NULL, _IONBF, 0);
    int s = make_listener(strncmp(mode, "tcp", 3) == 0 ? AF_INET : AF_UNIX, is("abstract"));

    if (is("pending") || is("tcp-pending")) {
        int early = client('p');
        sleep(3);
        serve(s, 'p');
        close(early);
        int late = client('n');
        serve(s, 'n');
        close(late);
    } else if (is("mixed")) {
        struct sockaddr_storage unix_addr = addr;
        socklen_t unix_len = addr_len;
        int t = make_listener(AF_INET, 0);
        sleep(3);
        int c = client('t');
        serve(t, 't');
        close(c);
        addr = unix_addr;
        addr_len = unix_len;
        c = client('u');
        serve(s, 'u');
        close(c);
    } else if (is("accept") || is("tcp-accept") || is("poll")) {
        pid_t pid = fork();
        if (pid < 0)
            fail("fork");
        if (pid == 0) {
            sleep(3);
            int c = client('c');
            sleep(1);
            close(c);
            _exit(0);
        }
        if (is("poll")) {
            struct pollfd p = {.fd = s, .events = POLLIN};
            if (poll(&p, 1, 15000) != 1)
                fail("poll");
        }
        serve(s, 'c');
        int status;
        waitpid(pid, &status, 0);
    } else if (is("survive") || is("abstract") || is("relisten") || is("tcp")) {
        if (is("relisten") && listen(s, 16) < 0)
            fail("second listen");
        sleep(3);
        int c = client('s');
        serve(s, 's');
        close(c);
    } else {
        printf("FAIL %s: no such mode\n", mode);
        return 1;
    }
    printf("PASS %s\n", mode);
    return 0;
}
