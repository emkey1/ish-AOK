//
//  BatteryStatus.h
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#ifndef BatteryStatus_h
#define BatteryStatus_h

// What the host says about its battery, its temperature and its clock, for the
// guest files built from them: /proc/ish/BAT0*, /sys/class/power_supply,
// /proc/ish/thermal_state and /proc/ish/timezone.
//
// Plain C, because the kernel includes it and so does the command-line build,
// which has no UIKit.
//
// Every function below except ISHHostStatusStart has TWO definitions, and both
// must exist: kernel/BatteryStatus.m for the app, platform/standalone.c for the
// CLI. standalone.c is compiled into libish.a too, and the app links that as an
// archive, which loads a member only to resolve a symbol nothing else defines.
// A function added to standalone.c alone therefore pulls the whole of
// standalone.o into the app, and every other function in it then collides with
// the app's own copy as a duplicate symbol.

#include <stdbool.h>
#include <stddef.h>

enum host_battery_state {
    HOST_BATTERY_UNKNOWN = 0,   // no battery, or a host that will not say
    HOST_BATTERY_UNPLUGGED,     // running from the battery
    HOST_BATTERY_CHARGING,
    HOST_BATTERY_FULL,          // plugged in, and full
};

struct host_battery_status {
    enum host_battery_state state;
    // 0.0 to 1.0, or negative when unknown: UIDevice's own convention.
    float level;
    // 1 or 0, or -1 when the host has no way to say.
    int low_power_mode;
};

// A copy of the cached reading. Never calls into UIKit and never waits on the
// main thread, so any guest thread may ask.
void hostBatteryStatus(struct host_battery_status *out);

// ProcessInfo.ThermalState. iOS offers only this coarse state, not a
// temperature.
enum host_thermal_state {
    HOST_THERMAL_UNKNOWN = -1,  // a host with no source
    HOST_THERMAL_NOMINAL = 0,
    HOST_THERMAL_FAIR,
    HOST_THERMAL_SERIOUS,
    HOST_THERMAL_CRITICAL,
};

enum host_thermal_state hostThermalState(void);

// The host's IANA time zone name, e.g. "Europe/London", copied into buf.
// Returns false, with buf empty, when there is no name to give.
bool hostTimeZoneName(char *buf, size_t size);

// The device's preferred languages as BCP 47 tags ("zh-Hans-CN", "en-GB"),
// most preferred first, one per line, copied into buf. Returns false, with buf
// empty, when there are none to give.
bool hostPreferredLanguages(char *buf, size_t size);

// App only: start keeping the cache the functions above read. Call it on the
// main thread before the guest boots; a second call does nothing.
void ISHHostStatusStart(void);

#endif /* BatteryStatus_h */
