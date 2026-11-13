//
//  ScrollbarView.m
//  iSH
//
//  Created by Theodore Dubois on 9/2/19.
//

#import "ScrollbarView.h"

@class ScrollbarViewDelegate;

@interface ScrollbarView ()

@property CGPoint contentViewOrigin;
- (void)pinContentView;
@property ScrollbarViewDelegate *outerDelegate;

@end

@interface ScrollbarViewDelegate : NSObject <UIScrollViewDelegate>
@property (weak) id<UIScrollViewDelegate> innerDelegate;
@end
@implementation ScrollbarViewDelegate

- (void)scrollViewDidScroll:(ScrollbarView *)scrollView {
    [scrollView pinContentView];
    [self.innerDelegate scrollViewDidScroll:scrollView];
}

// UIKit asks before it calls an optional delegate method, and forwarding alone
// does not answer that question: without this, a method only the inner
// delegate has (TerminalView's scroll-to-top pair) is never called.
- (BOOL)respondsToSelector:(SEL)aSelector {
    return [super respondsToSelector:aSelector] || [self.innerDelegate respondsToSelector:aSelector];
}

- (id)forwardingTargetForSelector:(SEL)aSelector {
    if ([self.innerDelegate respondsToSelector:aSelector])
        return self.innerDelegate;
    return [super forwardingTargetForSelector:aSelector];
}

@end

@implementation ScrollbarView

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.outerDelegate = [ScrollbarViewDelegate new];
        super.delegate = self.outerDelegate;
    }
    return self;
}

- (void)setContentView:(UIView *)contentView {
    _contentView = contentView;
    // Where the content sits relative to the visible area, not to the content
    // space: a view installed while the scroll view is scrolled (its frame set
    // from our bounds, whose origin IS contentOffset) used to record that
    // offset here, and every later scroll added it again -- the terminal's web
    // view was moved that far below the screen and the terminal drew nothing
    // but its scroll bar. Installing happens on every tty switch and every
    // rebuild of a reclaimed web view, so a long scrollback made it common
    // (TestFlight feedback on 564: "the text just disappears").
    CGPoint origin = contentView.frame.origin;
    self.contentViewOrigin = CGPointMake(origin.x - self.contentOffset.x, origin.y - self.contentOffset.y);
    [self pinContentView];
}

// Keep the content over the visible area. scrollViewDidScroll: does this for a
// scroll, but not every offset change goes through it in step with a layout
// pass (a resize, or a content size that clamps the offset).
- (void)pinContentView {
    UIView *contentView = self.contentView;
    if (contentView == nil)
        return;
    CGRect frame = contentView.frame;
    frame.origin.x = self.contentOffset.x + self.contentViewOrigin.x;
    frame.origin.y = self.contentOffset.y + self.contentViewOrigin.y;
    if (!CGRectEqualToRect(frame, contentView.frame))
        contentView.frame = frame;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self pinContentView];
}

- (id<UIScrollViewDelegate>)delegate {
    return self.outerDelegate.innerDelegate;
}

- (void)setDelegate:(id<UIScrollViewDelegate>)delegate {
    self.outerDelegate.innerDelegate = delegate;
}

@end
