// SwapFilePicker: asks the user for a directory to hold the swap file on
// external storage (a USB drive), with a folder picker.
//
// It keeps itself alive until it answers. UIDocumentPickerViewController holds
// its delegate weakly, so a picker object that only its caller held would be
// gone before the user chose anything, and the choice would never arrive.

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// `url` is the chosen directory, or nil if the user cancelled. Called once, on
// the main thread.
typedef void (^SwapFilePickerCompletion)(NSURL *_Nullable url);

@interface SwapFilePicker : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>

- (void)presentFrom:(UIViewController *)presenter completion:(SwapFilePickerCompletion)completion;

@end

NS_ASSUME_NONNULL_END
