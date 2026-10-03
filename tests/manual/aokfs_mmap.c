// aokfs_mmap.c — regression lock for mmap of files under /AOK.
//
// /AOK (aokfs) had no mmap. Reading its files worked, so nothing noticed until
// /AOK/bundled began carrying libraries and programs for guests to USE: ld.so
// maps every library, and LD_PRELOAD of a bundled shim printed "failed to map
// segment from shared object" and ran without it; the kernel's ELF loader maps
// every program, and called through the missing ->mmap -- exec of a bundled
// program took the whole app down (2026-10-03, bip).
//
// Most /AOK files are embedded in the app, so the mapping is a private copy.
// Asserted here, against read() of the same file:
//   - MAP_PRIVATE PROT_READ at offset 0 and at a later page matches the bytes
//   - the end of the last page, past EOF, reads as zero (as on Linux)
//   - a private mapping can be written (copy-on-write; the file is untouched)
//   - MAP_SHARED PROT_READ works; MAP_SHARED PROT_WRITE is EACCES (O_RDONLY)
//   - an unaligned offset is EINVAL, a directory ENODEV
// Arch-neutral; /AOK/bundled/glibc-x86_64/libish-pixman.so is just bytes here.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_common.h"

static const char *const target = "/AOK/bundled/glibc-x86_64/libish-pixman.so";

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    long page = sysconf(_SC_PAGESIZE);

    int fd = open(target, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= page) {
        printf("FAIL: %s is missing or no more than a page\n", target);
        return 1;
    }
    size_t size = (size_t) st.st_size;
    char *want = malloc(size);
    check(want != NULL && pread(fd, want, size, 0) == (ssize_t) size, "read the whole file");

    size_t len = (size + page - 1) / page * page;
    char *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    check(m != MAP_FAILED, "MAP_PRIVATE PROT_READ mapping");
    if (m != MAP_FAILED) {
        check(memcmp(m, want, size) == 0, "mapping matches read()");
        int zero = 1;
        for (size_t i = size; i < len; i++)
            zero &= m[i] == 0;
        check(zero, "past EOF in the last page reads zero");
        munmap(m, len);
    }

    m = mmap(NULL, len - page, PROT_READ, MAP_PRIVATE, fd, page);
    check(m != MAP_FAILED, "mapping at a page offset");
    if (m != MAP_FAILED) {
        check(memcmp(m, want + page, size - page) == 0, "offset mapping matches read()");
        munmap(m, len - page);
    }

    m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(m != MAP_FAILED, "MAP_PRIVATE PROT_WRITE mapping");
    if (m != MAP_FAILED) {
        m[0] ^= 0xff;
        char first;
        check(pread(fd, &first, 1, 0) == 1 && first == want[0], "private write leaves the file alone");
        munmap(m, page);
    }

    m = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    check(m != MAP_FAILED && memcmp(m, want, page) == 0, "MAP_SHARED PROT_READ mapping");
    if (m != MAP_FAILED)
        munmap(m, page);

    errno = 0;
    m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(m == MAP_FAILED && errno == EACCES, "MAP_SHARED PROT_WRITE on O_RDONLY is EACCES");

    errno = 0;
    m = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 1);
    check(m == MAP_FAILED && errno == EINVAL, "unaligned offset is EINVAL");
    close(fd);

    int dfd = open("/AOK/bundled", O_RDONLY | O_DIRECTORY);
    errno = 0;
    m = mmap(NULL, page, PROT_READ, MAP_PRIVATE, dfd, 0);
    check(dfd >= 0 && m == MAP_FAILED && errno == ENODEV, "a directory is ENODEV");
    close(dfd);
    free(want);

    if (failures_total) {
        printf("aokfs_mmap: %u failure(s)\n", failures_total);
        return 1;
    }
    printf("aokfs_mmap: PASS\n");
    return 0;
}
