//
//  UIViewController+Extras.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "UIViewController+Extras.h"
#include <stdlib.h>
#include <string.h>

// Points `popover` at `source`: a UIBarButtonItem, a UIView (its bounds), or
// nil (a point at the centre of `fallbackView`).
static void ISHAnchorPopoverAtSource(UIPopoverPresentationController *popover, id source, UIView *fallbackView) {
    if ([source isKindOfClass:UIBarButtonItem.class]) {
        popover.barButtonItem = source;
    } else if ([source isKindOfClass:UIView.class]) {
        popover.sourceView = source;
        popover.sourceRect = ((UIView *) source).bounds;
    } else {
        popover.sourceView = fallbackView;
        popover.sourceRect = CGRectMake(CGRectGetMidX(fallbackView.bounds), CGRectGetMidY(fallbackView.bounds), 1, 1);
    }
}

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
    ISHAnchorPopoverAtSource(popover, source, self.view);
}

@end

#pragma mark - ISHActionSheet

// On a Mac, at most this many non-cancel actions stay a real alert. macOS alerts
// are designed for up to three buttons, counting Cancel. Past that the sheet is a
// menu of choices, and a one-row alert is the wrong shape for a menu.
static const NSUInteger ISHActionSheetMacAlertMaxChoices = 2;
// ... and only while all the button titles together, Cancel included, fit in
// this many characters. The row does not wrap, and a title can be data (a
// bookmark's page title, a model id, a saved session's description). At a
// typical 7pt per character, plus about 40pt of padding per button, 60
// characters is roughly 600pt. That still fits beside the title and message on
// the smallest scaled Mac display (1024pt wide).
static const NSUInteger ISHActionSheetMacAlertMaxTitleCharacters = 60;

BOOL ISHActionSheetUsesMacPresentation(void) {
    // Debug override for the simulator: see the header.
    const char *forced = getenv("ISH_FORCE_MAC_SHEETS");
    if (forced != NULL && strcmp(forced, "1") == 0)
        return YES;
    NSProcessInfo *info = NSProcessInfo.processInfo;
    return info.isiOSAppOnMac || info.isMacCatalystApp;
}

void ISHSizeTableSectionTitlesOnMac(UITableView *tableView) {
    // Not Mac Catalyst: its UIKit sizes titles on the storyboard path, measured
    // with a Catalyst build of the same table.
    if (!NSProcessInfo.processInfo.isiOSAppOnMac)
        return;
    tableView.sectionHeaderHeight = UITableViewAutomaticDimension;
    tableView.sectionFooterHeight = UITableViewAutomaticDimension;
    tableView.estimatedSectionHeaderHeight = 44;
    tableView.estimatedSectionFooterHeight = 44;
}

#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
// How far below a window's plain top safe area the vertical corner adaptation
// puts the region when it makes room for the window controls. It was 43pt in
// every windowed state measured, on the iPadOS 26.5 and 27.0 simulators: a
// window anywhere on screen (plain 10pt, adapted 53pt), a window at the top of
// the screen under the status bar (32, 75), a window at launch before the
// status bar arrives (0, 43), and Slide Over (10, 53).
static const CGFloat ISHWindowingControlsClearance = 43;

static BOOL ISHWindowChromeIsOutsideContent(void) {
    NSProcessInfo *info = NSProcessInfo.processInfo;
    if (info.isiOSAppOnMac || info.isMacCatalystApp)
        return YES;
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260100
    if (@available(iOS 26.1, *)) {
        if (info.isiOSAppOnVision)
            return YES;
    }
#endif
    return NO;
}
#endif

