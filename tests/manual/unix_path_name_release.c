// A unix socket bound to a PATH holds its filesystem node while it is bound,
// and lets go of it when it closes.
//
// fs/sock.c keeps the node's inode (socket.unix_name_inode) as the socket's
// hold on its name: the inode carries the id of the host socket behind it, and
// release_unix_names() drops it on close. From 2019 (b02ceb3c) until this test,
// bind() overwrote that pointer with NULL straight after taking it, so the
// reference was never released: every node that was ever bound kept its inode
// -- and, through it, its FILESYSTEM -- for the life of the emulator. The
// guest-visible cost was the mount: a tmpfs a daemon had ever bound a socket in
// could not be unmounted again (EBUSY, even after the socket was closed and its
// node removed), and fakefs kept the unlinked node's row in meta.db.
//
// The rest pins down what the leak might have been holding up: a stale node,
// a node unlinked or renamed under a live listener, a path bound again and
// again, a second bind, datagrams. Linux (net/unix/af_unix.c): the bound
// socket holds the node's path (u->path) until it is released, a lookup holds
// nothing, and a node with no socket bound to it refuses (ECONNREFUSED).
// Every expectation below was measured on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "test_common.h"

static char dir[64];

static void expect_ok(const char *label, int ret) {
    if (ret < 0) {
        printf("FAIL %s: %s\n", label, strerror(errno));
        failures_total++;
    } else {
        test_logf("%s: ok\n", label);
    }
}

static void expect_err(const char *label, int ret, int want) {
    int e = errno;
    if (ret >= 0) {
        printf("FAIL %s: succeeded, want %s\n", label, strerror(want));
        failures_total++;
    } else if (e != want) {
        printf("FAIL %s: %s, want %s\n", label, strerror(e), strerror(want));
        failures_total++;
    } else {
        test_logf("%s: %s ok\n", label, strerror(e));
    }
}

static struct sockaddr_un addr_of(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    return a;
}

static int sock_of(int type) {
    int s = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (s < 0) {
        printf("FAIL socket: %s\n", strerror(errno));
        failures_total++;
    }
    return s;
}

static int do_bind(int s, const char *path) {
    struct sockaddr_un a = addr_of(path);
    return bind(s, (struct sockaddr *) &a, sizeof(a));
}

static int do_connect(int s, const char *path) {
    struct sockaddr_un a = addr_of(path);
    return connect(s, (struct sockaddr *) &a, sizeof(a));
}

static int listener_at(const char *label, const char *path) {
    int l = sock_of(SOCK_STREAM);
    if (do_bind(l, path) < 0 || listen(l, 8) < 0) {
        printf("FAIL %s: bind/listen %s: %s\n", label, path, strerror(errno));
        failures_total++;
    }
    return l;
}

static int pending(int l, int ms) {
    struct pollfd p = {.fd = l, .events = POLLIN};
    return poll(&p, 1, ms) == 1;
}

// Connect to `path`, accept on `l`, and pass a byte through.
static void expect_reaches(const char *label, const char *path, int l) {
    int c = sock_of(SOCK_STREAM);
    if (do_connect(c, path) < 0) {
        printf("FAIL %s: connect: %s\n", label, strerror(errno));
        failures_total++;
        close(c);
        return;
    }
    if (!pending(l, 5000)) {
        printf("FAIL %s: no connection arrived at the listener\n", label);
        failures_total++;
        close(c);
        return;
    }
    int s = accept(l, NULL, NULL);
    char b = 0;
    if (s < 0 || write(c, "k", 1) != 1 || read(s, &b, 1) != 1 || b != 'k') {
        printf("FAIL %s: byte did not pass through\n", label);
        failures_total++;
    } else {
        test_logf("%s: ok\n", label);
    }
    if (s >= 0)
        close(s);
    close(c);
}

static void expect_connect_err(const char *label, const char *path, int want) {
    int c = sock_of(SOCK_STREAM);
    expect_err(label, do_connect(c, path), want);
    close(c);
}

static void expect_missing(const char *label, const char *path) {
    struct stat st;
    expect_err(label, lstat(path, &st), ENOENT);
}

// The listener closes and its node stays behind, as it does on Linux: the name
// refuses, cannot be bound over, and is free again once the node is removed.
static void test_stale_node(void) {
    char path[96];
    snprintf(path, sizeof(path), "%s/stale", dir);
    int l = listener_at("stale: listener", path);
    expect_reaches("stale: reaches the listener", path, l);
    close(l);

    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode)) {
        printf("FAIL stale: the node did not outlive its socket\n");
        failures_total++;
    }
    expect_connect_err("stale: connect to a node nobody is bound to", path, ECONNREFUSED);
    int again = sock_of(SOCK_STREAM);
    expect_err("stale: bind over the stale node", do_bind(again, path), EADDRINUSE);
    close(again);

    expect_ok("stale: unlink", unlink(path));
    l = listener_at("stale: listener after unlink", path);
    expect_reaches("stale: the new listener", path, l);
    close(l);
    unlink(path);
}

