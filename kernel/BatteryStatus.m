//
//  BatteryStatus.m
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#import <Foundation/Foundation.h>
// BatteryStatus.m
#import "BatteryStatus.h"

char* printBatteryStatus(void) {
    UIDevice *device = [UIDevice currentDevice];
    device.batteryMonitoringEnabled = YES;

    float batteryLevel = device.batteryLevel;
    UIDeviceBatteryState batteryState = device.batteryState;
    BOOL lowPowerModeEnabled = [[NSProcessInfo processInfo] isLowPowerModeEnabled];

    NSString *stateString = @"";
    switch (batteryState) {
        case UIDeviceBatteryStateUnknown:
            stateString = @"Unknown";
            break;
        case UIDeviceBatteryStateUnplugged:
            stateString = @"Unplugged";
            break;
        case UIDeviceBatteryStateCharging:
            stateString = @"Charging";
            break;
        case UIDeviceBatteryStateFull:
            stateString = @"Full";
            break;
    }

    NSString *formattedOutput = [NSString stringWithFormat:
                                 @"battery_level: %.2f\n"
                                 "battery_state: %@\n"
                                 "low_power_mode: %@\n"
                                 "battery_health: %.2f\n",
                                 batteryLevel * 100, stateString, lowPowerModeEnabled ? @"Enabled" : @"Disabled", batteryLevel * 100];

    return (char *)[formattedOutput UTF8String];
}
