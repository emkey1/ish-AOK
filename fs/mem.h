#ifndef FS_NULL_H
#define FS_NULL_H

#include <stdbool.h>
#include "kernel/fs.h"
#include "fs/dev.h"

extern struct dev_ops mem_dev;

// The kernel-log stream behind /dev/kmsg and /proc/kmsg. *pos is an absolute
// position (kernel/log.h) that the caller keeps; the read blocks until there
// is something past it unless nonblock.
ssize_t kmsg_stream_read(unsigned long *pos, void *buf, size_t bufsize, bool nonblock);
int kmsg_stream_poll(unsigned long pos);
// Wake anything watching /dev/kmsg. Called from ish_vprintk with log_lock
// released.
void kmsg_notify_readers(void);

#endif
