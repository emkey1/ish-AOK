// mmap of a file at an offset past 4 GiB from a 32-bit process. i386 libcs
// reach it through mmap2, whose offset argument counts 4 KiB pages; AOK's
// sys_mmap2 shifted that 32-bit page count left by 12 before widening it, so
// every offset >= 4 GiB wrapped (0x100001000 became 0x1000). It surfaced as
// virtgpu_node's "mmap at MAP's offset" failure -- virtgpu MAP offsets start
// at 1 << 32 -- but any large file's tail was mapped from the wrong place.
//
// A sparse file with distinct markers at 4 KiB and at 4 GiB + 4 KiB: mapping
// the high offset must see the high marker, not the low one.
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == 8, "needs a 64-bit off_t");

int main(void) {
    char path[] = "/tmp/mmap2_high_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    unlink(path);
    const off_t high = ((off_t) 1 << 32) + 4096;
    if (pwrite(fd, "low!", 4, 4096) != 4 || pwrite(fd, "HIGH", 4, high) != 4) {
        perror("pwrite");
        printf("mmap2_high_offset: SKIP (cannot write a sparse file past 4 GiB here)\n");
        return 0;
    }
    int failures = 0;
    char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, high);
    if (p == MAP_FAILED) {
        perror("mmap high");
        failures++;
    } else {
        printf("at 4 GiB + 4 KiB: %.4s\n", p);
        if (memcmp(p, "HIGH", 4) != 0) {
            printf("FAIL mapped the wrong offset (got %.4s, want HIGH)\n", p);
            failures++;
        }
        munmap(p, 4096);
    }
    // Positive control: the low offset maps the low marker.
    char *q = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4096);
    if (q == MAP_FAILED || memcmp(q, "low!", 4) != 0) {
        printf("FAIL low offset control\n");
        failures++;
    }
    close(fd);
    printf("mmap2_high_offset: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
