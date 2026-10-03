//
//  LLMChatViewController.m
//  iSH-AOK
//
//  The Workspace LLM Chat screen: a view of one chat's agent (LLMChatAgent.m),
//  which does the work -- the transcript, the prompt, the status panel, and
//  the approvals the agent is waiting on.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "UserPreferences.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "MarkdownRenderer.h"
#import "Terminal.h"
#import "GuestFileBridge.h"
#import "LLMChatAnthropic.h"
#import "LLMChatStream.h"
#import "LLMChatInternal.h"
#import "LLMChatMCP.h"
#import "LLMChatAgent.h"
#if __has_include("libiSH_AOKApp-Swift.h")
#import "libiSH_AOKApp-Swift.h" // AOKFoundationModelsBridge (Swift, iOS 26+ FoundationModels wrapper)
#endif

UIViewController *ISHCreateLLMClientViewController(void) {
    return [LLMClientViewController new];
}

UIViewController *ISHCreateLLMClientViewControllerWithInitialPrompt(NSString *initialPrompt) {
    LLMClientViewController *viewController = [LLMClientViewController new];
    viewController.initialPrompt = initialPrompt;
    return viewController;
}

UIViewController *ISHCreateLLMSettingsViewController(void) {
    return [LLMSettingsViewController new];
}

BOOL ISHLLMClientEnabled(void) {
    return UserPreferences.shared.shouldEnableLLMClient;
}

static UIFont *ISHLLMMonospaceFont(CGFloat size) {
    if (@available(iOS 13.0, *))
        return [UIFont monospacedSystemFontOfSize:size weight:UIFontWeightRegular];
    return [UIFont fontWithName:@"Menlo" size:size] ?: [UIFont systemFontOfSize:size];
}

// A UIButton that remembers the text a tap on it should copy -- lets one
// target action (on the cell) serve any number of per-code-block Copy
// buttons without threading the payload through the responder chain.
@interface ISHLLMCopyButton : UIButton
@property (nonatomic, copy) NSString *payload;
@end
@implementation ISHLLMCopyButton
@end

// One chat bubble: a role-colored container holding a vertical stack of
// blocks -- selectable, wrapping text views for prose (so partial text can
// be highlighted and copied like anywhere else on iOS), and independent
// horizontally scrolling monospace views (each with its own Copy button)
// for fenced code, since code just doesn't read right line-wrapped.
@interface ISHLLMChatMessageCell : UITableViewCell
// Called when the "Thinking" disclosure is tapped; the controller flips its
// stored expansion state for the message and reloads the row.
@property (nonatomic, copy, nullable) void (^thinkingToggleHandler)(void);
// The chat's text scale (0 reads as 1.0). The fonts arrive already scaled;
// this only moves the fixed minimum a thought's text is kept above, which
// would otherwise stop it shrinking with the rest of the bubble.
@property (nonatomic) CGFloat textScale;
// Must be called BEFORE -configureWithBlocks: -- the thought disclosure is
// appended to the same stack, so ordering it first is what puts it above the
// answer (and what stops -configureWithBlocks: adding its empty-bubble
// placeholder to a message that is nothing but a thought so far).
- (void)configureThinkingWithText:(nullable NSString *)text
                       inProgress:(BOOL)inProgress
                         expanded:(BOOL)expanded
                         baseFont:(UIFont *)baseFont
                            color:(UIColor *)color
                          bgColor:(UIColor *)bgColor;
- (void)configureWithBlocks:(NSArray<ISHMarkdownBlock *> *)blocks
                 isAssistant:(BOOL)isAssistant
                     caption:(nullable NSString *)caption
                    baseFont:(UIFont *)baseFont
                    codeFont:(UIFont *)codeFont
                   textColor:(UIColor *)textColor
              secondaryColor:(UIColor *)secondaryColor
                 bubbleColor:(UIColor *)bubbleColor
             codeBubbleColor:(UIColor *)codeBubbleColor;
@end

@implementation ISHLLMChatMessageCell {
    UIView *_bubbleView;
    UIStackView *_blocksStack;
    NSLayoutConstraint *_bubbleLeading;
    NSLayoutConstraint *_bubbleTrailing;
}

- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)reuseIdentifier {
    self = [super initWithStyle:style reuseIdentifier:reuseIdentifier];
    if (self) {
        self.selectionStyle = UITableViewCellSelectionStyleNone;
        self.backgroundColor = UIColor.clearColor;

        _bubbleView = [UIView new];
        _bubbleView.translatesAutoresizingMaskIntoConstraints = NO;
        _bubbleView.layer.cornerRadius = 14.0;
        _bubbleView.layer.masksToBounds = YES;
        _bubbleView.userInteractionEnabled = YES;
        [self.contentView addSubview:_bubbleView];

        _blocksStack = [UIStackView new];
        _blocksStack.translatesAutoresizingMaskIntoConstraints = NO;
        _blocksStack.axis = UILayoutConstraintAxisVertical;
        _blocksStack.alignment = UIStackViewAlignmentFill;
        _blocksStack.spacing = 6.0;
        [_bubbleView addSubview:_blocksStack];

        [NSLayoutConstraint activateConstraints:@[
            [_blocksStack.topAnchor constraintEqualToAnchor:_bubbleView.topAnchor constant:8.0],
            [_blocksStack.bottomAnchor constraintEqualToAnchor:_bubbleView.bottomAnchor constant:-8.0],
            [_blocksStack.leadingAnchor constraintEqualToAnchor:_bubbleView.leadingAnchor constant:10.0],
            [_blocksStack.trailingAnchor constraintEqualToAnchor:_bubbleView.trailingAnchor constant:-10.0],

            [_bubbleView.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:3.0],
            [_bubbleView.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor constant:-3.0],
        ]];
        // Sub-required priority: UIKit briefly forces contentView.width to 0
        // while probing a self-sizing cell's height (the internal
        // 'fittingSizeHTarget' constraint), which at required priority
        // conflicts with the stack view's own edge constraints to its
        // arranged text view. Letting THIS constraint yield during that probe
        // (rather than UIKit picking amongst the stack view's auto-generated
        // ones) silences the console warning; the real, rendered layout
        // (non-zero width) satisfies it exactly as before.
        NSLayoutConstraint *bubbleWidthCap =
            [_bubbleView.widthAnchor constraintLessThanOrEqualToAnchor:self.contentView.widthAnchor multiplier:0.86];
        bubbleWidthCap.priority = UILayoutPriorityRequired - 1;
        bubbleWidthCap.active = YES;
        _bubbleLeading = [_bubbleView.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:12.0];
        _bubbleTrailing = [_bubbleView.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-12.0];
    }
    return self;
}