CGFloat ISHWindowingControlsTopInset(UIView *view) {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        UIWindow *window = view.window;
        if (window == nil)
            return 0;
        // A window that covers its whole screen is full screen, where the controls
        // are in the menu bar, not over the content. Decided from the geometry
        // rather than from the corner inset because that inset can be stale: an
        // app that launched in a window and then went full screen with the green
        // button still read a 10pt safe area and a 53pt corner inset in a
        // screen-sized window, measured on the iOS 26.5 simulator. Trusting it
        // there left a 21pt gap under the status bar.
        UIScreen *screen = window.windowScene.screen;
        if (screen != nil &&
            fabs(CGRectGetWidth(window.bounds) - CGRectGetWidth(screen.bounds)) < 1 &&
            fabs(CGRectGetHeight(window.bounds) - CGRectGetHeight(screen.bounds)) < 1)
            return 0;
        UIViewLayoutRegion *region = [UIViewLayoutRegion safeAreaLayoutRegionWithCornerAdaptation:UIViewLayoutRegionAdaptivityAxisVertical];
        CGFloat windowTop = [window edgeInsetsForLayoutRegion:region].top;
        CGFloat plainTop = window.safeAreaInsets.top;
        // A window whose corner adaptation made no room for its controls. Every
        // window has them in its top-leading corner, and every window measured
        // had them cleared by ISHWindowingControlsClearance; content starting at
        // the plain safe area instead is #580's 555 screenshot, a window at the
        // top of the screen with its first rows under the controls. Clear them
        // by the usual amount. An iOS app on a Mac or on Vision Pro has its
        // window chrome outside the content, so there is nothing to clear.
        if (windowTop < plainTop + 1 && !ISHWindowChromeIsOutsideContent())
            windowTop = plainTop + ISHWindowingControlsClearance;
        CGFloat viewTop = [view convertPoint:CGPointZero toView:window].y;
        return MAX(0, windowTop - viewTop);
    }
#endif
    return 0;
}

@interface ISHActionSheetItem : NSObject
@property (nonatomic, strong) UIAlertAction *action;
@property (nonatomic, copy, nullable) void (^handler)(UIAlertAction *action);
@end

@implementation ISHActionSheetItem
@end

@interface ISHActionSheet ()
@property (nonatomic, copy, nullable) NSString *title;
@property (nonatomic, copy, nullable) NSString *message;
@property (nonatomic) UIAlertControllerStyle style;
@property (nonatomic, strong) NSMutableArray<ISHActionSheetItem *> *items;
@end

// The Mac presentation: one row per non-cancel action, the message above the
// rows, the cancel action as a bar button.
@interface ISHActionSheetListViewController : UITableViewController <UIPopoverPresentationControllerDelegate>
- (instancetype)initWithSheet:(ISHActionSheet *)sheet;
- (CGSize)contentSizeFittingSize:(CGSize)available;
@property (nonatomic, readonly) BOOL dismissableWithoutChoice;
@end

@implementation ISHActionSheet

+ (instancetype)sheetWithTitle:(NSString *)title message:(NSString *)message style:(UIAlertControllerStyle)style {
    ISHActionSheet *sheet = [[self alloc] init];
    sheet.title = title;
    sheet.message = message;
    sheet.style = style;
    sheet.items = [NSMutableArray array];
    return sheet;
}

+ (instancetype)actionSheetWithTitle:(NSString *)title message:(NSString *)message {
    return [self sheetWithTitle:title message:message style:UIAlertControllerStyleActionSheet];
}

+ (instancetype)alertWithTitle:(NSString *)title message:(NSString *)message {
    return [self sheetWithTitle:title message:message style:UIAlertControllerStyleAlert];
}

- (UIAlertAction *)addActionWithTitle:(NSString *)title
                                style:(UIAlertActionStyle)style
                              handler:(void (^)(UIAlertAction *action))handler {
    // The UIAlertController gets this action, handler and all. The list keeps
    // its own copy of the handler, because UIAlertAction has no public way to
    // read one back.
    ISHActionSheetItem *item = [[ISHActionSheetItem alloc] init];
    item.action = [UIAlertAction actionWithTitle:title style:style handler:handler];
    item.handler = handler;
    [self.items addObject:item];
    return item.action;
}

