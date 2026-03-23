#ifndef KERNEL_INOTIFY_H
#define KERNEL_INOTIFY_H

#include <stdbool.h>

void inotify_notify_open(const char *path);
void inotify_notify_modify(const char *path);
void inotify_notify_attrib(const char *path);
void inotify_notify_create(const char *path, bool is_dir);
void inotify_notify_delete(const char *path, bool is_dir);
void inotify_notify_move(const char *old_path, const char *new_path, bool is_dir);

#endif