- (void)prepareForReuse {
    [super prepareForReuse];
    self.thinkingToggleHandler = nil;
    for (UIView *view in _blocksStack.arrangedSubviews) {
        [_blocksStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

// The collapsed/expanded chain-of-thought disclosure. Collapsed it is a single
// quiet line ("Thinking…" with a spinner while the model is still inside the
// block, "Thinking" once it has closed); expanded it also shows the thought
// text in a selectable view with its own Copy button, the same treatment
// fenced code gets.
- (void)configureThinkingWithText:(NSString *)text
                       inProgress:(BOOL)inProgress
                         expanded:(BOOL)expanded
                         baseFont:(UIFont *)baseFont
                            color:(UIColor *)color
                          bgColor:(UIColor *)bgColor {
    if (text.length == 0 && !inProgress)
        return;
    BOOL canExpand = text.length > 0;
    expanded = expanded && canExpand;

    CGFloat scale = self.textScale > 0 ? self.textScale : 1.0;
    CGFloat minimumSize = round(11.0 * scale * 2.0) / 2.0;
    UIFont *labelFont = [baseFont fontWithSize:MAX(minimumSize, baseFont.pointSize - 2.0)];
    UIButton *toggle = [UIButton buttonWithType:UIButtonTypeSystem];
    toggle.translatesAutoresizingMaskIntoConstraints = NO;
    NSString *disclosure = canExpand ? (expanded ? @"▾ " : @"▸ ") : @"";
    [toggle setTitle:[disclosure stringByAppendingString:(inProgress ? @"Thinking…" : @"Thinking")] forState:UIControlStateNormal];
    [toggle setTitleColor:color forState:UIControlStateNormal];
    toggle.titleLabel.font = ISHMarkdownFontWithTraits(labelFont, UIFontDescriptorTraitItalic);
    toggle.enabled = canExpand;
    toggle.accessibilityLabel = expanded ? @"Hide the model's thinking" : @"Show the model's thinking";
    [toggle addTarget:self action:@selector(thinkingToggleTapped:) forControlEvents:UIControlEventTouchUpInside];
    [toggle setContentHuggingPriority:UILayoutPriorityDefaultHigh forAxis:UILayoutConstraintAxisHorizontal];

    UIStackView *header = [UIStackView new];
    header.axis = UILayoutConstraintAxisHorizontal;
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 6.0;
    [header addArrangedSubview:toggle];
    if (inProgress) {
        UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        spinner.transform = CGAffineTransformMakeScale(0.7, 0.7);
        [spinner startAnimating];
        [header addArrangedSubview:spinner];
    }
    // Absorbs the leftover width so the toggle's tap target hugs its text
    // instead of spanning the bubble.
    [header addArrangedSubview:[UIView new]];
    [_blocksStack addArrangedSubview:header];

    if (!expanded)
        return;

    UIView *container = [UIView new];
    container.translatesAutoresizingMaskIntoConstraints = NO;
    container.backgroundColor = bgColor;
    container.layer.cornerRadius = 8.0;
    container.layer.masksToBounds = YES;

    NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new];
    paragraph.paragraphSpacing = 4.0;
    UITextView *thoughtView = [self selectableTextViewWithAttributedText:
        [[NSAttributedString alloc] initWithString:text attributes:@{
            NSFontAttributeName: labelFont,
            NSForegroundColorAttributeName: color,
            NSParagraphStyleAttributeName: paragraph,
        }]];
    [container addSubview:thoughtView];

    ISHLLMCopyButton *copyButton = [self copyButtonWithPayload:text];
    [container addSubview:copyButton];

    CGFloat headerHeight = 30.0;
    [NSLayoutConstraint activateConstraints:@[
        [copyButton.centerYAnchor constraintEqualToAnchor:container.topAnchor constant:headerHeight / 2.0],
        [copyButton.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-6.0],

        [thoughtView.topAnchor constraintEqualToAnchor:container.topAnchor constant:headerHeight],
        [thoughtView.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-8.0],
        [thoughtView.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:10.0],
        [thoughtView.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-10.0],
    ]];
    [_blocksStack addArrangedSubview:container];
}

- (void)thinkingToggleTapped:(id)sender {
    (void) sender;
    if (self.thinkingToggleHandler != nil)
        self.thinkingToggleHandler();
}

- (void)configureWithBlocks:(NSArray<ISHMarkdownBlock *> *)blocks
                 isAssistant:(BOOL)isAssistant
                     caption:(NSString *)caption
                    baseFont:(UIFont *)baseFont
                    codeFont:(UIFont *)codeFont
                   textColor:(UIColor *)textColor
              secondaryColor:(UIColor *)secondaryColor
                 bubbleColor:(UIColor *)bubbleColor
             codeBubbleColor:(UIColor *)codeBubbleColor {
    _bubbleView.backgroundColor = bubbleColor;
    // Assistant bubbles hug the leading edge, user bubbles the trailing edge
    // (only one of the two width-defining edge constraints is active at a
    // time; the other bubble edge is free, so the bubble's own width
    // constraint plus its content determine where it ends).
    _bubbleLeading.active = isAssistant;
    _bubbleTrailing.active = !isAssistant;

    for (ISHMarkdownBlock *block in blocks) {
        if (block.kind == ISHMarkdownBlockKindCode) {
            [_blocksStack addArrangedSubview:[self codeViewForBlock:block font:codeFont textColor:textColor bgColor:codeBubbleColor]];
        } else if (block.attributedText.length > 0) {
            [_blocksStack addArrangedSubview:[self selectableTextViewWithAttributedText:block.attributedText]];
        }
    }
    if (caption.length > 0) {
        UILabel *captionLabel = [UILabel new];
        captionLabel.numberOfLines = 0;
        captionLabel.font = ISHMarkdownFontWithTraits([baseFont fontWithSize:baseFont.pointSize - 1.0], UIFontDescriptorTraitItalic);
        captionLabel.textColor = secondaryColor;
        captionLabel.text = caption;
        [_blocksStack addArrangedSubview:captionLabel];
    }
    if (_blocksStack.arrangedSubviews.count == 0) {
        // Should not normally happen (callers skip empty messages), but an
        // empty bubble with no intrinsic height would collapse to nothing.
        UILabel *label = [UILabel new];
        label.numberOfLines = 0;
        label.font = baseFont;
        label.textColor = secondaryColor;
        label.text = @" ";
        [_blocksStack addArrangedSubview:label];
    }
}

// Non-editable, non-scrolling UITextView standing in for a UILabel -- looks
// identical (clear background, no insets, sizes to its content within the
// stack) but natively supports long-press-to-select and Copy, which UILabel
// never does.
- (UITextView *)selectableTextViewWithAttributedText:(NSAttributedString *)attributedText {
    UITextView *textView = [UITextView new];
    textView.translatesAutoresizingMaskIntoConstraints = NO;
    textView.editable = NO;
    textView.scrollEnabled = NO;
    textView.selectable = YES;
    textView.backgroundColor = UIColor.clearColor;
    textView.textContainerInset = UIEdgeInsetsZero;
    textView.textContainer.lineFragmentPadding = 0.0;
    textView.attributedText = attributedText;
    return textView;
}

// A non-wrapping, horizontally scrolling monospace view for one fenced code
// block, with a small Copy button pinned to its top-right corner. The
// scroll view's height is tied to its content (the label's natural height),
// so only the horizontal axis ever scrolls -- the standard recipe for a
// self-sizing horizontal scroll view inside a self-sizing table cell.
- (UIView *)codeViewForBlock:(ISHMarkdownBlock *)block font:(UIFont *)font textColor:(UIColor *)textColor bgColor:(UIColor *)bgColor {
    UIView *container = [UIView new];
    container.translatesAutoresizingMaskIntoConstraints = NO;
    container.backgroundColor = bgColor;
    container.layer.cornerRadius = 8.0;
    container.layer.masksToBounds = YES;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.showsHorizontalScrollIndicator = YES;
    scroll.alwaysBounceHorizontal = YES;
    [container addSubview:scroll];

    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = font;
    label.textColor = textColor;
    label.numberOfLines = 0;
    label.text = block.code;
    [scroll addSubview:label];

    ISHLLMCopyButton *copyButton = [self copyButtonWithPayload:block.code ?: @""];
    [container addSubview:copyButton];

    // Always reserve a header band for the Copy button, whether or not there's
    // a language tag -- code with no language tag is the common case, and
    // skimping on this space here previously let the button overlap (and
    // visually vanish behind) the first line of scrolling code.
    CGFloat headerHeight = 30.0;
    NSMutableArray<NSLayoutConstraint *> *constraints = [NSMutableArray arrayWithArray:@[
        [scroll.topAnchor constraintEqualToAnchor:container.topAnchor constant:headerHeight],
        [scroll.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-8.0],
        [scroll.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
        [scroll.heightAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.heightAnchor],

        [label.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [label.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [label.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:10.0],
        [label.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-10.0],
        [label.heightAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.heightAnchor],

        [copyButton.centerYAnchor constraintEqualToAnchor:container.topAnchor constant:headerHeight / 2.0],
        [copyButton.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-6.0],
    ]];
    if (block.language.length > 0) {
        UILabel *languageLabel = [UILabel new];
        languageLabel.translatesAutoresizingMaskIntoConstraints = NO;
        languageLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
        languageLabel.textColor = textColor;
        languageLabel.alpha = 0.6;
        languageLabel.text = block.language;
        [container addSubview:languageLabel];
        [constraints addObjectsFromArray:@[
            [languageLabel.centerYAnchor constraintEqualToAnchor:container.topAnchor constant:headerHeight / 2.0],
            [languageLabel.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:10.0],
        ]];
    }
    [NSLayoutConstraint activateConstraints:constraints];
    return container;
}

// The small Copy chip pinned to the top-right of a code block or an expanded
// thought -- one recipe so the two always look and behave alike.
- (ISHLLMCopyButton *)copyButtonWithPayload:(NSString *)payload {
    ISHLLMCopyButton *copyButton = [ISHLLMCopyButton buttonWithType:UIButtonTypeSystem];
    copyButton.translatesAutoresizingMaskIntoConstraints = NO;
    copyButton.payload = payload;
    [copyButton setTitle:@"Copy" forState:UIControlStateNormal];
    copyButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
    copyButton.backgroundColor = [UIColor colorWithWhite:0.5 alpha:0.18];
    copyButton.layer.cornerRadius = 5.0;
    copyButton.layer.masksToBounds = YES;
    // Deliberately the pre-UIButtonConfiguration API: -codeCopyButtonTapped:
    // swaps the title to "Copied" with -setTitle:forState:, which a configured
    // button ignores.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    copyButton.contentEdgeInsets = UIEdgeInsetsMake(2.0, 6.0, 2.0, 6.0);
#pragma clang diagnostic pop
    [copyButton addTarget:self action:@selector(codeCopyButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
    return copyButton;
}

- (void)codeCopyButtonTapped:(ISHLLMCopyButton *)sender {
    if (sender.payload.length == 0)
        return;
    UIPasteboard.generalPasteboard.string = sender.payload;
    NSString *original = [sender titleForState:UIControlStateNormal];
    [sender setTitle:@"Copied" forState:UIControlStateNormal];
    sender.enabled = NO;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [sender setTitle:original forState:UIControlStateNormal];
        sender.enabled = YES;
    });
}

@end

// At text scale 1.0. The cap is a number of lines, so it scales with the font.
static const CGFloat kISHLLMPromptFieldMaxHeight = 120.0;
static const CGFloat kISHLLMTranscriptEstimatedRowHeight = 60.0;
// The Workspace's text-scale range (WorkspaceTextScalable).
static const CGFloat kISHLLMMinimumTextScale = 0.5;
static const CGFloat kISHLLMMaximumTextScale = 3.0;

// UITextView that remembers the modifier flags of the most recent hardware key
// press. UIKit reports plain Return and Shift+Return identically through
// insertText/shouldChangeTextInRange, but the UIPress that precedes the
// insertion carries modifierFlags -- so the delegate can tell "Enter = send"
// apart from "Shift+Enter = newline". Software-keyboard Return never goes
// through pressesBegan, leaving the flags at 0 (i.e. it sends).
@interface LLMPromptTextView : UITextView
@property (nonatomic, readonly) BOOL shiftKeyDown;
@end

@implementation LLMPromptTextView {
    UIKeyModifierFlags _activeModifierFlags;
}

- (BOOL)shiftKeyDown {
    return (_activeModifierFlags & UIKeyModifierShift) != 0;
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    for (UIPress *press in presses) {
        if (press.key != nil)
            _activeModifierFlags = press.key.modifierFlags;
    }
    [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    _activeModifierFlags = 0;
    [super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    _activeModifierFlags = 0;
    [super pressesCancelled:presses withEvent:event];
}

@end

@interface LLMClientViewController () <ISHLLMAgentObserver>
@end

@implementation LLMClientViewController {
    UIStackView *_toolbarStackView;
    UITableView *_transcriptTable;
    NSArray<NSNumber *> *_visibleMessageIndices; // indices into the agent's messages, skipping role=="tool"
    UILabel *_emptyStateLabel; // shown over the table when there's nothing to display yet
    LLMPromptTextView *_promptField;
    UILabel *_promptPlaceholderLabel; // UITextView has no built-in placeholder
    UIButton *_sendButton;
    ISHLLMAgent *_agent; // the chat on screen; it keeps working when another is shown
    BOOL _viewing;       // on screen, so its finished replies count as seen
    BOOL _transcriptWasAtBottom; // sampled before each layout pass
    CGFloat _transcriptLaidOutHeight;

    // Status panel.
    UILabel *_statusLabel;
    UILabel *_elapsedLabel;
    UILabel *_detailLabel;
    UIProgressView *_contextBar;
    UILabel *_infoLabel;
    UIButton *_agentsButton;
    UIActivityIndicatorView *_activityIndicator;
    NSTimer *_statusTimer;

    // The approval this screen has up.
    ISHLLMAgentApproval *_presentedApproval;
    UIAlertController *_approvalAlert;

    NSMutableSet<NSNumber *> *_expandedThinkingIndices; // indices into the messages whose <think> block the user expanded
    UIButton *_chatsButton; // titled with the session, so the visible chat is always named
    UIButton *_destinationButton; // titled with the destination, one tap to switch
    UIBarButtonItem *_chatsBarButtonItem;
    UIBarButtonItem *_destinationBarButtonItem;
    NSLayoutConstraint *_toolbarHeightConstraint; // collapsed to 0 when the navigation bar already carries these controls
    CGFloat _workspaceTextScale; // 0 until set, which reads as 1.0
    NSLayoutConstraint *_promptFieldMaxHeightConstraint; // follows the text scale
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"LLM Chat";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _expandedThinkingIndices = [NSMutableSet set];

    _transcriptTable = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _transcriptTable.translatesAutoresizingMaskIntoConstraints = NO;
    _transcriptTable.dataSource = self;
    _transcriptTable.delegate = self;
    _transcriptTable.separatorStyle = UITableViewCellSeparatorStyleNone;
    _transcriptTable.rowHeight = UITableViewAutomaticDimension;
    _transcriptTable.estimatedRowHeight = kISHLLMTranscriptEstimatedRowHeight * self.workspaceTextScale;
    _transcriptTable.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [_transcriptTable registerClass:ISHLLMChatMessageCell.class forCellReuseIdentifier:@"message"];
    if (@available(iOS 13.0, *))
        _transcriptTable.backgroundColor = UIColor.systemBackgroundColor;
    [self.view addSubview:_transcriptTable];

    _emptyStateLabel = [UILabel new];
    _emptyStateLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _emptyStateLabel.numberOfLines = 0;
    _emptyStateLabel.textAlignment = NSTextAlignmentCenter;
    _emptyStateLabel.font = [self scaledBodyFont];
    _emptyStateLabel.hidden = YES;
    if (@available(iOS 13.0, *))
        _emptyStateLabel.textColor = UIColor.secondaryLabelColor;
    [self.view addSubview:_emptyStateLabel];

    UIView *inputBar = [UIView new];
    inputBar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:inputBar];

    // Multi-line, word-wrapping, auto-growing composer (capped, then scrolls
    // internally) -- Return/Shift+Return insert a newline like any text
    // view; only the Send button submits.
    _promptField = [LLMPromptTextView new];
    _promptField.translatesAutoresizingMaskIntoConstraints = NO;
    _promptField.font = [self scaledBodyFont];
    _promptField.textContainerInset = UIEdgeInsetsMake(8.0, 6.0, 8.0, 6.0);
    _promptField.scrollEnabled = NO; // NO lets intrinsicContentSize drive auto-grow below the max-height cap
    _promptField.layer.cornerRadius = 8.0;
    _promptField.autocorrectionType = UITextAutocorrectionTypeDefault;
    _promptField.delegate = self;
    _promptField.accessibilityLabel = @"Prompt input";
    if (@available(iOS 13.0, *)) {
        _promptField.backgroundColor = UIColor.tertiarySystemFillColor;
    } else {
        _promptField.backgroundColor = [UIColor colorWithWhite:0.93 alpha:1.0];
    }
    [inputBar addSubview:_promptField];

    _promptPlaceholderLabel = [UILabel new];
    _promptPlaceholderLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _promptPlaceholderLabel.text = @"Ask the configured model";
    _promptPlaceholderLabel.font = _promptField.font;
    _promptPlaceholderLabel.isAccessibilityElement = NO;
    _promptPlaceholderLabel.userInteractionEnabled = NO;
    if (@available(iOS 13.0, *)) {
        _promptPlaceholderLabel.textColor = UIColor.placeholderTextColor;
    } else {
        _promptPlaceholderLabel.textColor = [UIColor colorWithWhite:0.0 alpha:0.3];
    }
    [_promptField addSubview:_promptPlaceholderLabel];

    _sendButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _sendButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_sendButton setTitle:@"Send" forState:UIControlStateNormal];
    [_sendButton addTarget:self action:@selector(sendPrompt:) forControlEvents:UIControlEventTouchUpInside];
    [inputBar addSubview:_sendButton];

    _toolbarStackView = [UIStackView new];
    _toolbarStackView.translatesAutoresizingMaskIntoConstraints = NO;
    _toolbarStackView.axis = UILayoutConstraintAxisHorizontal;
    _toolbarStackView.alignment = UIStackViewAlignmentCenter;
    _toolbarStackView.distribution = UIStackViewDistributionFillEqually;
    _toolbarStackView.spacing = 6.0;
    [self.view addSubview:_toolbarStackView];
    // Four buttons, same as before, but the first two are now the chat and the
    // destination -- both name what they currently point at and both switch it
    // in one tap, which is the whole point of the row. Saving extracts moved
    // under Actions and clearing under Chats to keep the count at four; a
    // fifth crushes the labels on a narrow iPhone.
    _chatsButton = [self toolbarButtonWithTitle:@"Chats" action:NULL];
    _destinationButton = [self toolbarButtonWithTitle:@"Model" action:NULL];
    [self toolbarButtonWithTitle:@"Actions" action:@selector(showPromptActions:)];
    [self toolbarButtonWithTitle:@"Settings" action:@selector(showLLMSettings:)];

    _chatsBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Chats" style:UIBarButtonItemStylePlain target:nil action:NULL];
    _destinationBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Model" style:UIBarButtonItemStylePlain target:nil action:NULL];
    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithTitle:@"Settings" style:UIBarButtonItemStylePlain target:self action:@selector(showLLMSettings:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Actions" style:UIBarButtonItemStylePlain target:self action:@selector(showPromptActions:)],
        _destinationBarButtonItem,
        _chatsBarButtonItem,
    ];

    // Status panel: what the agent is doing, for how long, against what,
    // how full its context is, and whether other chats need attention -- so
    // the state is always visible and a stall is obvious.
    UIView *statusRow = [self buildStatusPanel];
    [self.view addSubview:statusRow];

    _toolbarHeightConstraint = [_toolbarStackView.heightAnchor constraintGreaterThanOrEqualToConstant:32.0];
    _promptFieldMaxHeightConstraint = [_promptField.heightAnchor constraintLessThanOrEqualToConstant:[self promptFieldMaxHeight]];

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarStackView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:6.0],
        [_toolbarStackView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [_toolbarStackView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        _toolbarHeightConstraint,

        [_transcriptTable.topAnchor constraintEqualToAnchor:_toolbarStackView.bottomAnchor constant:4.0],
        [_transcriptTable.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [_transcriptTable.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [_transcriptTable.bottomAnchor constraintEqualToAnchor:statusRow.topAnchor constant:-2.0],

        [_emptyStateLabel.centerXAnchor constraintEqualToAnchor:_transcriptTable.centerXAnchor],
        [_emptyStateLabel.centerYAnchor constraintEqualToAnchor:_transcriptTable.centerYAnchor],
        [_emptyStateLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:safeArea.leadingAnchor constant:24.0],
        [_emptyStateLabel.trailingAnchor constraintLessThanOrEqualToAnchor:safeArea.trailingAnchor constant:-24.0],

        [statusRow.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:12.0],
        [statusRow.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-12.0],
        [statusRow.bottomAnchor constraintEqualToAnchor:inputBar.topAnchor constant:-2.0],

        [inputBar.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [inputBar.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        // The keyboard's top, not the safe area's bottom: on an iPhone the
        // software keyboard otherwise covers the composer it was raised for.
        // With no keyboard the guide's top is the safe area's bottom.
        [inputBar.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor constant:-8.0],
        [inputBar.heightAnchor constraintGreaterThanOrEqualToConstant:44.0],

        [_promptField.leadingAnchor constraintEqualToAnchor:inputBar.leadingAnchor],
        [_promptField.topAnchor constraintEqualToAnchor:inputBar.topAnchor constant:4.0],
        [_promptField.bottomAnchor constraintEqualToAnchor:inputBar.bottomAnchor constant:-4.0],
        [_promptField.heightAnchor constraintGreaterThanOrEqualToConstant:36.0],
        _promptFieldMaxHeightConstraint,
        [_sendButton.leadingAnchor constraintEqualToAnchor:_promptField.trailingAnchor constant:8.0],
        [_sendButton.trailingAnchor constraintEqualToAnchor:inputBar.trailingAnchor],
        [_sendButton.centerYAnchor constraintEqualToAnchor:_promptField.centerYAnchor],
        [_sendButton.widthAnchor constraintEqualToConstant:56.0],

        [_promptPlaceholderLabel.topAnchor constraintEqualToAnchor:_promptField.topAnchor constant:8.0],
        [_promptPlaceholderLabel.leadingAnchor constraintEqualToAnchor:_promptField.leadingAnchor constant:10.0],
        [_promptPlaceholderLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_promptField.trailingAnchor constant:-10.0],
    ]];

    // Opens the chat that was last selected; on the first run in this build
    // that is the migrated pre-sessions transcript (see
    // ISHLLMLoadSessionIndexDocument).
    ISHLLMAgent *agent = [ISHLLMAgentManager.shared agentForSessionID:ISHLLMActiveSessionID()];
    if (agent == nil)
        agent = [ISHLLMAgentManager.shared agentForSessionID:ISHLLMStringValue(ISHLLMCreateSession(nil), @"id")];
    [self attachAgent:agent];
    if (self.initialPrompt.length > 0) {
        [self.view layoutIfNeeded]; // give _promptField a real width before sizing it to this initial text
        [self setPromptFieldText:self.initialPrompt];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_statusTimer invalidate];
    if (_viewing)
        [_agent endViewing];
    [_agent removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self updateToolbarVisibility];
    if (!_viewing) {
        _viewing = YES;
        [_agent beginViewing];
    }
    [_agent reloadIfChangedOnDisk];
    [self refreshTranscript];
    [self updateChatHeaderTitles]; // Settings can have changed the destination
    [self updateStatusPanel];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self presentPendingApprovalIfNeeded];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    if (_viewing) {
        _viewing = NO;
        [_agent endViewing];
    }
    // Another window can put the question up now.
    if (_presentedApproval.presenter == self)
        _presentedApproval.presenter = nil;
}

#pragma mark - The agent on screen

// The chat on screen is a view of its agent; switching attaches another one
// and leaves this one working.
- (void)attachAgent:(ISHLLMAgent *)agent {
    if (agent == nil || agent == _agent)
        return;
    if (_agent != nil) {
        [_agent removeObserver:self];
        if (_viewing)
            [_agent endViewing];
    }
    [self dismissPresentedApprovalAnimated:NO];
    _agent = agent;
    [_agent addObserver:self];
    if (_viewing)
        [_agent beginViewing];
    [_expandedThinkingIndices removeAllObjects]; // indices into the old chat's messages
    // The chat's destination becomes the one Settings edits.
    NSDictionary<NSString *, NSString *> *destination = [_agent destination];
    if (![ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID)])
        ISHLLMActivateDestination(destination);
    [self refreshTranscript];
    [self updateChatHeaderTitles];
    [self updateStatusPanel];
    [self presentPendingApprovalIfNeeded];
}

- (NSArray<NSDictionary<NSString *, id> *> *)messages {
    return _agent.messages ?: @[];
}

- (void)agentMessagesDidChange:(ISHLLMAgent *)agent {
    (void) agent;
    [self refreshTranscript];
    [self updateStatusPanel];
}

- (void)agent:(ISHLLMAgent *)agent didUpdateStreamingMessageAtIndex:(NSUInteger)index {
    (void) agent;
    (void) index;
    [self reloadLastRowAndScroll:YES];
}

- (void)agentStatusDidChange:(ISHLLMAgent *)agent {
    (void) agent;
    [self updateStatusPanel];
    [self presentPendingApprovalIfNeeded];
}

- (void)agentMetadataDidChange:(ISHLLMAgent *)agent {
    (void) agent;
    [self updateChatHeaderTitles];
    [self refreshTranscript];
    [self updateStatusPanel];
}

- (void)agentsStateDidChange:(NSNotification *)notification {
    (void) notification;
    [self updateStatusPanel];
    [self presentPendingApprovalIfNeeded];
}

- (BOOL)isBusy {
    return _agent.busy;
}

- (void)switchToSessionWithID:(NSString *)sessionID {
    if ([sessionID isEqualToString:_agent.sessionID])
        return;
    ISHLLMAgent *agent = [ISHLLMAgentManager.shared agentForSessionID:sessionID];
    if (agent == nil)
        return;
    // Sub-agents' chats are opened, but never become the chat a new window
    // starts on.
    if (agent.parentSessionID.length == 0)
        ISHLLMSetActiveSessionID(sessionID);
    [self attachAgent:agent];
}

- (void)startNewChat {
    NSDictionary<NSString *, id> *entry = ISHLLMCreateSession(nil);
    [self attachAgent:[ISHLLMAgentManager.shared agentForSessionID:ISHLLMStringValue(entry, @"id")]];
}

- (void)renameCurrentChat {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename Chat" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = self->_agent.title;
        textField.placeholder = @"Chat name";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        // Clearing the name hands the chat back to automatic titling.
        [self->_agent setCustomTitle:alert.textFields.firstObject.text ?: @""];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

// A per-chat system message: the persona/standing instructions for this
// conversation only, prepended to what every backend is sent.
- (void)editSystemPromptForCurrentChat {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"System Prompt"
                                                                  message:@"Standing instructions sent with every message in this chat. Leave empty for none."
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = self->_agent.systemPrompt;
        textField.placeholder = @"You are a concise assistant…";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeSentences;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self->_agent setSystemPrompt:alert.textFields.firstObject.text ?: @""];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)deleteCurrentChat {
    ISHLLMAgent *agent = _agent;
    NSString *title = agent.title.length > 0 ? agent.title : @"New Chat";
    NSString *message = agent.busy
        ? [NSString stringWithFormat:@"“%@” is still working. Stop it and delete the chat and its saved messages? This can't be undone.", title]
        : [NSString stringWithFormat:@"Delete “%@” and its saved messages? This can't be undone.", title];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Delete Chat" message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        NSString *nextSessionID = ISHLLMDeleteSession(agent.sessionID);
        [ISHLLMAgentManager.shared forgetSessionID:agent.sessionID];
        if (agent == self->_agent)
            [self attachAgent:[ISHLLMAgentManager.shared agentForSessionID:nextSessionID]];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)switchToDestinationWithID:(NSString *)destinationID {
    for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
        if (![ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:destinationID])
            continue;
        ISHLLMActivateDestination(destination);
        // A reply in flight keeps the destination it started with; the chat
        // uses this one from its next message.
        [_agent useDestinationWithID:destinationID];
        [self updateChatHeaderTitles];
        [self refreshTranscript];
        [self updateStatusPanel];
        return;
    }
}

// A clean slate. A reply still coming in is stopped first, and the chat
// cleared once it has landed, so nothing appends into the emptied chat.
- (void)clearTranscript:(id)sender {
    (void) sender;
    ISHLLMAgent *agent = _agent;
    if (!agent.busy) {
        [agent clearMessages];
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Still working"
                                                                  message:@"This chat is still working. Stop it and clear the chat?"
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Stop and Clear" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        [agent stopThen:^{
            [agent clearMessages];
        }];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)queryModelsInTranscript {
    [_agent queryModels];
}

- (void)compactConversation {
    [_agent compact];
}

#pragma mark - Sending

- (void)sendPrompt:(id)sender {
    (void) sender;
    NSString *prompt = [_promptField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (prompt.length == 0)
        return;
    if ([prompt isEqualToString:@"/changes"]) {
        [self setPromptFieldText:@""];
        [self showChanges];
        return;
    }
    if ([prompt isEqualToString:@"/undo"]) {
        [self setPromptFieldText:@""];
        [self undoLastChange];
        return;
    }
    if ([prompt isEqualToString:@"/mcp"]) {
        [self setPromptFieldText:@""];
        [self showMCPServers];
        return;
    }
    if ([prompt isEqualToString:@"/agents"]) {
        [self setPromptFieldText:@""];
        [self showAgentList];
        return;
    }
    if ([prompt isEqualToString:@"/chats"]) {
        [self setPromptFieldText:@""];
        [self showChatList];
        return;
    }
    if ([prompt isEqualToString:@"/new"]) {
        [self setPromptFieldText:@""];
        [self startNewChat];
        return;
    }
    // While a reply is coming in, the agent queues it for when it ends.
    [self setPromptFieldText:@""];
    [_agent sendPrompt:prompt];
    [self updateSendButton];
}

// Stop while the agent works and nothing is typed; Send otherwise -- a
// prompt written meanwhile is queued, a /command runs at once.
- (void)updateSendButton {
    BOOL stop = _agent.busy && _promptField.text.length == 0;
    _sendButton.enabled = YES;
    [_sendButton setTitle:(stop ? @"Stop" : @"Send") forState:UIControlStateNormal];
    _sendButton.accessibilityLabel = stop ? @"Stop generating" : @"Send";
    [_sendButton removeTarget:self action:NULL forControlEvents:UIControlEventTouchUpInside];
    [_sendButton addTarget:self action:(stop ? @selector(stopGenerating:) : @selector(sendPrompt:)) forControlEvents:UIControlEventTouchUpInside];
}

- (void)stopGenerating:(id)sender {
    (void) sender;
    [_agent stop];
}

#pragma mark - Status panel

// Built once in viewDidLoad: what the agent is doing and for how long, what
// it is talking to and how full the context is, and the other agents.
- (UIView *)buildStatusPanel {
    UIColor *secondary = UIColor.secondaryLabelColor;
    _activityIndicator = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _activityIndicator.hidesWhenStopped = YES;
    _statusLabel = [UILabel new];
    _statusLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _statusLabel.numberOfLines = 1;
    _statusLabel.adjustsFontSizeToFitWidth = YES;
    _statusLabel.minimumScaleFactor = 0.8;
    [_statusLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    _elapsedLabel = [UILabel new];
    _elapsedLabel.font = [UIFont monospacedDigitSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote].pointSize weight:UIFontWeightRegular];
    _elapsedLabel.textColor = secondary;
    _elapsedLabel.textAlignment = NSTextAlignmentRight;
    [_elapsedLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_elapsedLabel setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *phaseRow = [[UIStackView alloc] initWithArrangedSubviews:@[_activityIndicator, _statusLabel, _elapsedLabel]];
    phaseRow.axis = UILayoutConstraintAxisHorizontal;
    phaseRow.alignment = UIStackViewAlignmentCenter;
    phaseRow.spacing = 6.0;

    _detailLabel = [UILabel new];
    _detailLabel.font = ISHLLMMonospaceFont([UIFont preferredFontForTextStyle:UIFontTextStyleCaption1].pointSize);
    _detailLabel.textColor = secondary;
    _detailLabel.numberOfLines = 1;
    _detailLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;

    _contextBar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    [_contextBar.widthAnchor constraintEqualToConstant:44.0].active = YES;
    _infoLabel = [UILabel new];
    _infoLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    _infoLabel.textColor = secondary;
    _infoLabel.numberOfLines = 1;
    _infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    UIStackView *infoRow = [[UIStackView alloc] initWithArrangedSubviews:@[_contextBar, _infoLabel]];
    infoRow.axis = UILayoutConstraintAxisHorizontal;
    infoRow.alignment = UIStackViewAlignmentCenter;
    infoRow.spacing = 6.0;

    _agentsButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _agentsButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    _agentsButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [_agentsButton addTarget:self action:@selector(agentsButtonTapped:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *panel = [[UIStackView alloc] initWithArrangedSubviews:@[phaseRow, _detailLabel, infoRow, _agentsButton]];
    panel.translatesAutoresizingMaskIntoConstraints = NO;
    panel.axis = UILayoutConstraintAxisVertical;
    panel.alignment = UIStackViewAlignmentFill;
    panel.spacing = 1.0;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(agentsStateDidChange:) name:ISHLLMAgentStateDidChangeNotification object:nil];
    return panel;
}

// "just now", "40s ago", "12 min ago", then the time of day.
static NSString *ISHLLMAgoText(NSDate *when) {
    NSTimeInterval ago = -when.timeIntervalSinceNow;
    if (ago < 10)
        return @"just now";
    if (ago < 60)
        return [NSString stringWithFormat:@"%lds ago", (long) ago];
    if (ago < 3600)
        return [NSString stringWithFormat:@"%ld min ago", (long) (ago / 60)];
    return [NSDateFormatter localizedStringFromDate:when dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterShortStyle];
}

static NSString *ISHLLMElapsedText(NSDate *since) {
    if (since == nil)
        return @"";
    NSInteger seconds = MAX((NSInteger) 0, (NSInteger) -since.timeIntervalSinceNow);
    if (seconds >= 3600)
        return [NSString stringWithFormat:@"%ld:%02ld:%02ld", (long) (seconds / 3600), (long) (seconds / 60 % 60), (long) (seconds % 60)];
    return [NSString stringWithFormat:@"%ld:%02ld", (long) (seconds / 60), (long) (seconds % 60)];
}

- (void)updateStatusPanel {
    if (_statusLabel == nil || _agent == nil)
        return;
    ISHLLMAgent *agent = _agent;
    BOOL busy = agent.busy;

    _promptField.editable = YES; // the next prompt can be written while this one runs
    [self updateSendButton];

    // Line 1: the phase, the round and tool count, and how long.
    NSMutableString *phase = [[agent statusLine] mutableCopy];
    if (busy) {
        if (agent.round > 0)
            [phase appendFormat:@" · round %ld", (long) agent.round + 1];
        if (agent.toolCallsThisReply > 0)
            [phase appendFormat:@" · %ld tool call%@", (long) agent.toolCallsThisReply, agent.toolCallsThisReply == 1 ? @"" : @"s"];
        if (agent.queuedPrompts.count > 0)
            [phase appendFormat:@" · %lu queued", (unsigned long) agent.queuedPrompts.count];
        [_activityIndicator startAnimating];
    } else {
        [_activityIndicator stopAnimating];
        if (agent.lastFinished != nil && agent.lastOutcome.length > 0)
            [phase appendFormat:@" · %@", ISHLLMAgoText(agent.lastFinished)];
    }
    ISHLLMAgentApproval *approval = [agent pendingApprovalIncludingSubagents];
    _statusLabel.text = phase;
    _statusLabel.textColor = approval != nil ? UIColor.systemOrangeColor : UIColor.labelColor;
    _elapsedLabel.text = busy ? ISHLLMElapsedText(agent.replyStarted) : @"";

    // Line 2: what it is doing right now.
    NSString *detail = agent.phaseDetail;
    if (approval != nil && approval.agent != agent)
        detail = [NSString stringWithFormat:@"sub-agent %@ asks: %@", approval.agent.title ?: @"", detail ?: @""];
    if (detail.length > 0 && busy && agent.phaseStarted != nil && -agent.phaseStarted.timeIntervalSinceNow >= 5)
        detail = [detail stringByAppendingFormat:@"  (%@)", ISHLLMElapsedText(agent.phaseStarted)];
    _detailLabel.text = detail;
    _detailLabel.hidden = detail.length == 0;

    // Line 3: destination, context, tasks, changes, sub-agents.
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    [parts addObject:ISHLLMDestinationLabel([agent destination])];
    NSInteger used = [agent estimatedContextTokens];
    NSInteger window = [agent effectiveContextWindowTokens];
    if (used > 0) {
        [parts addObject:[NSString stringWithFormat:@"~%@/%@%@ ctx", ISHLLMFormattedTokenCount(used), ISHLLMFormattedTokenCountShort(window),
                          [agent contextWindowTokens] > 0 ? @"" : @"?"]];
    }
    float fraction = window > 0 ? (float) MIN(1.0, (double) used / (double) window) : 0.0f;
    _contextBar.progress = fraction;
    _contextBar.progressTintColor = fraction >= 0.75f ? UIColor.systemOrangeColor : UIColor.systemBlueColor;
    _contextBar.hidden = used <= 0;
    NSArray<NSDictionary *> *todos = agent.toolContext.todos;
    if (todos.count > 0) {
        NSUInteger done = 0;
        for (NSDictionary *todo in todos)
            done += [todo[@"status"] isEqual:@"completed"];
        [parts addObject:[NSString stringWithFormat:@"tasks %lu/%lu", (unsigned long) done, (unsigned long) todos.count]];
    }
    NSUInteger changes = 0;
    for (ISHLLMFileChange *change in agent.toolContext.changes)
        changes += !change.reverted;
    if (changes > 0)
        [parts addObject:[NSString stringWithFormat:@"%lu file change%@", (unsigned long) changes, changes == 1 ? @"" : @"s"]];
    NSUInteger subRunning = 0;
    for (ISHLLMAgent *subagent in agent.subagents)
        subRunning += subagent.busy;
    if (agent.subagents.count > 0)
        [parts addObject:[NSString stringWithFormat:@"%lu sub-agent%@%@", (unsigned long) agent.subagents.count, agent.subagents.count == 1 ? @"" : @"s",
                          subRunning > 0 ? [NSString stringWithFormat:@" (%lu working)", (unsigned long) subRunning] : @""]];
    if (agent.systemPrompt.length > 0)
        [parts addObject:@"system prompt"];
    _infoLabel.text = [parts componentsJoinedByString:@" · "];

    // Line 4: every other agent that wants attention.
    NSUInteger running = 0, waiting = 0, finished = 0;
    for (ISHLLMAgent *other in [ISHLLMAgentManager.shared agentsWantingAttention]) {
        if (other == agent || other.parent == agent)
            continue;
        if (other.pendingApproval != nil)
            waiting++;
        else if (other.busy)
            running++;
        else if (other.finishedUnseen)
            finished++;
    }
    NSMutableArray<NSString *> *others = [NSMutableArray array];
    if (waiting > 0)
        [others addObject:[NSString stringWithFormat:@"%lu need%@ approval", (unsigned long) waiting, waiting == 1 ? @"s" : @""]];
    if (running > 0)
        [others addObject:[NSString stringWithFormat:@"%lu working", (unsigned long) running]];
    if (finished > 0)
        [others addObject:[NSString stringWithFormat:@"%lu new answer%@", (unsigned long) finished, finished == 1 ? @"" : @"s"]];
    _agentsButton.hidden = others.count == 0;
    if (others.count > 0) {
        NSString *title = [NSString stringWithFormat:@"Other chats: %@ ›", [others componentsJoinedByString:@" · "]];
        [_agentsButton setTitle:title forState:UIControlStateNormal];
        [_agentsButton setTitleColor:waiting > 0 ? UIColor.systemOrangeColor : nil forState:UIControlStateNormal];
    }

    // The clock ticks only while something is running here.
    BOOL anyRunning = busy || running > 0;
    if (anyRunning && _statusTimer == nil) {
        __weak typeof(self) weakSelf = self;
        _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *timer) {
            typeof(self) self = weakSelf;
            if (self == nil) {
                [timer invalidate];
                return;
            }
            [self updateStatusPanel];
        }];
    } else if (!anyRunning && _statusTimer != nil) {
        [_statusTimer invalidate];
        _statusTimer = nil;
    }
}

- (void)agentsButtonTapped:(id)sender {
    (void) sender;
    [self showAgentList];
}

- (void)showAgentList {
    LLMAgentListViewController *list = [LLMAgentListViewController new];
    list.currentSessionID = _agent.sessionID;
    __weak typeof(self) weakSelf = self;
    list.agentSelected = ^(NSString *sessionID) {
        [weakSelf switchToSessionWithID:sessionID];
    };
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:list];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

#pragma mark - Approvals

// A tool call waiting on the user, from this agent or one of its
// sub-agents, goes up only here, on the chat that owns it -- a background
// chat's questions wait for that chat to be opened (the status line and the
// agent list say which).
- (void)presentPendingApprovalIfNeeded {
    if (_presentedApproval != nil) {
        if (_presentedApproval.resolved)
            [self dismissPresentedApprovalAnimated:YES];
        else
            return;
    }
    if (self.viewIfLoaded.window == nil || !_viewing)
        return;
    ISHLLMAgentApproval *approval = [_agent pendingApprovalIncludingSubagents];
    if (approval == nil || approval.resolved || approval.presenter != nil)
        return;
    UIViewController *presenter = [self ish_presentationViewController];
    if (presenter.isBeingPresented || presenter.isBeingDismissed)
        return;
    approval.presenter = self;
    _presentedApproval = approval;
    _approvalAlert = [self alertForApproval:approval];
    [presenter presentViewController:_approvalAlert animated:YES completion:nil];
}

- (void)dismissPresentedApprovalAnimated:(BOOL)animated {
    if (_presentedApproval.presenter == self)
        _presentedApproval.presenter = nil;
    _presentedApproval = nil;
    UIAlertController *alert = _approvalAlert;
    _approvalAlert = nil;
    if (alert.presentingViewController != nil)
        [alert dismissViewControllerAnimated:animated completion:nil];
}

// The "always" choices save a permission, so later calls of the same kind do
// not ask at all: a shell rule for this command (see ISHLLMSuggestedShellRule
// for why compound lines get none), or the category's own setting for files.
- (UIAlertController *)alertForApproval:(ISHLLMAgentApproval *)approval {
    ISHLLMToolInvocation *invocation = approval.invocation;
    NSString *reason = approval.reason;
    BOOL shell = invocation.category == ISHLLMToolCategoryShell;
    NSString *message = invocation.confirmationMessage;
    if (reason.length > 0)
        message = [message stringByAppendingFormat:@"\n\nAsking because of %@.", reason];
    NSString *title = invocation.confirmationTitle;
    if (approval.agent != _agent)
        title = [NSString stringWithFormat:@"Sub-agent “%@”: %@", approval.agent.title ?: @"", title];
    __weak typeof(self) weakSelf = self;
    void (^finish)(ISHLLMToolRunDecision) = ^(ISHLLMToolRunDecision decision) {
        typeof(self) self = weakSelf;
        if (self != nil && self->_presentedApproval == approval) {
            self->_presentedApproval = nil;
            self->_approvalAlert = nil;
        }
        approval.presenter = nil;
        [approval resolve:decision];
        // The next question, if another is waiting.
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf presentPendingApprovalIfNeeded];
        });
    };
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run" : @"Allow" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        finish(ISHLLMToolRunOnce);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run, don't ask again this reply" : @"Allow, don't ask again this reply" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        finish(ISHLLMToolRunAllowReply);
    }]];
    NSString *rule = shell ? ISHLLMSuggestedShellRule(invocation.command ?: @"") : nil;
    if (rule != nil) {
        [alert addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Always allow “%@”", rule] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            ISHLLMAddShellRule(rule, ISHLLMPermissionAllow);
            finish(ISHLLMToolRunOnce);
        }]];
    } else if (!shell && reason.length == 0) {
        // Only when the category setting is what asked: an edit outside the
        // working directory asks whatever the setting says.
        ISHLLMToolCategory category = invocation.category;
        NSString *always = category == ISHLLMToolCategoryEdit ? @"Always allow file edits"
            : category == ISHLLMToolCategoryMCP ? @"Always allow MCP tools" : @"Always allow reading files";
        [alert addAction:[UIAlertAction actionWithTitle:always style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            ISHLLMSetCategoryAction(category, ISHLLMPermissionAllow);
            finish(ISHLLMToolRunOnce);
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run, allow all this chat" : @"Allow all tools this chat" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        typeof(self) self = weakSelf;
        if (self == nil) {
            finish(ISHLLMToolRunOnce);
            return;
        }
        [self confirmAutoRunAllForChatWithCompletion:finish];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Don't Run" : @"Don't Allow" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *action) {
        finish(ISHLLMToolRunDecline);
    }]];
    return alert;
}

