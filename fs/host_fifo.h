#ifndef FS_HOST_FIFO_H
#define FS_HOST_FIFO_H
#include <stdbool.h>

struct fd;

// A named FIFO that is a HOST object (realfs, and fakefs, which opens its
// FIFOs through realfs): the reader and writer accounting Linux's pipe_poll
// answers from, kept by AOK because Darwin keeps none it will report. Darwin's
// poll and kqueue say nothing when a FIFO's last writer or last reader goes --
// measured: a reader at end of file polls as idle, not POLLHUP, and a writer
// whose readers have all gone polls POLLOUT, not POLLERR -- so a guest waiting
// for either never heard of it. (An anonymous pipe is reported properly by the
// host and is not tracked here.)

// After the host open of `fd`: if it is a FIFO, count this description as a
// reader, a writer or both, by `accmode` (the guest's O_ACCMODE_ bits).
void host_fifo_attach(struct fd *fd, int accmode);
// From the close: uncount it, and wake whoever's state that changed.
void host_fifo_detach(struct fd *fd);
// Whether `fd` is a tracked host FIFO.
bool host_fifo_tracked(struct fd *fd);
// Poll bits (host values, which equal the guest's) for a tracked FIFO, given
// what the host said: POLLIN only with bytes waiting, POLLHUP for a reader
// once a writer has come and gone, POLLERR for a writer with no reader left.
int host_fifo_poll(struct fd *fd, int host_revents);

#endif
