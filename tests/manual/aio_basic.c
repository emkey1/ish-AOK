// Linux native AIO (the io_* family), which iSH-AOK stubbed to ENOSYS until
// MariaDB dereferenced the nullptr that came back and crash-looped on install.
//
// Deliberately uses the RAW SYSCALLS rather than libaio, for two reasons: the
// test should fail when the kernel is wrong rather than when a library is
// missing, and libaio's io_getevents reads the context in userspace before
// deciding whether to make a syscall at all -- which is a property worth
// testing separately, not one to hide behind here.
//
// Run on i386 as well as 64-bit. struct iocb and struct io_event are
// fixed-width and therefore ABI-invariant, which is the claim this is checking.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// Older uapi headers predate it; the value is ABI, not a header detail.
#ifndef IOCB_CMD_POLL
#define IOCB_CMD_POLL 5
#endif

static void ck(const char *what, int ok, const char *detail) {
    if (!ok)
        failf(what, (uint64_t) errno, 0, 0, 0, 0, 0);
    test_logf("  %-44s %s%s%s\n", what, ok ? "ok" : "FAIL",
              ok || detail == NULL ? "" : "   ", ok || detail == NULL ? "" : detail);
}

static long io_setup_(unsigned n, aio_context_t *c) { return syscall(__NR_io_setup, n, c); }
static long io_destroy_(aio_context_t c) { return syscall(__NR_io_destroy, c); }
static long io_submit_(aio_context_t c, long n, struct iocb **p) { return syscall(__NR_io_submit, c, n, p); }
static long io_getevents_(aio_context_t c, long min, long nr, struct io_event *e, struct timespec *t) {
    return syscall(__NR_io_getevents, c, min, nr, e, t);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // A backstop, not a deadline: every wait below has its own timeout, so a
    // correct run finishes in milliseconds. This only fires if a getevents
    // that should have timed out did not.
    alarm(test_watchdog_secs(60));

    ck("struct iocb is 64 bytes", sizeof(struct iocb) == 64, NULL);
    ck("struct io_event is 32 bytes", sizeof(struct io_event) == 32, NULL);

    aio_context_t ctx = 0;
    long rc = io_setup_(64, &ctx);
    ck("io_setup", rc == 0, strerror(errno));
    if (rc != 0)
        return finish_suite("aio_basic");
    ck("io_setup returned a non-zero context", ctx != 0, NULL);

    // The property MariaDB actually depends on. An aio_context_t is not an
    // opaque cookie to libaio: io_getevents_0_4 casts it straight to a
    // struct aio_ring and reads ->magic, and only falls back to the syscall
    // when that is not AIO_RING_MAGIC. So the context id must point at a
    // READABLE page whose first bytes are not that magic -- readable or libaio
    // segfaults, non-magic or it reaps events out of a ring nothing fills.
    // A zeroed page satisfies both, which is why io_setup maps one.
    struct aio_ring_head { unsigned id, nr, head, tail, magic; };
    const volatile struct aio_ring_head *ring =
        (const volatile struct aio_ring_head *) (uintptr_t) ctx;
    unsigned magic = ring->magic;   // faults here if the page is not mapped
    ck("the context id points at a readable page", 1, NULL);
    ck("...that does not carry AIO_RING_MAGIC", magic != 0xa10a10a1u, NULL);

    // Linux requires the caller to have zeroed it; a second setup on the same
    // variable must be refused, which is how a double-init is caught.
    aio_context_t again = ctx;
    ck("io_setup on a non-zero variable is EINVAL",
       io_setup_(64, &again) == -1 && errno == EINVAL, strerror(errno));

    char path[] = "/tmp/aio_basic_XXXXXX";
    int fd = mkstemp(path);
    ck("temp file", fd >= 0, strerror(errno));
    unlink(path);

    // 1. a write, reaped, with the opaque data field intact
    static char wbuf[512];
    memset(wbuf, 'A', sizeof(wbuf));
    struct iocb cb;
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_PWRITE;
    cb.aio_fildes = fd;
    cb.aio_buf = (uint64_t)(uintptr_t) wbuf;
    cb.aio_nbytes = sizeof(wbuf);
    cb.aio_offset = 0;
    cb.aio_data = 0xfeedfacecafebeefULL;
    struct iocb *cbs[1] = { &cb };
    ck("io_submit(PWRITE)", io_submit_(ctx, 1, cbs) == 1, strerror(errno));

    struct io_event ev;
    memset(&ev, 0, sizeof(ev));
    ck("io_getevents reaps it", io_getevents_(ctx, 1, 1, &ev, NULL) == 1, strerror(errno));
    ck("res is the byte count", ev.res == (long) sizeof(wbuf), NULL);
    ck("data survived the round trip", ev.data == 0xfeedfacecafebeefULL, NULL);
    ck("obj points at the iocb", ev.obj == (uint64_t)(uintptr_t) &cb, NULL);

    // 2. read it back and compare
    static char rbuf[512];
    memset(rbuf, 0, sizeof(rbuf));
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_PREAD;
    cb.aio_fildes = fd;
    cb.aio_buf = (uint64_t)(uintptr_t) rbuf;
    cb.aio_nbytes = sizeof(rbuf);
    cb.aio_offset = 0;
    ck("io_submit(PREAD)", io_submit_(ctx, 1, cbs) == 1, strerror(errno));
    ck("reaps", io_getevents_(ctx, 1, 1, &ev, NULL) == 1, strerror(errno));
    ck("read got the bytes back", ev.res == (long) sizeof(rbuf) &&
       memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, NULL);

    // 3. fsync
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_FSYNC;
    cb.aio_fildes = fd;
    ck("io_submit(FSYNC)", io_submit_(ctx, 1, cbs) == 1, strerror(errno));
    ck("fsync reaps ok", io_getevents_(ctx, 1, 1, &ev, NULL) == 1 && ev.res == 0, NULL);

    // 4. the refusals: a bad descriptor, and an opcode with no implementation
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_PREAD;
    cb.aio_fildes = 9999;
    cb.aio_buf = (uint64_t)(uintptr_t) rbuf;
    cb.aio_nbytes = 16;
    // Linux rejects a bad fd in io_submit itself (aio_prep_rw's fget), and
    // queues NO event for it -- so a caller told "1 submitted" may always wait
    // for exactly one event. Only errors the transfer itself hits land in
    // io_event.res. Getting this backwards is a hang: InnoDB would wait for a
    // completion that is never coming.
    ck("io_submit(bad fd) is EBADF at submit time",
       io_submit_(ctx, 1, cbs) == -1 && errno == EBADF, strerror(errno));
    struct timespec nowait = { 0, 0 };
    ck("the rejected iocb queued no event",
       io_getevents_(ctx, 0, 1, &ev, &nowait) == 0, strerror(errno));

    // An unimplemented opcode is the same kind of refusal, not a completion.
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_POLL;
    cb.aio_fildes = fd;
    ck("io_submit(unsupported opcode) is EINVAL",
       io_submit_(ctx, 1, cbs) == -1 && errno == EINVAL, strerror(errno));

    // 5. IOCB_FLAG_RESFD: a completion must bump the eventfd
    int efd = eventfd(0, EFD_NONBLOCK);
    ck("eventfd", efd >= 0, strerror(errno));
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_PWRITE;
    cb.aio_fildes = fd;
    cb.aio_buf = (uint64_t)(uintptr_t) wbuf;
    cb.aio_nbytes = 64;
    cb.aio_offset = 0;
    cb.aio_flags = IOCB_FLAG_RESFD;
    cb.aio_resfd = efd;
    ck("io_submit(with RESFD)", io_submit_(ctx, 1, cbs) == 1, strerror(errno));
    io_getevents_(ctx, 1, 1, &ev, NULL);
    uint64_t counter = 0;
    ck("the eventfd was signalled",
       read(efd, &counter, sizeof(counter)) == sizeof(counter) && counter == 1, NULL);
    close(efd);

    // 6. min_nr with a timeout: asking for more than exists must time out and
    //    report what there was, not block forever and not lie about the count.
    struct timespec t = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    memset(&cb, 0, sizeof(cb));
    cb.aio_lio_opcode = IOCB_CMD_PWRITE;
    cb.aio_fildes = fd;
    cb.aio_buf = (uint64_t)(uintptr_t) wbuf;
    cb.aio_nbytes = 32;
    ck("one more submit", io_submit_(ctx, 1, cbs) == 1, strerror(errno));
    struct io_event evs[4];
    long got = io_getevents_(ctx, 2, 4, evs, &t);
    ck("min_nr=2 with one queued returns 1 after the timeout", got == 1, NULL);

    // 7. a dead context is EINVAL, not a crash
    ck("io_destroy", io_destroy_(ctx) == 0, strerror(errno));
    ck("io_getevents on a destroyed context is EINVAL",
       io_getevents_(ctx, 1, 1, &ev, NULL) == -1 && errno == EINVAL, strerror(errno));
    ck("io_destroy twice is EINVAL", io_destroy_(ctx) == -1 && errno == EINVAL, strerror(errno));

    close(fd);
    return finish_suite("aio_basic");
}
