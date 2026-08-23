//
//  UIViewController+Extras.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "UIViewController+Extras.h"

@implementation UIViewController (Extras)

- (void)presentError:(NSError *)error title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (BOOL)ish_canPushSubpage {
    UINavigationController *nav = self.navigationController;
    return nav != nil && !nav.navigationBarHidden;
}

- (void)anchorPopoverForAlertController:(UIAlertController *)alertController toSource:(id)source {
    UIPopoverPresentationController *popover = alertController.popoverPresentationController;
    if (popover == nil)
        return; // Not presented as a popover (e.g. iPhone) — no anchor required.
    if ([source isKindOfClass:UIBarButtonItem.class]) {
        popover.barButtonItem = source;
    } else if ([source isKindOfClass:UIView.class]) {
        popover.sourceView = source;
        popover.sourceRect = ((UIView *) source).bounds;
    } else {
        popover.sourceView = self.view;
        popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 1, 1);
    }
}

@end
