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

#pragma mark External display

// A Terminal owns exactly one WKWebView, so it can only ever be rendered in one
// place. To use an external display we relocate just the rendering: the webView
// is re-parented into a view on the external screen while this view keeps being
// the first responder that feeds the tty. Keyboard input never went through the
// webView, so typing keeps working on the device with nothing else changed.
// nil (the default) means the terminal renders locally in the scrollbar view.
@property (weak, nonatomic, readonly) UIView *externalHostView;
@property (readonly, nonatomic) BOOL rendersOnExternalDisplay;

- (void)relocateContentToView:(UIView *)hostView;
// Takes the rendering back only if it is still hosted in hostView. iOS can
// stand up a replacement external scene before tearing the old one down (seen
// when the display renegotiates its mode), and the departing scene must not
// yank the terminal off the display the new one just put it on.
- (void)restoreContentFromView:(UIView *)hostView;

@end
