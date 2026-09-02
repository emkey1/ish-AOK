// Wayland substrate: SCM_RIGHTS fd-passing shape + memfd seals, the two
// emulator bugs found bringing up a real Wayland compositor (wlroots/cage) +
// client (foot) + VNC bridge (wayvnc) stack under AOK.
//
// History: (1) libwayland's wl_connection_flush sets msg_controllen to the
// UNPADDED CMSG_LEN(n), not CMSG_SPACE(n) -- the last cmsg in a sendmsg
// control buffer ends without trailing alignment padding, which Linux
// accepts. guest_cmsg_parse required the aligned CMSG_SPACE to fit inside
// msg_controllen, so every single-fd SCM_RIGHTS send (every wl_shm client's
// first buffer, since one fd's cmsg is exactly 4 bytes short of aligned on a
// 64-bit guest) failed EINVAL, and libwayland logged "failed to flush
// wayland socket: Invalid argument" with no client able to proceed past
// initial buffer setup. (2) fcntl(F_ADD_SEALS/F_GET_SEALS) on a memfd was an
// unimplemented command (EINVAL from the default case), so wl_shm's seal step
// failed too ("failed to seal SHM backing memory file"), though that alone
// wasn't fatal to clients tolerant of an unsealed pool.
//
// Verifies (all ground-truthed against real Linux via mint/Lima oracle runs):
// SCM_RIGHTS carrying a memfd survives an unpadded (CMSG_LEN-exact, matching
// libwayland's shape) sendmsg/recvmsg round trip over both an unnamed
// socketpair and a named/listen/accept AF_UNIX stream socket, with a
// multi-iovec (ring-buffer-wrap-shaped) payload; cross-process MAP_SHARED
// coherence in both directions and MAP_PRIVATE readback of a passed fd,
// across a full 64K page-spanning buffer (not just a short string, which
// would hide a single corrupted page); a twice-forwarded fd (A -> B -> C,
// the wayvnc -> compositor -> client keymap fd journey) still maps correctly
// on the third hop; and F_ADD_SEALS/F_GET_SEALS round-trip on a memfd,
// including the MFD_ALLOW_SEALING gate (F_SEAL_SEAL when absent) and
// F_SEAL_SHRINK/GROW/WRITE enforcement in ftruncate, pwrite, and mmap.
// Arch-neutral.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_memfd_create
#if defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#endif
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif

static int memfd_create_(const char *name, unsigned flags) {
    return (int) syscall(SYS_memfd_create, name, flags);
}

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static void fill_pattern(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (char) ((i * 7 + (i >> 8)) & 0xff);
}

static int pattern_matches(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (buf[i] != (char) ((i * 7 + (i >> 8)) & 0xff))
            return 0;
    return 1;
}

// libwayland's wl_os_sendmsg_cloexec shape: one fd, msg_controllen set to
// the unpadded CMSG_LEN (not CMSG_SPACE), payload split across two iovecs.
static void send_fd_wayland_shape(int sock, int fd, struct iovec *iov, int iovlen) {
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr mh = { 0 };
    mh.msg_iov = iov;
    mh.msg_iovlen = (unsigned long) iovlen;
    mh.msg_control = ctrl;
    mh.msg_controllen = CMSG_LEN(sizeof(int));
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
    ssize_t sent = 0;
    for (int i = 0; i < iovlen; i++)
        sent += (ssize_t) iov[i].iov_len;
    ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
    check(n == sent, "sendmsg CMSG_LEN-exact, one fd, multi-iovec (libwayland shape)");
}

static int recv_fd(int sock, char *buf, size_t buflen, ssize_t *out_n) {
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = buf, .iov_len = buflen };
    struct msghdr mh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
    };
    *out_n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    if (*out_n <= 0)
        return -1;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
    return fd;
}

// Unnamed socketpair, unpadded single-fd cmsg, two-iovec payload.
static void test_socketpair_wayland_shape(void) {
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    int mfd = memfd_create_("pool", MFD_CLOEXEC);
    check(mfd >= 0, "memfd_create");
    check(ftruncate(mfd, 4096) == 0, "ftruncate");

    struct iovec iov[2] = {
        { .iov_base = (void *) "wl-", .iov_len = 3 },
        { .iov_base = (void *) "wrap", .iov_len = 4 },
    };
    send_fd_wayland_shape(sv[0], mfd, iov, 2);

    char buf[16];
    ssize_t n;
    int rfd = recv_fd(sv[1], buf, sizeof(buf), &n);
    check(rfd >= 0, "recvmsg gets SCM_RIGHTS fd from unpadded cmsg");
    check(n == 7 && memcmp(buf, "wl-wrap", 7) == 0, "recvmsg data intact across iovec split");

    close(mfd);
    if (rfd >= 0)
        close(rfd);
    close(sv[0]);
    close(sv[1]);
}

