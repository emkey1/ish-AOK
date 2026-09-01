#ifndef KERNEL_INOTIFY_H
#define KERNEL_INOTIFY_H

// fs.inotify.max_queued_events. Enforced in kernel/inotify.c -- at the cap an
// event is dropped and a single IN_Q_OVERFLOW is appended -- and reported by
// /proc/sys/fs/inotify/max_queued_events, which has to be the same number.
#define INOTIFY_MAX_QUEUED_EVENTS 16384

#include <stdbool.h>

// True when at least one inotify instance exists. Callers on hot paths use
// this to skip computing the event path (which can be expensive -- on fakefs
// it's a SQLite lookup) when there is no one to deliver an event to.
bool inotify_has_instances(void);

void inotify_notify_open(const char *path);
void inotify_notify_modify(const char *path);
void inotify_notify_access(const char *path);
// was_writable: the description was opened O_WRONLY or O_RDWR.
void inotify_notify_close(const char *path, bool was_writable);
void inotify_notify_attrib(const char *path);
void inotify_notify_create(const char *path, bool is_dir);
void inotify_notify_delete(const char *path, bool is_dir);
void inotify_notify_move(const char *old_path, const char *new_path, bool is_dir);

#endif
