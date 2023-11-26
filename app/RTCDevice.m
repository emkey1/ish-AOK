//
//  RTCDevice.m
//  iSH-AOK
//
//  Created by Michael Miller on 11/24/23.
//

#import <Foundation/Foundation.h>
#include "fs/poll.h"
#include "fs/dyndev.h"
#include "kernel/errno.h"
#include "debug.h"
#include "fs/devices.h"

// Real Time Clock file descriptor structure
typedef struct fd rtc_fd;

// Need to define this, as we are running on iOS, which doesn't have/give access to the RTC
typedef struct rtc_time {
    int tm_sec;   // seconds
    int tm_min;   // minutes
    int tm_hour;  // hours
    int tm_mday;  // day of the month
    int tm_mon;   // month
    int tm_year;  // year
} rtc_time;

// Get the time, put it in the appropriate structure
static rtc_time *get_current_time(rtc_fd *fd, size_t *len) {
    // Obtain the current date
    NSDate *currentDate = [NSDate date];
    NSCalendar *calendar = [NSCalendar currentCalendar];

    // Define the desired date components
    NSDateComponents *components = [calendar components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay | NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond)
                                               fromDate:currentDate];

    // Allocate and populate the rtc_time structure
    rtc_time *timeStruct = malloc(sizeof(rtc_time));
    if (!timeStruct) {
        // Handle memory allocation failure
        *len = 0;
        return NULL;
    }

    // Populate the structure
    // Note: tm_mon is 0-based (0 for January) and tm_year is years since 1900
    timeStruct->tm_sec = (int)[components second];
    timeStruct->tm_min = (int)[components minute];
    timeStruct->tm_hour = (int)[components hour];
    timeStruct->tm_mday = (int)[components day];
    timeStruct->tm_mon = (int)[components month] - 1; // Adjust for tm_mon
    timeStruct->tm_year = (int)[components year] - 1900; // Adjust for tm_year

    // Update the size
    *len = sizeof(rtc_time);

    return timeStruct;
}

static ssize_t rtc_read(rtc_fd *fd, void *buf, size_t bufsize) {
    return 0;
}

static int rtc_poll(rtc_fd *fd) {
    return 0;
}

static int rtc_open(int major, int minor, rtc_fd *fd) {
    return 0;
}

static int rtc_close(rtc_fd *fd) {
    return 0;
}

#define RTC_RD_TIME 0x80247009 // Example definition, adjust as necessary

static ssize_t rtc_ioctl_size(int cmd) {
    switch (cmd) {
        case RTC_RD_TIME:
            return sizeof(rtc_time);
    }
    return -1;
}

// Function to handle ioctl operations
static int rtc_ioctl(struct fd *fd, int cmd, void *arg) {
    @autoreleasepool {
        switch (cmd) {
            case RTC_RD_TIME: { // On a real Linux, there are a number of other possible ioctl()'s.  We don't really need them
                size_t length = 0;
                rtc_time *data = get_current_time(fd, &length);

                if (arg == NULL) {
                    return _EFAULT; // Null pointer argument
                }

                *(rtc_time *) arg = *data; // This is the magic that gets the value back to the "kernel"

                return 0; // Success
            }
            default:
                return _EFAULT; // Oops
        }
    }
}

struct dev_ops rtc_dev = {
    .open = rtc_open,
    .fd.read = rtc_read,
    .fd.poll = rtc_poll,
    .fd.close = rtc_close,
    .fd.ioctl = rtc_ioctl,
    .fd.ioctl_size = rtc_ioctl_size,  // Do NOT FORGET THIS if you want to implement ioctl(s) for your device.  They will not work without it.
};
