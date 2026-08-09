//
//  hostinfo.m
//  iSH-AOK
//
//  Created by Michael Miller on 12/25/23.
//

#import <Foundation/Foundation.h>
#import <mach-o/arch.h>
#import <stdio.h>
#import <sys/sysctl.h>
#import <sys/utsname.h>
#import "hostinfo.h"

static NSString *hostMachineIdentifier(void);
static NSString *hostProcessMachineIdentifier(void);
static BOOL hostIdentifierNeedsSimulatorOverride(NSString *identifier);

NSString* translateDeviceIdentifier(NSString *identifier) {
    NSDictionary<NSString *, NSString *> *deviceNames = @{
        // iPhones with 64-bit Processors
        @"arm64": @"iDevice Simulator (ARM)",
        @"x86_64": @"iDevice Simulator (x86_64)",
        @"iPhone6,1": @"iPhone 5S (GSM)",
        @"iPhone6,2": @"iPhone 5S (Global)",
        @"iPhone7,1": @"iPhone 6 Plus",
        @"iPhone7,2": @"iPhone 6",
        @"iPhone8,1": @"iPhone 6s",
        @"iPhone8,2": @"iPhone 6s Plus",
        @"iPhone8,4": @"iPhone SE (GSM)",
        @"iPhone9,1": @"iPhone 7",
        @"iPhone9,2": @"iPhone 7 Plus",
        @"iPhone9,3": @"iPhone 7",
        @"iPhone9,4": @"iPhone 7 Plus",
        @"iPhone10,1" : @"iPhone 8",
        @"iPhone10,2": @"iPhone 8 Plus",
        @"iPhone10,3": @"iPhone X Global",
        @"iPhone10,4": @"iPhone 8",
        @"iPhone10,5": @"iPhone 8 Plus",
        @"iPhone10,6": @"iPhone X GSM",
        @"iPhone11,2": @"iPhone XS",
        @"iPhone11,4": @"iPhone XS Max",
        @"iPhone11,6": @"iPhone XS Max Global",
        @"iPhone11,8": @"iPhone XR",
        @"iPhone12,1": @"iPhone 11",
        @"iPhone12,3": @"iPhone 11 Pro",
        @"iPhone12,5": @"iPhone 11 Pro Max",
        @"iPhone12,8": @"iPhone SE 2nd Gen",
        @"iPhone13,1": @"iPhone 12 Mini",
        @"iPhone13,2": @"iPhone 12",
        @"iPhone13,3": @"iPhone 12 Pro",
        @"iPhone13,4": @"iPhone 12 Pro Max",
        @"iPhone14,2": @"iPhone 13 Pro",
        @"iPhone14,3": @"iPhone 13 Pro Max",
        @"iPhone14,4": @"iPhone 13 Mini",
        @"iPhone14,5": @"iPhone 13",
        @"iPhone14,6": @"iPhone SE 3rd Gen",
        @"iPhone14,7": @"iPhone 14",
        @"iPhone14,8": @"iPhone 14 Plus",
        @"iPhone15,2": @"iPhone 14 Pro",
        @"iPhone15,3": @"iPhone 14 Pro Max",
        @"iPhone15,4": @"iPhone 15",
        @"iPhone15,5": @"iPhone 15 Plus",
        @"iPhone16,1": @"iPhone 15 Pro",
        @"iPhone16,2": @"iPhone 15 Pro Max",
        @"iPhone17,1": @"iPhone 16 Pro",
        @"iPhone17,2": @"iPhone 16 Pro Max",
        @"iPhone17,3": @"iPhone 16",
        @"iPhone17,4": @"iPhone 16 Plus",
        @"iPhone17,5": @"iPhone 16e",
        @"iPhone18,1": @"iPhone 17 Pro",
        @"iPhone18,2": @"iPhone 17 Pro Max",
        @"iPhone18,3": @"iPhone 17",
        @"iPhone18,4": @"iPhone Air",
        @"iPhone18,5": @"iPhone 17e",
        // iPads with 64-bit Processors
        @"iPad4,1": @"iPad Air (WiFi)",
        @"iPad4,2": @"iPad Air (GSM+CDMA)",
        @"iPad4,3": @"1st Gen iPad Air (China)",
        @"iPad4,4": @"iPad mini Retina (WiFi)",
        @"iPad4,5": @"iPad mini Retina (GSM+CDMA)",
        @"iPad4,6": @"iPad mini Retina (China)",
        @"iPad4,7": @"iPad mini 3 (WiFi)",
        @"iPad4,8": @"iPad mini 3 (GSM+CDMA)",
        @"iPad4,9": @"iPad Mini 3 (China)",
        @"iPad5,1": @"iPad mini 4 (WiFi)",
        @"iPad5,2": @"4th Gen iPad mini (WiFi+Cellular)",
        @"iPad5,3": @"iPad Air 2 (WiFi)",
        @"iPad5,4": @"iPad Air 2 (Cellular)",
        @"iPad6,3": @"iPad Pro (9.7 inch, WiFi)",
        @"iPad6,4": @"iPad Pro (9.7 inch, WiFi+LTE)",
        @"iPad6,7": @"iPad Pro (12.9 inch, WiFi)",
        @"iPad6,8": @"iPad Pro (12.9 inch, WiFi+LTE)",
        @"iPad6,11": @"iPad 5th Gen (WiFi)",
        @"iPad6,12": @"iPad 5th Gen (WiFi+Cellular)",
        @"iPad7,1": @"iPad Pro 12.9-inch 2nd Gen (WiFi)",
        @"iPad7,2": @"iPad Pro 12.9-inch 2nd Gen (WiFi+Cellular)",
        @"iPad7,3": @"iPad Pro 10.5-inch 2nd Gen",
        @"iPad7,4": @"iPad Pro 10.5-inch 2nd Gen",
        @"iPad7,5": @"iPad 6th Gen (WiFi)",
        @"iPad7,6": @"iPad 6th Gen (WiFi+Cellular)",
        @"iPad7,11": @"iPad 7th Gen 10.2-inch (WiFi)",
        @"iPad7,12": @"iPad 7th Gen 10.2-inch (WiFi+Cellular)",
        @"iPad8,1": @"iPad Pro 11 inch 3rd Gen (WiFi)",
        @"iPad8,2": @"iPad Pro 11 inch 3rd Gen (1TB, WiFi)",
        @"iPad8,3": @"iPad Pro 11 inch 3rd Gen (WiFi+Cellular)",
        @"iPad8,4": @"iPad Pro 11 inch 3rd Gen (1TB, WiFi+Cellular)",
        @"iPad8,5": @"iPad Pro 12.9 inch 3rd Gen (WiFi)",
        @"iPad8,6": @"iPad Pro 12.9 inch 3rd Gen (1TB, WiFi)",
        @"iPad8,7": @"iPad Pro 12.9 inch 3rd Gen (WiFi+Cellular)",
        @"iPad8,8": @"iPad Pro 12.9 inch 3rd Gen (1TB, WiFi+Cellular)",
        @"iPad8,9": @"iPad Pro 11 inch 4th Gen (WiFi)",
        @"iPad8,10": @"iPad Pro 11 inch 4th Gen (WiFi+Cellular)",
        @"iPad8,11": @"iPad Pro 12.9 inch 4th Gen (WiFi)",
        @"iPad8,12": @"iPad Pro 12.9 inch 4th Gen (WiFi+Cellular)",
        @"iPad11,1": @"iPad mini 5th Gen (WiFi)",
        @"iPad11,2": @"iPad mini 5th Gen",
        @"iPad11,3": @"iPad Air 3rd Gen (WiFi)",
        @"iPad11,4": @"iPad Air 3rd Gen",
        @"iPad11,6": @"iPad 8th Gen (WiFi)",
        @"iPad11,7": @"iPad 8th Gen (WiFi+Cellular)",
        @"iPad12,1": @"iPad 9th Gen (WiFi)",
        @"iPad12,2": @"iPad 9th Gen (WiFi+Cellular)",
        @"iPad14,1": @"iPad mini 6th Gen (WiFi)",
        @"iPad14,2": @"iPad mini 6th Gen (WiFi+Cellular)",
        @"iPad13,1": @"iPad Air 4th Gen (WiFi)",
        @"iPad13,2": @"iPad Air 4th Gen (WiFi+Cellular)",
        @"iPad13,4": @"iPad Pro 11 inch 3rd Gen (WiFi)",
        @"iPad13,5": @"iPad Pro 11 inch 3rd Gen (WiFi+Cellular US)",
        @"iPad13,6": @"iPad Pro 11 inch 3rd Gen (WiFi+Cellular Global)",
        @"iPad13,7": @"iPad Pro 11 inch 3rd Gen (WiFi+Cellular China)",
        @"iPad13,8": @"iPad Pro 12.9 inch 5th Gen",
        @"iPad13,9": @"iPad Pro 12.9 inch 5th Gen",
        @"iPad13,10": @"iPad Pro 12.9 inch 5th Gen",
        @"iPad13,11": @"iPad Pro 12.9 inch 5th Gen",
        @"iPad13,16": @"iPad Air 5th Gen (WiFi)",
        @"iPad13,17": @"iPad Air 5th Gen (WiFi+Cellular)",
        @"iPad13,18": @"iPad 10th Gen",
        @"iPad13,19": @"iPad 10th Gen",
        @"iPad14,3": @"iPad Pro 11 inch 4th Gen",
        @"iPad14,4": @"iPad Pro 11 inch 4th Gen",
        @"iPad14,5": @"iPad Pro 12.9 inch 6th Gen",
        @"iPad14,6": @"iPad Pro 12.9 inch 6th Gen",
        @"iPad14,8": @"iPad Air (11-inch, M2)",
        @"iPad14,9": @"iPad Air (11-inch, M2)",
        @"iPad14,10": @"iPad Air (13-inch, M2)",
        @"iPad14,11": @"iPad Air (13-inch, M2)",
        @"iPad15,3": @"iPad Air (11-inch, M3)",
        @"iPad15,4": @"iPad Air (11-inch, M3)",
        @"iPad15,5": @"iPad Air (13-inch, M3)",
        @"iPad15,6": @"iPad Air (13-inch, M3)",
        @"iPad15,7": @"iPad (A16)",
        @"iPad15,8": @"iPad (A16)",
        @"iPad16,1": @"iPad mini (A17 Pro)",
        @"iPad16,2": @"iPad mini (A17 Pro)",
        @"iPad16,3": @"iPad Pro 11-inch (M4)",
        @"iPad16,4": @"iPad Pro 11-inch (M4)",
        @"iPad16,5": @"iPad Pro 13-inch (M4)",
        @"iPad16,6": @"iPad Pro 13-inch (M4)",
        @"iPad16,8": @"iPad Air 11-inch (M4)",
        @"iPad16,9": @"iPad Air 11-inch (M4)",
        @"iPad16,10": @"iPad Air 13-inch (M4)",
        @"iPad16,11": @"iPad Air 13-inch (M4)",
        @"iPad17,1": @"iPad Pro 11-inch (M5)",
        @"iPad17,2": @"iPad Pro 11-inch (M5)",
        @"iPad17,3": @"iPad Pro 13-inch (M5)",
        @"iPad17,4": @"iPad Pro 13-inch (M5)",
        // iPod touch with 64-bit Processors
        @"iPod7,1": @"iPod touch 6th Gen",
        @"iPod9,1": @"iPod touch 7th Gen",
    };

    NSString *deviceName = deviceNames[identifier];
    if (deviceName) {
        return deviceName;
    } else {
        return identifier; // Return the original identifier if no mapping is found
    }
}

