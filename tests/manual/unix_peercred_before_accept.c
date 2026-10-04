// A connecting AF_UNIX client's SO_PEERCRED is the listener's credentials,
// available as soon as connect() returns -- before the server accepts.
//
// AOK filled a client's peer credentials only when the server accept()ed
// and the two ends were linked, so a client asking right after connect() got
// pid 0, uid -1, gid -1. sd-bus asks exactly then and calls an invalid pid
// ENODATA: on an Arch guest every `systemctl` failed with "Failed to connect
// to system scope bus via local transport: No data available".
//
// A child listens (so the listener's pid is not ours) and never accepts; the
// parent connects and reads SO_PEERCRED. Stream and seqpacket. Passes on
// Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static int run(int type, const char *name) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/peercred_%s.%d", name, (int) getpid());
    unlink(addr.sun_path);
    int ready[2];
    if (pipe(ready) != 0)
        return 1;
    pid_t child = fork();
    if (child == 0) {
        int l = socket(AF_UNIX, type, 0);
        char c = 'n';
        if (l >= 0 && bind(l, (struct sockaddr *) &addr, sizeof(addr)) == 0 && listen(l, 4) == 0)
            c = 'y';
        if (write(ready[1], &c, 1) != 1)
            _exit(2);
        pause(); // never accepts
        _exit(0);
    }
    close(ready[1]);
    char c = 0;
    int fails = 0;
    if (read(ready[0], &c, 1) != 1 || c != 'y') {
        printf("FAIL: %s: listener setup\n", name);
        fails++;
    } else {
        int s = socket(AF_UNIX, type, 0);
        struct ucred cred = {};
        socklen_t len = sizeof(cred);
        if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
            printf("FAIL: %s: connect: %s\n", name, strerror(errno));
            fails++;
        } else if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
            printf("FAIL: %s: SO_PEERCRED: %s\n", name, strerror(errno));
            fails++;
        } else if (cred.pid != child || cred.uid != geteuid() || cred.gid != getegid()) {
            printf("FAIL: %s: SO_PEERCRED before accept is pid %d uid %d gid %d, want %d %d %d\n",
                   name, (int) cred.pid, (int) cred.uid, (int) cred.gid,
                   (int) child, (int) geteuid(), (int) getegid());
            fails++;
        } else {
            test_logf("ok: %s: SO_PEERCRED before accept is the listener (pid %d)\n", name, (int) child);
        }
        close(s);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    unlink(addr.sun_path);
    return fails;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    int fails = run(SOCK_STREAM, "stream") + run(SOCK_SEQPACKET, "seqpacket");
    printf("unix_peercred_before_accept: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
