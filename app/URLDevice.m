//
//  URLDevice.m
//  iSH-AOK
//
//  /dev/url: write a URL, iOS opens it.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <ctype.h>
#include <string.h>
#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/errno.h"
#import "URLDevice.h"

// The longest URL we will look at. Safari's own limit is far below this and a
// scheme handler that wants more than 8 KB is not a thing; the cap exists so a
// guest cannot make the kernel allocate on its say-so.
#define URL_MAX_LEN 8192

// How long a write waits for iOS to say whether it opened the URL. openURL is
// asynchronous and answers on the main thread; the wait is what lets the write
// report the truth instead of a byte count it has not earned. Bounded because a
// guest write must never become an unkillable wait on the UI thread.
#define URL_OPEN_TIMEOUT_SEC 5

static ssize_t url_read(struct fd *UNUSED(fd), void *UNUSED(buf), size_t UNUSED(bufsize)) {
    // There is nothing to read back. End of file, not an error: `cat /dev/url`
    // should be empty rather than fail, the same way reading /dev/null is.
    return 0;
}

static ssize_t url_write(struct fd *UNUSED(fd), const void *buf, size_t size) {
    if (size == 0)
        return 0;
    if (size > URL_MAX_LEN)
        return _EINVAL;

    // `echo https://example.com > /dev/url` sends a trailing newline, and NSURL
    // will not parse one. Trim trailing whitespace rather than making every
    // caller remember -n.
    const char *bytes = buf;
    size_t len = size;
    while (len > 0 && isspace((unsigned char) bytes[len - 1]))
        len--;
    if (len == 0)
        return (ssize_t) size;  // a bare newline is a no-op, not an error

    NSString *string = [[NSString alloc] initWithBytes:bytes
                                               length:len
                                             encoding:NSUTF8StringEncoding];
    if (string == nil)
        return _EINVAL;
    NSURL *url = [NSURL URLWithString:string];
    // A URL with no scheme is a relative reference; iOS has nothing to route it
    // to, and openURL would fail with no explanation. Refuse it here so
    // `echo youtube.com > /dev/url` says EINVAL instead of failing silently.
    if (url == nil || url.scheme == nil || url.scheme.length == 0)
        return _EINVAL;

    __block BOOL opened = NO;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    // openURL must be called on the main thread; a guest write arrives on
    // whichever task thread made the syscall.
    dispatch_async(dispatch_get_main_queue(), ^{
        [[UIApplication sharedApplication] openURL:url
                                           options:@{}
                                 completionHandler:^(BOOL success) {
            opened = success;
            dispatch_semaphore_signal(done);
        }];
    });

    dispatch_time_t deadline =
        dispatch_time(DISPATCH_TIME_NOW, (int64_t) URL_OPEN_TIMEOUT_SEC * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(done, deadline) != 0)
        return _ETIMEDOUT;

    // iOS refuses the open outright when the app is not in the foreground, and
    // for a scheme no installed app claims. Both are EPERM here rather than a
    // successful-looking byte count: a write that always claimed success would
    // make `echo … > /dev/url` indistinguishable from working.
    return opened ? (ssize_t) size : _EPERM;
}

static int url_poll(struct fd *UNUSED(fd)) {
    // Always writable, never readable: there is no event to wait for.
    return POLL_WRITE;
}

static int url_open(int UNUSED(major), int UNUSED(minor), struct fd *UNUSED(fd)) {
    return 0;
}

struct dev_ops url_dev = {
    .open = url_open,
    .fd.read = url_read,
    .fd.write = url_write,
    .fd.poll = url_poll,
};
