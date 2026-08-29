#include "debug.h"
#include "kernel/task.h"
#include "kernel/signal.h"
#include "kernel/errno.h"

int err_map(int err) {
#define ERRCASE(err) \
        case err: return _##err;
    switch (err) {
        ERRCASE(EPERM)
        ERRCASE(ENOENT)
        ERRCASE(ESRCH)
        ERRCASE(EINTR)
        ERRCASE(EIO)
        ERRCASE(ENXIO)
        ERRCASE(E2BIG)
        ERRCASE(ENOEXEC)
        ERRCASE(EBADF)
        ERRCASE(ECHILD)
        ERRCASE(EAGAIN)
        ERRCASE(ENOMEM)
        ERRCASE(EACCES)
        ERRCASE(EFAULT)
        ERRCASE(ENOTBLK)
        ERRCASE(EBUSY)
        ERRCASE(EEXIST)
        ERRCASE(EXDEV)
        ERRCASE(ENODEV)
        ERRCASE(ENOTDIR)
        ERRCASE(EISDIR)
        ERRCASE(EINVAL)
        ERRCASE(ENFILE)
        ERRCASE(EMFILE)
        ERRCASE(ENOTTY)
        ERRCASE(ETXTBSY)
        ERRCASE(EFBIG)
        ERRCASE(ENOSPC)
        ERRCASE(ESPIPE)
        ERRCASE(EROFS)
        ERRCASE(EMLINK)
        ERRCASE(EPIPE)
        ERRCASE(EDOM)
        ERRCASE(ERANGE)
        ERRCASE(EDEADLK)
        ERRCASE(ENAMETOOLONG)
        ERRCASE(ENOLCK)
        ERRCASE(ENOSYS)
        ERRCASE(ENOTEMPTY)
        ERRCASE(ELOOP)
        ERRCASE(ENOSTR)
        ERRCASE(ENODATA)
        ERRCASE(ETIME)
        ERRCASE(ENOSR)
        ERRCASE(EREMOTE)
        ERRCASE(ENOLINK)
        ERRCASE(EPROTO)
        ERRCASE(EMULTIHOP)
        ERRCASE(EBADMSG)
        ERRCASE(EOVERFLOW)
        ERRCASE(EILSEQ)
        ERRCASE(EUSERS)
        ERRCASE(ENOTSOCK)
        ERRCASE(EDESTADDRREQ)
        ERRCASE(EMSGSIZE)
        ERRCASE(EPROTOTYPE)
        ERRCASE(ENOPROTOOPT)
        ERRCASE(EPROTONOSUPPORT)
        ERRCASE(ESOCKTNOSUPPORT)
        ERRCASE(EOPNOTSUPP)
#if EOPNOTSUPP != ENOTSUP
        ERRCASE(ENOTSUP)
#endif
        ERRCASE(EPFNOSUPPORT)
        ERRCASE(EAFNOSUPPORT)
        ERRCASE(EADDRINUSE)
        ERRCASE(EADDRNOTAVAIL)
        ERRCASE(ENETDOWN)
        ERRCASE(ENETUNREACH)
        ERRCASE(ENETRESET)
        ERRCASE(ECONNABORTED)
        ERRCASE(ECONNRESET)
        ERRCASE(ENOBUFS)
        ERRCASE(EISCONN)
        ERRCASE(ENOTCONN)
        ERRCASE(ESHUTDOWN)
        ERRCASE(ETOOMANYREFS)
        ERRCASE(ETIMEDOUT)
        ERRCASE(ECONNREFUSED)
        ERRCASE(EHOSTDOWN)
        ERRCASE(EHOSTUNREACH)
        ERRCASE(EALREADY)
        ERRCASE(EINPROGRESS)
        ERRCASE(ESTALE)
        ERRCASE(EDQUOT)
    }
#undef ERRCASE
    printk("ERROR: unknown error %d\n", err);
    return -(err | 0x1000);
}

// suppress_sigpipe is MSG_NOSIGNAL: the caller asked for EPIPE without the
// signal. Linux decides this per-send, which is why it cannot live in the
// signal disposition -- a library doing send(..., MSG_NOSIGNAL) must not have
// to alter process-wide state that its caller can observe.
int errno_map_flags(bool suppress_sigpipe) {
    if (errno == EPIPE && !suppress_sigpipe) {
        if(strcmp(current->comm, "dpkg-deb") != 0) { // Suppress SIGPIPE only for dpkg-deb.
            send_signal(current, SIGPIPE_, SIGINFO_NIL);
        } else {
            printk("INFO: EPIPE in dpkg-deb (%d)\n", current->pid);
            return(0);
        }
    }
    return err_map(errno);
}

int errno_map() {
    return errno_map_flags(false);
}
