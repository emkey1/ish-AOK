//
//  UIDevice.m
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

char* printUIDevice(void);

@interface MyDeviceUtil : NSObject
+ (NSString*) getAllDeviceInfo;
@end

@implementation MyDeviceUtil


+ (NSString*) getAllDeviceInfo {
    UIDevice *device = [UIDevice currentDevice];
    NSString *text = @"";
    switch (device.orientation) {
        case UIDeviceOrientationPortrait:
            text = @"Portrait";
            break;
        case UIDeviceOrientationPortraitUpsideDown:
            text = @"PortraitUpsideDown";
            break;
        case UIDeviceOrientationLandscapeLeft:
            text = @"LandscapeLeft";
            break;
        case UIDeviceOrientationLandscapeRight:
            text = @"LandscapeRight";
            break;
        default:
            text = @"Unknown";
            break;
    }
    NSMutableString *result = [[NSMutableString alloc] init];
    
    [result appendFormat:@"Model: %@\n", device.name];
    [result appendFormat:@"OS Name: %@\n", device.systemName];
    [result appendFormat:@"OS Version: %@\n", device.systemVersion];
    [result appendFormat:@"Device Orientation: %@\n", text];
    [result appendFormat:@"Battery Monitoring Enabled: %@\n", device.isBatteryMonitoringEnabled ? @"YES" : @"NO"];
    
    return result;
}

@end

char* printUIDevice(void) {
    NSString *info = [MyDeviceUtil getAllDeviceInfo];
    const char *cString = [info UTF8String];
    return strdup(cString);
}