static NSString *hostChipIdentifier(NSString *identifier) {
    NSDictionary<NSString *, NSString *> *chipNames = @{
        @"iPhone6,1": @"A7",
        @"iPhone6,2": @"A7",
        @"iPhone7,1": @"A8",
        @"iPhone7,2": @"A8",
        @"iPhone8,1": @"A9",
        @"iPhone8,2": @"A9",
        @"iPhone8,4": @"A9",
        @"iPhone9,1": @"A10",
        @"iPhone9,2": @"A10",
        @"iPhone9,3": @"A10",
        @"iPhone9,4": @"A10",
        @"iPhone10,1": @"A11",
        @"iPhone10,2": @"A11",
        @"iPhone10,3": @"A11",
        @"iPhone10,4": @"A11",
        @"iPhone10,5": @"A11",
        @"iPhone10,6": @"A11",
        @"iPhone11,2": @"A12",
        @"iPhone11,4": @"A12",
        @"iPhone11,6": @"A12",
        @"iPhone11,8": @"A12",
        @"iPhone12,1": @"A13",
        @"iPhone12,3": @"A13",
        @"iPhone12,5": @"A13",
        @"iPhone12,8": @"A13",
        @"iPhone13,1": @"A14",
        @"iPhone13,2": @"A14",
        @"iPhone13,3": @"A14",
        @"iPhone13,4": @"A14",
        @"iPhone14,2": @"A15",
        @"iPhone14,3": @"A15",
        @"iPhone14,4": @"A15",
        @"iPhone14,5": @"A15",
        @"iPhone14,6": @"A15",
        @"iPhone14,7": @"A15",
        @"iPhone14,8": @"A15",
        @"iPhone15,2": @"A16",
        @"iPhone15,3": @"A16",
        @"iPhone15,4": @"A16",
        @"iPhone15,5": @"A16",
        @"iPhone16,1": @"A17 Pro",
        @"iPhone16,2": @"A17 Pro",
        @"iPhone17,1": @"A18 Pro",
        @"iPhone17,2": @"A18 Pro",
        @"iPhone17,3": @"A18",
        @"iPhone17,4": @"A18",
        @"iPhone17,5": @"A18",
        @"iPhone18,1": @"A19 Pro",
        @"iPhone18,2": @"A19 Pro",
        @"iPhone18,3": @"A19",
        @"iPhone18,4": @"A19 Pro",
        @"iPhone18,5": @"A19",
        @"iPad4,1": @"A7",
        @"iPad4,2": @"A7",
        @"iPad4,3": @"A7",
        @"iPad4,4": @"A8",
        @"iPad4,5": @"A8",
        @"iPad4,6": @"A8",
        @"iPad4,7": @"A8",
        @"iPad4,8": @"A8",
        @"iPad4,9": @"A8",
        @"iPad5,1": @"A8",
        @"iPad5,2": @"A8",
        @"iPad5,3": @"A8X",
        @"iPad5,4": @"A8X",
        @"iPad6,3": @"A9X",
        @"iPad6,4": @"A9X",
        @"iPad6,7": @"A9X",
        @"iPad6,8": @"A9X",
        @"iPad6,11": @"A9",
        @"iPad6,12": @"A9",
        @"iPad7,1": @"A10X",
        @"iPad7,2": @"A10X",
        @"iPad7,3": @"A10X",
        @"iPad7,4": @"A10X",
        @"iPad7,5": @"A10",
        @"iPad7,6": @"A10",
        @"iPad7,11": @"A10",
        @"iPad7,12": @"A10",
        @"iPad8,1": @"A12X",
        @"iPad8,2": @"A12X",
        @"iPad8,3": @"A12X",
        @"iPad8,4": @"A12X",
        @"iPad8,5": @"A12X",
        @"iPad8,6": @"A12X",
        @"iPad8,7": @"A12X",
        @"iPad8,8": @"A12X",
        @"iPad8,9": @"A12Z",
        @"iPad8,10": @"A12Z",
        @"iPad8,11": @"A12Z",
        @"iPad8,12": @"A12Z",
        @"iPad11,1": @"A12",
        @"iPad11,2": @"A12",
        @"iPad11,3": @"A12",
        @"iPad11,4": @"A12",
        @"iPad11,6": @"A12",
        @"iPad11,7": @"A12",
        @"iPad12,1": @"A13",
        @"iPad12,2": @"A13",
        @"iPad13,1": @"A14",
        @"iPad13,2": @"A14",
        @"iPad13,4": @"M1",
        @"iPad13,5": @"M1",
        @"iPad13,6": @"M1",
        @"iPad13,7": @"M1",
        @"iPad13,8": @"M1",
        @"iPad13,9": @"M1",
        @"iPad13,10": @"M1",
        @"iPad13,11": @"M1",
        @"iPad13,16": @"M1",
        @"iPad13,17": @"M1",
        @"iPad13,18": @"A14",
        @"iPad13,19": @"A14",
        @"iPad14,1": @"A15",
        @"iPad14,2": @"A15",
        @"iPad14,3": @"M2",
        @"iPad14,4": @"M2",
        @"iPad14,5": @"M2",
        @"iPad14,6": @"M2",
        @"iPad14,8": @"M2",
        @"iPad14,9": @"M2",
        @"iPad14,10": @"M2",
        @"iPad14,11": @"M2",
        @"iPad15,3": @"M3",
        @"iPad15,4": @"M3",
        @"iPad15,5": @"M3",
        @"iPad15,6": @"M3",
        @"iPad15,7": @"A16",
        @"iPad15,8": @"A16",
        @"iPad16,1": @"A17 Pro",
        @"iPad16,2": @"A17 Pro",
        @"iPad16,3": @"M4",
        @"iPad16,4": @"M4",
        @"iPad16,5": @"M4",
        @"iPad16,6": @"M4",
        @"iPod7,1": @"A8",
        @"iPod9,1": @"A10",
    };

    return chipNames[identifier];
}