// A daemon removes its socket's name while it is still listening: the
// connections it has keep working, nobody new can find it, its own
// getsockname still names the path, and the path can be bound by somebody
// else -- whose listener then gets the connections, and keeps getting them
// after the first one closes.
static void test_unlink_while_listening(void) {
    char path[96];
    snprintf(path, sizeof(path), "%s/unlinked", dir);
    int l = listener_at("unlink: listener", path);

    int c = sock_of(SOCK_STREAM);
    expect_ok("unlink: connect", do_connect(c, path));
    int s = pending(l, 5000) ? accept(l, NULL, NULL) : -1;
    expect_ok("unlink: accept", s);

    expect_ok("unlink: unlink the listening socket's node", unlink(path));
    char b = 0;
    if (s < 0 || write(c, "u", 1) != 1 || read(s, &b, 1) != 1 || b != 'u') {
        printf("FAIL unlink: the accepted connection stopped passing bytes\n");
        failures_total++;
    }
    expect_connect_err("unlink: connect to the removed name", path, ENOENT);

    struct sockaddr_un got;
    socklen_t got_len = sizeof(got);
    if (getsockname(l, (struct sockaddr *) &got, &got_len) < 0 ||
            strcmp(got.sun_path, path) != 0) {
        printf("FAIL unlink: getsockname lost the name (%s)\n",
               got_len > sizeof(sa_family_t) ? got.sun_path : "unnamed");
        failures_total++;
    }

    int l2 = listener_at("unlink: second listener on the same path", path);
    expect_reaches("unlink: the second listener", path, l2);
    if (pending(l, 200)) {
        printf("FAIL unlink: a connection to the new node reached the old listener\n");
        failures_total++;
    }
    close(l);
    expect_reaches("unlink: the second listener after the first closed", path, l2);

    if (s >= 0)
        close(s);
    close(c);
    close(l2);
    unlink(path);
}

// A node renamed under a live listener is still that listener's node (the
// bind-then-rename-into-place idiom).
static void test_rename_while_listening(void) {
    char tmp[96], path[96];
    snprintf(tmp, sizeof(tmp), "%s/rename.tmp", dir);
    snprintf(path, sizeof(path), "%s/renamed", dir);
    int l = listener_at("rename: listener", tmp);
    expect_ok("rename: rename the node", rename(tmp, path));
    expect_reaches("rename: reaches the listener by its new name", path, l);
    expect_connect_err("rename: the old name", tmp, ENOENT);
    close(l);
    expect_connect_err("rename: the new name once the listener closed", path, ECONNREFUSED);
    unlink(path);
}

// The restart loop of a daemon, many times over: each round's listener, and
// only that one, is reached.
static void test_rebind_cycles(void) {
    char path[96];
    snprintf(path, sizeof(path), "%s/cycle", dir);
    for (int i = 0; i < 100; i++) {
        unlink(path);
        int l = sock_of(SOCK_STREAM);
        if (do_bind(l, path) < 0 || listen(l, 8) < 0) {
            printf("FAIL cycle %d: bind/listen: %s\n", i, strerror(errno));
            failures_total++;
            close(l);
            break;
        }
        char label[48];
        snprintf(label, sizeof(label), "cycle %d", i);
        expect_reaches(label, path, l);
        close(l);
    }
    expect_connect_err("cycle: after the last listener closed", path, ECONNREFUSED);
    unlink(path);
}

