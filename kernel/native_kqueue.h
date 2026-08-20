// A kqueue front end for native programs, over the guest's ppoll.
//
// Why this exists. A native program is host arm64 code, so a runtime built for
// Apple reaches for kqueue -- mio does, and therefore so do tokio, and
// therefore so does every large async Rust program. Unrouted, kqueue() gives
// it the HOST's event queue and kevent() then registers GUEST descriptor
// numbers in it, which are meaningless there. It does not half-work:
// tokio's Runtime::build fails with EBADF before a task runs.
//
// Why ppoll rather than the guest's epoll. The registration table has to live
// on this side either way -- kqueue is keyed by (ident, filter) and epoll by
// descriptor alone, so one epoll registration cannot carry the two udata
// values a kqueue fd may have -- and once the table is here, ppoll needs no
// second copy of it inside the guest. mio's waker is EVFILT_USER, which has no
// epoll counterpart and wants a pipe regardless. A program watching a handful
// of descriptors is not limited by rebuilding a pollfd array per call.
#ifndef NATIVE_KQUEUE_H
#define NATIVE_KQUEUE_H

#include <stddef.h>

// Returns a guest descriptor. Closing it through the shim's close() releases
// the queue; see nlibc_kqueue_close_hook.
int nlibc_kqueue(void);

// changelist/eventlist are `struct kevent *` and timeout is
// `const struct timespec *`; spelled as void here so that <sys/event.h> does
// not have to reach every file that includes the shim's header.
int nlibc_kevent(int kq, const void *changelist, int nchanges,
                 void *eventlist, int nevents, const void *timeout);

// Called from nlibc_close before the descriptor goes back to the guest.
// Returns true if the descriptor was a kqueue and has been dealt with.
bool nlibc_kqueue_close_hook(int fd);

// Called after a successful dup of any descriptor. A dup of a kqueue names the
// SAME queue, and mio depends on it: Waker::new does selector.try_clone(), so
// the waker fires EVFILT_USER through a different descriptor number than the
// one the poller waits on. Without this that call got EBADF, which is what
// tokio reported as "Runtime::build failed".
void nlibc_kqueue_dup_hook(int oldfd, int newfd);

#endif
