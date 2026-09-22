#ifndef KERNEL_ANONFD_CKPT_H
#define KERNEL_ANONFD_CKPT_H

// Descriptors with no file behind them, across a checkpoint: how one is
// described when the image is written and built again when it is read.
//
// These are the anon_inode descriptors -- epoll, inotify, eventfd, signalfd,
// timerfd, pidfd -- and memfd. None has a path to reopen and none has a host
// descriptor to hand back, so each needs its own rule, and before these
// existed there was none: struct fd is zero-initialised, so every one of them
// had real_fd 0 and was taken for a standard stream. A restored dbus-daemon
// got /dev/null where its epoll set had been, epoll_pwait failed at once
// forever, and the daemon spun at a full core without ever serving the bus --
// and every login waits on the bus. See kernel/checkpoint.c's
// ckpt_classify_fd.
//
// Each rule lives with the thing it describes, the way fs/sock.c's
// sock_ckpt_describe/sock_ckpt_rebuild do: only the file that owns a
// descriptor type knows what its state is.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fs/fd.h"

// ---- epoll ----------------------------------------------------------------
//
// The set itself is rebuilt empty; its registrations are written after every
// task, because a registration names a descriptor that may belong to another
// process, and are re-added once every descriptor in the image exists.
bool epoll_fd_is(struct fd *fd);
struct fd *epoll_ckpt_new(void);
// `each` is called for every registration with the set's lock held: it may
// look things up, but must not block or take a lock.
typedef void (*epoll_ckpt_each_fn)(void *ctx, struct fd *target,
                                   int32_t guest_fd, int types, uint64_t data);
void epoll_ckpt_each(struct fd *ep, epoll_ckpt_each_fn each, void *ctx);
int epoll_ckpt_add(struct fd *ep, struct fd *target, int32_t guest_fd,
                   int types, uint64_t data);

// ---- eventfd --------------------------------------------------------------
bool eventfd_fd_is(struct fd *fd);
struct fd *eventfd_ckpt_new(uint64_t val, bool semaphore);

// ---- signalfd -------------------------------------------------------------
bool signalfd_fd_is(struct fd *fd);
uint64_t signalfd_ckpt_mask(struct fd *fd);
struct fd *signalfd_ckpt_new(uint64_t mask);

// ---- timerfd --------------------------------------------------------------
//
// The time left, not the deadline: the image is restored by a process whose
// clocks started somewhere else.
struct timerfd_ckpt {
    uint32_t real_clockid;
    uint32_t armed;
    int64_t value_sec, value_nsec;          // until the next expiry
    int64_t interval_sec, interval_nsec;
    uint64_t expirations;                   // counted but not yet read
    // The guest clockid. real_clockid cannot say it: on Darwin the guest's
    // MONOTONIC and BOOTTIME are both CLOCK_MONOTONIC, and a restored timer
    // armed with TFD_TIMER_ABSTIME has to rebase onto the right one.
    uint32_t clock;
    uint32_t reserved;
};
bool timerfd_fd_is(struct fd *fd);
void timerfd_ckpt_describe(struct fd *fd, struct timerfd_ckpt *out);
struct fd *timerfd_ckpt_new(const struct timerfd_ckpt *d);

// ---- inotify --------------------------------------------------------------
//
// Watches keep their numbers -- a program holds a map from watch descriptor
// to what it is watching -- and events already queued are carried too.
bool inotify_fd_is(struct fd *fd);
char *inotify_ckpt_describe(struct fd *fd, size_t *len);  // malloc'd
struct fd *inotify_ckpt_new(const char *blob, size_t len);

// ---- pidfd ----------------------------------------------------------------
//
// Made unbound where the image had it, and bound once every task exists,
// since it usually names a child that is restored after the parent holding
// it. A pidfd never bound answers as Linux does for a reaped process.
struct task;
bool pidfd_fd_is(struct fd *fd);
int32_t pidfd_ckpt_pid(struct fd *fd);         // 0: the process is gone
struct fd *pidfd_ckpt_new_unbound(void);
void pidfd_ckpt_bind(struct fd *fd, struct task *task);

// ---- memfd ----------------------------------------------------------------
//
// Name, seals and contents. A memfd whose contents are larger than
// `max_contents` is not described (NULL): the caller says so rather than
// carrying half of it.
bool memfd_fd_is(struct fd *fd);
char *memfd_ckpt_describe(struct fd *fd, size_t *len, uint64_t max_contents);
struct fd *memfd_ckpt_new(const char *blob, size_t len);

#endif
