#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

int main() {
    struct stat64 statbuf;
    if (fstat64(1, &statbuf) < 0) {
        perror("fstat"); return 1;
    }
    /* write(1, &statbuf, sizeof(statbuf)); */
    char buf[1000];
    int len;
#define PTIF(n) \
    len = snprintf(buf, sizeof(buf), #n " %llx\n", (long long) statbuf.st_##n); \
    if (len >= 0 && (size_t) len < sizeof(buf)) \
        write(1, buf, len); \
    else if (len >= 0) \
        write(1, buf, sizeof(buf) - 1);
    PTIF(atime);
    PTIF(mtime);
    PTIF(ctime);
    /* PTIF(dev) */
    /* PTIF(ino); */
    /* PTIF(mode); */
    /* PTIF(nlink); */
    /* PTIF(uid); */
    /* PTIF(gid); */
    /* PTIF(rdev); */
    /* PTIF(size); */
    /* PTIF(blksize); */
    /* PTIF(blocks); */
    return 0;
}
