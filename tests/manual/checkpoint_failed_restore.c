// checkpoint_failed_restore.c -- helper for checkpoint_failed_restore.sh.
//   lsn listen PORT   bind 127.0.0.1:PORT, listen, and sleep (a daemon)
//   lsn probe PORT    try to bind it; print BIND=ok or BIND=<errno name>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = bind(s, (struct sockaddr *) &a, sizeof(a));
    if (strcmp(argv[1], "probe") == 0) {
        printf("BIND=%s\n", r == 0 ? "ok" : errno == EADDRINUSE ? "EADDRINUSE" : strerror(errno));
        return 0;
    }
    if (r != 0 || listen(s, 4) != 0) { perror("listen"); return 1; }
    for (int i = 0; i < 40; i++) sleep(1);
    return 0;
}
