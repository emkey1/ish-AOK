// Hack to work around the idiotic way iOS handles suspending apps that have
// listening sockets.
// Basically the actual socket part of the file just gets freed, and the socket
// ceases to be a socket. Any attempt to do socket things with it will just
// immediately fail, and anyone blocked on accept will never wake up.
// Solution: keep track of all the listening sockets, and the threads that are
// blocked on them. On suspend, make a record of the names and configuration of
// all the listening sockets. On resume, open new sockets, reconfigure them,
// use dup2 to replace the original sockets, and get any thread waiting on them
// to restart the wait.
// (AF_UNIX listeners are spared: XNU creates them exempt from this. A resume
// replaces only the listeners it finds dead -- see sockrestart.c.)
// This file contains hooks into various other places to do all that.
// https://developer.apple.com/library/archive/technotes/tn2277/_index.html
#ifndef FS_SOCKRESTART_H
#define FS_SOCKRESTART_H
#include <stdbool.h>
#include "util/list.h"
struct fd;

void sockrestart_begin_listen(struct fd *sock);
void sockrestart_end_listen(struct fd *sock);
void sockrestart_begin_listen_wait(struct fd *sock);
void sockrestart_end_listen_wait(struct fd *sock);
bool sockrestart_should_restart_listen_wait(int);
// Both return how many listening sockets they handled, so the caller can log
// it. A save of 0 and a restore of 0 are the two states that look identical
// from outside and mean completely different things.
unsigned sockrestart_on_suspend(void);
unsigned sockrestart_on_resume(void);

struct fd_sockrestart {
    struct list listen;
    int backlog;
};

struct task_sockrestart {
    int count;
    bool punt;
    struct list listen;
};

#endif
