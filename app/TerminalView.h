//
//  TerminalView.h
//  iSH
//
//  Created by Theodore Dubois on 11/3/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

enum OverrideAppearance {
    OverrideAppearanceNone,
    OverrideAppearanceLight,
    OverrideAppearanceDark,
};

@interface TerminalView : UIView <UITextInput, WKScriptMessageHandler, UIScrollViewDelegate>

@property IBInspectable (nonatomic) BOOL canBecomeFirstResponder;

@property (nonatomic) CGFloat overrideFontSize;
@property (readonly) CGFloat effectiveFontSize;
@property (nonatomic) enum OverrideAppearance overrideAppearance;

@property (nonatomic) UIKeyboardAppearance keyboardAppearance;

@property (weak) IBOutlet UIInputView *inputAccessoryView;
@property (weak) IBOutlet UIButton *controlKey;

@property (nonatomic) Terminal *terminal;

// MARK: Scrollback search
// Drives hterm's bundled find engine (see the find* exports in term.js). The
// query text lives in native chrome owned by TerminalViewController, not in
// hterm's own find bar, which stays hidden.
- (void)findOpen;
- (void)findSetText:(NSString *)text;
- (void)findNext;
- (void)findPrevious;
- (void)findClose;
// Called on the main thread as the batched search progresses. ordinal is the
// 0-based index of the selected match, or -1 when nothing is selected yet.
@property (nonatomic, copy) void (^findResultsDidChange)(NSInteger count, NSInteger ordinal);

@end