// The same four controls exist twice: as navigation bar items, for the
// terminal's modal presentation, and as an in-view row, for the Workspace
// window, which is a bare child view controller with no navigation bar. Only
// one of them can be right at a time -- showing both put "New Chat" next to a
// button also labelled "New Chat" -- so the row collapses whenever a
// navigation bar is there to carry them.
- (void)updateToolbarVisibility {
    BOOL hasNavigationBar = self.navigationController != nil && !self.navigationController.navigationBarHidden;
    _toolbarStackView.hidden = hasNavigationBar;
    _toolbarHeightConstraint.constant = hasNavigationBar ? 0.0 : 32.0;
}

#pragma mark - Chat and destination switching

- (UIButton *)toolbarButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.titleLabel.minimumScaleFactor = 0.8;
    [button setTitle:title forState:UIControlStateNormal];
    if (action != NULL)
        [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    else
        button.showsMenuAsPrimaryAction = YES; // menu is attached in -updateChatHeaderTitles
    [_toolbarStackView addArrangedSubview:button];
    return button;
}

// Toolbar labels are narrow; keep them recognisable rather than complete.
static NSString *ISHLLMShortenedButtonTitle(NSString *text, NSUInteger limit) {
    NSString *trimmed = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length == 0)
        return @"";
    if (trimmed.length <= limit)
        return trimmed;
    return [[trimmed substringToIndex:limit - 1] stringByAppendingString:@"…"];
}