static NSDictionary<NSString *, NSNumber *> *hostFallbackCoreTopology(NSString *identifier) {
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *topologies = @{
        @"iPhone6,1": @{@"performance": @2, @"efficiency": @0},
        @"iPhone6,2": @{@"performance": @2, @"efficiency": @0},
        @"iPhone7,1": @{@"performance": @2, @"efficiency": @0},
        @"iPhone7,2": @{@"performance": @2, @"efficiency": @0},
        @"iPhone8,1": @{@"performance": @2, @"efficiency": @0},
        @"iPhone8,2": @{@"performance": @2, @"efficiency": @0},
        @"iPhone8,4": @{@"performance": @2, @"efficiency": @0},
        @"iPhone9,1": @{@"performance": @2, @"efficiency": @2},
        @"iPhone9,2": @{@"performance": @2, @"efficiency": @2},
        @"iPhone9,3": @{@"performance": @2, @"efficiency": @2},
        @"iPhone9,4": @{@"performance": @2, @"efficiency": @2},
        @"iPhone10,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone10,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone10,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone10,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone10,5": @{@"performance": @2, @"efficiency": @4},
        @"iPhone10,6": @{@"performance": @2, @"efficiency": @4},
        @"iPhone11,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone11,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone11,6": @{@"performance": @2, @"efficiency": @4},
        @"iPhone11,8": @{@"performance": @2, @"efficiency": @4},
        @"iPhone12,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone12,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone12,5": @{@"performance": @2, @"efficiency": @4},
        @"iPhone12,8": @{@"performance": @2, @"efficiency": @4},
        @"iPhone13,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone13,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone13,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone13,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,5": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,6": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,7": @{@"performance": @2, @"efficiency": @4},
        @"iPhone14,8": @{@"performance": @2, @"efficiency": @4},
        @"iPhone15,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone15,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone15,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone15,5": @{@"performance": @2, @"efficiency": @4},
        @"iPhone16,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone16,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone17,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone17,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone17,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone17,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone17,5": @{@"performance": @2, @"efficiency": @4},
        @"iPhone18,1": @{@"performance": @2, @"efficiency": @4},
        @"iPhone18,2": @{@"performance": @2, @"efficiency": @4},
        @"iPhone18,3": @{@"performance": @2, @"efficiency": @4},
        @"iPhone18,4": @{@"performance": @2, @"efficiency": @4},
        @"iPhone18,5": @{@"performance": @2, @"efficiency": @4},
        @"iPad4,1": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,2": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,3": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,4": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,5": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,6": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,7": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,8": @{@"performance": @2, @"efficiency": @0},
        @"iPad4,9": @{@"performance": @2, @"efficiency": @0},
        @"iPad5,1": @{@"performance": @2, @"efficiency": @0},
        @"iPad5,2": @{@"performance": @2, @"efficiency": @0},
        @"iPad5,3": @{@"performance": @3, @"efficiency": @0},
        @"iPad5,4": @{@"performance": @3, @"efficiency": @0},
        @"iPad6,3": @{@"performance": @2, @"efficiency": @0},
        @"iPad6,4": @{@"performance": @2, @"efficiency": @0},
        @"iPad6,7": @{@"performance": @2, @"efficiency": @0},
        @"iPad6,8": @{@"performance": @2, @"efficiency": @0},
        @"iPad6,11": @{@"performance": @2, @"efficiency": @0},
        @"iPad6,12": @{@"performance": @2, @"efficiency": @0},
        @"iPad7,1": @{@"performance": @3, @"efficiency": @3},
        @"iPad7,2": @{@"performance": @3, @"efficiency": @3},
        @"iPad7,3": @{@"performance": @3, @"efficiency": @3},
        @"iPad7,4": @{@"performance": @3, @"efficiency": @3},
        @"iPad7,5": @{@"performance": @2, @"efficiency": @2},
        @"iPad7,6": @{@"performance": @2, @"efficiency": @2},
        @"iPad7,11": @{@"performance": @2, @"efficiency": @2},
        @"iPad7,12": @{@"performance": @2, @"efficiency": @2},
        @"iPad8,1": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,2": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,3": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,4": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,5": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,6": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,7": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,8": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,9": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,10": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,11": @{@"performance": @4, @"efficiency": @4},
        @"iPad8,12": @{@"performance": @4, @"efficiency": @4},
        @"iPad11,1": @{@"performance": @2, @"efficiency": @4},
        @"iPad11,2": @{@"performance": @2, @"efficiency": @4},
        @"iPad11,3": @{@"performance": @2, @"efficiency": @4},
        @"iPad11,4": @{@"performance": @2, @"efficiency": @4},
        @"iPad11,6": @{@"performance": @2, @"efficiency": @4},
        @"iPad11,7": @{@"performance": @2, @"efficiency": @4},
        @"iPad12,1": @{@"performance": @2, @"efficiency": @4},
        @"iPad12,2": @{@"performance": @2, @"efficiency": @4},
        @"iPad13,1": @{@"performance": @2, @"efficiency": @4},
        @"iPad13,2": @{@"performance": @2, @"efficiency": @4},
        @"iPad13,4": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,5": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,6": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,7": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,8": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,9": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,10": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,11": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,16": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,17": @{@"performance": @4, @"efficiency": @4},
        @"iPad13,18": @{@"performance": @2, @"efficiency": @4},
        @"iPad13,19": @{@"performance": @2, @"efficiency": @4},
        @"iPad14,1": @{@"performance": @2, @"efficiency": @4},
        @"iPad14,2": @{@"performance": @2, @"efficiency": @4},
        @"iPad14,3": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,4": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,5": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,6": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,8": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,9": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,10": @{@"performance": @4, @"efficiency": @4},
        @"iPad14,11": @{@"performance": @4, @"efficiency": @4},
        @"iPad15,3": @{@"performance": @4, @"efficiency": @4},
        @"iPad15,4": @{@"performance": @4, @"efficiency": @4},
        @"iPad15,5": @{@"performance": @4, @"efficiency": @4},
        @"iPad15,6": @{@"performance": @4, @"efficiency": @4},
        @"iPad15,7": @{@"performance": @2, @"efficiency": @4},
        @"iPad15,8": @{@"performance": @2, @"efficiency": @4},
        @"iPad16,1": @{@"performance": @2, @"efficiency": @4},
        @"iPad16,2": @{@"performance": @2, @"efficiency": @4},
        // M4 iPad Pro ships as a 10-core (4P+6E) die; the 256/512 GB models are
        // binned to 9 cores (3P+6E). The live hw.perflevel sysctls report the
        // real per-unit count, so this fallback only applies if those are absent.
        @"iPad16,3": @{@"performance": @4, @"efficiency": @6},
        @"iPad16,4": @{@"performance": @4, @"efficiency": @6},
        @"iPad16,5": @{@"performance": @4, @"efficiency": @6},
        @"iPad16,6": @{@"performance": @4, @"efficiency": @6},
        // iPod touch (A8 has no efficiency cluster; A10 is a 2+2 Fusion part)
        @"iPod7,1": @{@"performance": @2, @"efficiency": @0},
        @"iPod9,1": @{@"performance": @2, @"efficiency": @2},
    };

    return topologies[identifier];
}

