#ifndef FS_DEVICES_H
#define FS_DEVICES_H

// losely based on devices.txt from linux

// --- memory devices ---
#define MEM_MAJOR 1
// /dev/null
#define DEV_NULL_MINOR 3
// /dev/zero
#define DEV_ZERO_MINOR 5
// /dev/full
#define DEV_FULL_MINOR 7
// /dev/random
#define DEV_RANDOM_MINOR 8
// /dev/urandom
#define DEV_URANDOM_MINOR 9
// /dev/kmsg
#define DEV_KMSG_MINOR 11

// --- misc devices ---
#define MISC_MAJOR 10
// /dev/fuse
#define DEV_FUSE_MINOR 229

// --- tty devices ---
// /dev/ttyX where X is minor
#define TTY_CONSOLE_MAJOR 4

// --- alternate tty devices ---
#define TTY_ALTERNATE_MAJOR 5
// /dev/tty
#define DEV_TTY_MINOR 0
// /dev/console
#define DEV_CONSOLE_MINOR 1
// /dev/ptmx
#define DEV_PTMX_MINOR 2

// --- pseudo tty devices ---
#define TTY_PSEUDO_MASTER_MAJOR 128
#define TTY_PSEUDO_SLAVE_MAJOR 136

// --- dynamic devices ---
#define DYN_DEV_MAJOR 240
// /dev/rtc
#define DEV_RTC_MAJOR 252

// The simulated swap area, as a BLOCK device. Block and char majors live in
// separate 256-entry tables (fs/dev.c), so 241 colliding with a char major
// would be harmless to dispatch -- but /proc/devices prints both sections to
// one reader, so this is chosen unused in both. 241 is inside Linux's
// devices.txt 240-254 "local/experimental" range, which is what this is: AOK's
// swap area is not a Linux device and 241 is a local number, not a claim on an
// upstream one.
#define AOKSWAP_MAJOR 241
#define DEV_AOKSWAP_MINOR 0
#define DEV_RTC_MINOR 2


// /dev/clipboard
#define DEV_CLIPBOARD_MINOR 0
// /dev/gps
#define DEV_LOCATION_MINOR 1
// /dev/dsp
#define DEV_DSP_MINOR 3
// /dev/url
#define DEV_URL_MINOR 4

#endif
