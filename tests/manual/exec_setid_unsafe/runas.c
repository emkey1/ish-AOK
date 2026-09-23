/* runas UID GID GROUP prog args...: become an ordinary user, as camd's uid 1000
 * (supplementary group GROUP) ran the probe. */
#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 5) return 2;
    gid_t groups[2] = { (gid_t) atoi(argv[2]), (gid_t) atoi(argv[3]) };
    if (setgroups(2, groups) || setresgid(atoi(argv[2]), atoi(argv[2]), atoi(argv[2])) ||
        setresuid(atoi(argv[1]), atoi(argv[1]), atoi(argv[1]))) { perror("runas"); return 1; }
    execv(argv[4], argv + 4);
    perror("execv");
    return 127;
}