static BOOL readSysctlUnsigned(const char *name, unsigned *value) {
    size_t size = sizeof(*value);
    return sysctlbyname(name, value, &size, NULL, 0) == 0;
}

static BOOL readSysctlUnsigned64(const char *name, uint64_t *value) {
    size_t size = sizeof(*value);
    if (sysctlbyname(name, value, &size, NULL, 0) == 0 && size == sizeof(*value))
        return YES;
    // Several of these are published as 32-bit on some hosts.
    unsigned narrow = 0;
    if (!readSysctlUnsigned(name, &narrow))
        return NO;
    *value = narrow;
    return YES;
}

static NSDictionary<NSString *, NSNumber *> *hostRuntimeCoreTopology(void) {
    unsigned perfLevelCount = 0;
    if (!readSysctlUnsigned("hw.nperflevels", &perfLevelCount) || perfLevelCount == 0)
        return nil;

    unsigned performance = 0;
    unsigned efficiency = 0;
    unsigned homogeneous = 0;

    for (unsigned i = 0; i < perfLevelCount; i++) {
        char countName[64];
        snprintf(countName, sizeof(countName), "hw.perflevel%u.physicalcpu", i);

        unsigned coreCount = 0;
        if (!readSysctlUnsigned(countName, &coreCount)) {
            snprintf(countName, sizeof(countName), "hw.perflevel%u.logicalcpu", i);
            if (!readSysctlUnsigned(countName, &coreCount))
                continue;
        }

        // Tier strictly by perf-level INDEX: hw.perflevel0 is by Apple's
        // definition the highest-performance level, so index order is the
        // authoritative signal and hw.perflevel<N>.name is just a label.
        //
        // This used to substring-match the name ("perf" -> performance, "eff"
        // -> efficiency) and only fall back to index order, which the M5
        // generation breaks outright: an M5 Max reports perflevel0.name
        // "Super" (the FAST cluster) and perflevel1.name "Performance" (the
        // slow one), so the name match tagged the slower tier as performance
        // and the faster tier fell through to the same bucket -- 18 cores
        // reported as performance=18 efficiency=0. A name check that runs
        // before the index check can only ever override good information with
        // a guess, so it is gone.
        if (perfLevelCount == 1) {
            homogeneous += coreCount;
        } else if (i == 0) {
            performance += coreCount;
        } else if (i == perfLevelCount - 1) {
            efficiency += coreCount;
        } else {
            // A hypothetical 3+-level part: only the extremes are named tiers.
            homogeneous += coreCount;
        }
    }

    if (performance == 0 && efficiency == 0 && homogeneous == 0)
        return nil;

    return @{
        @"performance": @(performance),
        @"efficiency": @(efficiency),
        @"homogeneous": @(homogeneous),
    };
}

