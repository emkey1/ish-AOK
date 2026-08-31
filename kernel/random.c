#include <fcntl.h>
#include "kernel/calls.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonRandom.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/random.h>
#endif

int get_random(char *buf, size_t len) {
#ifdef __APPLE__
    return CCRandomGenerateBytes(buf, len) != kCCSuccess;
#else
    return syscall(SYS_getrandom, buf, len, 0) < 0;
#endif
}

#define GRND_NONBLOCK_ 0x0001
#define GRND_RANDOM_   0x0002
#define GRND_INSECURE_ 0x0004

// getrandom refused any request over 1 MiB with EIO -- a size limit Linux does
// not have, reported with an errno that says the random source broke rather
// than that the request was too big. Anything seeding a large buffer in one
// call (a key schedule, a big shuffle) got a hardware-failure story instead of
// its bytes. And flags were ignored entirely, so an unknown flag succeeded --
// which is how a caller probes for a feature, and it was told yes for
// everything.
//
// The 1 MiB stays as the CHUNK size: filling a guest-sized request through one
// malloc is a guest-controlled allocation, and there is no reason to make it.
static dword_t sys_getrandom_common(guest_addr_t buf_addr, dword_t len, dword_t flags) {
    if (flags & ~(GRND_NONBLOCK_ | GRND_RANDOM_ | GRND_INSECURE_))
        return _EINVAL;
    // The two source flags contradict each other: one asks for the blocking
    // entropy pool, the other says do not block even if it is unseeded.
    if ((flags & GRND_RANDOM_) && (flags & GRND_INSECURE_))
        return _EINVAL;
    if (len == 0)
        return 0;

    // AOK's source is always ready, so GRND_NONBLOCK never has to say EAGAIN
    // and GRND_RANDOM never blocks -- which is what a seeded /dev/urandom does
    // on Linux too.
    static const dword_t CHUNK = 1 << 20;
    dword_t chunk = len < CHUNK ? len : CHUNK;
    char *buf = malloc(chunk);
    if (buf == NULL)
        return _ENOMEM;

    dword_t done = 0;
    while (done < len) {
        dword_t want = len - done < chunk ? len - done : chunk;
        if (get_random(buf, want) != 0) {
            free(buf);
            // Nothing written yet is the error; a short fill is a short read.
            return done > 0 ? done : (dword_t) _EIO;
        }
        if (user_write(buf_addr + done, buf, want)) {
            free(buf);
            return done > 0 ? done : (dword_t) _EFAULT;
        }
        done += want;
    }
    free(buf);
    return done;
}

dword_t sys_getrandom(addr_t buf_addr, dword_t len, dword_t flags) {
    return sys_getrandom_common(buf_addr, len, flags);
}

dword_t sys_getrandom_guest(guest_addr_t buf_addr, dword_t len, dword_t flags) {
    return sys_getrandom_common(buf_addr, len, flags);
}