// A unix socket is named once. A second bind to a fresh path is EINVAL and
// leaves no node there; the first name keeps working. An accepted socket
// shares its listener's name, so it cannot be bound either.
static void test_second_bind(void) {
    char path[96], other[96];
    snprintf(path, sizeof(path), "%s/first", dir);
    snprintf(other, sizeof(other), "%s/second", dir);
    int l = listener_at("second: listener", path);
    expect_err("second: bind a bound socket to a new path", do_bind(l, other), EINVAL);
    expect_missing("second: no node at the refused path", other);

    struct sockaddr_un abs;
    memset(&abs, 0, sizeof(abs));
    abs.sun_family = AF_UNIX;
    snprintf(abs.sun_path + 1, sizeof(abs.sun_path) - 1, "aok-upnr-%d", (int) getpid());
    socklen_t abs_len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 +
                                     strlen(abs.sun_path + 1));
    expect_err("second: bind a bound socket to an abstract name",
               bind(l, (struct sockaddr *) &abs, abs_len), EINVAL);
    // ...and the refused abstract name is free for somebody else.
    int a = sock_of(SOCK_STREAM);
    expect_ok("second: the refused abstract name is still free",
              bind(a, (struct sockaddr *) &abs, abs_len));
    close(a);

    int c = sock_of(SOCK_STREAM);
    expect_ok("second: connect", do_connect(c, path));
    int s = pending(l, 5000) ? accept(l, NULL, NULL) : -1;
    expect_ok("second: accept", s);
    if (s >= 0) {
        expect_err("second: bind an accepted socket", do_bind(s, other), EINVAL);
        expect_missing("second: no node after the accepted socket's bind", other);
        close(s);
    }
    close(c);

    struct sockaddr_un got;
    socklen_t got_len = sizeof(got);
    if (getsockname(l, (struct sockaddr *) &got, &got_len) < 0 ||
            strcmp(got.sun_path, path) != 0) {
        printf("FAIL second: getsockname no longer names the first path\n");
        failures_total++;
    }
    expect_reaches("second: the first name still reaches it", path, l);
    close(l);
    unlink(path);
    unlink(other);
}

// Datagrams: a bound receiver gets them, and once it is closed its node
// refuses them.
static void test_dgram(void) {
    char path[96];
    snprintf(path, sizeof(path), "%s/dgram", dir);
    int r = sock_of(SOCK_DGRAM);
    expect_ok("dgram: bind", do_bind(r, path));
    int w = sock_of(SOCK_DGRAM);
    struct sockaddr_un a = addr_of(path);
    expect_ok("dgram: sendto", (int) sendto(w, "d", 1, 0, (struct sockaddr *) &a, sizeof(a)));
    char b = 0;
    if (!pending(r, 5000) || recv(r, &b, 1, 0) != 1 || b != 'd') {
        printf("FAIL dgram: the datagram did not arrive\n");
        failures_total++;
    }
    close(r);
    expect_err("dgram: sendto a node nobody is bound to",
               (int) sendto(w, "d", 1, 0, (struct sockaddr *) &a, sizeof(a)), ECONNREFUSED);
    close(w);
    unlink(path);
}

// The bound socket holds its filesystem: busy while it is bound, free once it
// closes, whether or not the node is removed. Needs mount(2) -- root, or a
// mount namespace -- and is skipped without it.
static void test_mount_pin(void) {
    char mnt[96], path[128];
    snprintf(mnt, sizeof(mnt), "%s/mnt", dir);
    snprintf(path, sizeof(path), "%s/s", mnt);
    if (mkdir(mnt, 0755) < 0 && errno != EEXIST) {
        printf("FAIL mount: mkdir %s: %s\n", mnt, strerror(errno));
        failures_total++;
        return;
    }
    if (mount("none", mnt, "tmpfs", 0, NULL) < 0) {
        test_logf("mount: skipped (%s)\n", strerror(errno));
        rmdir(mnt);
        return;
    }
    int l = listener_at("mount: listener", path);
    expect_err("mount: umount while a socket is bound in it", umount(mnt), EBUSY);
    close(l);
    // The node is still there. Nothing is bound to it, so nothing holds the
    // filesystem.
    int ret = umount(mnt);
    expect_ok("mount: umount once the socket closed", ret);
    if (ret < 0 && umount2(mnt, MNT_DETACH) < 0)
        printf("FAIL mount: could not detach %s either: %s\n", mnt, strerror(errno));

    // Twice more with the node removed first, and with a connection accepted
    // and closed -- the lookups must not hold anything either.
    if (mount("none", mnt, "tmpfs", 0, NULL) == 0) {
        l = listener_at("mount: second listener", path);
        expect_reaches("mount: reaches the second listener", path, l);
        close(l);
        unlink(path);
        ret = umount(mnt);
        expect_ok("mount: umount after connections and unlink", ret);
        if (ret < 0)
            umount2(mnt, MNT_DETACH);
    }
    rmdir(mnt);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    snprintf(dir, sizeof(dir), "/tmp/upnr-%d", (int) getpid());
    if (mkdir(dir, 0755) < 0) {
        printf("unix_path_name_release: FAIL mkdir %s: %s\n", dir, strerror(errno));
        return 1;
    }

    test_stale_node();
    test_unlink_while_listening();
    test_rename_while_listening();
    test_rebind_cycles();
    test_second_bind();
    test_dgram();
    test_mount_pin();

    rmdir(dir);
    if (failures_total != 0) {
        printf("unix_path_name_release: FAIL (%u)\n", failures_total);
        return 1;
    }
    printf("unix_path_name_release: PASS\n");
    return 0;
}
