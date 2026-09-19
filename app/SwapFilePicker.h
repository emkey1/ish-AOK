// SwapFilePicker: presents a UIDocumentPickerViewController for the user to
// choose a directory on a USB drive for swap storage. Non-blocking: presents
// the picker and calls the completion handler on the main thread when the
// user picks or cancels.

#import <UIKit/UIKit.h>

typedef void (^SwapFilePickerCompletion)(NSURL * _Nullable url, int error);

@interface SwapFilePicker : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>

@property (nonatomic, copy) SwapFilePickerCompletion completion;

- (void)presentFrom:(UIViewController *)presenter completion:(SwapFilePickerCompletion)completion;

@end