- (void)updateChatHeaderTitles {
    NSString *sessionTitle = _agent.title.length > 0 ? _agent.title : @"New Chat";
    NSString *destinationName = ISHLLMDestinationDisplayName([_agent destination] ?: ISHLLMActiveDestination());
    self.title = sessionTitle;

    // Both menus are built from disk every time they are opened rather than
    // captured here: a chat renamed in the list, or a destination added from
    // Settings, has to show up without something else happening to refresh
    // this view first (a form-sheet dismissal doesn't re-run viewWillAppear).
    UIMenu *chatMenu = [self liveMenuWithElements:^NSArray<UIMenuElement *> *(LLMClientViewController *client) {
        return [client chatMenuElements];
    }];
    UIMenu *destinationMenu = [self liveMenuWithElements:^NSArray<UIMenuElement *> *(LLMClientViewController *client) {
        return [client destinationMenuElements];
    }];

    [_chatsButton setTitle:ISHLLMShortenedButtonTitle(sessionTitle, 14) forState:UIControlStateNormal];
    _chatsButton.accessibilityLabel = [NSString stringWithFormat:@"Chats. Current chat: %@", sessionTitle];
    _chatsButton.menu = chatMenu;
    [_destinationButton setTitle:ISHLLMShortenedButtonTitle(destinationName, 14) forState:UIControlStateNormal];
    _destinationButton.accessibilityLabel = [NSString stringWithFormat:@"Chat destination: %@", destinationName];
    _destinationButton.menu = destinationMenu;

    // The navigation bar shows the chat name as the title, so its button says
    // what it does instead of repeating the name; the in-view row has no title
    // above it and carries the name itself.
    _chatsBarButtonItem.title = @"Chats";
    _chatsBarButtonItem.menu = chatMenu;
    _destinationBarButtonItem.title = ISHLLMShortenedButtonTitle(destinationName, 14);
    _destinationBarButtonItem.menu = destinationMenu;
}

