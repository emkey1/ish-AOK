//
//  hostinfo.m
//  iSH-AOK
//
//  Created by Michael Miller on 12/25/23.
//

#import <Foundation/Foundation.h>
#import <sys/utsname.h>

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
    };
    
    NSString *deviceName = deviceNames[identifier];
    if (deviceName) {
        return deviceName;
    } else {
        return identifier; // Return the original identifier if no mapping is found
    }
}

char* printHostInfo(void) {
    struct utsname systemInfo;
    uname(&systemInfo);
    
    NSString *machineIdentifier = [NSString stringWithCString:systemInfo.machine encoding:NSUTF8StringEncoding];
    NSString *translatedMachine = translateDeviceIdentifier(machineIdentifier);
    
    // Create an NSString with formatted device info
    NSString *deviceInfo = [NSString stringWithFormat:@"Host OS Name: %s\nHost OS Release: %s\nHost OS Version: %s\nHost Hardware: %@\n",
                            systemInfo.sysname,    // Operating system name (e.g., Darwin)
                            systemInfo.release,    // Operating system release (e.g., 20.3.0)
                            systemInfo.version,    // Operating system version
                            translatedMachine];    // Translated hardware identifier
    
    // Convert NSString to C-string
    char *cString = strdup([deviceInfo UTF8String]);
    return cString;
}
