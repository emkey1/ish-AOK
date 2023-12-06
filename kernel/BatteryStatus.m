//
//  BatteryStatus.m
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#import <Foundation/Foundation.h>
// BatteryStatus.m
#import "BatteryStatus.h"

char* printBatteryStatus(int type) {
    UIDevice *device = [UIDevice currentDevice];
    device.batteryMonitoringEnabled = YES;

    float batteryLevel = device.batteryLevel;
    UIDeviceBatteryState batteryState = device.batteryState;
    BOOL lowPowerModeEnabled = [[NSProcessInfo processInfo] isLowPowerModeEnabled];

    NSString *stateString = @"";
    // Charging, Discharging, Full, Unknown, or Not charging.
    switch (batteryState) {
        case UIDeviceBatteryStateUnknown:
            stateString = @"Unknown";
            break;
        case UIDeviceBatteryStateUnplugged:
            stateString = @"Discharging";
            break;
        case UIDeviceBatteryStateCharging:
            stateString = @"Charging";
            break;
        case UIDeviceBatteryStateFull:
            stateString = @"Full";
            break;
        default:
            stateString = @"Not Available"; // Handle any unexpected cases
            break;
    }

    if(type == 3) {
        NSString *formattedOutput = [NSString stringWithFormat:
                                     @"battery_level: %.2f\n"
                                     "battery_state: %@\n"
                                     "low_power_mode: %@\n",
                                     batteryLevel * 100, stateString, lowPowerModeEnabled ? @"Enabled" : @"Disabled"];
        return (char *)[formattedOutput UTF8String];
    } else if(type == 2) { // Capacity
        NSString *formattedOutput = [NSString stringWithFormat:
                                     @"%.2f\n", batteryLevel * 100];
        return (char *)[formattedOutput UTF8String];
    } else if (type == 1) { // Status
        NSString *formattedOutput = [NSString stringWithFormat:
                                     @"%@\n", stateString];
        return (char *)[formattedOutput UTF8String];
    }
    
    return NULL;

}
