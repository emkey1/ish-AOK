//
//  URLDevice.h
//  iSH-AOK
//
//  /dev/url: write a URL, iOS opens it.
//

#ifndef URLDevice_h
#define URLDevice_h

#include "fs/dev.h"

// The /dev/url character device. Writing a URL to it hands that URL to iOS,
// which routes it to whatever app claims the scheme -- the same thing tapping
// a link does. Reads return nothing; there is no state to read back.
//
// This is a device rather than a command on purpose: it is the shape
// /dev/clipboard and /dev/location already use for "something only the host
// can do", it composes with redirection and pipes the way a shell expects, and
// it needs no new binary in the guest filesystem.
extern struct dev_ops url_dev;

#endif /* URLDevice_h */
