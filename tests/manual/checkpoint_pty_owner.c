// checkpoint_pty_owner.c -- a pty the guest made, owned by a user, across a
// checkpoint. Driven by checkpoint_pty_owner.sh.
//
// The shape is sshd-session (root, holding the master) with a user's shell on
// the slave, which login/sshd chowned to that user. The restore rebuilds the
// pair by opening /dev/ptmx again, and two things went wrong with that:
//
//  - the rebuilt slave was owned by root, not the user it had been chowned to;
//  - a task was restored holding its PARENT's credentials, so the user's
//    shell's forked child -- which had opened the terminal itself, by path --
//    reopened it as uid 1000, against a root-owned node, and got EACCES. That
//    refused the whole session: "pid 753 could not restore fd 0 (pts
//    /dev/pts/1): -13" on device.
//
// Prints OWNER=uid:gid:mode of the slave, as the uid-1000 grandchild sees it,
// once after the checkpoint -- the restored run is what the script checks.
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("openpt"); return 1; }
    char *sn = ptsname(m);
    chown(sn, 1000, 5);          // what login/sshd do to the user's tty
    chmod(sn, 0620);
    printf("slave %s\n", sn); fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        setgid(1000); setuid(1000);
        int s = open(sn, O_RDWR);
        if (s < 0) { perror("child open slave"); _exit(1); }
        dup2(s, 0); dup2(s, 1); if (s > 1) close(s);
        // A grandchild inheriting the terminal, like a shell's foreground job:
        // on restore ITS parent already has uid 1000, which is the device case.
        pid_t gc = fork();
        if (gc == 0) {
            // Its OWN open of the terminal, by path: a separate struct fd.
            int g = open(sn, O_RDWR); if (g < 0) { perror("gc open"); _exit(3); }
            dup2(g, 0); close(g);
            for (int i = 0; i < 12; i++) sleep(1);
            // After the checkpoint: whose terminal is this now?
            struct stat sb;
            if (fstat(0, &sb) == 0) {
                char b[96];
                int n = snprintf(b, sizeof(b), "OWNER=%u:%u:%o\n", (unsigned) sb.st_uid,
                                 (unsigned) sb.st_gid, (unsigned) (sb.st_mode & 07777));
                int f = open("/tmp/ptyown-owner", O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (f >= 0) { write(f, b, n); close(f); }
            }
            _exit(0); }
        for (int i = 0; i < 12; i++) sleep(1);
        // The grandchild's witness has to be on disk before this process
        // reports -- the parent exits once this one has, and pid 1 exiting
        // ends the whole run, grandchild and all.
        waitpid(gc, NULL, 0);
        int f = open("/tmp/ptyown-child", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (f >= 0) { write(f, "child-alive\n", 12); close(f); }
        _exit(0);
    }
    for (int i = 0; i < 12; i++) sleep(1);
    int st = 0; waitpid(p, &st, 0);
    printf("PARENT-DONE child-exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
