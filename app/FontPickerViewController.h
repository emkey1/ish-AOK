//
//  FontPickerViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface FontPickerViewController : UITableViewController

// First touch of every installed font is what makes building the list expensive.
// Call this ahead of time to get that out of the way off the main thread.
+ (void)prewarm;

@end

NS_ASSUME_NONNULL_END
