#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
// For the ISH_SOCKRESTART_TEST_DESTROY=defunct knob, which uses private calls:
// a device build carries no trace of it.
#if TARGET_OS_OSX || TARGET_OS_SIMULATOR
#define SOCKRESTART_TEST_DEFUNCT 1
#include <dlfcn.h>
#endif
#endif
#include "fs/sockrestart.h"
#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/task.h"
#include "util/list.h"
extern const struct fd_ops socket_fdops;

static lock_t sockrestart_lock = LOCK_INITIALIZER;
static struct list listen_fds = LIST_INITIALIZER(listen_fds);

void sockrestart_begin_listen(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock, 0);
    // Once per socket, however many times it listens. A second listen() is
    // legal -- it only changes the backlog -- and adding the node again linked
    // it to itself, so the next on_suspend walked that one node for ever with
    // the lock held: the backgrounding app hung, and so did every guest
    // accept behind the lock.
    if (list_null(&sock->sockrestart.listen))
        list_add(&listen_fds, &sock->sockrestart.listen);
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock, 0);
    list_remove_safe(&sock->sockrestart.listen);
    unlock(&sockrestart_lock);
}

static struct list listen_tasks = LIST_INITIALIZER(listen_tasks);

void sockrestart_begin_listen_wait(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock, 0);
    if (current->sockrestart.count == 0)
        list_add(&listen_tasks, &current->sockrestart.listen);
    current->sockrestart.count++;
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen_wait(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock, 0);
    current->sockrestart.count--;
    if (current->sockrestart.count == 0)
        list_remove(&current->sockrestart.listen);
    unlock(&sockrestart_lock);
}

bool sockrestart_should_restart_listen_wait(int skip) {
    lock(&sockrestart_lock, 0);
    bool punt = current->sockrestart.punt;

    current->sockrestart.punt = false;
    unlock(&sockrestart_lock);
    
    if(skip)
        return punt;
    
    if(punt == false) {
        current->stuck_count++;
    } else {
        current->stuck_count = 0;
    }
    
    if(current->stuck_count < 3) {
        return punt;
    } else {
        printk("INFO: punting(%d) (%s:%d) \n", current->stuck_count, current->comm, current->pid);
        current->stuck_count = 0;
        return true;
    }
}

struct saved_socket {
    struct fd *sock;
    int type;
    int proto;
    int backlog;
    int flags;
    union {
        char name[128];
        struct sockaddr name_addr;
    };
    socklen_t name_len;
    struct list saved;
};

static struct list saved_sockets = LIST_INITIALIZER(saved_sockets);

// Whether the guest's listener still works, so that a resume replaces only the
// dead ones.
//
// The resume runs whether or not the app was ever frozen -- the app calls it
// on every return to the foreground, and from the timer that fires when a
// backgrounded app turns out never to have been suspended -- and replacing a
// live listener throws away every connection waiting in its queue. For TCP
// the rebind used to double as this test: the port is only free once the
// listener is gone. An AF_UNIX socket's name is a FILE, which outlives the
// socket bound to it, so its rebind fails either way and a dead unix listener
// was never replaced.
//
// Dead is either of:
//   - The descriptor no longer has the name the save recorded: it has been
//     replaced (ISH_SOCKRESTART_TEST_DESTROY's fresh unbound socket).
//   - listen() fails. A suspension leaves the socket in place, NAME AND ALL,
//     marked defunct, and XNU's solisten refuses a defunct socket with
//     EINVAL. On a live listener it only restates the backlog the guest gave;
//     its queue is untouched.
// Not a connect probe: it would queue a connection the guest's server then
// accepts, and a defunct AF_UNIX listener still accepts connects
// (unp_connect never looks at SOF_DEFUNCT), so it would not tell anyway.
static bool listener_is_dead(struct saved_socket *saved) {
    union {
        char name[sizeof(saved->name)];
        struct sockaddr addr;
    } now;
    socklen_t now_len = sizeof(now.name);
    if (getsockname(saved->sock->real_fd, &now.addr, &now_len) < 0 ||
            now_len != saved->name_len || memcmp(now.name, saved->name, now_len) != 0)
        return true;
    return listen(saved->sock->real_fd, saved->backlog) < 0;
}

