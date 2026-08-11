#ifndef FS_DYN_DEV_H
#define FS_DYN_DEV_H

// dyn_dev's are dynamically added devices (character only for now)
// with custom dev_ops assigned
// It's useful to add new device "drivers" in runtime (for example,
// devices only present on some platforms)

// dev_ops handing char device with DYN_DEV_MAJOR major number
extern struct dev_ops dyn_dev_char;

// Implement fake rtc.
extern struct dev_ops rtc_dev_char;

// Registeres new block/character device with provided major and
// minor numbers, handled by provided ops
//
// ops should be valid for "kernel" lifetime (should not be freed, but
// might be static), and should not be null
//
// type is DEV_BLOCK or DEV_CHAR
// (only char is supported for now)
//
// major should be DYN_DEV_MAJOR
//
// minor should be 0-255
//
// Return value:
//  - 0 on success
//  - _EEXIST if provided minor number is alredy taken
//  - _EINVAL if provided arguments are invalid
extern int dyn_dev_register(struct dev_ops *ops, int type, int major, int minor);

// Whether a device has actually been registered for this major/minor. A real
// devtmpfs publishes exactly the devices the kernel has, so the population
// done by devtmpfs_mount() (fs/tmp.c) uses this to skip the platform-specific
// nodes -- clipboard/location/dsp are registered by the iOS app and simply do
// not exist under the command-line build, where a node for them would be a
// dangling entry that only ever opens with ENXIO.
extern bool dyn_dev_is_registered(int major, int minor);

#endif
