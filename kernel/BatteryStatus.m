//
//  BatteryStatus.m
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <string.h>
#import "BatteryStatus.h"

// The one copy of the host's state that the kernel reads.
//
// It replaces printBatteryStatus(), which asked UIDevice afresh on every read
// and returned -[NSString UTF8String] of a temporary string. Two things were
// wrong with that. The pointer belonged to an autoreleased object that nothing
// kept alive once the function had returned, and fs/proc formatted from it
// afterwards. And UIDevice is main-thread-only, while the read came from
// whichever guest thread opened the file.
//
// So the main queue is now the only writer. It takes a reading at start and
// again whenever iOS says something changed. A reader holds the lock only long
// enough to copy the values out, and formats from its own copy.
static os_unfair_lock host_status_lock = OS_UNFAIR_LOCK_INIT;

// How old a reading may be before the next reader asks for a new one, and the
// flag that keeps a burst of readers from queueing a refresh each.
#define HOST_STATUS_MAX_AGE_SECONDS 2.0
static double host_status_taken_at;
static atomic_bool host_status_refresh_queued;

// Big enough for any IANA name: the longest, America/Argentina/ComodRivadavia,
// is 32 bytes.
#define HOST_TIMEZONE_NAME_MAX 128

static struct {
    struct host_battery_status battery;
    enum host_thermal_state thermal;
    char zone_name[HOST_TIMEZONE_NAME_MAX];
} host_status = {
    // Until the first reading: nothing known. The guest boots after that
    // reading (ISHHostStatusStart is called first), so this is only ever seen
    // by something that ran before the app finished launching.
    .battery = {.state = HOST_BATTERY_UNKNOWN, .level = -1, .low_power_mode = -1},
    .thermal = HOST_THERMAL_UNKNOWN,
    .zone_name = "",
};

static enum host_battery_state host_battery_state_from(UIDeviceBatteryState state) {
    switch (state) {
        case UIDeviceBatteryStateUnplugged:
            return HOST_BATTERY_UNPLUGGED;
        case UIDeviceBatteryStateCharging:
            return HOST_BATTERY_CHARGING;
        case UIDeviceBatteryStateFull:
            return HOST_BATTERY_FULL;
        case UIDeviceBatteryStateUnknown:
        default:
            return HOST_BATTERY_UNKNOWN;
    }
}

static enum host_thermal_state host_thermal_state_from(NSProcessInfoThermalState state) {
    switch (state) {
        case NSProcessInfoThermalStateNominal:
            return HOST_THERMAL_NOMINAL;
        case NSProcessInfoThermalStateFair:
            return HOST_THERMAL_FAIR;
        case NSProcessInfoThermalStateSerious:
            return HOST_THERMAL_SERIOUS;
        case NSProcessInfoThermalStateCritical:
            return HOST_THERMAL_CRITICAL;
        default:
            // A state newer than this code: say so rather than guess which of
            // the four it is closest to.
            return HOST_THERMAL_UNKNOWN;
    }
}

// Main thread only.
static void host_status_refresh(void) {
    UIDevice *device = UIDevice.currentDevice;
    // Off, UIDevice answers "unknown" and -1 whatever the battery is doing.
    device.batteryMonitoringEnabled = YES;
    NSProcessInfo *process = NSProcessInfo.processInfo;

    struct host_battery_status battery = {
        .state = host_battery_state_from(device.batteryState),
        .level = device.batteryLevel,
        .low_power_mode = process.isLowPowerModeEnabled ? 1 : 0,
    };
    // "Full" means plugged in and charged, so a level at the bottom of the
    // scale with it is not a reading of anything. An iOS app on a Mac answered
    // Full and 0.01 while the host sat at 80%, which reached the guest as a 1%
    // battery: waybar drew it red and flashing. Nothing here can turn that into
    // the real figure, so it counts as no reading, which the guest already
    // understands -- an empty /sys/class/power_supply, as on a Linux machine
    // with no battery.
    if (battery.state == HOST_BATTERY_FULL && battery.level >= 0 && battery.level <= 0.05f) {
        battery.state = HOST_BATTERY_UNKNOWN;
        battery.level = -1;
    }
    enum host_thermal_state thermal = host_thermal_state_from(process.thermalState);

    // Copied out of the NSString here, on the thread that owns it, so the
    // kernel never holds a pointer into an object.
    char zone_name[HOST_TIMEZONE_NAME_MAX] = "";
    NSString *name = NSTimeZone.localTimeZone.name;
    if (name == nil || ![name getCString:zone_name maxLength:sizeof(zone_name) encoding:NSUTF8StringEncoding])
        zone_name[0] = '\0';

    os_unfair_lock_lock(&host_status_lock);
    host_status.battery = battery;
    host_status.thermal = thermal;
    memcpy(host_status.zone_name, zone_name, sizeof(zone_name));
    host_status_taken_at = CFAbsoluteTimeGetCurrent();
    os_unfair_lock_unlock(&host_status_lock);
    atomic_store(&host_status_refresh_queued, false);
}