// Wraps a menu whose children are produced on demand. The provider takes the
// controller as an argument so this can hold it weakly -- a button retains its
// menu, and a menu capturing self strongly would outlive the chat.
- (UIMenu *)liveMenuWithElements:(NSArray<UIMenuElement *> *(^)(LLMClientViewController *client))provider {
    __weak typeof(self) weakSelf = self;
    UIDeferredMenuElement *deferred = [UIDeferredMenuElement elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *elements)) {
        typeof(self) client = weakSelf;
        completion(client != nil ? provider(client) : @[]);
    }];
    return [UIMenu menuWithTitle:@"" image:nil identifier:nil options:0 children:@[deferred]];
}

- (NSArray<UIMenuElement *> *)chatMenuElements {
    UIAction *newChat = [UIAction actionWithTitle:@"New Chat"
                                            image:[UIImage systemImageNamed:@"square.and.pencil"]
                                       identifier:nil
                                          handler:^(__unused UIAction *action) { [self startNewChat]; }];
    UIAction *agents = [UIAction actionWithTitle:@"Agents…"
                                           image:[UIImage systemImageNamed:@"person.2"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self showAgentList]; }];
    agents.subtitle = @"Chats working or waiting (also /agents)";
    UIAction *browse = [UIAction actionWithTitle:@"All Chats…"
                                           image:[UIImage systemImageNamed:@"list.bullet"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self showChatList]; }];

    // The handful of most recent chats inline, so the common switch is one
    // gesture; the full list (rename, delete, search by title) is behind
    // "All Chats…".
    NSMutableArray<UIAction *> *recent = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *entry in ISHLLMSessionEntriesByRecency()) {
        if (recent.count >= 5)
            break;
        NSString *entryID = ISHLLMStringValue(entry, @"id");
        NSString *title = ISHLLMStringValue(entry, @"title");
        UIAction *item = [UIAction actionWithTitle:title.length > 0 ? title : @"New Chat"
                                             image:nil
                                        identifier:nil
                                           handler:^(__unused UIAction *action) { [self switchToSessionWithID:entryID]; }];
        item.state = [entryID isEqualToString:_agent.sessionID] ? UIMenuElementStateOn : UIMenuElementStateOff;
        [recent addObject:item];
    }

    UIAction *rename = [UIAction actionWithTitle:@"Rename Chat…"
                                           image:[UIImage systemImageNamed:@"pencil"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self renameCurrentChat]; }];
    UIAction *systemPrompt = [UIAction actionWithTitle:_agent.systemPrompt.length > 0 ? @"System Prompt (set)…" : @"System Prompt…"
                                                 image:[UIImage systemImageNamed:@"text.badge.star"]
                                            identifier:nil
                                               handler:^(__unused UIAction *action) { [self editSystemPromptForCurrentChat]; }];
    UIAction *clear = [UIAction actionWithTitle:@"Clear Messages"
                                          image:[UIImage systemImageNamed:@"eraser"]
                                     identifier:nil
                                        handler:^(__unused UIAction *action) { [self clearTranscript:nil]; }];
    UIAction *delete = [UIAction actionWithTitle:@"Delete Chat"
                                           image:[UIImage systemImageNamed:@"trash"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self deleteCurrentChat]; }];
    delete.attributes = UIMenuElementAttributesDestructive;

    UIMenu *switchSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:recent];
    UIAction *workingDirectory = [UIAction actionWithTitle:@"Working Directory…"
                                                     image:[UIImage systemImageNamed:@"folder"]
                                                identifier:nil
                                                   handler:^(__unused UIAction *action) { [self editWorkingDirectoryForCurrentChat]; }];
    workingDirectory.subtitle = _agent.toolContext.workingDirectory;
    UIAction *summarize = [UIAction actionWithTitle:@"Summarize Chat"
                                              image:[UIImage systemImageNamed:@"text.redaction"]
                                         identifier:nil
                                            handler:^(__unused UIAction *action) { [self compactConversation]; }];
    summarize.subtitle = @"Send the model a summary instead of the history";
    UIAction *changes = [UIAction actionWithTitle:@"Changes…"
                                            image:[UIImage systemImageNamed:@"plusminus"]
                                       identifier:nil
                                          handler:^(__unused UIAction *action) { [self showChanges]; }];
    NSUInteger changeCount = _agent.toolContext.changes.count;
    changes.subtitle = changeCount == 0 ? @"No file changes yet (also /changes)" : [NSString stringWithFormat:@"%lu file change%@ · /undo reverts the last", (unsigned long) changeCount, changeCount == 1 ? @"" : @"s"];
    UIMenu *currentSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[rename, systemPrompt, workingDirectory, changes, summarize, clear, delete]];
    return @[newChat, browse, agents, switchSection, currentSection];
}