// Named AF_UNIX socket (bind/listen/accept, like wayland-0), matching a real
// compositor's listening socket rather than an anonymous socketpair.
static void test_named_socket(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/wayland_scm_shm.%d", (int) getpid());
    unlink(path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    check(bind(lfd, (struct sockaddr *) &addr, sizeof(addr)) == 0, "bind named socket");
    check(listen(lfd, 1) == 0, "listen named socket");

    pid_t pid = fork();
    if (pid == 0) {
        close(lfd);
        int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cfd, (struct sockaddr *) &addr, sizeof(addr)) != 0)
            _exit(1);
        int mfd = memfd_create_("pool2", MFD_CLOEXEC);
        if (mfd < 0 || ftruncate(mfd, 4096) != 0)
            _exit(1);
        struct iovec iov = { .iov_base = (void *) "connected", .iov_len = 9 };
        send_fd_wayland_shape(cfd, mfd, &iov, 1);
        _exit(failures_total ? 1 : 0);
    }

    int afd = accept(lfd, NULL, NULL);
    check(afd >= 0, "accept on named socket");
    char buf[16];
    ssize_t n;
    int rfd = recv_fd(afd, buf, sizeof(buf), &n);
    check(rfd >= 0, "recvmsg fd over named/accepted socket");
    check(n == 9 && memcmp(buf, "connected", 9) == 0, "recvmsg data over named socket");

    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sender child exited clean");

    if (rfd >= 0)
        close(rfd);
    close(afd);
    close(lfd);
    unlink(path);
}

// A full 64K page-spanning buffer (not a short string) catches a corrupted
// page that a small readback would miss. Checks both MAP_PRIVATE (what xkb
// requires for a keymap fd) and MAP_SHARED of the same received fd.
static void test_cross_process_mmap_coherence(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    int mfd = memfd_create_("keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check(mfd >= 0, "memfd_create for mmap test");
    check(ftruncate(mfd, 65536) == 0, "ftruncate 64K");
    char *w = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    check(w != MAP_FAILED, "sender mmap");
    fill_pattern(w, 65536);
    check(fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "seal before sharing (matches wl_shm)");

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        close(mfd);
        char c;
        ssize_t n;
        int rfd = recv_fd(sv[1], &c, 1, &n);
        if (rfd < 0)
            _exit(1);
        char *pm = mmap(NULL, 65536, PROT_READ, MAP_PRIVATE, rfd, 0);
        int ok1 = pm != MAP_FAILED && pattern_matches(pm, 65536);
        char *sm = mmap(NULL, 65536, PROT_READ, MAP_SHARED, rfd, 0);
        int ok2 = sm != MAP_FAILED && pattern_matches(sm, 65536);
        _exit(ok1 && ok2 ? 0 : 1);
    }

    close(sv[1]);
    struct iovec iov = { .iov_base = (void *) "k", .iov_len = 1 };
    send_fd_wayland_shape(sv[0], mfd, &iov, 1);

    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child MAP_PRIVATE and MAP_SHARED readback of full 64K page-spanning buffer");

    munmap(w, 65536);
    close(mfd);
    close(sv[0]);
}