- (BOOL)presentsAsList {
    if (!ISHActionSheetUsesMacPresentation())
        return NO;
    NSUInteger choices = 0;
    NSUInteger characters = 0;
    for (ISHActionSheetItem *item in self.items) {
        if (item.action.style != UIAlertActionStyleCancel)
            choices++;
        characters += item.action.title.length;
    }
    return choices > ISHActionSheetMacAlertMaxChoices || characters > ISHActionSheetMacAlertMaxTitleCharacters;
}

- (void)presentFromViewController:(UIViewController *)presenter
                       sourceView:(UIView *)sourceView
                       sourceRect:(CGRect)sourceRect {
    [self presentFromViewController:presenter anchor:^(UIPopoverPresentationController *popover) {
        if (sourceView != nil) {
            popover.sourceView = sourceView;
            popover.sourceRect = sourceRect;
        } else {
            ISHAnchorPopoverAtSource(popover, nil, presenter.view);
        }
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }];
}

- (void)presentFromViewController:(UIViewController *)presenter source:(id)source {
    [self presentFromViewController:presenter anchor:^(UIPopoverPresentationController *popover) {
        ISHAnchorPopoverAtSource(popover, source, presenter.view);
    }];
}

- (void)presentFromViewController:(UIViewController *)presenter
                           anchor:(void (^)(UIPopoverPresentationController *popover))anchor {
    if (self.presentsAsList) {
        [self presentListFromViewController:presenter anchor:anchor];
        return;
    }
    // iPhone and iPad, unchanged: the UIAlertController every call site built
    // for itself before this helper existed.
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:self.title
                                                                   message:self.message
                                                            preferredStyle:self.style];
    for (ISHActionSheetItem *item in self.items)
        [alert addAction:item.action];
    if (self.style == UIAlertControllerStyleActionSheet) {
        UIPopoverPresentationController *popover = alert.popoverPresentationController;
        if (popover != nil)
            anchor(popover);
    }
    [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)presentListFromViewController:(UIViewController *)presenter
                               anchor:(void (^)(UIPopoverPresentationController *popover))anchor {
    ISHActionSheetListViewController *list = [[ISHActionSheetListViewController alloc] initWithSheet:self];
    UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:list];
    UIView *space = presenter.view.window ?: presenter.view;
    list.preferredContentSize = [list contentSizeFittingSize:space.bounds.size];
    if (self.style == UIAlertControllerStyleActionSheet) {
        navigation.modalPresentationStyle = UIModalPresentationPopover;
        UIPopoverPresentationController *popover = navigation.popoverPresentationController;
        anchor(popover);
        popover.delegate = list;
    } else {
        // An alert has no anchor: a centred sheet, modal like the alert was.
        navigation.modalPresentationStyle = UIModalPresentationFormSheet;
        navigation.presentationController.delegate = list;
    }
    navigation.modalInPresentation = !list.dismissableWithoutChoice;
    [presenter presentViewController:navigation animated:YES completion:nil];
}

@end

static NSString *const ISHActionSheetRowIdentifier = @"ISHActionSheetRow";
static const CGFloat ISHActionSheetRowHorizontalInset = 20.0;
static const CGFloat ISHActionSheetRowVerticalInset = 11.0;
static const CGFloat ISHActionSheetMinimumRowHeight = 44.0;
static const CGFloat ISHActionSheetHeaderVerticalInset = 12.0;

@implementation ISHActionSheetListViewController {
    NSString *_message;
    UIAlertControllerStyle _style;
    NSArray<ISHActionSheetItem *> *_choices;
    ISHActionSheetItem *_cancelItem;
    UILabel *_messageLabel;
    CGFloat _headerLaidOutWidth;
    CGFloat _rowsMeasuredWidth;
    CGFloat _maximumHeight;
    CGFloat _visibleHeightAtLastAdjustment;
    NSUInteger _sizeAdjustments;
    BOOL _finished;
}