// Asks for a new reading when the one held is older than the age above. The
// caller is a guest thread, so this only queues the work: it returns at once
// and the reading it just took stands.
//
// The notifications below are not enough on their own. UIDevice answers the
// first read after batteryMonitoringEnabled is set before it has a real value,
// and a host that posts no battery notifications -- an iOS app on a Mac posts
// none -- would keep that first answer for the life of the process. A reader
// asking again is what replaces it.
static void host_status_refresh_if_stale(void) {
    os_unfair_lock_lock(&host_status_lock);
    double age = CFAbsoluteTimeGetCurrent() - host_status_taken_at;
    os_unfair_lock_unlock(&host_status_lock);
    if (age < HOST_STATUS_MAX_AGE_SECONDS)
        return;
    if (atomic_exchange(&host_status_refresh_queued, true))
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        host_status_refresh();
    });
}

void ISHHostStatusStart(void) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            ISHHostStatusStart();
        });
        return;
    }
    static BOOL started;
    if (started)
        return;
    started = YES;

    host_status_refresh();
    // Again once the battery has had a moment to answer: the reading above is
    // taken immediately after batteryMonitoringEnabled was set, and UIDevice
    // has nothing real to give yet (-1 on a device). A Mac answered "Full, 1%"
    // there while the host sat at 80%, and nothing corrected it.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (1.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        host_status_refresh();
    });

    // Every one of these re-reads everything: a reading is a handful of
    // property reads, and one path is easier to get right than six. Some are
    // posted on whichever thread noticed the change, hence the main queue.
    // Becoming active is the catch-all for whatever changed while the app was
    // suspended.
    NSArray<NSNotificationName> *changes = @[
        UIDeviceBatteryLevelDidChangeNotification,
        UIDeviceBatteryStateDidChangeNotification,
        NSProcessInfoPowerStateDidChangeNotification,
        NSProcessInfoThermalStateDidChangeNotification,
        NSSystemTimeZoneDidChangeNotification,
        UIApplicationDidBecomeActiveNotification,
    ];
    for (NSNotificationName change in changes) {
        [NSNotificationCenter.defaultCenter addObserverForName:change
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) {
            // The system zone is cached per process until told otherwise.
            if ([note.name isEqualToString:NSSystemTimeZoneDidChangeNotification])
                [NSTimeZone resetSystemTimeZone];
            host_status_refresh();
        }];
    }
}

void hostBatteryStatus(struct host_battery_status *out) {
    if (out == NULL)
        return;
    host_status_refresh_if_stale();
    os_unfair_lock_lock(&host_status_lock);
    *out = host_status.battery;
    os_unfair_lock_unlock(&host_status_lock);
}

enum host_thermal_state hostThermalState(void) {
    host_status_refresh_if_stale();
    os_unfair_lock_lock(&host_status_lock);
    enum host_thermal_state thermal = host_status.thermal;
    os_unfair_lock_unlock(&host_status_lock);
    return thermal;
}

bool hostPreferredLanguages(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return false;
    buf[0] = '\0';
    // Read fresh each time: the user can change the language list in Settings
    // while the app runs, and this is only asked for when a guest reads it.
    CFArrayRef languages = CFLocaleCopyPreferredLanguages();
    if (languages == NULL)
        return false;
    size_t used = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(languages); i++) {
        char tag[64];
        CFStringRef language = CFArrayGetValueAtIndex(languages, i);
        if (!CFStringGetCString(language, tag, sizeof(tag), kCFStringEncodingUTF8))
            continue;
        size_t len = strlen(tag);
        if (len == 0 || used + len + 2 > size)
            break;
        memcpy(buf + used, tag, len);
        used += len;
        buf[used++] = '\n';
        buf[used] = '\0';
    }
    CFRelease(languages);
    return used > 0;
}

bool hostTimeZoneName(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return false;
    os_unfair_lock_lock(&host_status_lock);
    size_t len = strlen(host_status.zone_name);
    bool fits = len > 0 && len < size;
    if (fits)
        memcpy(buf, host_status.zone_name, len + 1);
    os_unfair_lock_unlock(&host_status_lock);
    if (!fits)
        buf[0] = '\0';
    return fits;
}
