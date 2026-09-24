#import "SwapFilePicker.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@implementation SwapFilePicker {
    SwapFilePickerCompletion _completion;
    // Held until the picker answers; see the header.
    SwapFilePicker *_keepAlive;
}

- (void)presentFrom:(UIViewController *)presenter completion:(SwapFilePickerCompletion)completion {
    _completion = [completion copy];
    _keepAlive = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
        picker.delegate = self;
        picker.allowsMultipleSelection = NO;
        picker.presentationController.delegate = self;
        [presenter presentViewController:picker animated:YES completion:nil];
    });
}

- (void)finishWithURL:(NSURL *_Nullable)url {
    SwapFilePickerCompletion completion = _completion;
    _completion = nil;
    if (completion != nil)
        completion(url);
    _keepAlive = nil;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    [self finishWithURL:urls.firstObject];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    [self finishWithURL:nil];
}

// Swiped down rather than cancelled: the same answer.
- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    [self finishWithURL:nil];
}

@end