- (void)showChatList {
    LLMChatSessionListViewController *listViewController = [LLMChatSessionListViewController new];
    listViewController.currentSessionID = _agent.sessionID;
    __weak typeof(self) weakSelf = self;
    listViewController.sessionSelected = ^(NSString *sessionID) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (sessionID.length == 0) {
            [self startNewChat]; // the list's compose button
            return;
        }
        if ([sessionID isEqualToString:self->_agent.sessionID]) {
            // Renamed in the list: the entry changed under the agent.
            [self->_agent reloadIfChangedOnDisk];
            [self updateChatHeaderTitles];
            return;
        }
        [self switchToSessionWithID:sessionID];
    };
    // The chat is a deeply nested child view controller in Workspace mode, so
    // modals go through the window's top view controller (see
    // -ish_presentationViewController); presenting from self silently fails.
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:listViewController];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

- (NSArray<UIMenuElement *> *)destinationMenuElements {
    NSArray<NSDictionary<NSString *, NSString *> *> *destinations = ISHLLMDestinations();
    NSString *activeID = ISHLLMStringValue([_agent destination], kISHLLMDestinationID);
    NSMutableArray<UIAction *> *items = [NSMutableArray array];
    for (NSDictionary<NSString *, NSString *> *destination in destinations) {
        NSString *destinationID = ISHLLMStringValue(destination, kISHLLMDestinationID);
        UIAction *item = [UIAction actionWithTitle:ISHLLMDestinationDisplayName(destination)
                                             image:nil
                                        identifier:nil
                                           handler:^(__unused UIAction *action) { [self switchToDestinationWithID:destinationID]; }];
        item.subtitle = ISHLLMDestinationSubtitle(destination);
        item.state = [destinationID isEqualToString:activeID] ? UIMenuElementStateOn : UIMenuElementStateOff;
        [items addObject:item];
    }

    NSDictionary<NSString *, NSString *> *current = [_agent destination];
    UIAction *chooseModel = [UIAction actionWithTitle:[NSString stringWithFormat:@"Edit “%@”…", ISHLLMDestinationDisplayName(current)]
                                                image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                                           identifier:nil
                                              handler:^(__unused UIAction *action) { [self editDestination:current]; }];
    chooseModel.subtitle = @"Model, server, key; choose from the server's models";
    UIAction *addDestination = [UIAction actionWithTitle:@"Add Destination…"
                                                   image:[UIImage systemImageNamed:@"plus"]
                                              identifier:nil
                                                 handler:^(__unused UIAction *action) { [self addDestinationFromPreset]; }];
    UIAction *manage = [UIAction actionWithTitle:@"Manage Destinations…"
                                           image:[UIImage systemImageNamed:@"list.bullet"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self showDestinationList]; }];
    UIMenu *switchSection = [UIMenu menuWithTitle:@"Chat With" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:items];
    UIMenu *manageSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[chooseModel, addDestination, manage]];
    return @[switchSection, manageSection];
}

// Adds a second (third, …) destination straight from the chat: pick a preset,
// it lands as a new saved destination and becomes the active one. The preset's
// URL and model are filled in; an API key, if the provider needs one, is
// prompted for right here so nothing has to go through Settings.
- (void)addDestinationFromPreset {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Add Destination"
                                                         message:@"Pick a provider preset. Server URL, model and key stay editable in Settings."];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        [alert addActionWithTitle:preset[@"name"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSDictionary<NSString *, NSString *> *destination = @{
                kISHLLMDestinationID: NSUUID.UUID.UUIDString,
                kISHLLMDestinationName: preset[@"name"] ?: @"Destination",
                kISHLLMDestinationProvider: preset[@"name"] ?: @"Custom",
                kISHLLMDestinationURL: preset[@"url"] ?: @"",
                kISHLLMDestinationModel: preset[@"model"] ?: @"",
                kISHLLMDestinationAPIKey: @"",
            };
            ISHLLMSaveDestination(destination);
            // A reply already in flight keeps the destination it started with.
            [self switchToDestinationWithID:destination[kISHLLMDestinationID]];
            if (ISHLLMProviderRequiresAPIKey())
                [self promptForAPIKeyForNewDestination:destination];
        }];
    }
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:[self ish_presentationViewController] source:_destinationButton];
}

- (void)promptForAPIKeyForNewDestination:(NSDictionary<NSString *, NSString *> *)destination {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"%@ API Key", ISHLLMDestinationDisplayName(destination)]
                                                                  message:@"This provider needs a key. It is stored with the destination and can be changed in Settings."
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.secureTextEntry = YES;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.placeholder = @"API key";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Later" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *key = alert.textFields.firstObject.text ?: @"";
        if (key.length == 0)
            return;
        UserPreferences.shared.llmAPIKey = key;
        ISHLLMSyncActiveDestinationFromPreferences();
        [self updateChatHeaderTitles];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

// The one place a chat's model is set: its destination's editor.
- (void)editDestination:(NSDictionary<NSString *, NSString *> *)destination {
    LLMDestinationEditorViewController *editor = [LLMDestinationEditorViewController new];
    editor.destination = destination;
    __weak typeof(self) weakSelf = self;
    editor.destinationSaved = ^{
        typeof(self) self = weakSelf;
        [self updateChatHeaderTitles];
        [self refreshTranscript];
        [self updateStatusPanel];
    };
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:editor];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

- (void)showDestinationList {
    LLMDestinationListViewController *listViewController = [LLMDestinationListViewController new];
    __weak typeof(self) weakSelf = self;
    listViewController.destinationsChanged = ^{
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        [self updateChatHeaderTitles];
        [self refreshTranscript];
        [self updateStatusPanel];
    };
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:listViewController];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

// /mcp: straight to LLM Settings -> MCP Servers.
- (void)showMCPServers {
    UIViewController *serversViewController = [LLMMCPServersViewController new];
    if (self.ish_canPushSubpage) {
        [self.navigationController pushViewController:serversViewController animated:YES];
    } else {
        UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:serversViewController];
        ISHConfigureLLMSettingsNavigationController(navigationController);
        [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
    }
}

- (void)showLLMSettings:(id)sender {
    (void) sender;
    LLMSettingsViewController *settingsViewController = [LLMSettingsViewController new];
    settingsViewController.hidesModels = YES; // the model button is where a model is chosen
    // NOT `navigationController != nil`. In Workspace mode the chat is a bare
    // child of a host whose navigation bar is hidden, so it INHERITS that
    // navigation controller -- pushing succeeds and leaves the user with no
    // chevron, no title, and no working pop gesture. See -ish_canPushSubpage.
    if (self.ish_canPushSubpage) {
        [self.navigationController pushViewController:settingsViewController animated:YES];
    } else {
        UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
        ISHConfigureLLMSettingsNavigationController(navigationController);
        [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
    }
}

- (NSString *)latestAssistantMessage {
    for (NSDictionary<NSString *, id> *message in [self messages].reverseObjectEnumerator) {
        if ([message[@"role"] isEqualToString:@"assistant"])
            return [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    }
    return @"";
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)extractCodeBlocksFromText:(NSString *)text {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *blocks = [NSMutableArray array];
    NSUInteger searchStart = 0;
    while (searchStart < text.length) {
        NSRange fenceStart = [text rangeOfString:@"```" options:0 range:NSMakeRange(searchStart, text.length - searchStart)];
        if (fenceStart.location == NSNotFound)
            break;
        NSUInteger languageStart = NSMaxRange(fenceStart);
        NSRange languageLine = [text rangeOfString:@"\n" options:0 range:NSMakeRange(languageStart, text.length - languageStart)];
        if (languageLine.location == NSNotFound)
            break;
        NSString *language = [[text substringWithRange:NSMakeRange(languageStart, languageLine.location - languageStart)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSUInteger codeStart = NSMaxRange(languageLine);
        NSRange fenceEnd = [text rangeOfString:@"```" options:0 range:NSMakeRange(codeStart, text.length - codeStart)];
        if (fenceEnd.location == NSNotFound)
            break;
        NSString *code = [text substringWithRange:NSMakeRange(codeStart, fenceEnd.location - codeStart)];
        [blocks addObject:@{@"language": language ?: @"", @"code": code ?: @""}];
        searchStart = NSMaxRange(fenceEnd);
    }
    return blocks;
}

- (void)showExtractActions:(id)sender {
    NSArray<NSDictionary<NSString *, NSString *> *> *blocks = [self extractCodeBlocksFromText:self.latestAssistantMessage];
    NSString *savePath = @"/AOK/persist/llm-extracts";
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Save From Chat"
                                                         message:blocks.count > 0 ? [@"Save destination: " stringByAppendingString:savePath] : [@"No fenced code blocks found in the last reply. Text in the transcript can be highlighted and copied directly, and each code block has its own Copy button. Save destination: " stringByAppendingString:savePath]];
    for (NSUInteger i = 0; i < blocks.count; i++) {
        NSDictionary<NSString *, NSString *> *block = blocks[i];
        NSString *language = block[@"language"].length > 0 ? block[@"language"] : @"text";
        NSString *title = [NSString stringWithFormat:@"Save block %lu (%@)", (unsigned long) i + 1, language];
        [alert addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self saveCodeBlock:block index:i + 1];
        }];
    }
    if (blocks.count > 1) {
        [alert addActionWithTitle:@"Save All Blocks" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            for (NSUInteger i = 0; i < blocks.count; i++)
                [self saveCodeBlock:blocks[i] index:i + 1];
        }];
    }
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sender];
}

- (void)saveCodeBlock:(NSDictionary<NSString *, NSString *> *)block index:(NSUInteger)index {
    NSString *language = block[@"language"] ?: @"";
    NSString *ext = ISHLLMExtensionForFenceLanguage(language);
    [self saveExtractText:block[@"code"] extension:ext label:[NSString stringWithFormat:@"block-%lu", (unsigned long) index]];
}