- (instancetype)initWithSheet:(ISHActionSheet *)sheet {
    self = [super initWithStyle:UITableViewStylePlain];
    if (self == nil)
        return nil;
    self.title = sheet.title;
    _message = [sheet.message copy];
    _style = sheet.style;
    NSMutableArray<ISHActionSheetItem *> *choices = [NSMutableArray array];
    for (ISHActionSheetItem *item in sheet.items) {
        // UIAlertController allows one cancel action and throws on a second,
        // so the first one is the only one.
        if (item.action.style == UIAlertActionStyleCancel) {
            if (_cancelItem == nil)
                _cancelItem = item;
        } else {
            [choices addObject:item];
        }
    }
    _choices = choices;
    return self;
}

// An action sheet's popover goes away when you click outside it, as it does on an
// iPad. An alert without a cancel action (the resume picker) must be answered.
- (BOOL)dismissableWithoutChoice {
    return _cancelItem != nil || _style == UIAlertControllerStyleActionSheet;
}

+ (UIFont *)rowFont {
    return [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
}

+ (UIFont *)messageFont {
    return [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
}

+ (CGFloat)heightOfText:(NSString *)text font:(UIFont *)font width:(CGFloat)width {
    if (text.length == 0)
        return 0;
    CGRect rect = [text boundingRectWithSize:CGSizeMake(MAX(width, 1.0), CGFLOAT_MAX)
                                     options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading
                                  attributes:@{NSFontAttributeName: font}
                                     context:nil];
    return ceil(CGRectGetHeight(rect));
}

- (CGFloat)rowHeightForItem:(ISHActionSheetItem *)item width:(CGFloat)width {
    CGFloat text = [self.class heightOfText:item.action.title
                                       font:self.class.rowFont
                                      width:width - 2 * ISHActionSheetRowHorizontalInset];
    return MAX(ISHActionSheetMinimumRowHeight, text + 2 * ISHActionSheetRowVerticalInset + 1.0);
}

- (CGFloat)headerHeightForWidth:(CGFloat)width {
    if (_message.length == 0)
        return 0;
    CGFloat text = [self.class heightOfText:_message
                                       font:self.class.messageFont
                                      width:width - 2 * ISHActionSheetRowHorizontalInset];
    return text + 2 * ISHActionSheetHeaderVerticalInset;
}

- (CGFloat)contentHeightForWidth:(CGFloat)width {
    CGFloat height = [self headerHeightForWidth:width];
    for (ISHActionSheetItem *item in _choices)
        height += [self rowHeightForItem:item width:width];
    return height;
}

// Wide enough for the longest title on one line, within 280...440pt, and tall
// enough for every row up to about two thirds of the space. Past that the list
// scrolls.
- (CGSize)contentSizeFittingSize:(CGSize)available {
    UIFont *rowFont = self.class.rowFont;
    CGFloat widest = 0;
    for (ISHActionSheetItem *item in _choices)
        widest = MAX(widest, ceil([item.action.title sizeWithAttributes:@{NSFontAttributeName: rowFont}].width));
    UIFont *titleFont = [UIFont systemFontOfSize:17.0 weight:UIFontWeightSemibold];
    CGFloat titleWidth = ceil([self.title ?: @"" sizeWithAttributes:@{NSFontAttributeName: titleFont}].width);
    CGFloat buttonWidth = _cancelItem != nil
        ? ceil([_cancelItem.action.title sizeWithAttributes:@{NSFontAttributeName: rowFont}].width) + 24.0
        : 0;
    // The title is centred, so the cancel button's width is reserved on both sides.
    // The 24pt spare covers the arrow's inset when the popover points sideways.
    CGFloat width = MAX(widest + 2 * ISHActionSheetRowHorizontalInset + 24.0, titleWidth + 2 * buttonWidth + 32.0);
    CGFloat maximumWidth = MAX(280.0, MIN(440.0, available.width - 40.0));
    width = MIN(MAX(width, 280.0), maximumWidth);
    _maximumHeight = MAX(220.0, MIN(560.0, available.height * 0.66));
    return CGSizeMake(width, MIN([self contentHeightForWidth:width], _maximumHeight));
}

- (void)viewDidLoad {
    [super viewDidLoad];
    UITableView *tableView = self.tableView;
    [tableView registerClass:UITableViewCell.class forCellReuseIdentifier:ISHActionSheetRowIdentifier];
    tableView.cellLayoutMarginsFollowReadableWidth = NO;
    tableView.estimatedRowHeight = 0; // exact heights, so contentSize matches the size asked for
    tableView.alwaysBounceVertical = NO;
    tableView.tableFooterView = [[UIView alloc] initWithFrame:CGRectZero];
    if (_message.length > 0) {
        _messageLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        _messageLabel.text = _message;
        _messageLabel.font = self.class.messageFont;
        _messageLabel.textColor = UIColor.secondaryLabelColor;
        _messageLabel.numberOfLines = 0;
        UIView *header = [[UIView alloc] initWithFrame:CGRectZero];
        [header addSubview:_messageLabel];
        tableView.tableHeaderView = header;
    }
    if (_cancelItem != nil) {
        self.navigationItem.leftBarButtonItem =
            [[UIBarButtonItem alloc] initWithTitle:_cancelItem.action.title
                                             style:UIBarButtonItemStylePlain
                                            target:self
                                            action:@selector(cancelTapped:)];
    }
}

// The width row text actually gets. A popover gives its content a safe-area
// inset on the side its arrow is on, and table cells are inset by that too.
- (CGFloat)rowContentWidth {
    UITableView *tableView = self.tableView;
    UIEdgeInsets safe = tableView.safeAreaInsets;
    CGFloat width = CGRectGetWidth(tableView.bounds) - safe.left - safe.right;
    // Not laid out yet: measure at the width asked for, not at zero (which
    // would wrap every character onto its own line).
    return width > 0 ? width : self.preferredContentSize.width;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    UITableView *tableView = self.tableView;
    if (CGRectGetWidth(tableView.bounds) <= 0)
        return;
    CGFloat width = [self rowContentWidth];
    // Row heights depend on the width (titles wrap). The width can change once
    // the popover settles which side its arrow is on, so re-query them then.
    if (width != _rowsMeasuredWidth) {
        BOOL remeasure = _rowsMeasuredWidth > 0;
        _rowsMeasuredWidth = width;
        if (remeasure) {
            [tableView beginUpdates];
            [tableView endUpdates];
        }
    }
    UIView *header = tableView.tableHeaderView;
    CGFloat headerLeft = tableView.safeAreaInsets.left + ISHActionSheetRowHorizontalInset;
    if (header != nil && _messageLabel != nil && (width != _headerLaidOutWidth || CGRectGetMinX(_messageLabel.frame) != headerLeft)) {
        _headerLaidOutWidth = width;
        CGFloat height = [self headerHeightForWidth:width];
        _messageLabel.frame = CGRectMake(headerLeft, ISHActionSheetHeaderVerticalInset,
                                         width - 2 * ISHActionSheetRowHorizontalInset,
                                         height - 2 * ISHActionSheetHeaderVerticalInset);
        header.frame = CGRectMake(0, 0, CGRectGetWidth(tableView.bounds), height);
        tableView.tableHeaderView = header; // re-set, or the table keeps the old height
    }
    // Show exactly the rows (up to the cap), measured rather than predicted: the
    // navigation controller adds its own guess at the bar height, the popover
    // insets its content around the arrow and corners, and a narrower popover
    // than asked for wraps titles onto more lines. So grow or shrink by what is
    // actually missing. Bounded, and it stops once a change makes no difference,
    // since a popover with no more room would otherwise be asked again on every
    // layout pass.
    UIEdgeInsets insets = tableView.adjustedContentInset;
    CGFloat visible = CGRectGetHeight(tableView.bounds) - insets.top - insets.bottom;
    CGFloat wanted = MIN([self contentHeightForWidth:width], _maximumHeight);
    CGSize preferred = self.preferredContentSize;
    if (_maximumHeight > 0 && visible > 0 && fabs(wanted - visible) > 0.5 &&
        _sizeAdjustments < 4 && visible != _visibleHeightAtLastAdjustment) {
        _sizeAdjustments++;
        _visibleHeightAtLastAdjustment = visible;
        self.preferredContentSize = CGSizeMake(preferred.width, MAX(1.0, preferred.height + wanted - visible));
    }
}

#pragma mark Table

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger) _choices.count;
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self rowHeightForItem:_choices[(NSUInteger) indexPath.row] width:[self rowContentWidth]];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:ISHActionSheetRowIdentifier forIndexPath:indexPath];
    UIAlertAction *action = _choices[(NSUInteger) indexPath.row].action;
    UIListContentConfiguration *content = cell.defaultContentConfiguration;
    content.text = action.title;
    content.textProperties.font = self.class.rowFont;
    content.textProperties.numberOfLines = 0;
    if (!action.enabled)
        content.textProperties.color = UIColor.tertiaryLabelColor;
    else if (action.style == UIAlertActionStyleDestructive)
        content.textProperties.color = UIColor.systemRedColor;
    else
        content.textProperties.color = UIColor.labelColor;
    // Fixed margins, so -rowHeightForItem:width: measures the width the text gets.
    content.axesPreservingSuperviewLayoutMargins = UIAxisNeither;
    content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(ISHActionSheetRowVerticalInset, ISHActionSheetRowHorizontalInset,
                                                                   ISHActionSheetRowVerticalInset, ISHActionSheetRowHorizontalInset);
    cell.contentConfiguration = content;
    cell.selectionStyle = action.enabled ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    cell.accessibilityTraits = UIAccessibilityTraitButton | (action.enabled ? 0 : UIAccessibilityTraitNotEnabled);
    return cell;
}