static NSDictionary<NSString *, NSNumber *> *hostCoreTopology(void) {
    NSString *processMachineIdentifier = hostProcessMachineIdentifier();
    if (!hostIdentifierNeedsSimulatorOverride(processMachineIdentifier)) {
        NSDictionary<NSString *, NSNumber *> *runtimeTopology = hostRuntimeCoreTopology();
        if (runtimeTopology != nil)
            return runtimeTopology;
    }
    return hostFallbackCoreTopology(hostMachineIdentifier());
}

static BOOL hostIdentifierNeedsSimulatorOverride(NSString *identifier) {
    return [identifier isEqualToString:@"arm64"] ||
           [identifier isEqualToString:@"arm64e"] ||
           [identifier isEqualToString:@"x86_64"] ||
           [identifier isEqualToString:@"i386"];
}

static NSString *hostMachineIdentifier(void) {
    NSString *machineIdentifier = hostProcessMachineIdentifier();
    if (hostIdentifierNeedsSimulatorOverride(machineIdentifier)) {
        NSString *simulatorIdentifier = NSProcessInfo.processInfo.environment[@"SIMULATOR_MODEL_IDENTIFIER"];
        if (simulatorIdentifier.length > 0)
            machineIdentifier = simulatorIdentifier;
    }

    return machineIdentifier;
}