- (void)saveExtractText:(NSString *)text extension:(NSString *)ext label:(NSString *)label {
    NSURL *directoryURL = ISHLLMExtractsDirectoryURL();
    [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *filename = [NSString stringWithFormat:@"llm-%@-%@.%@", [formatter stringFromDate:NSDate.date], label, ext ?: @"txt"];
    NSURL *url = [directoryURL URLByAppendingPathComponent:ISHLLMSanitizeFilenameComponent(filename) isDirectory:NO];
    NSError *error = nil;
    BOOL ok = [text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error];
    NSString *ishPath = [@"/AOK/persist/llm-extracts" stringByAppendingPathComponent:url.lastPathComponent];
    NSString *message = ok ? [NSString stringWithFormat:@"Saved %@", ishPath] : (error.localizedDescription ?: @"Save failed");
    [_agent appendLocalRole:@"assistant" content:message];
}

- (NSString *)terminalContextPromptWithInstruction:(NSString *)instruction {
    Terminal *terminal = Terminal.activeTerminals.firstObject;
    NSString *context = terminal != nil ? Terminal_debugReadRows(terminal.type, terminal.number, 80) : @"";
    if (context.length == 0)
        context = @"No active terminal output was available.";
    return [NSString stringWithFormat:@"%@\n\nTerminal output:\n```text\n%@\n```", instruction, context];
}

- (void)showPromptActions:(id)sender {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Prompt Actions" message:@"Use terminal context or saved prompt templates."];
    NSArray<NSDictionary<NSString *, NSString *> *> *actions = @[
        @{@"title": @"Explain terminal output", @"instruction": @"Explain the important details in this terminal output. If there is an error, identify the likely cause."},
        @{@"title": @"Suggest fix for error", @"instruction": @"Find the most likely error in this terminal output and suggest concrete commands or edits to fix it."},
        @{@"title": @"Draft shell command", @"instruction": @"Based on this terminal context, draft the next safe shell command. Explain briefly before the command."},
    ];
    for (NSDictionary<NSString *, NSString *> *descriptor in actions) {
        [alert addActionWithTitle:descriptor[@"title"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self setPromptFieldText:[self terminalContextPromptWithInstruction:descriptor[@"instruction"]]];
        }];
    }
    [alert addActionWithTitle:@"Load Prompt Template" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showPromptTemplatePickerFromSender:sender];
    }];
    // Saving code out of the last reply used to have its own toolbar button;
    // the toolbar now spends two of its four slots on the chat and the
    // destination, so it lives here.
    [alert addActionWithTitle:@"Save From Chat…" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showExtractActions:sender];
    }];
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sender];
}

- (void)showPromptTemplatePickerFromSender:(id)sender {
    NSURL *templatesURL = [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-prompts" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:templatesURL withIntermediateDirectories:YES attributes:nil error:nil];
    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:templatesURL includingPropertiesForKeys:nil options:0 error:nil];
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Prompt Templates" message:@"Templates are text files in /AOK/persist/llm-prompts."];
    for (NSURL *fileURL in files) {
        if (fileURL.lastPathComponent.length == 0)
            continue;
        [alert addActionWithTitle:fileURL.lastPathComponent style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSString *template = [NSString stringWithContentsOfURL:fileURL encoding:NSUTF8StringEncoding error:nil];
            if (template.length > 0)
                [self setPromptFieldText:template];
        }];
    }
    [alert addActionWithTitle:@"Create Examples" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [@"Review this code for correctness, portability, and security.\n\n```\nPASTE_CODE_HERE\n```\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"code-review.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"Turn this into a robust shell script with error handling:\n\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"make-script.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [self->_agent appendLocalRole:@"assistant" content:@"Created example prompt templates in /AOK/persist/llm-prompts."];
    }];
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sender];
}

// A message gets its own bubble/row if it has visible content, or a
// tool-call caption to show, or it's the trailing in-progress streaming
// placeholder (empty content until the first chunk arrives). Tool-role
// messages are never shown -- their output still goes to the model, just
// not the screen.
- (void)recomputeVisibleMessageIndices {
    NSArray<NSDictionary<NSString *, id> *> *messages = [self messages];
    NSMutableArray<NSNumber *> *indices = [NSMutableArray array];
    for (NSUInteger i = 0; i < messages.count; i++) {
        NSDictionary<NSString *, id> *message = messages[i];
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        if ([role isEqualToString:@"tool"])
            continue;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        BOOL hasCommandNote = [self commandCountForMessage:message] > 0;
        BOOL isTrailingStreamingPlaceholder = (i == messages.count - 1) && [role isEqualToString:@"assistant"];
        if (content.length == 0 && !hasCommandNote && !isTrailingStreamingPlaceholder)
            continue;
        [indices addObject:@(i)];
    }
    _visibleMessageIndices = indices;
}

// The transcript shows one short line per tool call ("$ make", "edit
// src/main.c"), not the calls' output. The full calls and results are still
// kept in the saved transcript file and sent to the model.
- (NSUInteger)commandCountForMessage:(NSDictionary<NSString *, id> *)message {
    NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
    return ISHLLMToolCallDescriptions(toolCalls ?: @[]).count;
}

- (void)refreshTranscript {
    [self recomputeVisibleMessageIndices];
    if (_emptyStateLabel != nil) {
        _emptyStateLabel.hidden = _visibleMessageIndices.count > 0;
        if (!_emptyStateLabel.hidden) {
            // Intentionally omit the server URL here -- it shows after Clear and
            // may contain a private host/IP the user doesn't want on screen.
            NSString *model = [_agent modelName];
            _emptyStateLabel.text = [NSString stringWithFormat:@"%@\n\nModel: %@%@%@\n\n%@",
                                      [self messages].count > 0 ? @"Nothing to show yet." : @"Send a prompt to start this chat.",
                                      ISHLLMDestinationLabel([_agent destination]),
                                      model.length > 0 ? @"" : @"\nNo model set: choose one with the model button.",
                                      _agent.systemPrompt.length > 0 ? @"\nSystem prompt set for this chat." : @"",
                                      [_agent toolsSummaryText] ?: @""];
        }
    }
    [_transcriptTable reloadData];
    [self scrollTranscriptToBottomAnimated:NO];
}

#pragma mark - WorkspaceTextScalable

// Not a WorkspaceThemedToolViewController, so the property is implemented
// here, to the same rules: 1.0 is the default, the Workspace owns the steps
// and saves the value, and a value set before the views exist is picked up
// when -viewDidLoad builds them from -scaledBodyFont.
- (CGFloat)workspaceTextScale {
    return _workspaceTextScale > 0 ? _workspaceTextScale : 1.0;
}

- (void)setWorkspaceTextScale:(CGFloat)workspaceTextScale {
    CGFloat clamped = workspaceTextScale > 0
        ? MAX(kISHLLMMinimumTextScale, MIN(kISHLLMMaximumTextScale, workspaceTextScale))
        : 1.0;
    if (fabs(clamped - self.workspaceTextScale) < 0.001)
        return;
    _workspaceTextScale = clamped;
    if (self.isViewLoaded)
        [self applyTextScale];
}

// Every transcript and prompt font derives from this, and always from the
// unscaled Body style, so re-applying never compounds. At 1.0 it is the
// style's own font, untouched.
- (UIFont *)scaledBodyFont {
    UIFont *bodyFont = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    CGFloat scale = self.workspaceTextScale;
    if (fabs(scale - 1.0) < 0.001)
        return bodyFont;
    return [bodyFont fontWithSize:round(bodyFont.pointSize * scale * 2.0) / 2.0];
}

- (CGFloat)promptFieldMaxHeight {
    return round(kISHLLMPromptFieldMaxHeight * self.workspaceTextScale);
}

// The toolbar row, status line, Send button and the Copy chips are chrome and
// stay fixed: the toolbar is four equal buttons that already crush on a
// narrow window. The bubbles and the prompt are what a person reads.
- (void)applyTextScale {
    if (_transcriptTable == nil)
        return;
    UIFont *font = [self scaledBodyFont];
    _emptyStateLabel.font = font;
    _promptField.font = font;
    _promptPlaceholderLabel.font = font;
    _promptFieldMaxHeightConstraint.constant = [self promptFieldMaxHeight];
    // After the cap check, which can flip scrollEnabled and with it whether
    // the field sizes to its text.
    [self promptFieldTextDidChange];
    [_promptField invalidateIntrinsicContentSize];
    _transcriptTable.estimatedRowHeight = kISHLLMTranscriptEstimatedRowHeight * self.workspaceTextScale;

    // Keep the reading position. -refreshTranscript would jump to the bottom,
    // which is right only for someone already there (a streaming reply).
    // Otherwise the row at the top stays at the top, at the same fraction of
    // its height, since every row above it changes height too.
    UITableView *table = _transcriptTable;
    UIEdgeInsets insets = table.adjustedContentInset;
    CGFloat visibleTop = table.contentOffset.y + insets.top;
    CGFloat bottomGap = table.contentSize.height + insets.bottom - (table.contentOffset.y + CGRectGetHeight(table.bounds));
    BOOL atBottom = bottomGap <= 20.0;
    NSIndexPath *anchor = nil;
    CGFloat anchorFraction = 0.0;
    if (!atBottom) {
        NSArray<NSIndexPath *> *visibleRows = [table.indexPathsForVisibleRows sortedArrayUsingSelector:@selector(compare:)];
        for (NSIndexPath *path in visibleRows) {
            CGRect rect = [table rectForRowAtIndexPath:path];
            if (CGRectGetMaxY(rect) > visibleTop) {
                anchor = path;
                anchorFraction = rect.size.height > 0 ? MAX(0.0, (visibleTop - rect.origin.y) / rect.size.height) : 0.0;
                break;
            }
        }
    }

    [table reloadData];
    [self.view layoutIfNeeded];
    if (anchor == nil || anchor.row >= [table numberOfRowsInSection:0]) {
        [self scrollTranscriptToBottomAnimated:NO];
        return;
    }
    // Scrolled to first so the row is measured rather than estimated.
    [table scrollToRowAtIndexPath:anchor atScrollPosition:UITableViewScrollPositionTop animated:NO];
    [table layoutIfNeeded];
    CGRect rect = [table rectForRowAtIndexPath:anchor];
    insets = table.adjustedContentInset;
    CGFloat maxOffset = MAX(-insets.top, table.contentSize.height + insets.bottom - CGRectGetHeight(table.bounds));
    CGFloat offset = MIN(maxOffset, MAX(-insets.top, rect.origin.y + anchorFraction * rect.size.height - insets.top));
    table.contentOffset = CGPointMake(table.contentOffset.x, offset);
}

// A transcript showing its newest message keeps showing it when the view
// changes height -- the keyboard coming up shrinks the table from below, which
// would otherwise push the latest reply out of sight.
- (BOOL)transcriptIsAtBottom {
    UITableView *table = _transcriptTable;
    CGFloat visibleBottom = table.contentOffset.y + CGRectGetHeight(table.bounds) - table.adjustedContentInset.bottom;
    return visibleBottom >= table.contentSize.height - 4.0;
}

- (void)viewWillLayoutSubviews {
    [super viewWillLayoutSubviews];
    _transcriptWasAtBottom = _transcriptTable != nil && [self transcriptIsAtBottom];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGFloat height = CGRectGetHeight(_transcriptTable.bounds);
    if (height != _transcriptLaidOutHeight && _transcriptWasAtBottom)
        [self scrollTranscriptToBottomAnimated:NO];
    _transcriptLaidOutHeight = height;
}

- (void)scrollTranscriptToBottomAnimated:(BOOL)animated {
    NSInteger rows = [_transcriptTable numberOfRowsInSection:0];
    if (rows <= 0)
        return;
    [_transcriptTable scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:rows - 1 inSection:0] atScrollPosition:UITableViewScrollPositionBottom animated:animated];
}

// Streaming updates only ever touch the trailing placeholder message, which
// -recomputeVisibleMessageIndices always keeps as the last row -- so a token
// arriving mid-stream only needs that one row reloaded, not the whole table.
- (void)reloadLastRowAndScroll:(BOOL)scroll {
    // A streamed reply's placeholder can become visible only with its first
    // text; the row count follows the messages.
    NSUInteger visible = _visibleMessageIndices.count;
    [self recomputeVisibleMessageIndices];
    if (_visibleMessageIndices.count != visible) {
        [_transcriptTable reloadData];
        if (scroll)
            [self scrollTranscriptToBottomAnimated:NO];
        return;
    }
    NSInteger rows = [_transcriptTable numberOfRowsInSection:0];
    if (rows <= 0)
        return;
    NSIndexPath *path = [NSIndexPath indexPathForRow:rows - 1 inSection:0];
    [UIView performWithoutAnimation:^{
        [self->_transcriptTable reloadRowsAtIndexPaths:@[path] withRowAnimation:UITableViewRowAnimationNone];
    }];
    if (scroll)
        [self scrollTranscriptToBottomAnimated:NO];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return (NSInteger) _visibleMessageIndices.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    ISHLLMChatMessageCell *cell = [tableView dequeueReusableCellWithIdentifier:@"message" forIndexPath:indexPath];
    if ((NSUInteger) indexPath.row < _visibleMessageIndices.count)
        [self configureCell:cell forMessageAtIndex:_visibleMessageIndices[indexPath.row].unsignedIntegerValue];
    return cell;
}

