#import "SwapFilePicker.h"
#import "SceneDelegate.h"

@implementation SwapFilePicker

- (void)presentFrom:(UIViewController *)presenter completion:(SwapFilePickerCompletion)completion {
    _completion = completion;
    dispatch_async(dispatch_get_main_queue(), ^(void) {
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
            initWithDocumentTypes:@[ @"public.folder" ]
            inMode:UIDocumentPickerModeOpen];
        picker.delegate = self;
        picker.allowsMultipleSelection = NO;
        picker.presentationController.delegate = self;
        [presenter presentViewController:picker animated:YES completion:nil];
    });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (_completion)
        _completion(nil, -84); // _ECANCELED
    _completion = nil;
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    [self documentPickerWasCancelled:(UIDocumentPickerViewController *)presentationController];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (_completion) {
        if (urls.count == 0)
            _completion(nil, -84);
        else
            _completion(urls.firstObject, 0);
    }
    _completion = nil;
}

@end