static NSString *hostProcessMachineIdentifier(void) {
    struct utsname systemInfo;
    uname(&systemInfo);

    NSString *machineIdentifier = [NSString stringWithCString:systemInfo.machine encoding:NSUTF8StringEncoding];
    if (machineIdentifier == nil)
        machineIdentifier = @"unknown";
    return machineIdentifier;
}

char* printHostInfo(void) {
    struct utsname systemInfo;
    uname(&systemInfo);

    NSString *machineIdentifier = hostMachineIdentifier();
    NSString *translatedMachine = translateDeviceIdentifier(machineIdentifier);
    char *hostArchitecture = copyHostArchitecture();
    char *hostCoreTopology = copyHostCoreTopology();

    NSString *deviceInfo = [NSString stringWithFormat:@"Host OS Name: %s\nHost OS Release: %s\nHost OS Version: %s\nHost Architecture: %s\nHost Machine Identifier: %@\nHost Device Name: %@\nHost Core Types: %s\n",
                            systemInfo.sysname,    // Operating system name (e.g., Darwin)
                            systemInfo.release,    // Operating system release (e.g., 20.3.0)
                            systemInfo.version,    // Operating system version
                            hostArchitecture,
                            machineIdentifier,
                            translatedMachine,
                            hostCoreTopology];

    free(hostArchitecture);
    free(hostCoreTopology);
    char *cString = strdup([deviceInfo UTF8String]);
    return cString;
}

