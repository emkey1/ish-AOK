#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
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
    unsigned saved_count = 0;
    struct fd *sock;
    list_for_each_entry(&listen_fds, sock, sockrestart.listen) {
        struct saved_socket *saved = malloc(sizeof(struct saved_socket));
        if (saved == NULL)
            continue; // better than a crash
        saved->sock = fd_retain(sock);
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
        // ISH_SOCKRESTART_TEST_DESTROY=1 -- do to the socket what a SUSPENSION
        // does to it, so the rebuild can be exercised anywhere.
        //
        // On a Mac nothing destroys a listener, so the rebuild's bind always
        // hits EADDRINUSE against the original, returns 0 restored, and every
        // line after it -- the dup2, the punt -- has never run outside a real
        // iOS suspension. Replacing the descriptor with a fresh unbound socket
        // of the same type reproduces exactly what iOS leaves behind: the fd
        // is still open, the port is released, and the guest's listener is
        // dead without the guest being told.
        if (getenv("ISH_SOCKRESTART_TEST_DESTROY") != NULL) {
            int dead = socket(saved->name_addr.sa_family, saved->type, saved->proto);
            if (dead >= 0) {
                dup2(dead, sock->real_fd);
                close(dead);
                printk("INFO: sockrestart: test-destroyed the listener at fd %d\n",
                       sock->real_fd);
            }
        }
    }
    unlock(&sockrestart_lock);
    return saved_count;
}

unsigned sockrestart_on_resume() {
    lock(&sockrestart_lock, 0);
    unsigned restored = 0;
    // Sockets we had RECORDED, as opposed to ones we managed to put back.
    // The punt below keys on this, not on the successes: see why there.
    unsigned processed = 0;
    struct saved_socket *saved, *tmp;
    list_for_each_entry_safe(&saved_sockets, saved, tmp, saved) {
        list_remove(&saved->saved);
        processed++;
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
        fd_close(saved->sock);
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
    return restored;
}
