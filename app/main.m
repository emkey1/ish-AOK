//
//  main.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"
extern void run_at_boot(void);
#import <Foundation/Foundation.h>
#import <Foundation/NSProcessInfo.h>

void disable_app_nap(void) {
   if ([[NSProcessInfo processInfo] respondsToSelector:@selector(beginActivityWithOptions:reason:)]) {
      [[NSProcessInfo processInfo] beginActivityWithOptions:0x00FFFFFF reason:@"Not sleepy and don't want to nap"];
   }
}

int main(int argc, char * argv[]) {
    @autoreleasepool {
        disable_app_nap();  // No napping I say. -mke
        run_at_boot();
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