// A -> B -> C: B relays the fd it received without ever mapping it itself
// (the wayvnc -> compositor -> client keymap journey).
static void test_twice_forwarded_fd(void) {
    const char *content = "twice-forwarded fd content marker";
    int ab[2], bc[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, ab);
    socketpair(AF_UNIX, SOCK_STREAM, 0, bc);

    pid_t b = fork();
    if (b == 0) {
        close(ab[0]);
        close(bc[1]);
        char c;
        ssize_t n;
        int fd = recv_fd(ab[1], &c, 1, &n);
        if (fd < 0)
            _exit(1);
        struct iovec iov = { .iov_base = (void *) "b", .iov_len = 1 };
        send_fd_wayland_shape(bc[0], fd, &iov, 1);
        _exit(failures_total ? 1 : 0);
    }
    pid_t c = fork();
    if (c == 0) {
        close(ab[0]);
        close(ab[1]);
        close(bc[0]);
        char ch;
        ssize_t n;
        int fd = recv_fd(bc[1], &ch, 1, &n);
        if (fd < 0)
            _exit(1);
        char *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        int ok = m != MAP_FAILED && strcmp(m, content) == 0;
        _exit(ok ? 0 : 1);
    }

    close(ab[1]);
    close(bc[0]);
    close(bc[1]);
    int mfd = memfd_create_("relay", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (ftruncate(mfd, 4096) == 0) {
        char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (m != MAP_FAILED)
            strcpy(m, content);
        fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    }
    struct iovec iov = { .iov_base = (void *) "a", .iov_len = 1 };
    send_fd_wayland_shape(ab[0], mfd, &iov, 1);

    int st_b, st_c;
    waitpid(b, &st_b, 0);
    waitpid(c, &st_c, 0);
    check(WIFEXITED(st_b) && WEXITSTATUS(st_b) == 0, "relay hop (B) forwarded without mapping");
    check(WIFEXITED(st_c) && WEXITSTATUS(st_c) == 0,
          "twice-forwarded fd readable at third hop (C)");

    close(mfd);
    close(ab[0]);
}

// F_ADD_SEALS/F_GET_SEALS themselves: the MFD_ALLOW_SEALING gate, seal
// persistence, and enforcement in ftruncate/pwrite/mmap.
static void test_memfd_seals(void) {
    int mfd = memfd_create_("sealed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check(mfd >= 0, "memfd_create with MFD_ALLOW_SEALING");
    check(ftruncate(mfd, 4096) == 0, "initial ftruncate");

    int seals = fcntl(mfd, F_GET_SEALS);
    check(seals == 0, "F_GET_SEALS starts empty when sealing is allowed");

    check(fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "F_ADD_SEALS(F_SEAL_SHRINK)");
    seals = fcntl(mfd, F_GET_SEALS);
    check(seals >= 0 && (seals & F_SEAL_SHRINK), "F_GET_SEALS reflects SHRINK");

    check(ftruncate(mfd, 4095) < 0 && errno == EPERM, "shrink blocked by F_SEAL_SHRINK");
    check(ftruncate(mfd, 8192) == 0, "grow still allowed without F_SEAL_GROW");

    check(fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE) == 0, "F_ADD_SEALS(F_SEAL_WRITE)");
    check(write(mfd, "x", 1) < 0 && errno == EPERM, "write blocked by F_SEAL_WRITE");
    char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    check(m == MAP_FAILED && errno == EPERM,
          "writable MAP_SHARED blocked by F_SEAL_WRITE");
    char *ro = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0);
    check(ro != MAP_FAILED, "read-only MAP_SHARED still allowed under F_SEAL_WRITE");
    if (ro != MAP_FAILED)
        munmap(ro, 4096);

    check(fcntl(mfd, F_ADD_SEALS, F_SEAL_SEAL) == 0, "F_ADD_SEALS(F_SEAL_SEAL)");
    check(fcntl(mfd, F_ADD_SEALS, F_SEAL_GROW) < 0 && errno == EPERM,
          "further sealing blocked once F_SEAL_SEAL is set");
    close(mfd);

    // Without MFD_ALLOW_SEALING, Linux starts the file permanently
    // seal-sealed (F_SEAL_SEAL from creation).
    int mfd2 = memfd_create_("unsealable", MFD_CLOEXEC);
    check(mfd2 >= 0, "memfd_create without MFD_ALLOW_SEALING");
    int seals2 = fcntl(mfd2, F_GET_SEALS);
    check(seals2 >= 0 && (seals2 & F_SEAL_SEAL),
          "F_SEAL_SEAL implicitly set without MFD_ALLOW_SEALING");
    check(fcntl(mfd2, F_ADD_SEALS, F_SEAL_SHRINK) < 0 && errno == EPERM,
          "sealing rejected without MFD_ALLOW_SEALING");
    close(mfd2);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    test_socketpair_wayland_shape();
    test_named_socket();
    test_cross_process_mmap_coherence();
    test_twice_forwarded_fd();
    test_memfd_seals();

    if (failures_total) {
        printf("wayland_scm_shm: %u FAILURES\n", failures_total);
        return 1;
    }
    printf("wayland_scm_shm: PASS\n");
    return 0;
}
