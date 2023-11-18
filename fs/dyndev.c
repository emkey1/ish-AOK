#include "kernel/errno.h"
#include "fs/dev.h"
#include "fs/dyndev.h"
#include "fs/devices.h"

#define MAX_MINOR 255

typedef struct fd rtc_fd;

// Handles DYNDEV_MAJOR device number
// XXX(stek29): unregister might be added later
struct dyn_dev_info {
    // devs & next_dev lock
    lock_t devs_lock;
    // table of dev_ops registered by minor number
    struct dev_ops *devs[MAX_MINOR+1];
};

struct dyn_dev_info dyn_info_char = {
    .devs_lock = LOCK_INITIALIZER,
    .devs = {},
};

int dyn_dev_register(struct dev_ops *ops, int type, int major, int minor) {
    // Validate arguments
    if (minor < 0 || minor > MAX_MINOR) {
        return _EINVAL;
    }
    if (major != DYN_DEV_MAJOR) {
        return _EINVAL;
    }
    if (ops == NULL) {
        return _EINVAL;
    }
    if (type != DEV_CHAR) {
        return _EINVAL;
    }

    lock(&dyn_info_char.devs_lock, 0);

    // Make sure minor number isn't taken yet
    if (dyn_info_char.devs[minor] != NULL) {
        unlock(&dyn_info_char.devs_lock);
        return _EEXIST;
    }

    dyn_info_char.devs[minor] = ops;
    unlock(&dyn_info_char.devs_lock);

    return 0;
}

static intptr_t dyn_open(int type, int major, int minor, struct fd *fd) {
    assert((type == DEV_CHAR) || (type == DEV_BLOCK));
    assert(major == DYN_DEV_MAJOR || major == DEV_RTC_MAJOR); // mkemkemke
    // it's safe to access devs without locking (read-only)
    
    if(major == DEV_RTC_MAJOR) {
        //return rtc_dev_char(major, minor, fd);
    } else {
        struct dev_ops *ops = dyn_info_char.devs[minor];
        if (ops == NULL) {
            return _ENXIO;
        }
        fd->ops = &ops->fd;
        
        // Succeed if there's no open provided by ops
        if (!ops->open)
            return 0;
        return ops->open(major, minor, fd);
    }
    
    return 0;
}

static intptr_t dyn_open_char(int major, int minor, struct fd *fd) {
    return dyn_open(DEV_CHAR, major, minor, fd);
}

static intptr_t rtc_open(int major, int minor, struct fd *fd) {
    //return &(intptr_t)(major, minor, fd);
    return (intptr_t)fd;
}

struct rtc_time {
    int tm_sec;   /* seconds */
    int tm_min;   /* minutes */
    int tm_hour;  /* hours */
    int tm_mday;  /* day of the month */
    int tm_mon;   /* month */
    int tm_year;  /* year */
};

intptr_t rtc_dev(void *buf, size_t count) {
    if (count < sizeof(struct rtc_time)) {
        errno = EFAULT;
        return -1;
    }

    time_t now;
    struct tm *tm_now;
    struct rtc_time emulatedRTC;

    time(&now);
    tm_now = localtime(&now);

    emulatedRTC.tm_sec = tm_now->tm_sec;
    emulatedRTC.tm_min = tm_now->tm_min;
    emulatedRTC.tm_hour = tm_now->tm_hour;
    emulatedRTC.tm_mday = tm_now->tm_mday;
    emulatedRTC.tm_mon = tm_now->tm_mon;
    emulatedRTC.tm_year = tm_now->tm_year + 1900;

    memcpy(buf, &emulatedRTC, sizeof(emulatedRTC));
    return sizeof(emulatedRTC);
}

struct dev_ops dyn_dev_char = {
    .open = dyn_open_char,
};

struct dev_ops rtc_dev_char = {
    .open = rtc_dev,
};
