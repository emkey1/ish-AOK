//
//  AccessibilityFixes.m
//  iSH
//
//  Created by Saagar Jha on 12/31/22.
//

#import "hook.h"
#import <UIKit/UIKit.h>
#import <dlfcn.h>
#import <objc/runtime.h>

// Work around https://bugs.webkit.org/show_bug.cgi?id=249976, which causes
// https://github.com/ish-app/ish/issues/1937.

static void replacement(void) {
    NSLog(@"Hooked PageClientImpl::assistiveTechnologyMakeFirstResponder");
}

static bool patched;

static void patch_if_needed(void) {
    if (!patched && UIAccessibilityIsVoiceOverRunning()) {
        patched = true;
        // This can take a little while.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            Dl_info info;
            dladdr((__bridge void *)objc_getClass("WKWebView"), &info);
            void *symbol = find_symbol(info.dli_fbase, "__ZN6WebKit14PageClientImpl37assistiveTechnologyMakeFirstResponderEv");
            bool hooked = symbol != NULL && hook((void *)symbol, (void *)replacement);
            if (!hooked) {
                NSLog(@"Accessibility patch skipped (symbol missing or debugger attached)");
            }
        });
    }
}

__attribute__((constructor)) void accessibilityfixes_init(void) {
    // WebKit fixed 249976 in February 2023, so no system from iOS 17 on has
    // the bug. The hook finds a WebKit symbol by its mangled name and
    // intercepts it with a hardware breakpoint, which is nothing to do on a
    // system that does not need it.
    if (@available(iOS 17.0, *))
        return;
    if (@available(iOS 15.7, *)) {
        [NSNotificationCenter.defaultCenter addObserverForName:UIAccessibilityVoiceOverStatusDidChangeNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification *notification) {
            patch_if_needed();
        }];
        patch_if_needed();
    }
}