// A dead AF_UNIX listener's name is still taken by its socket file. The
// path is this process's own ishsock name for that one guest socket (fs/sock.c
// unix_host_sun_path), so nothing else can be using it; remove it only if it
// is still a socket all the same.
static void unlink_stale_unix_name(struct saved_socket *saved) {
    struct sockaddr_un *un = (struct sockaddr_un *) &saved->name_addr;
    size_t offset = offsetof(struct sockaddr_un, sun_path);
    if (saved->name_len <= offset)
        return;
    char path[sizeof(un->sun_path) + 1];
    size_t path_len = saved->name_len - offset;
    if (path_len > sizeof(un->sun_path))
        path_len = sizeof(un->sun_path);
    memcpy(path, un->sun_path, path_len);
    path[path_len] = '\0';
    struct stat st;
    if (path[0] != '\0' && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
        unlink(path);
}

// ISH_SOCKRESTART_TEST_DESTROY=defunct -- what iOS actually does to the
// sockets of an app it suspends, done on a Mac. The system calls
// pid_shutdown_sockets(pid, SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL) on the
// process (a private libsystem_kernel call; the level is 2 in xnu's
// sys/proc.h), which defuncts every socket that may be defuncted -- and XNU's
// socreate marks every PF_LOCAL socket SOF_NODEFUNCT, which that call does not
// override. So a suspension kills TCP and UDP listeners and leaves AF_UNIX
// ones working. Measured on macOS 26: afterwards a TCP listener refuses
// connections and fails listen() with EINVAL, and an AF_UNIX listener still
// serves. Looked up at run time, so the app never links the private symbol.
static void test_defunct(void) {
#ifdef SOCKRESTART_TEST_DEFUNCT
    int (*shutdown_sockets)(int pid, int level) =
            (int (*)(int, int)) dlsym(RTLD_DEFAULT, "pid_shutdown_sockets");
    if (shutdown_sockets == NULL) {
        printk("WARNING: sockrestart: no pid_shutdown_sockets\n");
        return;
    }
    if (shutdown_sockets(getpid(), 2) < 0) {
        printk("WARNING: sockrestart: pid_shutdown_sockets failed: %s\n", strerror(errno));
        return;
    }
    printk("sockrestart: test: defuncted this process's sockets\n");
#else
    printk("WARNING: sockrestart: ISH_SOCKRESTART_TEST_DESTROY=defunct needs a Mac\n");
#endif
}

// Clear the socket's SOF_NODEFUNCT, so test_defunct takes it too. SO_DEFUNCTOK
// is 0x1100 in xnu's sys/socket.h, private again; any process may set it.
static void test_make_defunctable(int fd) {
#ifdef SOCKRESTART_TEST_DEFUNCT
    int one = 1;
    setsockopt(fd, SOL_SOCKET, 0x1100, &one, sizeof(one));
#else
    (void) fd;
#endif
}

// these should only be called from the main thread, but it's easiest to just lock for the whole time

unsigned sockrestart_on_suspend() {
    lock(&sockrestart_lock, 0);
    // Idempotent, not asserted. This is now called when the app is
    // BACKGROUNDED rather than when suspension is imminent, because iOS can
    // tear the host sockets down before any "about to suspend" callback runs --
    // which is exactly how the first version of this failed on a device: the
    // listener died, the save had not happened yet, and resume had nothing to
    // rebuild. Backgrounding can be reported more than once (per scene, and
    // again if the app never actually suspends), so a second call with a
    // populated list means "already saved" and must not abort.
    if (!list_empty(&saved_sockets)) {
        unlock(&sockrestart_lock);
        return 0;
    }
    enum { DESTROY_NONE, DESTROY_REPLACE, DESTROY_DEFUNCT, DESTROY_DEFUNCT_ALL } destroy = DESTROY_NONE;
    const char *destroy_spec = getenv("ISH_SOCKRESTART_TEST_DESTROY");
    if (destroy_spec != NULL)
        destroy = strcmp(destroy_spec, "defunct") == 0 ? DESTROY_DEFUNCT :
                  strcmp(destroy_spec, "defunct-all") == 0 ? DESTROY_DEFUNCT_ALL :
                  DESTROY_REPLACE;
    unsigned saved_count = 0;
    struct fd *sock;
    list_for_each_entry(&listen_fds, sock, sockrestart.listen) {
        struct saved_socket *saved = malloc(sizeof(struct saved_socket));
        if (saved == NULL)
            continue; // better than a crash
        // Not plain fd_retain. A socket whose last reference has just gone is
        // still on this list: sock_close takes it off in sockrestart_end_listen,
        // which is waiting for the lock held here. Retaining it would hand the
        // resume a pointer to an fd that is freed as soon as we unlock.
        saved->sock = fd_retain_if_live(sock);
        if (saved->sock == NULL) {
            free(saved);
            continue;
        }
        saved->proto = sock->socket.protocol;
        saved->backlog = sock->sockrestart.backlog;
        saved->flags = fcntl(sock->real_fd, F_GETFL);
        unsigned size = sizeof(saved->type);
        getsockopt(sock->real_fd, SOL_SOCKET, SO_TYPE, &saved->type, &size);
        assert(size == sizeof(saved->type));
        saved->name_len = sizeof(saved->name);
        getsockname(sock->real_fd, (struct sockaddr *) &saved->name, &saved->name_len);
        list_add(&saved_sockets, &saved->saved);
        saved_count++;
        // ISH_SOCKRESTART_TEST_DESTROY -- kill the listeners on the way down,
        // so the rebuild can be exercised on a Mac, where nothing else does:
        //   =defunct      what a suspension does (test_defunct). TCP and UDP
        //                 listeners die; AF_UNIX ones are left working.
        //   =defunct-all  the same, with the AF_UNIX listeners made eligible
        //                 first: the real defunct state, on the one kind of
        //                 listener iOS spares.
        //   anything else a fresh unbound socket dup2'd over each listener. The
        //                 fd stays open, the name is released, and the guest is
        //                 not told. Works on any host, but it is not quite a
        //                 defunct socket: a blocked accept() gets EINVAL from
        //                 it before the rebuild, where a defunct listener
        //                 would keep it waiting.
        if (destroy == DESTROY_DEFUNCT_ALL) {
            test_make_defunctable(sock->real_fd);
        } else if (destroy == DESTROY_REPLACE) {
            int dead = socket(saved->name_addr.sa_family, saved->type, saved->proto);
            if (dead >= 0) {
                dup2(dead, sock->real_fd);
                close(dead);
                printk("sockrestart: test: destroyed the listener at fd %d\n",
                       sock->real_fd);
            }
        }
    }
    if (destroy == DESTROY_DEFUNCT || destroy == DESTROY_DEFUNCT_ALL)
        test_defunct();
    unlock(&sockrestart_lock);
    return saved_count;
}

unsigned sockrestart_on_resume() {
    // The references the save took are dropped after the unlock, not in the
    // loop. If the guest closed a listener while we were away, ours is the
    // last reference, and dropping it runs sock_close -> sockrestart_end_listen,
    // which takes sockrestart_lock. Doing that under the lock deadlocked the
    // resuming thread on itself, and every guest socket close after it queued
    // up behind the lock it never let go of: the whole guest hung.
    struct list done;
    list_init(&done);
    lock(&sockrestart_lock, 0);
    unsigned restored = 0;
    // Sockets we had RECORDED, as opposed to ones we managed to put back.
    // The punt below keys on this, not on the successes: see why there.
    unsigned processed = 0;
    struct saved_socket *saved, *tmp;
    list_for_each_entry_safe(&saved_sockets, saved, tmp, saved) {
        list_remove(&saved->saved);
        list_add(&done, &saved->saved);
        processed++;
        // Only we still hold it: the guest has closed this listener, so there
        // is nothing to put back, and a rebuild would only listen again at an
        // address nobody is serving until the close below.
        if (saved->sock->refcount == 1)
            continue;
        if (!listener_is_dead(saved))
            continue;
        int new_sock = socket(saved->name_addr.sa_family, saved->type, saved->proto);
        if (new_sock < 0) {
            printk("WARNING: restarting socket(%d, %d, %d) failed: %s\n",
                    saved->name_addr.sa_family, saved->type, saved->proto, strerror(errno));
            goto thank_u_next;
        }
        // The socket being replaced is still open at saved->sock->real_fd --
        // it is only closed by the dup2 below, and it cannot be closed sooner
        // without giving up the fd number the server is using. So the address
        // may still be considered taken, and a plain bind would fail with
        // EADDRINUSE. This whole path had never run before, so nothing had
        // ever surfaced that.
        int reuse = 1;
        setsockopt(new_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(new_sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
        if (saved->name_addr.sa_family == AF_UNIX)
            unlink_stale_unix_name(saved);
        if (bind(new_sock, (struct sockaddr *) &saved->name, saved->name_len) < 0) {
            printk("WARNING: rebinding socket failed: %s\n", strerror(errno));
            close(new_sock);
            goto thank_u_next;
        }
        if (saved->flags >= 0)
            fcntl(new_sock, F_SETFL, saved->flags);
        if (listen(new_sock, saved->backlog) < 0) {
            printk("WARNING: relistening socket failed: %s\n", strerror(errno));
            close(new_sock);
            goto thank_u_next;
        }
        if (dup2(new_sock, saved->sock->real_fd) < 0) {
            printk("WARNING: replacing socket fd failed: %s\n", strerror(errno));
            close(new_sock);
            goto thank_u_next;
        }
        close(new_sock);
        restored++;

thank_u_next:
        ;
    }
    // Kick the accept()ers whenever a suspension was RECORDED -- not only when
    // a rebuild succeeded.
    //
    // The guard used to be `restored != 0`, to avoid firing a SIGUSR1 at every
    // listening task on every unlock of the phone. That much is right and is
    // preserved: `processed` is zero unless a backgrounding actually recorded
    // listeners, which is the same condition by a better name.
    //
    // Keying on the SUCCESSES was wrong, though. A task waiting on a listener
    // that was destroyed and could NOT be rebuilt is in exactly the state that
    // most needs waking: its wait refers to a socket that no longer exists, and
    // nothing else will ever disturb it. Leaving it asleep was the difference
    // between an error it can report and a daemon that waits for ever -- and a
    // rebuild that partly failed punted for the sockets that worked while
    // abandoning the tasks behind the ones that did not.
    if (processed != 0) {
        struct task *task;
        list_for_each_entry(&listen_tasks, task, sockrestart.listen) {
            task->sockrestart.punt = true;
            pthread_kill(task->thread, SIGUSR1);
        }
    }
    unlock(&sockrestart_lock);
    list_for_each_entry_safe(&done, saved, tmp, saved) {
        list_remove(&saved->saved);
        fd_close(saved->sock);
        free(saved);
    }
    return restored;
}