time_t buildTimestamp(void) {
    // The executable's own timestamp moves every time a new build is
    // installed. Deliberately not __DATE__/__TIME__: that is this file's
    // *compile* time and an incremental build that relinks without
    // recompiling hostinfo.m would report a stale one.
    NSString *executable = [[NSBundle mainBundle] executablePath];
    if (executable == nil)
        return 0;
    NSDictionary<NSFileAttributeKey, id> *attrs =
        [[NSFileManager defaultManager] attributesOfItemAtPath:executable error:NULL];
    NSDate *mtime = attrs[NSFileModificationDate];
    if (mtime == nil)
        return 0;
    return (time_t) [mtime timeIntervalSince1970];
}

char *copyBuildVersion(void) {
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *shortVersion = [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    NSString *build = [bundle objectForInfoDictionaryKey:@"CFBundleVersion"];

    // The bundle version alone is not enough to identify a build. It is
    // hand-maintained and routinely not bumped between development builds, so
    // two devices reporting "1.3 (546)" can be running binaries weeks apart --
    // which defeats the one question `uname -v` exists to answer, and makes
    // "does this device have that fix?" unanswerable during testing. The build
    // timestamp does move, so report it too.
    NSString *built = nil;
    time_t stamp = buildTimestamp();
    if (stamp != 0) {
        NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
        // Fixed POSIX locale and UTC: this is a machine-comparable stamp,
        // not something to localize. UTC specifically because the point is
        // comparing builds ACROSS devices, and devices do not agree on a
        // timezone -- formatting locally made two iPads running binaries
        // one minute apart report stamps eight hours apart, which is
        // exactly the confusion this is supposed to remove.
        fmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        fmt.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
        fmt.dateFormat = @"yyyy-MM-dd HH:mm'Z'";
        built = [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:stamp]];
    }

    NSMutableString *out = [NSMutableString string];
    if (shortVersion.length > 0)
        [out appendString:shortVersion];
    if (build.length > 0)
        [out appendString:out.length > 0 ? [NSString stringWithFormat:@" (%@)", build] : build];
    if (built != nil)
        [out appendString:[NSString stringWithFormat:@"%@built %@", out.length > 0 ? @" " : @"", built]];
    if (out.length == 0)
        [out appendString:[NSString stringWithUTF8String:__DATE__ " " __TIME__]];
    return strdup([out UTF8String]);
}

