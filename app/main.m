//
//  main.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"

extern void run_at_boot(void);

int main(int argc, char * argv[]) {
    @autoreleasepool {
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            run_at_boot();
        });

        int retVal = 0;
        @try {
            // Your existing setup code here
            retVal = UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
        }
        @catch (NSException *exception) {
            // Handle or log the exception
            NSLog(@"Uncaught exception: %@", exception.description);
            NSLog(@"Stack trace: %@", [exception callStackSymbols]);
        }
        @finally {
            // Perform any final cleanup or logging if necessary
            return retVal;
        }
    }
}
