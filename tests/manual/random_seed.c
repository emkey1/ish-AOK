/*
 * random_seed.c — self-checking regression lock for the /dev/{u,}random
 * seeding ioctls and the /proc/sys/kernel/random pool files.
 *
 * iSH-AOK draws randomness from the host CSPRNG and keeps no real entropy
 * pool, but /dev/random and /dev/urandom had no ioctl handler at all, so any
 * RND* ioctl fell through to the generic "no ioctl_size" path and returned
 * ENOTTY ("Not a tty").  At boot the OpenRC seedrng service does exactly:
 *
 *     fd = open("/dev/urandom");
 *     ioctl(fd, RNDADDENTROPY, &pool);   // seed + credit
 *
 * which therefore failed, producing on Alpine:
 *
 *     seedrng: Unable to seed: Not a tty
 *     Error seeding random number generator
 *
 * seedrng also reads /proc/sys/kernel/random/poolsize to size its seed; that
 * file was missing, giving the secondary:
 *
 *     seedrng: Unable to determine pool size, falling back to 256 bits:
 *              No such file or directory
 *
 * This test reproduces the seedrng flow directly. Because the random device
 * has no pool, the credit/reseed ioctls are accepted as no-ops; only the
 * "how much entropy" query returns a value. A bogus 'R'-family ioctl must
 * still report ENOTTY so the generic dispatch isn't broken.
 *
 * Exits 0 and prints "random_seed: PASS" on success; non-zero on any failure.
 * Accepts -v for verbose output. Builds under musl-only Alpine (no kernel
 * headers needed — the ioctl ABI is defined locally).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "test_common.h"

/* linux/random.h ioctls. Standard asm-generic _IOC encoding, identical for the
 * i386 and amd64 ABIs (the rand_pool_info header is two 32-bit ints on both).
 * powerpc encodes _IOC its own way: three direction bits (none 1, read 2,
 * write 4) above a 13-bit size. */
#if defined(__powerpc64__)
#define RNDGETENTCNT_   0x40045200 /* _IOR('R',0x00,int)        get entropy count   */
#define RNDADDTOENTCNT_ 0x80045201 /* _IOW('R',0x01,int)        credit entropy      */
#define RNDADDENTROPY_  0x80085203 /* _IOW('R',0x03,int[2]+buf) add + credit        */
#define RNDZAPENTCNT_   0x20005204 /* _IO('R',0x04)             zero entropy count  */
#define RNDCLEARPOOL_   0x20005206 /* _IO('R',0x06)             clear pool          */
#define RNDRESEEDCRNG_  0x20005207 /* _IO('R',0x07)             reseed CRNG         */
#else
#define RNDGETENTCNT_   0x80045200 /* _IOR('R',0x00,int)        get entropy count   */
#define RNDADDTOENTCNT_ 0x40045201 /* _IOW('R',0x01,int)        credit entropy      */
#define RNDADDENTROPY_  0x40085203 /* _IOW('R',0x03,int[2]+buf) add + credit        */
#define RNDZAPENTCNT_   0x00005204 /* _IO('R',0x04)             zero entropy count  */
#define RNDCLEARPOOL_   0x00005206 /* _IO('R',0x06)             clear pool          */
#define RNDRESEEDCRNG_  0x00005207 /* _IO('R',0x07)             reseed CRNG         */
#endif

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld\n", label, got, exp);
    if (bad)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) exp, 0, 0);
}

/* Exercise the full RND ioctl family on one device node. */
static void test_device(const char *path) {
    errno = 0;
    int fd = open(path, O_RDONLY);
    test_log_if(fd < 0, "%s: open errno=%d (%s)\n", path, errno, strerror(errno));
    check(path, fd >= 0, 1);
    if (fd < 0)
        return;

    /* RNDGETENTCNT — pure output; must yield a positive entropy estimate. */
    int cnt = -424242;
    errno = 0;
    int rc = ioctl(fd, RNDGETENTCNT_, &cnt);
    test_log_if(rc != 0, "  RNDGETENTCNT errno=%d (%s)\n", errno, strerror(errno));
    check("getentcnt.ok", rc == 0, 1);
    check("getentcnt.positive", rc == 0 && cnt > 0, 1);

    /* RNDADDENTROPY — the exact ioctl seedrng issues to seed + credit. This
     * is the one that used to return ENOTTY and break boot-time seeding. */
    struct { int entropy_count; int buf_size; unsigned char buffer[32]; } pool;
    pool.entropy_count = (int) sizeof(pool.buffer) * 8; /* credit all 256 bits */
    pool.buf_size = (int) sizeof(pool.buffer);
    memset(pool.buffer, 0x42, sizeof(pool.buffer));
    errno = 0;
    rc = ioctl(fd, RNDADDENTROPY_, &pool);
    test_log_if(rc != 0, "  RNDADDENTROPY errno=%d (%s)  <-- the seedrng failure\n",
                errno, strerror(errno));
    check("addentropy.ok", rc == 0, 1);
    check("addentropy.not_enotty", !(rc != 0 && errno == ENOTTY), 1);

    /* RNDADDTOENTCNT — input int, accepted as a no-op. */
    int add = 256;
    errno = 0;
    rc = ioctl(fd, RNDADDTOENTCNT_, &add);
    test_log_if(rc != 0, "  RNDADDTOENTCNT errno=%d (%s)\n", errno, strerror(errno));
    check("addtoentcnt.ok", rc == 0, 1);

    /* RNDRESEEDCRNG — no argument, accepted as a no-op. */
    errno = 0;
    rc = ioctl(fd, RNDRESEEDCRNG_);
    test_log_if(rc != 0, "  RNDRESEEDCRNG errno=%d (%s)\n", errno, strerror(errno));
    check("reseedcrng.ok", rc == 0, 1);

    /* A bogus 'R'-family ioctl must still report ENOTTY: the device only
     * claims the RND ioctls, everything else falls through to the default. */
    errno = 0;
    rc = ioctl(fd, 0x000052ff, &cnt);
    test_log_if(!(rc != 0 && errno == ENOTTY),
                "  bogus ioctl: rc=%d errno=%d (expected ENOTTY)\n", rc, errno);
    check("bogus.enotty", rc != 0 && errno == ENOTTY, 1);

    close(fd);
}

/* Read a non-negative integer out of a one-line /proc file. -1 on error. */
static long read_proc_int(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        test_log_if(1, "%s: open errno=%d (%s)\n", path, errno, strerror(errno));
        return -1;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

/* seedrng reads poolsize to size its seed; other RNG consumers read
 * entropy_avail before deciding whether to block. Both must exist and be
 * positive (the pool is host-backed, so it always reads as full). */
static void test_proc_pool(void) {
    long poolsize = read_proc_int("/proc/sys/kernel/random/poolsize");
    test_logf("poolsize=%ld\n", poolsize);
    check("proc.poolsize.positive", poolsize > 0, 1);

    long avail = read_proc_int("/proc/sys/kernel/random/entropy_avail");
    test_logf("entropy_avail=%ld\n", avail);
    check("proc.entropy_avail.positive", avail > 0, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    test_device("/dev/urandom");
    test_device("/dev/random");
    test_proc_pool();

    return finish_suite("random_seed");
}