- (BOOL)tableView:(UITableView *)tableView shouldHighlightRowAtIndexPath:(NSIndexPath *)indexPath {
    return _choices[(NSUInteger) indexPath.row].action.enabled;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    ISHActionSheetItem *item = _choices[(NSUInteger) indexPath.row];
    if (!item.action.enabled)
        return;
    [self finishWithItem:item dismiss:YES];
}

- (void)cancelTapped:(id)sender {
    [self finishWithItem:_cancelItem dismiss:YES];
}

// Runs one handler, once, after the list is gone. The same order a
// UIAlertController uses, so a handler that presents something is not refused
// because the list is still on screen.
- (void)finishWithItem:(ISHActionSheetItem *)item dismiss:(BOOL)dismiss {
    if (_finished)
        return;
    _finished = YES;
    void (^handler)(UIAlertAction *) = item.handler;
    UIAlertAction *action = item.action;
    void (^run)(void) = ^{
        if (handler != nil)
            handler(action);
    };
    UIViewController *presented = self.navigationController ?: self;
    if (!dismiss || presented.presentingViewController == nil) {
        run();
        return;
    }
    [presented dismissViewControllerAnimated:YES completion:run];
}

#pragma mark Presentation

// Stay a popover even when the window is compact (a narrow Mac window, iPhone
// with the debug override). An adapted full-screen list would have no way out
// other than the cancel button.
- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController *)controller
                                                               traitCollection:(UITraitCollection *)traitCollection {
    return UIModalPresentationNone;
}

- (BOOL)presentationControllerShouldDismiss:(UIPresentationController *)presentationController {
    return self.dismissableWithoutChoice;
}

// Clicked outside: already dismissed, so only the cancel handler remains, as
// for an iPad action-sheet popover.
- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    [self finishWithItem:_cancelItem dismiss:NO];
}

@end