char *copyHostArchitecture(void) {
    const char *archName = NULL;
    const NXArchInfo *archInfo = NXGetLocalArchInfo();
    if (archInfo != NULL && archInfo->name != NULL)
        archName = archInfo->name;

    if (archName == NULL) {
#if defined(__aarch64__) || defined(__arm64__)
        archName = "arm64";
#elif defined(__x86_64__)
        archName = "x86_64";
#elif defined(__i386__)
        archName = "i386";
#elif defined(__arm__)
        archName = "arm";
#else
        archName = "unknown";
#endif
    }

    NSString *machineIdentifier = hostMachineIdentifier();
    NSString *chipIdentifier = hostChipIdentifier(machineIdentifier);
    if (chipIdentifier.length > 0) {
        NSString *description = [NSString stringWithFormat:@"%s(%@)", archName, chipIdentifier];
        return strdup([description UTF8String]);
    }

    return strdup(archName);
}

char *copyHostMachineIdentifier(void) {
    return strdup([hostMachineIdentifier() UTF8String]);
}

char *copyHostDeviceName(void) {
    NSString *translatedMachine = translateDeviceIdentifier(hostMachineIdentifier());
    return strdup([translatedMachine UTF8String]);
}

char *copyHostCoreTopology(void) {
    // A10 and A10X are first-generation "Fusion" SoCs: the die carries two core
    // clusters (e.g. the A10X's 3 performance + 3 efficiency cores) but the
    // hardware runs only ONE cluster at a time -- it switches between them
    // rather than scheduling all of them together. iOS therefore exposes only
    // half the physical cores through hw.ncpu, and the live perf-level sysctls
    // collapse to a single homogeneous tier. Report the true die from the static
    // table and spell out how many actually run at once. Every Apple SoC from
    // the A11 on can run all cores simultaneously, so this stays a closed set.
    NSString *machineIdentifier = hostMachineIdentifier();
    NSString *chip = hostChipIdentifier(machineIdentifier);
    if ([chip isEqualToString:@"A10"] || [chip isEqualToString:@"A10X"]) {
        NSDictionary<NSString *, NSNumber *> *die = hostFallbackCoreTopology(machineIdentifier);
        if (die != nil) {
            NSInteger performance = die[@"performance"].integerValue;
            NSInteger efficiency = die[@"efficiency"].integerValue;
            NSInteger total = performance + efficiency;

            unsigned usable = 0;
            if (!readSysctlUnsigned("hw.ncpu", &usable) || usable == 0 || (NSInteger) usable >= total)
                usable = (unsigned) (total / 2); // two equal clusters; one runs at a time

            NSString *summary = [NSString stringWithFormat:
                @"performance=%ld efficiency=%ld (%ld cores, only %u run simultaneously)",
                (long) performance, (long) efficiency, (long) total, usable];
            return strdup([summary UTF8String]);
        }
    }

    NSDictionary<NSString *, NSNumber *> *topology = hostCoreTopology();
    if (topology == nil)
        return strdup("unknown");

    NSInteger performance = topology[@"performance"].integerValue;
    NSInteger efficiency = topology[@"efficiency"].integerValue;
    NSInteger homogeneous = topology[@"homogeneous"].integerValue;

    NSString *summary = nil;
    if (performance > 0 || efficiency > 0) {
        summary = [NSString stringWithFormat:@"performance=%ld efficiency=%ld",
                   (long) performance, (long) efficiency];
    } else {
        summary = [NSString stringWithFormat:@"homogeneous=%ld", (long) homogeneous];
    }
    return strdup([summary UTF8String]);
}

void hostCacheGeometry(struct host_cache_geometry *out) {
    if (out == NULL)
        return;
    *out = (struct host_cache_geometry) {0};

    // Apple Silicon publishes per-tier geometry under hw.perflevel<N>.* and a
    // plain hw.l1*/hw.l2cachesize pair. The plain names report the SMALLER
    // tier (on an M5 Max, hw.l1dcachesize is perflevel1's 64K, not
    // perflevel0's 128K), which is exactly what we want: /sys presents one
    // homogeneous machine, and a library that blocks or tiles on cache size
    // must use the conservative number or it will thrash on the small cores.
    // Fall back to the fast tier only if the plain names are missing.
    if (!readSysctlUnsigned64("hw.l1icachesize", &out->l1i_size))
        readSysctlUnsigned64("hw.perflevel0.l1icachesize", &out->l1i_size);
    if (!readSysctlUnsigned64("hw.l1dcachesize", &out->l1d_size))
        readSysctlUnsigned64("hw.perflevel0.l1dcachesize", &out->l1d_size);
    if (!readSysctlUnsigned64("hw.l2cachesize", &out->l2_size))
        readSysctlUnsigned64("hw.perflevel0.l2cachesize", &out->l2_size);
    readSysctlUnsigned64("hw.cachelinesize", &out->line_size);
}