- (void)configureCell:(ISHLLMChatMessageCell *)cell forMessageAtIndex:(NSUInteger)messageIndex {
    NSArray<NSDictionary<NSString *, id> *> *messages = [self messages];
    if (messageIndex >= messages.count)
        return;
    NSDictionary<NSString *, id> *message = messages[messageIndex];
    NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
    NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    BOOL isAssistant = [role isEqualToString:@"assistant"];

    // Read on every configure, so a reload after a text-size change re-renders
    // each bubble from its raw content at the new size.
    UIFont *baseFont = [self scaledBodyFont];
    UIFont *codeFont = ISHLLMMonospaceFont(baseFont.pointSize - 1.0);
    cell.textScale = self.workspaceTextScale;
    UIColor *textColor = UIColor.blackColor;
    UIColor *secondaryColor = UIColor.grayColor;
    UIColor *assistantBubbleColor = [UIColor colorWithWhite:0.9 alpha:1.0];
    UIColor *codeBubbleColor = [UIColor colorWithWhite:0.0 alpha:0.06];
    UIColor *linkColor = [UIColor colorWithRed:0.0 green:0.48 blue:1.0 alpha:1.0];
    if (@available(iOS 13.0, *)) {
        textColor = UIColor.labelColor;
        secondaryColor = UIColor.secondaryLabelColor;
        assistantBubbleColor = UIColor.secondarySystemBackgroundColor;
        codeBubbleColor = UIColor.tertiarySystemFillColor; // adapts to light/dark
        linkColor = UIColor.linkColor;
    }
    UIColor *userBubbleColor = UIColor.systemBlueColor;
    UIColor *userTextColor = UIColor.whiteColor;

    // A reasoning model's <think> block is pulled out of the rendered text and
    // shown as a collapsed disclosure above the answer (unless the user turned
    // that off in Settings). The message keeps its raw content either way, so
    // the thought stays expandable, copyable and in the saved transcript.
    NSMutableArray<NSString *> *thoughts = [NSMutableArray array];
    BOOL thinkingOpen = NO;
    NSString *displayContent = content;
    if (isAssistant && ISHLLMHideThinkingEnabled())
        displayContent = ISHLLMSplitThinkingFromContent(content, thoughts, &thinkingOpen);
    NSString *thinkingText = [thoughts componentsJoinedByString:@"\n\n"];

    // Assistant replies are rendered as Markdown, with fenced code split into
    // its own scrollable blocks; the user's own prompt is shown verbatim in a
    // single block so their literal text is never reinterpreted.
    NSArray<ISHMarkdownBlock *> *blocks = isAssistant
        ? ISHMarkdownBlocksFromMarkdown(displayContent, baseFont, textColor, secondaryColor, linkColor)
        : @[ISHMarkdownPlainTextBlock(displayContent, baseFont, userTextColor)];

    NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
    NSArray<NSString *> *toolLines = ISHLLMToolCallDescriptions(toolCalls ?: @[]);
    NSString *caption = toolLines.count > 0 ? [toolLines componentsJoinedByString:@"\n"] : nil;

    __weak __typeof(self) weakSelf = self;
    cell.thinkingToggleHandler = ^{
        [weakSelf toggleThinkingExpandedForMessageIndex:messageIndex];
    };
    [cell configureThinkingWithText:thinkingText
                         inProgress:thinkingOpen
                           expanded:[_expandedThinkingIndices containsObject:@(messageIndex)]
                           baseFont:baseFont
                              color:secondaryColor
                            bgColor:codeBubbleColor];

    [cell configureWithBlocks:blocks
                   isAssistant:isAssistant
                       caption:caption
                      baseFont:baseFont
                      codeFont:codeFont
                     textColor:isAssistant ? textColor : userTextColor
                secondaryColor:isAssistant ? secondaryColor : [userTextColor colorWithAlphaComponent:0.85]
                   bubbleColor:isAssistant ? assistantBubbleColor : userBubbleColor
               codeBubbleColor:codeBubbleColor];
}

// Expansion is keyed by index into the messages, which only ever grow by
// appending (a clear empties both), so an index stays pointed at the message
// the user expanded.
- (void)toggleThinkingExpandedForMessageIndex:(NSUInteger)messageIndex {
    NSNumber *key = @(messageIndex);
    if ([_expandedThinkingIndices containsObject:key])
        [_expandedThinkingIndices removeObject:key];
    else
        [_expandedThinkingIndices addObject:key];
    NSUInteger row = [_visibleMessageIndices indexOfObject:key];
    if (row == NSNotFound) {
        [_transcriptTable reloadData];
        return;
    }
    [UIView performWithoutAnimation:^{
        [self->_transcriptTable reloadRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:(NSInteger) row inSection:0]]
                                       withRowAnimation:UITableViewRowAnimationNone];
    }];
}

// Return sends the prompt; Shift+Return inserts a newline. UIKit delivers
// both as a "\n" replacement here, so the shift state comes from the prompt
// field's pressesBegan bookkeeping (see LLMPromptTextView). The software
// keyboard has no Shift+Return, so its Return always sends; multi-character
// insertions (paste, dictation) containing newlines pass through untouched.
- (BOOL)textView:(UITextView *)textView shouldChangeTextInRange:(NSRange)range replacementText:(NSString *)text {
    (void) range;
    if (textView == _promptField && [text isEqualToString:@"\n"] && !_promptField.shiftKeyDown) {
        [self sendPrompt:nil];
        return NO;
    }
    return YES;
}

- (void)textViewDidChange:(UITextView *)textView {
    if (textView == _promptField)
        [self promptFieldTextDidChange];
}

// Also called after every programmatic _promptField.text assignment (prompt
// templates, terminal-context actions, clearing after send), since
// -textViewDidChange: only fires for user-driven edits.
- (void)promptFieldTextDidChange {
    [self updateSendButton];
    _promptPlaceholderLabel.hidden = _promptField.text.length > 0;
    CGSize fitSize = [_promptField sizeThatFits:CGSizeMake(_promptField.bounds.size.width, CGFLOAT_MAX)];
    _promptField.scrollEnabled = fitSize.height > [self promptFieldMaxHeight];
}

- (void)setPromptFieldText:(NSString *)text {
    _promptField.text = text ?: @"";
    [self promptFieldTextDidChange];
}

// The view controller to present alerts from. When this chat is embedded in a
// workspace tool window it is a deeply nested child VC, and presenting directly
// from it can silently fail (the modal never appears) -- which would stall the
// tool loop forever waiting on a confirmation. Presenting from the top of our own
// window works in both the embedded and the modal (terminal) cases.
- (UIViewController *)ish_presentationViewController {
    UIViewController *presenter = self;
    UIWindow *window = self.viewIfLoaded.window;
    if (window.rootViewController != nil)
        presenter = window.rootViewController;
    while (presenter.presentedViewController != nil && !presenter.presentedViewController.isBeingDismissed)
        presenter = presenter.presentedViewController;
    return presenter ?: self;
}

// "Allow all this chat" removes the human from the loop, which is exactly what
// prompt injection needs: a fetched web page can instruct the model to run
// further commands with no one confirming them. Spell that out once before the
// first chat-wide auto-approve; after acknowledgment the option works directly.
- (void)confirmAutoRunAllForChatWithCompletion:(void (^)(ISHLLMToolRunDecision decision))completion {
    static NSString *const kAutoRunWarningShownKey = @"LLM Tools AutoRun Warning Acknowledged";
    if ([NSUserDefaults.standardUserDefaults boolForKey:kAutoRunWarningShownKey]) {
        completion(ISHLLMToolRunAllowChat);
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Allow every tool call this chat?"
        message:@"Every command and file change the model requests for the rest of this chat will happen without confirmation. Content the model reads (a web page, a file) can instruct it to run destructive commands, overwrite files or read private data, and nothing will stop that but the model itself. Permissions set to Deny still apply. Confirmation comes back when you clear the chat."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Allow Once Instead" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunOnce);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Allow All" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:kAutoRunWarningShownKey];
        completion(ISHLLMToolRunAllowChat);
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

#pragma mark - Working directory

- (void)editWorkingDirectoryForCurrentChat {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Working Directory"
        message:@"Where this chat's commands start and its relative file paths point, and where AGENTS.md is looked for. File edits outside it always ask. Leave empty for the home directory."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = self->_agent.toolContext.workingDirectory;
        textField.placeholder = @"/root/project";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *raw = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
        [self setWorkingDirectoryFromInput:raw];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)setWorkingDirectoryFromInput:(NSString *)raw {
    if (raw.length == 0) {
        [_agent setWorkingDirectory:nil];
        return;
    }
    NSString *path = ISHLLMResolveGuestPath(raw, _agent.toolContext.workingDirectory);
    ISHLLMAgent *agent = _agent;
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge statAtGuestPath:path completion:^(ISHGuestFileItem *item, NSError *error) {
        typeof(self) self = weakSelf;
        if (item == nil || item.kind != ISHGuestFileKindDirectory) {
            if (self == nil)
                return;
            UIAlertController *failure = [UIAlertController alertControllerWithTitle:@"Not a directory"
                message:[NSString stringWithFormat:@"%@: %@", path, item == nil ? (error.localizedDescription ?: @"not found") : @"not a directory"]
                preferredStyle:UIAlertControllerStyleAlert];
            [failure addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
            [[self ish_presentationViewController] presentViewController:failure animated:YES completion:nil];
            return;
        }
        [agent setWorkingDirectory:path];
    }];
}

#pragma mark - Change review and undo

- (void)showChanges {
    LLMChangesViewController *list = [LLMChangesViewController new];
    list.toolContext = _agent.toolContext;
    __weak typeof(self) weakSelf = self;
    ISHLLMAgent *agent = _agent;
    list.revertRequested = ^(ISHLLMFileChange *change, UIViewController *presenter, void (^done)(void)) {
        [weakSelf revertChange:change ofAgent:agent presenter:presenter force:NO completion:done];
    };
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:list];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

- (void)undoLastChange {
    if (_agent.busy) {
        [_agent appendLocalRole:@"assistant" content:@"Wait for the reply to finish before undoing a change."];
        return;
    }
    for (ISHLLMFileChange *change in _agent.toolContext.changes.reverseObjectEnumerator) {
        if (!change.reverted) {
            [self revertChange:change ofAgent:_agent presenter:[self ish_presentationViewController] force:NO completion:nil];
            return;
        }
    }
    [_agent appendLocalRole:@"assistant" content:@"No file change in this chat to undo."];
}

// A file edited again since the change asks first: reverting would throw the
// later edit away too.
- (void)revertChange:(ISHLLMFileChange *)change ofAgent:(ISHLLMAgent *)agent presenter:(UIViewController *)presenter
               force:(BOOL)force completion:(void (^)(void))completion {
    __weak typeof(self) weakSelf = self;
    [agent revertChange:change force:force completion:^(BOOL reverted, BOOL changedSince, NSString *message) {
        if (changedSince) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Changed since"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *action) {
                if (completion != nil)
                    completion();
            }]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Revert Anyway" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
                [weakSelf revertChange:change ofAgent:agent presenter:presenter force:YES completion:completion];
            }]];
            [presenter presentViewController:alert animated:YES completion:nil];
            return;
        }
        if (!reverted) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Could not revert"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
            [presenter presentViewController:alert animated:YES completion:nil];
        }
        if (completion != nil)
            completion();
    }];
}

@end
