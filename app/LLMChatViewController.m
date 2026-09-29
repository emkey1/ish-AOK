//
//  LLMChatViewController.m
//  iSH-AOK
//
//  The Workspace LLM Chat screen: transcript, prompt, and the tool loop.
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
#if __has_include("libiSH_AOKApp-Swift.h")
#import "libiSH_AOKApp-Swift.h" // AOKFoundationModelsBridge (Swift, iOS 26+ FoundationModels wrapper)
#endif
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "kernel/init.h" // run_guest_command_capture (guest-shell tool)

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

@implementation LLMClientViewController {
    UIStackView *_toolbarStackView;
    UITableView *_transcriptTable;
    NSArray<NSNumber *> *_visibleMessageIndices; // indices into _messages, skipping role=="tool"
    UILabel *_emptyStateLabel; // shown over the table when there's nothing to display yet
    LLMPromptTextView *_promptField;
    UILabel *_promptPlaceholderLabel; // UITextView has no built-in placeholder
    UIButton *_sendButton;
    NSMutableArray<NSDictionary<NSString *, id> *> *_messages;
    NSURLSessionDataTask *_activeTask;
    int _activeStreamFD; // raw socket fd of an in-flight direct-HTTP stream, 0 if none; Stop shuts it down to unblock recv()
    BOOL _cancelled; // set by Stop; checked before continuing a streaming/tool-loop/Apple FM request
    BOOL _autoRunCommandsThisReply; // skip per-command confirm for the current reply
    NSMutableDictionary<NSString *, NSNumber *> *_commandDecisionsThisReply; // Apple FM only: command text -> boxed ISHLLMToolRunDecision, so a repeat call for the same command isn't re-prompted
    BOOL _autoRunCommandsThisChat;  // skip per-command confirm until the chat is cleared
    NSString *_guestEnvironmentNote; // cached distro/tool probe for the tool system prompt
    NSString *_guestHomeDirectory; // the tool account's $HOME, from the same probe; the default working directory
    ISHLLMToolContext *_toolContext; // this chat's working directory and the files its model has read
    NSString *_projectInstructions; // AGENTS.md (or CLAUDE.md) found from the working directory, reloaded per prompt
    NSString *_projectInstructionsSource; // its path
    UILabel *_statusLabel;
    UIActivityIndicatorView *_activityIndicator;
    NSMutableSet<NSNumber *> *_expandedThinkingIndices; // indices into _messages whose <think> block the user expanded
    BOOL _streamingThinkingOpen; // the in-flight reply is currently inside an unterminated <think>, drives the status line
    NSInteger _knownContextWindowTokens; // 0 = unknown; best-effort from /models, see probeContextWindowIfNeeded
    NSString *_knownContextWindowProbeKey; // "model|endpoint" the value above was probed for; re-probes when it changes
    BOOL _contextWindowProbeInFlight;
    NSString *_sessionID; // the chat currently on screen; _messages is its content
    NSString *_sessionTitle;
    NSString *_sessionSystemPrompt; // per-chat system message, "" for none
    BOOL _sessionTitleIsAutomatic; // still derived from the first user turn, so it keeps following it
    UIButton *_chatsButton; // titled with the session, so the visible chat is always named
    UIButton *_destinationButton; // titled with the destination, one tap to switch
    UIBarButtonItem *_chatsBarButtonItem;
    UIBarButtonItem *_destinationBarButtonItem;
    NSLayoutConstraint *_toolbarHeightConstraint; // collapsed to 0 when the navigation bar already carries these controls
    void (^_pendingIdleAction)(void); // a chat/destination switch waiting for the in-flight reply to land
    BOOL _sending; // authoritative in-flight flag; see -isBusy
    NSURLSessionDataTask *_auxiliaryTask; // /models probes, kept out of _activeTask so Stop still owns the reply
    double _lastKnownSessionUpdate; // "updated" stamp this instance last wrote, to spot another window's edits
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

    _messages = [NSMutableArray array];
    _commandDecisionsThisReply = [NSMutableDictionary dictionary];
    _toolContext = [ISHLLMToolContext new];
    _expandedThinkingIndices = [NSMutableSet set];
    // Opens the chat that was last selected; on the first run in this build
    // that is the migrated pre-sessions transcript (see
    // ISHLLMLoadSessionIndexDocument).
    [self loadSessionWithID:ISHLLMActiveSessionID()];

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

    // Status row: a spinner + label so the connection/work state is always visible
    // (and so a stall is obvious instead of looking like a silent hang).
    _activityIndicator = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _activityIndicator.hidesWhenStopped = YES;
    _statusLabel = [UILabel new];
    _statusLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    _statusLabel.numberOfLines = 1;
    _statusLabel.adjustsFontSizeToFitWidth = YES;
    _statusLabel.minimumScaleFactor = 0.8;
    if (@available(iOS 13.0, *))
        _statusLabel.textColor = UIColor.secondaryLabelColor;
    UIStackView *statusRow = [[UIStackView alloc] initWithArrangedSubviews:@[_activityIndicator, _statusLabel]];
    statusRow.translatesAutoresizingMaskIntoConstraints = NO;
    statusRow.axis = UILayoutConstraintAxisHorizontal;
    statusRow.alignment = UIStackViewAlignmentCenter;
    statusRow.spacing = 6.0;
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
        [statusRow.trailingAnchor constraintLessThanOrEqualToAnchor:safeArea.trailingAnchor constant:-12.0],
        [statusRow.bottomAnchor constraintEqualToAnchor:inputBar.topAnchor constant:-2.0],

        [inputBar.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [inputBar.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        [inputBar.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-8.0],
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

    [self refreshTranscript];
    [self updateChatHeaderTitles];
    [self setStatus:[self idleStatusText] busy:NO];
    if (self.initialPrompt.length > 0) {
        [self.view layoutIfNeeded]; // give _promptField a real width before sizing it to this initial text
        [self setPromptFieldText:self.initialPrompt];
    }
}

- (void)dealloc {
    [_activeTask cancel];
    if (_activeStreamFD > 0)
        shutdown(_activeStreamFD, SHUT_RDWR);
#if __has_include("libiSH_AOKApp-Swift.h")
    [AOKFoundationModelsBridge cancelActiveRequest];
#endif
}

// LLM Settings is pushed from here, and several of its switches change how the
// existing transcript renders (Hide Thinking) or what the status line says
// (model, provider), so re-render on the way back instead of only at load.
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self updateToolbarVisibility];
    [self reloadSessionIfChangedElsewhere];
    [self refreshTranscript];
    [self updateChatHeaderTitles]; // Settings can have changed the destination
    if (!_activityIndicator.isAnimating)
        [self setStatus:[self idleStatusText] busy:NO];
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
    NSString *sessionTitle = _sessionTitle.length > 0 ? _sessionTitle : @"New Chat";
    NSString *destinationName = ISHLLMDestinationDisplayName(ISHLLMActiveDestination());
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

// Deliberately NOT derived from the spinner. Two backends -- Apple Foundation
// Models and the OpenAI tool loop -- run without an NSURLSessionTask or a
// socket fd, so the spinner was the only evidence they were working, and
// several ordinary actions (saving a system prompt, adding a destination)
// legitimately refresh the status line and would stop it mid-reply. A switch
// guarded on that would then walk straight past a live request.
- (BOOL)isBusy {
    return _sending || _activeTask != nil || _activeStreamFD != 0;
}

// Switching chats or destinations mid-reply would let the in-flight response
// land in a transcript it doesn't belong to: the streaming paths hold a bare
// index into _messages, and the completion handlers append through self, so
// swapping _messages under them corrupts the chat that is switched TO.
//
// Rather than epoch-stamping every completion path (there are seven, and
// missing one is silent corruption), the switch is queued and run from
// -setSending:NO -- the single funnel every path already ends in. Stopping
// therefore lets the current reply finish landing in its own chat, and only
// then does the swap happen. Returns YES if the caller's work was deferred.
- (BOOL)confirmSwitchWhileBusyWithAction:(NSString *)actionTitle continuation:(void (^)(void))continuation {
    if (![self isBusy])
        return NO;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Still replying"
                                                                  message:[NSString stringWithFormat:@"A reply is still coming in. Stop it before you %@?", actionTitle]
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Stop and Continue" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        // The reply can land while this alert is on screen -- a long answer
        // finishing during the two seconds it takes to read the dialog is the
        // common case, not a corner one. -setSending:NO has then already run
        // and will not run again, so a queued action would wait forever and
        // the Stop would leave _cancelled set to mark the NEXT reply as
        // user-cancelled. Both handlers are main-queue, so this check cannot
        // race a completion: just go, nothing is in flight.
        if (![self isBusy]) {
            continuation();
            return;
        }
        self->_pendingIdleAction = [continuation copy];
        [self stopGenerating:nil];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
    return YES;
}

// Everything that has to be true for _messages to describe the chat named by
// _sessionID. Called from viewDidLoad and on every switch -- if a per-chat
// piece of state is added later, it belongs here.
- (void)loadSessionWithID:(NSString *)sessionID {
    NSDictionary<NSString *, id> *entry = ISHLLMSessionEntryWithID(sessionID);
    if (entry == nil) {
        entry = ISHLLMSessionEntryWithID(ISHLLMActiveSessionID());
        if (entry == nil)
            entry = ISHLLMCreateSession(nil);
    }
    _sessionID = ISHLLMStringValue(entry, @"id");
    _sessionTitle = ISHLLMStringValue(entry, @"title");
    _sessionSystemPrompt = ISHLLMStringValue(entry, @"system");
    _sessionTitleIsAutomatic = ![entry[@"titleIsCustom"] boolValue];
    _lastKnownSessionUpdate = [entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0;
    [_messages setArray:ISHLLMLoadSessionMessages(_sessionID)];

    // Reopening a chat puts it back on the destination it was last used with,
    // which is what makes "each chat keeps its own destination" true rather
    // than just recorded. A destination since deleted leaves the current one.
    NSString *sessionDestinationID = ISHLLMStringValue(entry, @"destination");
    if (sessionDestinationID.length > 0) {
        for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
            if ([ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:sessionDestinationID]) {
                ISHLLMActivateDestination(destination);
                break;
            }
        }
    }

    // Per-chat state that must not survive the switch. The auto-run grants in
    // particular are a safety decision the user made about one conversation --
    // carrying them into another would run commands they never approved.
    _autoRunCommandsThisChat = NO;
    _autoRunCommandsThisReply = NO;
    [_commandDecisionsThisReply removeAllObjects];
    // Which files the model has read is part of the conversation too: a read
    // in another chat is no licence to write here.
    _toolContext = [ISHLLMToolContext new];
    NSString *workingDirectory = ISHLLMStringValue(entry, @"workingDirectory");
    _toolContext.workingDirectory = workingDirectory.length > 0 ? workingDirectory : _guestHomeDirectory;
    [_expandedThinkingIndices removeAllObjects]; // indices into _messages, which just changed
    _streamingThinkingOpen = NO;
    _cancelled = NO;
    // _guestEnvironmentNote is a property of the guest, not of the chat, so it
    // deliberately survives; the context-window probe re-keys itself off the
    // model and endpoint (see contextWindowProbeKey).
}

// The title/system-prompt half of -loadSessionWithID:, for when the chat on
// screen is the one that changed and its messages must be left alone.
- (void)reloadCurrentSessionMetadata {
    NSDictionary<NSString *, id> *entry = ISHLLMSessionEntryWithID(_sessionID);
    if (entry == nil)
        return;
    _sessionTitle = ISHLLMStringValue(entry, @"title");
    _sessionSystemPrompt = ISHLLMStringValue(entry, @"system");
    _sessionTitleIsAutomatic = ![entry[@"titleIsCustom"] boolValue];
    [self updateChatHeaderTitles];
}

// The chat can be open in two places at once -- a Workspace window and the
// terminal's modal -- and each holds its own copy of the messages, so whoever
// saves last would otherwise overwrite the other's turns wholesale. Coming
// back to a view whose chat has a newer stamp on disk than this instance last
// wrote, re-read it.
- (void)reloadSessionIfChangedElsewhere {
    if (_sessionID.length == 0 || [self isBusy])
        return;
    NSDictionary<NSString *, id> *entry = ISHLLMSessionEntryWithID(_sessionID);
    if (entry == nil) {
        // Deleted from the other window; fall back to whatever is selected now.
        [self loadSessionWithID:ISHLLMActiveSessionID()];
        return;
    }
    double updated = [entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0;
    if (updated > _lastKnownSessionUpdate)
        [self loadSessionWithID:_sessionID];
}

- (void)switchToSessionWithID:(NSString *)sessionID {
    if ([sessionID isEqualToString:_sessionID])
        return;
    // Weak in the deferred continuations: a request that never completes would
    // otherwise pin the view controller through _pendingIdleAction.
    __weak typeof(self) weakSelf = self;
    if ([self confirmSwitchWhileBusyWithAction:@"switch chats" continuation:^{ [weakSelf switchToSessionWithID:sessionID]; }])
        return;
    [self saveTranscript]; // flush the outgoing chat before its buffer is replaced
    ISHLLMSetActiveSessionID(sessionID);
    [self loadSessionWithID:sessionID];
    [self setSending:NO];
    [self refreshTranscript];
    [self updateChatHeaderTitles];
    [self setStatus:[self idleStatusText] busy:NO];
}

- (void)startNewChat {
    __weak typeof(self) weakSelf = self;
    if ([self confirmSwitchWhileBusyWithAction:@"start a new chat" continuation:^{ [weakSelf startNewChat]; }])
        return;
    [self saveTranscript];
    NSDictionary<NSString *, id> *entry = ISHLLMCreateSession(nil);
    [self loadSessionWithID:ISHLLMStringValue(entry, @"id")];
    [self setSending:NO];
    [self refreshTranscript];
    [self updateChatHeaderTitles];
    [self setStatus:[self idleStatusText] busy:NO];
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
        item.state = [entryID isEqualToString:_sessionID] ? UIMenuElementStateOn : UIMenuElementStateOff;
        [recent addObject:item];
    }

    UIAction *rename = [UIAction actionWithTitle:@"Rename Chat…"
                                           image:[UIImage systemImageNamed:@"pencil"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self renameCurrentChat]; }];
    UIAction *systemPrompt = [UIAction actionWithTitle:_sessionSystemPrompt.length > 0 ? @"System Prompt (set)…" : @"System Prompt…"
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
    workingDirectory.subtitle = _toolContext.workingDirectory;
    UIAction *summarize = [UIAction actionWithTitle:@"Summarize Chat"
                                              image:[UIImage systemImageNamed:@"text.redaction"]
                                         identifier:nil
                                            handler:^(__unused UIAction *action) { [self compactConversation]; }];
    summarize.subtitle = @"Send the model a summary instead of the history";
    UIAction *changes = [UIAction actionWithTitle:@"Changes…"
                                            image:[UIImage systemImageNamed:@"plusminus"]
                                       identifier:nil
                                          handler:^(__unused UIAction *action) { [self showChanges]; }];
    NSUInteger changeCount = _toolContext.changes.count;
    changes.subtitle = changeCount == 0 ? @"No file changes yet (also /changes)" : [NSString stringWithFormat:@"%lu file change%@ · /undo reverts the last", (unsigned long) changeCount, changeCount == 1 ? @"" : @"s"];
    UIMenu *currentSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[rename, systemPrompt, workingDirectory, changes, summarize, clear, delete]];
    return @[newChat, browse, switchSection, currentSection];
}

- (void)showChatList {
    LLMChatSessionListViewController *listViewController = [LLMChatSessionListViewController new];
    listViewController.currentSessionID = _sessionID;
    __weak typeof(self) weakSelf = self;
    listViewController.sessionSelected = ^(NSString *sessionID) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (sessionID.length == 0) {
            [self startNewChat]; // the list's compose button; creating it here keeps the busy check
            return;
        }
        if ([sessionID isEqualToString:self->_sessionID]) {
            // Renamed (or deleted-and-replaced by itself) in the list. Refresh
            // only the metadata: a full load would re-read _messages from disk
            // and throw away a reply still streaming into it.
            [self reloadCurrentSessionMetadata];
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

- (void)renameCurrentChat {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename Chat" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = self->_sessionTitle;
        textField.placeholder = @"Chat name";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *title = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        // Clearing the name hands the chat back to automatic titling.
        self->_sessionTitleIsAutomatic = title.length == 0;
        self->_sessionTitle = title.length > 0 ? title : ISHLLMSessionTitleFromMessages(self->_messages);
        ISHLLMUpdateSessionEntry(self->_sessionID, @{@"title": self->_sessionTitle ?: @"", @"titleIsCustom": @(!self->_sessionTitleIsAutomatic)});
        [self updateChatHeaderTitles];
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
        textField.text = self->_sessionSystemPrompt;
        textField.placeholder = @"You are a concise assistant…";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeSentences;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        self->_sessionSystemPrompt = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
        ISHLLMUpdateSessionEntry(self->_sessionID, @{@"system": self->_sessionSystemPrompt});
        [self updateChatHeaderTitles];
        [self refreshIdleStatus];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)deleteCurrentChat {
    __weak typeof(self) weakSelf = self;
    if ([self confirmSwitchWhileBusyWithAction:@"delete this chat" continuation:^{ [weakSelf deleteCurrentChat]; }])
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Delete Chat"
                                                                  message:[NSString stringWithFormat:@"Delete “%@” and its saved messages? This can't be undone.", _sessionTitle.length > 0 ? _sessionTitle : @"New Chat"]
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        NSString *nextSessionID = ISHLLMDeleteSession(self->_sessionID);
        [self loadSessionWithID:nextSessionID];
        [self setSending:NO];
        [self refreshTranscript];
        [self updateChatHeaderTitles];
        [self setStatus:[self idleStatusText] busy:NO];
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (NSArray<UIMenuElement *> *)destinationMenuElements {
    NSArray<NSDictionary<NSString *, NSString *> *> *destinations = ISHLLMDestinations();
    NSString *activeID = ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID);
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

    UIAction *chooseModel = [UIAction actionWithTitle:@"Choose Model…"
                                                image:[UIImage systemImageNamed:@"cube"]
                                           identifier:nil
                                              handler:^(__unused UIAction *action) { [self queryModelsInTranscript]; }];
    UIAction *addDestination = [UIAction actionWithTitle:@"Add Destination…"
                                                   image:[UIImage systemImageNamed:@"plus"]
                                              identifier:nil
                                                 handler:^(__unused UIAction *action) { [self addDestinationFromPreset]; }];
    UIAction *manage = [UIAction actionWithTitle:@"Manage Destinations…"
                                           image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                                      identifier:nil
                                         handler:^(__unused UIAction *action) { [self showDestinationList]; }];
    UIMenu *switchSection = [UIMenu menuWithTitle:@"Chat With" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:items];
    UIMenu *manageSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[chooseModel, addDestination, manage]];
    return @[switchSection, manageSection];
}

- (void)switchToDestinationWithID:(NSString *)destinationID {
    if ([ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID) isEqualToString:destinationID])
        return;
    __weak typeof(self) weakSelf = self;
    if ([self confirmSwitchWhileBusyWithAction:@"switch destinations" continuation:^{ [weakSelf switchToDestinationWithID:destinationID]; }])
        return;
    for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
        if (![ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:destinationID])
            continue;
        ISHLLMActivateDestination(destination);
        // The chat remembers what it last talked to, so reopening it later
        // comes back on the same destination.
        ISHLLMUpdateSessionEntry(_sessionID, @{@"destination": destinationID});
        [self updateChatHeaderTitles];
        [self refreshTranscript];
        [self setStatus:[self idleStatusText] busy:NO];
        return;
    }
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
            // Selecting it goes through the same guard as any other switch, so
            // a reply already in flight isn't retargeted mid-request.
            [self switchToDestinationWithID:destination[kISHLLMDestinationID]];
            if (ISHLLMProviderRequiresAPIKey() && ![self isBusy])
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

- (void)showDestinationList {
    LLMDestinationListViewController *listViewController = [LLMDestinationListViewController new];
    __weak typeof(self) weakSelf = self;
    listViewController.destinationsChanged = ^{
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        [self updateChatHeaderTitles];
        [self refreshTranscript];
        [self refreshIdleStatus];
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
    UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
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

// This is also the only "start a new session" affordance: every backend is
// effectively stateless per call (a fresh NSURLSession request, or a fresh
// LanguageModelSession for Apple Foundation Models -- see
// -appleFoundationModelsPromptWithHistory), and reconstructs its notion of
// the conversation from _messages each time. Emptying it -- plus cancelling
// anything in flight and dropping the per-chat approvals/guest-probe cache
// below -- is a genuine clean slate, not just a visual clear.
- (void)clearTranscript:(id)sender {
    (void) sender;
    // Emptying _messages is the same hazard as switching chats: an in-flight
    // reply appends through a stale index, and the _cancelled reset below
    // would swallow the Stop that a queued switch is waiting on. So it takes
    // the same deferral.
    __weak typeof(self) weakSelf = self;
    if ([self confirmSwitchWhileBusyWithAction:@"clear this chat" continuation:^{ [weakSelf clearTranscript:nil]; }])
        return;
    // Cancel anything in flight directly rather than via -stopGenerating: --
    // that sets _cancelled=YES for a completion handler to consume and reset;
    // with nothing in flight to call one, it would stick and wrongly mark
    // the *next* reply as user-cancelled.
    [_activeTask cancel];
    if (_activeStreamFD > 0)
        shutdown(_activeStreamFD, SHUT_RDWR);
#if __has_include("libiSH_AOKApp-Swift.h")
    [AOKFoundationModelsBridge cancelActiveRequest];
#endif
    _cancelled = NO;
    [self setSending:NO];
    [_messages removeAllObjects];
    [_expandedThinkingIndices removeAllObjects]; // indices are into _messages, which just emptied
    _autoRunCommandsThisChat = NO; // a fresh chat re-arms per-command confirmation
    _autoRunCommandsThisReply = NO;
    [_commandDecisionsThisReply removeAllObjects];
    [_toolContext forgetReads]; // the model starts over, so must read again before writing
    _guestEnvironmentNote = nil; // re-probe the guest on the next tool-enabled reply
    [self saveTranscript];
    [self refreshTranscript];
    [self updateChatHeaderTitles]; // an automatically-titled chat goes back to "New Chat"
}

- (NSString *)latestAssistantMessage {
    for (NSDictionary<NSString *, id> *message in _messages.reverseObjectEnumerator) {
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
    [self appendRole:@"assistant" content:message];
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
        [self appendRole:@"assistant" content:@"Created example prompt templates in /AOK/persist/llm-prompts."];
    }];
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sender];
}

- (void)appendRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content}];
    [self saveTranscript];
    [self refreshTranscript];
}

- (void)appendLocalRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content, @"local": @"1"}];
    [self saveTranscript];
    [self refreshTranscript];
}

- (BOOL)messageIsLocalOnly:(NSDictionary<NSString *, id> *)message {
    if ([message[@"local"] isEqual:@"1"])
        return YES;
    NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
    NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    if ([role isEqualToString:@"user"] && [content hasPrefix:@"/"])
        return YES;
    if ([role isEqualToString:@"assistant"]) {
        if ([content hasPrefix:@"Model set to "] || [content hasPrefix:@"Current model:"] ||
            [content hasPrefix:@"Model query failed:"] || [content hasPrefix:@"No models found"] ||
            [content hasPrefix:@"Invalid models URL"] || [content containsString:@" models returned by "])
            return YES;
    }
    return NO;
}

// Everything from the latest summary on; what came before it reaches the
// model only as that summary. The transcript on screen keeps all of it.
- (NSArray<NSDictionary<NSString *, id> *> *)messagesSentToModel {
    for (NSInteger i = (NSInteger) _messages.count - 1; i >= 0; i--) {
        if ([_messages[(NSUInteger) i][@"compacted"] isEqual:@"1"])
            return [_messages subarrayWithRange:NSMakeRange((NSUInteger) i, _messages.count - (NSUInteger) i)];
    }
    return _messages;
}

- (NSArray<NSDictionary<NSString *, id> *> *)providerMessages {
    // Which tool results are recent enough to send in full: walk them newest
    // to oldest, keeping full content until the running estimate would blow
    // the budget (sized against the real context window when known -- see
    // toolResultContextBudgetTokens). Always keep at least the single most
    // recent result in full, even if it alone exceeds the budget. Identity
    // (not equality) keyed, since two results can have identical content.
    NSInteger budget = [self toolResultContextBudgetTokens];
    NSMutableSet<NSValue *> *fullContentToolMessages = [NSMutableSet set];
    NSInteger runningTokens = 0;
    NSArray<NSDictionary<NSString *, id> *> *live = [self messagesSentToModel];
    for (NSDictionary<NSString *, id> *message in live.reverseObjectEnumerator) {
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        if (![role isEqualToString:@"tool"])
            continue;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        NSInteger tokens = ISHLLMEstimateTokenCount(content);
        if (runningTokens + tokens > budget && fullContentToolMessages.count > 0)
            break;
        runningTokens += tokens;
        [fullContentToolMessages addObject:[NSValue valueWithNonretainedObject:message]];
    }

    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    // This chat's own standing instructions lead every request. Stored on the
    // session rather than in the transcript so editing it applies to the whole
    // conversation, retroactively, the way a system prompt is expected to.
    if (_sessionSystemPrompt.length > 0)
        [messages addObject:@{@"role": @"system", @"content": _sessionSystemPrompt}];
    for (NSDictionary<NSString *, id> *message in live) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : nil;
        if (role.length == 0)
            continue;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        // A tool result: the model needs the matching tool_call_id to thread it.
        // Compact everything not in fullContentToolMessages to its one-line summary.
        if ([role isEqualToString:@"tool"]) {
            NSString *toolCallID = [message[@"tool_call_id"] isKindOfClass:NSString.class] ? message[@"tool_call_id"] : nil;
            if (toolCallID.length == 0)
                continue;
            BOOL keepFull = [fullContentToolMessages containsObject:[NSValue valueWithNonretainedObject:message]];
            NSString *sentContent = content;
            if (!keepFull) {
                NSString *summary = [message[@"summary"] isKindOfClass:NSString.class] ? message[@"summary"] : nil;
                sentContent = [NSString stringWithFormat:@"(output omitted to save context: %@. Call the tool again if you need it.)",
                    summary.length > 0 ? summary : @"result compacted"];
            }
            [messages addObject:@{@"role": @"tool", @"tool_call_id": toolCallID, @"content": sentContent}];
            continue;
        }
        // An assistant turn that requested tools: keep tool_calls; content may be
        // empty, which is valid alongside tool_calls and must not be dropped.
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        if (toolCalls.count > 0) {
            NSMutableDictionary *entry = [@{@"role": role, @"content": content, @"tool_calls": toolCalls} mutableCopy];
            // Only the Anthropic translation reads it; an OpenAI-compatible
            // server could reject an unknown field.
            if (ISHLLMUsesAnthropicAPI() && [message[kISHLLMAnthropicContentKey] isKindOfClass:NSArray.class])
                entry[kISHLLMAnthropicContentKey] = message[kISHLLMAnthropicContentKey];
            [messages addObject:entry];
            continue;
        }
        if (content.length > 0)
            [messages addObject:@{@"role": role, @"content": content}];
    }
    return messages;
}

// Writes the open chat and refreshes its index entry. Called after every
// append, so it stays the "the transcript on disk matches the screen" point
// it always was -- it just writes a per-session file now.
- (void)saveTranscript {
    if (_sessionID.length == 0)
        return;
    // A chat deleted from the list (here or in another window) has had its file
    // removed already; writing it back would resurrect the transcript as an
    // orphan no index entry points at.
    if (ISHLLMSessionEntryWithID(_sessionID) == nil)
        return;
    ISHLLMWriteSessionMessages(_sessionID, _messages);

    NSMutableDictionary<NSString *, id> *updates = [NSMutableDictionary dictionary];
    updates[@"updated"] = @(NSDate.date.timeIntervalSince1970);
    updates[@"count"] = @(_messages.count);
    // An untitled chat keeps following its first user turn, so a new chat
    // names itself as soon as it is used; a renamed one is left alone.
    BOOL titleChanged = NO;
    if (_sessionTitleIsAutomatic) {
        NSString *derived = ISHLLMSessionTitleFromMessages(_messages);
        NSString *title = derived.length > 0 ? derived : @"New Chat";
        if (![title isEqualToString:_sessionTitle]) {
            _sessionTitle = title;
            updates[@"title"] = title;
            titleChanged = YES;
        }
    }
    _lastKnownSessionUpdate = [updates[@"updated"] doubleValue];
    ISHLLMUpdateSessionEntry(_sessionID, updates);
    if (titleChanged)
        [self updateChatHeaderTitles]; // after the write, so the menu reads the new title back
}

// A message gets its own bubble/row if it has visible content, or a
// tool-call caption to show, or it's the trailing in-progress streaming
// placeholder (empty content until the first chunk arrives). Tool-role
// messages are never shown -- their output still goes to the model, just
// not the screen.
- (void)recomputeVisibleMessageIndices {
    NSMutableArray<NSNumber *> *indices = [NSMutableArray array];
    for (NSUInteger i = 0; i < _messages.count; i++) {
        NSDictionary<NSString *, id> *message = _messages[i];
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        if ([role isEqualToString:@"tool"])
            continue;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        BOOL hasCommandNote = [self commandCountForMessage:message] > 0;
        BOOL isTrailingStreamingPlaceholder = (i == _messages.count - 1) && [role isEqualToString:@"assistant"];
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
            NSString *model = UserPreferences.shared.llmModel;
            _emptyStateLabel.text = [NSString stringWithFormat:@"%@\n\nDestination: %@%@%@\n\n%@",
                                      _messages.count > 0 ? @"Nothing to show yet." : @"Send a prompt to start this chat.",
                                      ISHLLMDestinationDisplayName(ISHLLMActiveDestination()),
                                      model.length > 0 ? [@"\nModel: " stringByAppendingString:model] : @"\nNo model set — pick one from the destination menu.",
                                      _sessionSystemPrompt.length > 0 ? @"\nSystem prompt set for this chat." : @"",
                                      [self toolsSummaryText]];
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
    if (messageIndex >= _messages.count)
        return;
    NSDictionary<NSString *, id> *message = _messages[messageIndex];
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

// Expansion is keyed by index into _messages, which only ever grows by
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

// Names the phase in the status row while a reply streams in: "Thinking…"
// whenever the model is inside an unterminated <think> block, "Working…"
// (setSending's default) otherwise. Only meaningful when the blocks are being
// hidden -- with them shown inline the reasoning is already on screen.
- (void)updateStreamingThinkingStatusForContent:(NSString *)content {
    if (!ISHLLMHideThinkingEnabled())
        return;
    BOOL open = NO;
    NSMutableArray<NSString *> *thoughts = [NSMutableArray array];
    (void) ISHLLMSplitThinkingFromContent(content ?: @"", thoughts, &open);
    if (open == _streamingThinkingOpen)
        return;
    _streamingThinkingOpen = open;
    [self setStatus:(open ? @"Thinking…" : @"Working…") busy:YES];
}

- (void)setSending:(BOOL)sending {
    _sending = sending;
    _sendButton.enabled = YES; // stays tappable while sending -- it becomes Stop
    _promptField.editable = !sending;
    [_sendButton setTitle:(sending ? @"Stop" : @"Send") forState:UIControlStateNormal];
    _sendButton.accessibilityLabel = sending ? @"Stop generating" : @"Send";
    [_sendButton removeTarget:self action:NULL forControlEvents:UIControlEventTouchUpInside];
    [_sendButton addTarget:self action:(sending ? @selector(stopGenerating:) : @selector(sendPrompt:)) forControlEvents:UIControlEventTouchUpInside];
    _streamingThinkingOpen = NO; // each reply starts outside a <think> block
    if (sending) {
        [self setStatus:@"Working…" busy:YES];
        return;
    }
    [self setStatus:[self idleStatusText] busy:NO];
    // A chat or destination switch the user asked for while a reply was still
    // arriving (see -confirmSwitchWhileBusyWithAction:continuation:). Run it
    // one turn later: several handlers call setSending:NO *before* appending
    // the reply they just received, and that reply belongs to the outgoing
    // chat.
    if (_pendingIdleAction != nil) {
        void (^action)(void) = _pendingIdleAction;
        _pendingIdleAction = nil;
        dispatch_async(dispatch_get_main_queue(), action);
    }
}

// Cancels whichever request is currently in flight: an NSURLSession task, a
// raw-socket direct-HTTP stream (shut down from here to unblock its blocking
// recv()), the OpenAI tool loop (checked at its next checkpoint -- the
// in-flight HTTP call itself can't be interrupted mid-request), or an Apple
// Foundation Models generation. Whatever partial text has streamed in stays
// on screen; only further progress stops.
- (void)stopGenerating:(id)sender {
    (void) sender;
    _cancelled = YES;
    [_activeTask cancel];
    [_auxiliaryTask cancel];
    if (_activeStreamFD > 0)
        shutdown(_activeStreamFD, SHUT_RDWR);
#if __has_include("libiSH_AOKApp-Swift.h")
    [AOKFoundationModelsBridge cancelActiveRequest];
#endif
}

// Status row driver. busy spins the indicator; the text names the current phase
// so the connection state is always visible and a stall is obvious.
- (void)setStatus:(NSString *)text busy:(BOOL)busy {
    _statusLabel.text = text ?: @"";
    if (busy)
        [_activityIndicator startAnimating];
    else
        [_activityIndicator stopAnimating];
}

// Status refresh for actions that can happen WHILE a reply is arriving (saving
// a system prompt, managing destinations): they must not paint "Ready" over a
// live phase caption.
- (void)refreshIdleStatus {
    if (![self isBusy])
        [self setStatus:[self idleStatusText] busy:NO];
}

- (NSString *)idleStatusText {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return @"Apple Foundation Models";
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0)
        return @"Set a model in Settings";
    return [NSString stringWithFormat:@"Ready · %@%@", model, [self contextUsageStatusSuffix]];
}

// " · ~4.2K/32K ctx" once there's a conversation to estimate, "~4.2K ctx"
// if the model's real context window isn't known (see
// probeContextWindowIfNeeded), or "" before anything has been sent.
- (NSString *)contextUsageStatusSuffix {
    NSInteger used = ISHLLMEstimateMessagesTokenCount([self providerMessages]);
    if (used <= 0)
        return @"";
    NSString *usedText = ISHLLMFormattedTokenCount(used);
    if (_knownContextWindowTokens > 0)
        return [NSString stringWithFormat:@" · ~%@/%@ ctx", usedText, ISHLLMFormattedTokenCount(_knownContextWindowTokens)];
    return [NSString stringWithFormat:@" · ~%@ ctx", usedText];
}

// True (and handled) if `error` is the result of the user hitting Stop,
// rather than a real failure -- shows a quiet "(stopped)" note instead of
// "Request failed: ...cancelled...".
- (BOOL)handleUserCancelledError:(NSError *)error {
    if (error == nil || !(error.domain == NSURLErrorDomain && error.code == NSURLErrorCancelled) || !_cancelled)
        return NO;
    _cancelled = NO;
    [self appendRole:@"assistant" content:@"(stopped)"];
    return YES;
}

- (void)handleLLMResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
    if ([self handleUserCancelledError:error])
        return;
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *choices = dict[@"choices"];
        NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
        content = ISHLLMSanitizedAssistantContent(content ?: @"");
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = dict[@"error"][@"message"];
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        content = [NSString stringWithFormat:@"Unexpected response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
}

- (void)handleGeminiResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
    if ([self handleUserCancelledError:error])
        return;
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *candidates = dict[@"candidates"];
        NSDictionary *candidate = candidates.count > 0 && [candidates[0] isKindOfClass:NSDictionary.class] ? candidates[0] : nil;
        NSDictionary *candidateContent = [candidate[@"content"] isKindOfClass:NSDictionary.class] ? candidate[@"content"] : nil;
        NSArray *parts = [candidateContent[@"parts"] isKindOfClass:NSArray.class] ? candidateContent[@"parts"] : nil;
        NSMutableString *text = [NSMutableString string];
        for (id part in parts) {
            if ([part isKindOfClass:NSDictionary.class] && [part[@"text"] isKindOfClass:NSString.class])
                [text appendString:part[@"text"]];
        }
        content = ISHLLMSanitizedAssistantContent(text);
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = dict[@"error"][@"message"];
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        content = [NSString stringWithFormat:@"Unexpected Gemini response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
}

// Apple Foundation Models streams cumulative snapshots (not deltas like the
// OpenAI-compatible SSE path), so the in-progress message is overwritten
// rather than appended to.
- (void)setStreamingAssistantContent:(NSString *)content atMessageIndex:(NSUInteger)index {
    if (index >= _messages.count)
        return;
    NSMutableDictionary<NSString *, NSString *> *message = [_messages[index] mutableCopy];
    message[@"content"] = content ?: @"";
    _messages[index] = message;
    [self updateStreamingThinkingStatusForContent:message[@"content"]];
    [self reloadLastRowAndScroll:YES];
}

- (void)appendStreamingAssistantChunk:(NSString *)chunk toMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count || chunk.length == 0)
        return;
    NSMutableDictionary<NSString *, NSString *> *message = [_messages[index] mutableCopy];
    NSString *content = ISHLLMStreamingAssistantContent([message[@"content"] ?: @"" stringByAppendingString:chunk]);
    message[@"content"] = content;
    _messages[index] = message;
    [self updateStreamingThinkingStatusForContent:content];
    [self reloadLastRowAndScroll:YES];
}

// End-of-stream cleanup: collapse trailing whitespace the streaming accumulator
// intentionally preserved, so the saved transcript matches the non-streaming path.
- (void)finalizeStreamingAssistantMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count)
        return;
    NSMutableDictionary<NSString *, NSString *> *message = [_messages[index] mutableCopy];
    NSString *finalized = ISHLLMSanitizedAssistantContent(message[@"content"] ?: @"");
    if ([finalized isEqualToString:message[@"content"]])
        return;
    message[@"content"] = finalized;
    _messages[index] = message;
    [self reloadLastRowAndScroll:NO];
}

- (void)appendModelListFromData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
    _auxiliaryTask = nil;
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model query failed: %@", error.localizedDescription]];
        return;
    }
    NSArray<NSString *> *models = ISHLLMModelIdentifiersFromResponseData(data);
    if (models.count == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        NSString *message = statusCode > 0 ? [NSString stringWithFormat:@"No models found. HTTP %ld", (long) statusCode] : @"No models found.";
        if (raw.length > 0)
            message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
        [self appendLocalRole:@"assistant" content:message];
        return;
    }
    NSUInteger limit = MIN(models.count, 80);
    NSMutableString *message = [NSMutableString stringWithFormat:@"%lu models returned by %@:", (unsigned long) models.count, ISHLLMModelsEndpoint()];
    for (NSUInteger i = 0; i < limit; i++)
        [message appendFormat:@"\n- %@", models[i]];
    if (models.count > limit)
        [message appendFormat:@"\nShowing first %lu of %lu.", (unsigned long) limit, (unsigned long) models.count];
    [self appendLocalRole:@"assistant" content:message];
}

- (void)queryModelsInTranscript {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current on-device model: %@\n%@", UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"system-language-model", ISHLLMAppleFoundationModelsUnavailableMessage()]];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        [self appendLocalRole:@"assistant" content:@"Invalid models URL."];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendLocalRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
        return;
    }
    [self setSending:YES];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self appendModelListFromData:data statusCode:statusCode error:error];
            });
        });
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    ISHLLMApplyAuthHeaders(request, apiKey);
    // Not _activeTask: that slot belongs to the reply, and a probe parked in it
    // both survives the request (nothing nils it, so the chat reads as busy
    // forever) and displaces a streaming reply that Stop would then miss.
    __weak typeof(self) weakSelf = self;
    _auxiliaryTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelListFromData:data statusCode:http.statusCode error:error];
        });
    }];
    [_auxiliaryTask resume];
}

- (void)appendModelLoadResultWithModel:(NSString *)model data:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
    _auxiliaryTask = nil;
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but load failed: %@", model, error.localizedDescription]];
        return;
    }
    if (statusCode >= 200 && statusCode < 300) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Provider accepted a warm-up request.", model]];
        return;
    }
    NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
    NSString *message = [NSString stringWithFormat:@"Model set to %@, but provider returned HTTP %ld.", model, (long) statusCode];
    if (raw.length > 0)
        message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
    [self appendLocalRole:@"assistant" content:message];
}

- (void)setAndLoadModelFromCommand:(NSString *)command {
    NSString *model = [[command substringFromIndex:@"/model".length] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0) {
        NSString *current = UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"not set";
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current model: %@\nUsage: /model <model-name>", current]];
        return;
    }
    UserPreferences.shared.llmModel = model;
    ISHLLMSyncActiveDestinationFromPreferences();
    [self updateChatHeaderTitles];
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Apple Foundation Models uses the system on-device model when available. %@", model, ISHLLMAppleFoundationModelsUnavailableMessage()]];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. %@", model, ISHLLMMissingAPIKeyMessage()]];
        return;
    }

    NSURL *url = ISHLLMProbeURL();
    if (url == nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but the provider URL is invalid.", model]];
        return;
    }
    NSDictionary *body = ISHLLMProbeBody(model, @"Reply with ok.", 1);
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    [self setSending:YES];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self appendModelLoadResultWithModel:model data:data statusCode:statusCode error:error];
            });
        });
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    ISHLLMApplyAuthHeaders(request, apiKey);
    request.HTTPBody = bodyData;
    __weak typeof(self) weakSelf = self;
    _auxiliaryTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelLoadResultWithModel:model data:data statusCode:http.statusCode error:error];
        });
    }];
    [_auxiliaryTask resume];
}

#if __has_include("libiSH_AOKApp-Swift.h")
// Reinstalled at the start of every tools-enabled request (cheap -- just
// reassigning a closure). AOKFoundationModelsBridge.shellCommandHandler is
// process-wide static state, so leaving a stale one from a since-closed chat
// window installed forever would mean a later chat's tool calls silently hit
// the "chat window closed" fallback below instead of ever running.
- (void)installAppleFoundationModelsShellHandler {
    __weak typeof(self) weakSelf = self;
    AOKFoundationModelsBridge.shellCommandHandler = ^(NSString *command, void (^handlerCompletion)(NSString *)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil) {
                handlerCompletion(@"The chat window closed before this command could run.");
                return;
            }
            [self runAppleFoundationModelsShellCommand:command completion:handlerCompletion];
        });
    };
}

// Mirrors runToolCalls: (this is the OpenAI tool-loop equivalent), but for a
// single command with no tool_call_id bookkeeping -- FoundationModels drives
// the call sequencing internally. FoundationModels' streamResponse has been
// observed to invoke a tool more than once for what is logically a single
// call (e.g. if it restarts generation mid-stream); _commandDecisionsThisReply
// remembers this reply's decision per exact command text so a repeat doesn't
// re-prompt or re-run work the user already approved or declined.
- (void)runAppleFoundationModelsShellCommand:(NSString *)command completion:(void (^)(NSString *))completion {
    __weak typeof(self) weakSelf = self;
    void (^recordAndComplete)(NSString *, NSString *) = ^(NSString *resultText, NSString *summary) {
        typeof(self) self = weakSelf;
        if (self != nil) {
            [self->_messages addObject:@{
                @"role": @"tool",
                @"name": @"run_shell",
                @"content": resultText ?: @"",
                @"summary": summary.length > 0 ? summary : (resultText ?: @""),
            }];
            [self refreshTranscript];
            [self saveTranscript];
        }
        completion(resultText ?: @"");
    };
    NSDictionary *toolCall = @{
        @"id": NSUUID.UUID.UUIDString,
        @"function": @{@"name": @"run_shell", @"arguments": @{@"command": command ?: @""}},
    };
    ISHLLMToolInvocation *invocation = [ISHLLMToolInvocation invocationWithToolCall:toolCall context:_toolContext];

    if (command.length > 0) {
        NSNumber *priorDecision = _commandDecisionsThisReply[command];
        if (priorDecision != nil) {
            if (priorDecision.integerValue == ISHLLMToolRunDecline) {
                recordAndComplete(@"The user declined to run this command.", @"declined by user (repeat request)");
            } else {
                [self setStatus:@"Running command…" busy:YES];
                ISHLLMRunToolInvocation(invocation, _toolContext, recordAndComplete);
            }
            return;
        }
    }
    [self performToolInvocation:invocation decision:^(BOOL approved) {
        typeof(self) self = weakSelf;
        if (self != nil && command.length > 0)
            self->_commandDecisionsThisReply[command] = @(approved ? ISHLLMToolRunOnce : ISHLLMToolRunDecline);
    } completion:recordAndComplete];
}
#endif

// respond(to:)/streamResponse(to:) take a single prompt with no memory of
// earlier calls -- each one starts a brand-new LanguageModelSession. Unlike
// the OpenAI-compatible path (which sends the full message array every
// request), Apple Foundation Models has no equivalent per-call history
// parameter here, so the prior turns are flattened into the prompt text
// itself; without this, every reply is answered with zero awareness that
// the conversation before it ever happened. Kept to a rough character
// budget, most recent turns first, so a long chat doesn't overflow
// FoundationModels' small (~4096 token) context window.
- (NSString *)appleFoundationModelsPromptWithHistory {
    NSMutableArray<NSString *> *turns = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *message in _messages) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        if (content.length == 0 || [role isEqualToString:@"tool"] || [role isEqualToString:@"system"])
            continue;
        NSString *label = [role isEqualToString:@"assistant"] ? @"Assistant" : @"User";
        [turns addObject:[NSString stringWithFormat:@"%@: %@", label, content]];
    }
    NSUInteger budget = 6000;
    NSMutableArray<NSString *> *kept = [NSMutableArray array];
    NSUInteger total = 0;
    for (NSString *turn in turns.reverseObjectEnumerator) {
        total += turn.length;
        if (total > budget && kept.count > 0)
            break;
        [kept insertObject:turn atIndex:0];
    }
    // This backend takes one flat prompt, so the chat's system prompt leads it
    // rather than riding as a separate message. It is never trimmed away.
    if (_sessionSystemPrompt.length > 0)
        [kept insertObject:[NSString stringWithFormat:@"Instructions: %@", _sessionSystemPrompt] atIndex:0];
    return [kept componentsJoinedByString:@"\n\n"];
}

- (void)sendPromptToAppleFoundationModels:(NSString *)prompt {
    if (!ISHLLMFoundationModelsReady()) {
        [self appendRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
        return;
    }
#if __has_include("libiSH_AOKApp-Swift.h")
    BOOL toolsEnabled = UserPreferences.shared.llmToolsEnabled;
    if (toolsEnabled) {
        _autoRunCommandsThisReply = NO; // a new prompt re-arms confirmation for this reply
        [_commandDecisionsThisReply removeAllObjects];
        [self installAppleFoundationModelsShellHandler];
    }
    __weak typeof(self) weakSelf = self;
    void (^startRequest)(void) = ^{
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        NSString *instructions = [@"The prompt is this conversation so far, formatted as alternating \"User:\"/\"Assistant:\" turns. Continue it naturally as the Assistant, responding only to the latest User message -- the earlier turns are context, not something to repeat back."
            stringByAppendingString:toolsEnabled ? [@" " stringByAppendingString:ISHLLMToolSystemNote(self->_guestEnvironmentNote, self->_toolContext.workingDirectory, NO)] : @""];
        NSString *promptWithHistory = [self appleFoundationModelsPromptWithHistory];
        [self setSending:YES];
        [self->_messages addObject:@{@"role": @"assistant", @"content": @""}];
        NSUInteger streamingIndex = self->_messages.count - 1;
        [self refreshTranscript];
        [AOKFoundationModelsBridge streamResponseToPrompt:promptWithHistory.length > 0 ? promptWithHistory : prompt
                                              instructions:instructions
                                              toolsEnabled:toolsEnabled
                                                 onPartial:^(NSString *partial) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self setStreamingAssistantContent:partial atMessageIndex:streamingIndex];
            });
        }
                                                completion:^(NSString *finalText, NSString *errorMessage) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self == nil)
                    return;
                [self setSending:NO];
                if (self->_cancelled) {
                    self->_cancelled = NO;
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex]; // keep whatever streamed in before Stop
                } else if (finalText.length == 0 && errorMessage.length > 0) {
                    [self setStreamingAssistantContent:[NSString stringWithFormat:@"Request failed: %@", errorMessage] atMessageIndex:streamingIndex];
                } else {
                    [self setStreamingAssistantContent:ISHLLMSanitizedAssistantContent(finalText ?: @"") atMessageIndex:streamingIndex];
                }
                [self saveTranscript];
            });
        }];
    };
    if (toolsEnabled)
        [self prepareGuestEnvironmentNoteThen:startRequest];
    else
        startRequest();
#else
    [self appendRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
#endif
}

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
    if ([prompt isEqualToString:@"/compact"]) {
        [self setPromptFieldText:@""];
        [self compactConversation];
        return;
    }
    if ([prompt isEqualToString:@"/models"]) {
        [self setPromptFieldText:@""];
        [self appendLocalRole:@"user" content:prompt];
        [self queryModelsInTranscript];
        return;
    }
    if ([prompt isEqualToString:@"/model"] || [prompt hasPrefix:@"/model "]) {
        [self setPromptFieldText:@""];
        [self appendLocalRole:@"user" content:prompt];
        [self setAndLoadModelFromCommand:prompt];
        return;
    }
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self setPromptFieldText:@""];
        [self appendRole:@"user" content:prompt];
        [self sendPromptToAppleFoundationModels:prompt];
        return;
    }
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0) {
        [self appendRole:@"assistant" content:@"Set an LLM model in Settings before sending a prompt."];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
        return;
    }

    [self setPromptFieldText:@""];
    [self appendRole:@"user" content:prompt];
    [self probeContextWindowIfNeeded];
    [self setSending:YES];

    // A conversation about to outgrow the model's window is summarised
    // first, as OpenCode does, rather than failing or losing its start.
    // Only when the window is known: guessing would compact chats that fit.
    if ([self shouldCompactBeforeSending]) {
        __weak typeof(self) weakSelf = self;
        [self summarizeConversationKeepingLastMessage:YES then:^(__unused BOOL ok) {
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            if (self->_cancelled) {
                self->_cancelled = NO;
                [self setSending:NO];
                return;
            }
            [self dispatchPromptWithModel:model apiKey:apiKey];
        }];
        return;
    }
    [self dispatchPromptWithModel:model apiKey:apiKey];
}

// The part of -sendPrompt: after the prompt is on the transcript: pick the
// transport (tool loop, Gemini, streaming) and send.
- (void)dispatchPromptWithModel:(NSString *)model apiKey:(NSString *)apiKey {
    // Guest-shell tool use (OpenAI-compatible only): runs a non-streaming
    // function-calling loop so the model can run commands in the iSH shell.
    // Anthropic's Messages API always goes through this loop, with or
    // without tools: it is where the Messages translation lives.
    if (ISHLLMUsesAnthropicAPI() && !UserPreferences.shared.llmToolsEnabled) {
        [self runToolLoopRound:0 model:model apiKey:apiKey];
        return;
    }
    if (!ISHLLMUsesGeminiAPI() && UserPreferences.shared.llmToolsEnabled) {
        _autoRunCommandsThisReply = NO; // a new prompt re-arms confirmation for this reply
        __weak typeof(self) weakSelf = self;
        [self prepareGuestEnvironmentNoteThen:^{
            // MCP servers' tools join the built-in ones; a server that could
            // not connect is named in the chat and left out.
            [ISHLLMMCPManager.shared prepareWithCompletion:^(NSArray<NSString *> *problems) {
                typeof(self) self = weakSelf;
                if (self == nil)
                    return;
                if (problems.count > 0)
                    [self appendLocalRole:@"assistant" content:[problems componentsJoinedByString:@"\n"]];
                [self runToolLoopRound:0 model:model apiKey:apiKey];
            }];
        }];
        return;
    }

    if (ISHLLMUsesGeminiAPI()) {
        NSURL *geminiURL = [NSURL URLWithString:ISHLLMGeminiGenerateEndpoint()];
        if (geminiURL == nil) {
            [self appendRole:@"assistant" content:@"Invalid Gemini server URL."];
            [self setSending:NO];
            return;
        }
        NSMutableArray<NSDictionary<NSString *, id> *> *contents = [NSMutableArray array];
        for (NSDictionary<NSString *, id> *message in [self providerMessages]) {
            NSString *messageRole = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
            if ([messageRole isEqualToString:@"system"])
                continue; // carried in system_instruction below, not as a turn
            NSString *role = [messageRole isEqualToString:@"assistant"] ? @"model" : @"user";
            NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
            if (content.length > 0)
                [contents addObject:@{@"role": role, @"parts": @[@{@"text": content}]}];
        }
        NSMutableDictionary *body = [@{@"contents": contents} mutableCopy];
        if (_sessionSystemPrompt.length > 0)
            body[@"system_instruction"] = @{@"parts": @[@{@"text": _sessionSystemPrompt}]};
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:geminiURL];
        request.HTTPMethod = @"POST";
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        __weak typeof(self) weakSelf = self;
        _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self handleGeminiResponseData:data response:response error:error];
            });
        }];
        [_activeTask resume];
        return;
    }

    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    ISHLLMApplyAuthHeaders(request, apiKey);

    // Both transports stream now: http through the raw socket (ATS blocks it
    // from NSURLSession), https through the data-task delegate below.
    BOOL useDirectHTTP = [[url.scheme lowercaseString] isEqualToString:@"http"];
    NSDictionary *body = @{
        @"model": model,
        @"messages": [self providerMessages],
        @"stream": @YES,
        @"stop": @[@"<file_sep>"],
    };
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];

    if (useDirectHTTP) {
        NSData *requestBody = request.HTTPBody;
        NSData *fallbackRequestBody = [NSJSONSerialization dataWithJSONObject:@{
            @"model": model,
            @"messages": [self providerMessages],
            @"stream": @NO,
            @"stop": @[@"<file_sep>"],
        } options:0 error:nil];
        NSString *requestAPIKey = apiKey;
        [_messages addObject:@{@"role": @"assistant", @"content": @""}];
        NSUInteger streamingIndex = _messages.count - 1;
        [self refreshTranscript];
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            typeof(self) self = weakSelf; // held strong for the duration of the blocking call below, so its fd stays valid for Stop to shut down
            if (self == nil)
                return;
            NSInteger statusCode = 0;
            NSError *directError = nil;
            __block BOOL receivedChunk = NO;
            BOOL streamed = ISHLLMDirectHTTPPostStreaming(url, requestBody, requestAPIKey, ^(NSString *chunk) {
                receivedChunk = YES;
                dispatch_async(dispatch_get_main_queue(), ^{
                    typeof(self) self = weakSelf;
                    if (self != nil)
                        [self appendStreamingAssistantChunk:chunk toMessageAtIndex:streamingIndex];
                });
            }, &self->_activeStreamFD, &statusCode, &directError);
            if (self->_cancelled) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    typeof(self) self = weakSelf;
                    if (self == nil)
                        return;
                    self->_cancelled = NO;
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                    [self setSending:NO];
                    [self saveTranscript];
                });
                return;
            }
            NSData *responseBody = nil;
            if (!streamed || !receivedChunk)
                responseBody = ISHLLMDirectHTTPPost(url, fallbackRequestBody, requestAPIKey, &statusCode, &directError);
            NSHTTPURLResponse *directResponse = nil;
            if (statusCode > 0) {
                directResponse = [[NSHTTPURLResponse alloc] initWithURL:url
                                                             statusCode:statusCode
                                                            HTTPVersion:@"HTTP/1.1"
                                                           headerFields:nil];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self == nil)
                    return;
                if (streamed && receivedChunk && directError == nil) {
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                    [self setSending:NO];
                    [self saveTranscript];
                    return;
                }
                if (streamingIndex < self->_messages.count)
                    [self->_messages removeObjectAtIndex:streamingIndex];
                [self handleLLMResponseData:responseBody response:directResponse error:directError];
            });
        });
        return;
    }

    // https: the tokens arrive as Server-Sent Events on an NSURLSession data
    // task delegate. If none arrive -- a non-200, or a server that ignores
    // "stream": true and answers with one JSON body -- the buffered response
    // goes to the ordinary handler instead, so nothing regresses to a worse
    // outcome than the single-shot request this replaces.
    [_messages addObject:@{@"role": @"assistant", @"content": @""}];
    NSUInteger streamingIndex = _messages.count - 1;
    [self refreshTranscript];
    __weak typeof(self) weakSelf = self;
    ISHLLMStreamingResponseDelegate *streamDelegate = [ISHLLMStreamingResponseDelegate new];
    streamDelegate.chunkHandler = ^(NSString *chunk) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendStreamingAssistantChunk:chunk toMessageAtIndex:streamingIndex];
        });
    };
    streamDelegate.completionHandler = ^(BOOL receivedChunks, NSData *responseBody, NSInteger statusCode, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            self->_activeTask = nil;
            if (receivedChunks) {
                // Whatever streamed in stays on screen, including when the user
                // hit Stop or the connection dropped part way -- the same
                // contract the direct-socket path has.
                BOOL userStopped = self->_cancelled;
                self->_cancelled = NO;
                [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                if (error != nil && !userStopped)
                    [self appendRole:@"assistant" content:[NSString stringWithFormat:@"(stream interrupted: %@)", error.localizedDescription]];
                [self setSending:NO];
                [self saveTranscript];
                return;
            }
            if (streamingIndex < self->_messages.count)
                [self->_messages removeObjectAtIndex:streamingIndex];
            // An endpoint that rejects "stream": true outright used never to be
            // asked, so give it the single-shot request it used to get rather
            // than turning a working configuration into an error.
            if (error == nil && statusCode >= 400 && !self->_cancelled) {
                [self retryWithoutStreamingRequest:request];
                return;
            }
            NSHTTPURLResponse *streamResponse = nil;
            if (statusCode > 0) {
                streamResponse = [[NSHTTPURLResponse alloc] initWithURL:url
                                                             statusCode:statusCode
                                                            HTTPVersion:@"HTTP/1.1"
                                                           headerFields:nil];
            }
            [self handleLLMResponseData:responseBody response:streamResponse error:error];
        });
    };
    NSURLSession *streamSession = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                                delegate:streamDelegate
                                                           delegateQueue:nil];
    _activeTask = [streamSession dataTaskWithRequest:request];
    [_activeTask resume];
}

// Re-issues the request that just came back as an HTTP error with streaming
// switched off. Only reached once per prompt (this path is a plain completion
// handler, so a second failure surfaces as the error it is).
- (void)retryWithoutStreamingRequest:(NSURLRequest *)streamingRequest {
    NSMutableURLRequest *request = [streamingRequest mutableCopy];
    NSMutableDictionary *body = [[NSJSONSerialization JSONObjectWithData:streamingRequest.HTTPBody ?: NSData.data options:0 error:nil] mutableCopy];
    if (![body isKindOfClass:NSMutableDictionary.class]) {
        [self appendRole:@"assistant" content:@"Could not encode the request."];
        [self setSending:NO];
        return;
    }
    body[@"stream"] = @NO;
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    [self setStatus:@"Retrying without streaming…" busy:YES];
    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self handleLLMResponseData:data response:response error:error];
        });
    }];
    [_activeTask resume];
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
    _promptPlaceholderLabel.hidden = _promptField.text.length > 0;
    CGSize fitSize = [_promptField sizeThatFits:CGSizeMake(_promptField.bounds.size.width, CGFLOAT_MAX)];
    _promptField.scrollEnabled = fitSize.height > [self promptFieldMaxHeight];
}

- (void)setPromptFieldText:(NSString *)text {
    _promptField.text = text ?: @"";
    [self promptFieldTextDidChange];
}

// MARK: - Guest-shell tool loop
//
// One "round" = one non-streaming chat request that advertises the run_shell
// tool. If the model answers with tool calls we run each command in the guest,
// append the results as `tool` messages, and start another round; otherwise the
// round's content is the final answer. Bounded by ISHLLMToolMaxRounds().


// Run the distro/tool probe at most once per chat session, then continue. The
// note is injected as a system message so the model's tool use matches the
// actual guest (Alpine/BusyBox vs Debian/Devuan/glibc, curl vs wget, ...).
- (void)prepareGuestEnvironmentNoteThen:(void (^)(void))continuation {
    // Kick the distro/tool probe off in the background but DON'T block the model
    // request on it. A slow or wedged guest probe must never delay or hang the
    // chat -- the first request may go without the env note; later requests pick
    // it up once it's ready. (The current-time anchor is added separately, so the
    // request always has that regardless of the probe.)
    if (_guestEnvironmentNote == nil) {
        _guestEnvironmentNote = @""; // mark in-flight so we only probe once per chat
        __weak typeof(self) weakSelf = self;
        dispatch_async(ISHLLMGuestCommandQueue(), ^{
            NSString *home = nil;
            NSString *note = ISHLLMDetectGuestEnvironmentNote(&home);
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self == nil)
                    return;
                if (note.length > 0)
                    self->_guestEnvironmentNote = note;
                if (home.length > 0) {
                    self->_guestHomeDirectory = home;
                    if (self->_toolContext.workingDirectory.length == 0)
                        self->_toolContext.workingDirectory = home;
                }
            });
        });
    }
    [self loadProjectInstructionsThen:continuation];
}

// AGENTS.md is read again for every prompt, so an edit to it (by the user,
// or by the model) applies from the next message. The bridge answers in
// milliseconds; if it does not answer within two seconds the prompt goes
// without the instructions rather than waiting on it.
- (void)loadProjectInstructionsThen:(void (^)(void))continuation {
    NSString *workingDirectory = _toolContext.workingDirectory;
    __block BOOL finished = NO;
    __weak typeof(self) weakSelf = self;
    void (^finish)(NSString *, NSString *, BOOL) = ^(NSString *text, NSString *source, BOOL loaded) {
        if (finished)
            return;
        finished = YES;
        typeof(self) self = weakSelf;
        if (self != nil && loaded) {
            self->_projectInstructions = text;
            self->_projectInstructionsSource = source;
        }
        continuation();
    };
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *source = nil;
        NSString *text = ISHLLMLoadProjectInstructions(workingDirectory, &source);
        dispatch_async(dispatch_get_main_queue(), ^{
            finish(text, source, YES);
        });
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(nil, nil, NO);
    });
}

// "model|models-endpoint" -- re-probes whenever either changes (switching
// models or servers), even if the model name is reused across providers.
- (NSString *)contextWindowProbeKey {
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [NSString stringWithFormat:@"%@|%@", model, ISHLLMModelsEndpoint()];
}

// Best-effort, once-per-model-selection probe of the model's real context
// window via the /models endpoint (see ISHLLMContextWindowFromModelsResponse
// for which providers actually expose this). Silent and non-blocking, same
// rule as the guest-env probe: a slow or unsupported provider must never
// delay or hang the chat. If nothing usable comes back we just keep using
// the conservative default budget in toolResultContextBudgetTokens.
- (void)probeContextWindowIfNeeded {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return;
    NSString *probeKey = [self contextWindowProbeKey];
    if (_contextWindowProbeInFlight || [probeKey isEqualToString:_knownContextWindowProbeKey])
        return;
    _contextWindowProbeInFlight = YES;
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    __weak typeof(self) weakSelf = self;
    ISHLLMFetchModelsDataAsync(^(NSData *data, NSInteger statusCode, NSError *error) {
        (void) statusCode;
        NSInteger tokens = error == nil ? ISHLLMContextWindowFromModelsResponse(data, model) : 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            self->_contextWindowProbeInFlight = NO;
            self->_knownContextWindowProbeKey = probeKey; // remember we tried, even if 0 (unknown) -- don't hammer a provider that just doesn't expose it
            self->_knownContextWindowTokens = tokens;
            if (!self->_activityIndicator.isAnimating)
                [self setStatus:[self idleStatusText] busy:NO];
        });
    });
}

// Token budget reserved for FULL (non-compacted) tool-result content when
// resending history to the model -- see providerMessages. Sized against the
// real context window when known; a fixed conservative default otherwise.
- (NSInteger)toolResultContextBudgetTokens {
    if (_knownContextWindowTokens > 0)
        return MAX(1024, (NSInteger) (_knownContextWindowTokens * kISHLLMToolContextBudgetFraction));
    return kISHLLMToolContextDefaultBudgetTokens;
}

- (void)runToolLoopRound:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        _cancelled = NO;
        [self appendRole:@"assistant" content:@"(stopped)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }
    if (round >= ISHLLMToolMaxRounds()) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Stopped after %ld tool calls in a row (adjustable in Settings as \"Tool Call Rounds\"). Send another message to continue.", (long) ISHLLMToolMaxRounds()]];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }

    [self setStatus:(round == 0 ? @"Contacting model…" : @"Thinking…") busy:YES];
    if (ISHLLMUsesAnthropicAPI()) {
        [self runAnthropicRound:round model:model apiKey:apiKey];
        return;
    }
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    NSString *systemNote = ISHLLMToolSystemNote(_guestEnvironmentNote, _toolContext.workingDirectory, YES);
    if (_projectInstructions.length > 0)
        systemNote = [systemNote stringByAppendingFormat:@"\n\nProject instructions from %@ -- follow them:\n\n%@", _projectInstructionsSource, _projectInstructions];
    if (systemNote.length > 0)
        [messages addObject:@{@"role": @"system", @"content": systemNote}];
    [messages addObjectsFromArray:[self providerMessages]];
    NSDictionary *body = @{
        @"model": model,
        @"messages": messages,
        @"stream": @NO,
        @"tools": ISHLLMChatToolDefinitions(),
        @"stop": @[@"<file_sep>"],
    };
    if ([NSJSONSerialization dataWithJSONObject:body options:0 error:nil] == nil) {
        [self appendRole:@"assistant" content:@"Could not encode the request."];
        [self setSending:NO];
        return;
    }
    [self streamRoundToURL:url body:body anthropic:NO round:round model:model apiKey:apiKey];
}

// The non-streaming request, for a server that refused the streamed one.
- (void)sendRoundWithoutStreamingToURL:(NSURL *)url body:(NSDictionary *)body anthropic:(BOOL)anthropic
                                 round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    NSMutableDictionary *plain = [body mutableCopy];
    [plain removeObjectForKey:@"stream"];
    if (!anthropic)
        plain[@"stream"] = @NO;
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:plain options:0 error:nil];
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSInteger statusCode = 0;
        NSError *error = nil;
        NSData *data = anthropic ? ISHLLMAnthropicPost(plain, apiKey, &statusCode, &error)
                                 : ISHLLMSynchronousChatPost(url, bodyData, apiKey, &statusCode, &error);
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            [self handleToolRoundData:data statusCode:statusCode error:error round:round model:model apiKey:apiKey];
        });
    });
}

// One round of the tool loop, streamed: the reply's text appears in a
// placeholder bubble as it arrives, then the assembled message -- the same
// shape a non-streaming response has -- goes through -handleToolRoundData:
// exactly as before, so tool calls, permissions and saving are unchanged.
// Stop cancels the request itself (the task, or the socket). A server that
// answers the streamed request with an HTTP error gets one plain retry.
- (void)streamRoundToURL:(NSURL *)url body:(NSDictionary *)body anthropic:(BOOL)anthropic
                   round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    NSMutableDictionary *streamed = [body mutableCopy];
    streamed[@"stream"] = @YES;
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:streamed options:0 error:nil];
    [_messages addObject:@{@"role": @"assistant", @"content": @""}];
    NSUInteger placeholder = _messages.count - 1;
    [self refreshTranscript];

    ISHLLMOpenAIStreamAssembler *openAI = anthropic ? nil : [ISHLLMOpenAIStreamAssembler new];
    ISHLLMAnthropicStreamAssembler *claude = anthropic ? [ISHLLMAnthropicStreamAssembler new] : nil;
    __weak typeof(self) weakSelf = self;
    void (^payloadHandler)(NSString *) = ^(NSString *payload) {
        NSString *text = anthropic ? [claude consumePayload:payload] : [openAI consumePayload:payload];
        if (text.length == 0)
            return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf appendStreamingAssistantChunk:text toMessageAtIndex:placeholder];
        });
    };
    // Runs on the transport's thread, after the last payload.
    void (^finished)(NSData *, NSInteger, NSError *) = ^(NSData *plainBody, NSInteger statusCode, NSError *error) {
        BOOL sawEvents = anthropic ? claude.sawEvents : openAI.sawEvents;
        NSData *assembled = nil;
        if (sawEvents) {
            NSDictionary *response = anthropic ? claude.response : ({
                NSDictionary *message = openAI.responseMessage;
                message[@"error"] != nil ? @{@"error": message[@"error"]} : @{@"choices": @[@{@"message": message}]};
            });
            assembled = [NSJSONSerialization dataWithJSONObject:response options:0 error:nil];
            if (anthropic && !claude.finished && error == nil)
                error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNetworkConnectionLost
                                        userInfo:@{NSLocalizedDescriptionKey: @"the reply stream ended early"}];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            self->_activeTask = nil;
            NSString *partial = placeholder < self->_messages.count ? ISHLLMStringValue(self->_messages[placeholder], @"content") : @"";
            if (placeholder < self->_messages.count)
                [self->_messages removeObjectAtIndex:placeholder];
            if (self->_cancelled) {
                // What arrived before Stop stays, as the streaming chat does.
                self->_cancelled = NO;
                [self appendRole:@"assistant" content:partial.length > 0 ? [partial stringByAppendingString:@"\n\n(stopped)"] : @"(stopped)"];
                [self setSending:NO];
                [self saveTranscript];
                return;
            }
            if (!sawEvents && error == nil && statusCode >= 400) {
                [self refreshTranscript];
                [self sendRoundWithoutStreamingToURL:url body:body anthropic:anthropic round:round model:model apiKey:apiKey];
                return;
            }
            [self handleToolRoundData:sawEvents ? assembled : plainBody statusCode:statusCode error:error round:round model:model apiKey:apiKey];
        });
    };

    NSMutableDictionary<NSString *, NSString *> *extraHeaders = [NSMutableDictionary dictionary];
    if (anthropic && body[@"fallbacks"] != nil)
        extraHeaders[@"anthropic-beta"] = @"server-side-fallback-2026-07-01";
    if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            typeof(self) self = weakSelf; // held for the call, so Stop can shut its socket
            if (self == nil)
                return;
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSMutableData *plainBody = [NSMutableData data];
            ISHLLMDirectHTTPPostStreamingPayloads(url, bodyData, apiKey, extraHeaders, payloadHandler,
                                                  &self->_activeStreamFD, &statusCode, plainBody, &error);
            finished(plainBody, statusCode, error);
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [request setValue:@"text/event-stream" forHTTPHeaderField:@"Accept"];
    ISHLLMApplyAuthHeaders(request, apiKey);
    for (NSString *name in extraHeaders)
        [request setValue:extraHeaders[name] forHTTPHeaderField:name];
    request.HTTPBody = bodyData;
    ISHLLMRawStreamDelegate *delegate = [ISHLLMRawStreamDelegate new];
    delegate.payloadHandler = payloadHandler;
    delegate.completionHandler = finished;
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.defaultSessionConfiguration;
    // A model can think for minutes before the first byte of its answer.
    configuration.timeoutIntervalForRequest = 600;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:nil];
    _activeTask = [session dataTaskWithRequest:request];
    [_activeTask resume];
}

// One Messages API request. The stable half of the system note (tool
// guidance, environment, AGENTS.md) is cached with the tools; the clock
// follows the cache breakpoint. The reply arrives as the same assistant
// message shape the OpenAI path produces, so everything after is shared.
- (void)runAnthropicRound:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    BOOL tools = UserPreferences.shared.llmToolsEnabled;
    NSString *stable = nil;
    if (tools) {
        stable = ISHLLMToolSystemNoteWithoutClock(_guestEnvironmentNote, _toolContext.workingDirectory, YES);
        if (_projectInstructions.length > 0)
            stable = [stable stringByAppendingFormat:@"\n\nProject instructions from %@ -- follow them:\n\n%@", _projectInstructionsSource, _projectInstructions];
    }
    // Streamed, so a long answer shows as it is written; 32000 leaves room
    // for thinking that a non-streaming request's timeout would not.
    NSDictionary *body = ISHLLMAnthropicRequestBody(model, 32000, [self providerMessages], stable, ISHLLMClockNote(),
                                                    tools ? ISHLLMChatToolDefinitions() : nil);
    [self streamRoundToURL:[NSURL URLWithString:ISHLLMAnthropicMessagesEndpoint()] body:body anthropic:YES
                     round:round model:model apiKey:apiKey];
}

- (void)handleToolRoundData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        _cancelled = NO;
        [self appendRole:@"assistant" content:@"(stopped)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        [self setSending:NO];
        return;
    }
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSDictionary *dict = [json isKindOfClass:NSDictionary.class] ? json : nil;
    NSArray *choices = [dict[@"choices"] isKindOfClass:NSArray.class] ? dict[@"choices"] : nil;
    NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
    NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
    NSString *anthropicNote = nil;
    if (ISHLLMUsesAnthropicAPI() && dict != nil) {
        NSString *anthropicError = nil;
        message = ISHLLMAnthropicMessageFromResponse(dict, &anthropicError, &anthropicNote);
        if (message == nil) {
            [self appendRole:@"assistant" content:anthropicError ?: @"Unexpected response from the Anthropic API."];
            [self setSending:NO];
            [self saveTranscript];
            return;
        }
    }
    if (message == nil) {
        NSString *errorMessage = [dict[@"error"] isKindOfClass:NSDictionary.class] && [dict[@"error"][@"message"] isKindOfClass:NSString.class]
            ? dict[@"error"][@"message"] : nil;
        if (errorMessage.length == 0) {
            NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
            errorMessage = [NSString stringWithFormat:@"Unexpected response%@%@",
                statusCode > 0 ? [NSString stringWithFormat:@" (%ld)", (long) statusCode] : @"",
                raw.length > 0 ? [@": " stringByAppendingString:(raw.length > 400 ? [raw substringToIndex:400] : raw)] : @"."];
        }
        [self appendRole:@"assistant" content:errorMessage];
        [self setSending:NO];
        return;
    }

    NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    NSArray<NSDictionary *> *toolCalls = ISHLLMValidToolCalls(message);
    if (toolCalls.count == 0) {
        NSString *finalText = ISHLLMSanitizedAssistantContent(content);
        if (anthropicNote.length > 0)
            finalText = finalText.length > 0 ? [finalText stringByAppendingFormat:@"\n\n%@", anthropicNote] : anthropicNote;
        [self appendRole:@"assistant" content:finalText.length > 0 ? finalText : @"(The model returned an empty response.)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }

    // Record the assistant turn (provider needs it paired with the tool results),
    // then execute each requested command.
    NSMutableDictionary *turn = [@{
        @"role": @"assistant",
        @"content": ISHLLMSanitizedAssistantContent(content),
        @"tool_calls": toolCalls,
    } mutableCopy];
    // Sent back verbatim next round: thinking blocks must return unchanged.
    if ([message[kISHLLMAnthropicContentKey] isKindOfClass:NSArray.class])
        turn[kISHLLMAnthropicContentKey] = message[kISHLLMAnthropicContentKey];
    [_messages addObject:turn];
    [self refreshTranscript];
    [self saveTranscript];
    [self runToolCalls:toolCalls index:0 round:round model:model apiKey:apiKey];
}

- (void)runToolCalls:(NSArray<NSDictionary *> *)toolCalls index:(NSUInteger)index round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        _cancelled = NO;
        [self appendRole:@"assistant" content:@"(stopped)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }
    if (index >= toolCalls.count) {
        [self runToolLoopRound:round + 1 model:model apiKey:apiKey];
        return;
    }
    ISHLLMToolInvocation *invocation = [ISHLLMToolInvocation invocationWithToolCall:toolCalls[index] context:_toolContext];
    __weak typeof(self) weakSelf = self;
    [self performToolInvocation:invocation decision:nil completion:^(NSString *resultText, NSString *summary) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        [self->_messages addObject:@{
            @"role": @"tool",
            @"tool_call_id": invocation.callID,
            @"name": invocation.name.length > 0 ? invocation.name : @"run_shell",
            @"content": resultText ?: @"",
            @"summary": summary.length > 0 ? summary : (resultText ?: @""),
        }];
        [self refreshTranscript];
        [self saveTranscript];
        if (self->_cancelled) {
            self->_cancelled = NO;
            [self appendRole:@"assistant" content:@"(stopped)"];
            [self setSending:NO];
            return;
        }
        [self runToolCalls:toolCalls index:index + 1 round:round model:model apiKey:apiKey];
    }];
}

- (NSString *)statusTextForRunningInvocation:(ISHLLMToolInvocation *)invocation {
    NSString *name = invocation.name;
    if ([name isEqualToString:@"read_file"])
        return @"Reading file…";
    if ([name isEqualToString:@"write_file"])
        return @"Writing file…";
    if ([name isEqualToString:@"edit_file"])
        return @"Editing file…";
    if ([name isEqualToString:@"list_directory"])
        return @"Listing directory…";
    if ([name isEqualToString:@"glob"] || [name isEqualToString:@"grep"])
        return @"Searching…";
    return @"Running command…";
}

// The one path every tool call takes, from either backend: the permission
// rules' answer, then the user's when the rules say ask, then the tool.
// `decision` (optional) hears whether it ran, for the Apple FM repeat guard.
- (void)performToolInvocation:(ISHLLMToolInvocation *)invocation
                     decision:(void (^)(BOOL approved))decision
                   completion:(void (^)(NSString *result, NSString *summary))completion {
    __weak typeof(self) weakSelf = self;
    ISHLLMToolContext *context = _toolContext;
    void (^run)(void) = ^{
        typeof(self) self = weakSelf;
        if (self != nil)
            [self setStatus:[self statusTextForRunningInvocation:invocation] busy:YES];
        ISHLLMRunToolInvocation(invocation, context, completion);
    };
    if (invocation.problem != nil) {
        run();
        return;
    }
    NSString *reason = nil;
    ISHLLMPermissionAction action = [invocation permissionWithReason:&reason];
    if (action == ISHLLMPermissionDeny) {
        if (decision != nil)
            decision(NO);
        completion([NSString stringWithFormat:@"Not run: the user's tool permissions refuse this (%@). Do not try to reach the same result another way; tell the user what you needed instead.",
                    reason ?: [NSString stringWithFormat:@"%@ is set to Deny", ISHLLMToolCategoryTitle(invocation.category)]],
                   @"refused by permissions");
        return;
    }
    if (action == ISHLLMPermissionAllow || _autoRunCommandsThisChat || _autoRunCommandsThisReply) {
        if (decision != nil)
            decision(YES);
        run();
        return;
    }
    [self setStatus:@"Waiting for approval…" busy:YES];
    [self confirmToolInvocation:invocation reason:reason completion:^(ISHLLMToolRunDecision choice) {
        typeof(self) self = weakSelf;
        if (self == nil) {
            completion(@"The chat window closed before this could run.", @"not run");
            return;
        }
        if (decision != nil)
            decision(choice != ISHLLMToolRunDecline);
        if (choice == ISHLLMToolRunDecline) {
            completion(invocation.category == ISHLLMToolCategoryShell ? @"The user declined to run this command." : @"The user declined this tool call.",
                       @"declined by user");
            return;
        }
        if (choice == ISHLLMToolRunAllowReply)
            self->_autoRunCommandsThisReply = YES;
        else if (choice == ISHLLMToolRunAllowChat)
            self->_autoRunCommandsThisChat = YES;
        run();
    }];
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

// The "always" choices save a permission, so later calls of the same kind do
// not ask at all: a shell rule for this command (see ISHLLMSuggestedShellRule
// for why compound lines get none), or the category's own setting for files.
- (void)confirmToolInvocation:(ISHLLMToolInvocation *)invocation reason:(NSString *)reason
                   completion:(void (^)(ISHLLMToolRunDecision decision))completion {
    BOOL shell = invocation.category == ISHLLMToolCategoryShell;
    NSString *message = invocation.confirmationMessage;
    if (reason.length > 0)
        message = [message stringByAppendingFormat:@"\n\nAsking because of %@.", reason];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:invocation.confirmationTitle
        message:message
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run" : @"Allow" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunOnce);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run, don't ask again this reply" : @"Allow, don't ask again this reply" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunAllowReply);
    }]];
    NSString *rule = shell ? ISHLLMSuggestedShellRule(invocation.command ?: @"") : nil;
    if (rule != nil) {
        [alert addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Always allow \u201c%@\u201d", rule] style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            ISHLLMAddShellRule(rule, ISHLLMPermissionAllow);
            completion(ISHLLMToolRunOnce);
        }]];
    } else if (!shell && reason.length == 0) {
        // Only when the category setting is what asked: an edit outside the
        // working directory asks whatever the setting says.
        ISHLLMToolCategory category = invocation.category;
        NSString *title = category == ISHLLMToolCategoryEdit ? @"Always allow file edits"
            : category == ISHLLMToolCategoryMCP ? @"Always allow MCP tools" : @"Always allow reading files";
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            ISHLLMSetCategoryAction(category, ISHLLMPermissionAllow);
            completion(ISHLLMToolRunOnce);
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Run, allow all this chat" : @"Allow all tools this chat" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [self confirmAutoRunAllForChatWithCompletion:completion];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:shell ? @"Don't Run" : @"Don't Allow" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunDecline);
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

#pragma mark - Working directory, summaries, what the tools can do

// One paragraph for the empty chat, so what the model can do here is on
// screen rather than three levels into Settings.
- (NSString *)toolsSummaryText {
    if (!UserPreferences.shared.llmToolsEnabled)
        return @"Tools are off: the model can only talk. Turn on Tools in LLM Settings to let it read, search and edit files and run commands.";
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return @"Tools: shell commands only (the on-device model's context is too small for the file tools).";
    if (ISHLLMUsesGeminiAPI())
        return @"Tools are not available with the Gemini API.";
    NSString *where = _toolContext.workingDirectory.length > 0 ? _toolContext.workingDirectory : @"your home directory";
    NSUInteger mcpServers = 0;
    for (NSDictionary *server in ISHLLMMCPServers())
        mcpServers += [server[@"enabled"] boolValue];
    NSString *mcp = mcpServers == 0 ? @"" : [NSString stringWithFormat:@" · MCP (%lu server%@): %@", (unsigned long) mcpServers,
                                             mcpServers == 1 ? @"" : @"s", ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryMCP))];
    return [NSString stringWithFormat:@"Tools: files and shell, working in %@.\nReading: %@ · Edits: %@ · Commands: %@%@\n/compact summarizes a long chat; /undo reverts the last file change; /mcp lists MCP servers.",
            where,
            ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryRead)),
            ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryEdit)),
            ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryShell)), mcp];
}

- (void)editWorkingDirectoryForCurrentChat {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Working Directory"
        message:@"Where this chat's commands start and its relative file paths point, and where AGENTS.md is looked for. File edits outside it always ask. Leave empty for the home directory."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = self->_toolContext.workingDirectory;
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
        _toolContext.workingDirectory = _guestHomeDirectory;
        ISHLLMUpdateSessionEntry(_sessionID, @{@"workingDirectory": @""});
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Working directory: %@ (the home directory).", _guestHomeDirectory ?: @"the home directory"]];
        return;
    }
    NSString *path = ISHLLMResolveGuestPath(raw, _toolContext.workingDirectory);
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge statAtGuestPath:path completion:^(ISHGuestFileItem *item, NSError *error) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (item == nil || item.kind != ISHGuestFileKindDirectory) {
            UIAlertController *failure = [UIAlertController alertControllerWithTitle:@"Not a directory"
                message:[NSString stringWithFormat:@"%@: %@", path, item == nil ? (error.localizedDescription ?: @"not found") : @"not a directory"]
                preferredStyle:UIAlertControllerStyleAlert];
            [failure addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
            [[self ish_presentationViewController] presentViewController:failure animated:YES completion:nil];
            return;
        }
        self->_toolContext.workingDirectory = path;
        ISHLLMUpdateSessionEntry(self->_sessionID, @{@"workingDirectory": path});
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Working directory: %@", path]];
    }];
}

- (BOOL)shouldCompactBeforeSending {
    if (_knownContextWindowTokens <= 0 || ISHLLMUsesGeminiAPI() || ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return NO;
    return ISHLLMEstimateMessagesTokenCount([self providerMessages]) > (NSInteger) (_knownContextWindowTokens * 0.75);
}

// "/compact" and the menu item.
- (void)compactConversation {
    if ([self isBusy])
        return;
    if (ISHLLMUsesGeminiAPI() || ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendLocalRole:@"assistant" content:@"Summarizing needs an OpenAI-compatible destination."];
        return;
    }
    [self setSending:YES];
    __weak typeof(self) weakSelf = self;
    [self summarizeConversationKeepingLastMessage:NO then:^(__unused BOOL ok) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        self->_cancelled = NO;
        [self setSending:NO];
    }];
}

// Asks the model for a summary of the history it is sent, then records it
// as a message marked "compacted": from then on the model gets the summary
// in place of everything before it (see -messagesSentToModel). With
// keepLastMessage the newest message -- the prompt about to be sent -- stays
// after the summary, not inside it.
- (void)summarizeConversationKeepingLastMessage:(BOOL)keepLastMessage then:(void (^)(BOOL ok))continuation {
    NSMutableArray<NSDictionary<NSString *, id> *> *history = [[self providerMessages] mutableCopy];
    if (keepLastMessage && history.count > 0)
        [history removeLastObject];
    NSUInteger conversational = 0;
    for (NSDictionary *message in history)
        conversational += ![message[@"role"] isEqual:@"system"];
    if (conversational < 2) {
        if (!keepLastMessage)
            [self appendLocalRole:@"assistant" content:@"Nothing to summarize yet."];
        continuation(NO);
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    [history addObject:@{@"role": @"user", @"content":
        @"Summarize this conversation so that it can be continued from the summary alone: what the user wants, "
        @"decisions made, files read or changed and their current state, commands run and results that still matter, "
        @"and what remains to do. Be complete but brief. Reply with the summary only."}];
    BOOL anthropic = ISHLLMUsesAnthropicAPI();
    NSDictionary *anthropicBody = anthropic ? ISHLLMAnthropicRequestBody(model ?: @"", 16000, history, nil, nil, nil) : nil;
    NSData *body = [NSJSONSerialization dataWithJSONObject:@{@"model": model ?: @"", @"messages": history, @"stream": @NO} options:0 error:nil];
    if (url == nil || body == nil) {
        [self appendLocalRole:@"assistant" content:@"Could not summarize: invalid server URL or request."];
        continuation(NO);
        return;
    }
    [self setStatus:@"Summarizing the conversation…" busy:YES];
    NSUInteger insertAt = keepLastMessage && _messages.count > 0 ? _messages.count - 1 : _messages.count;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSInteger statusCode = 0;
        NSError *error = nil;
        NSData *data = anthropic ? ISHLLMAnthropicPost(anthropicBody, apiKey, &statusCode, &error)
                                 : ISHLLMSynchronousChatPost(url, body, apiKey, &statusCode, &error);
        id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSArray *choices = [json isKindOfClass:NSDictionary.class] && [json[@"choices"] isKindOfClass:NSArray.class] ? json[@"choices"] : nil;
        NSDictionary *message = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0][@"message"] : nil;
        if (anthropic && [json isKindOfClass:NSDictionary.class])
            message = ISHLLMAnthropicMessageFromResponse(json, NULL, NULL);
        NSString *content = [message isKindOfClass:NSDictionary.class] && [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
        NSString *summary = [ISHLLMSanitizedAssistantContent(content ?: @"") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            if (summary.length == 0) {
                NSString *why = error.localizedDescription ?: (statusCode > 0 ? [NSString stringWithFormat:@"HTTP %ld", (long) statusCode] : @"empty reply");
                [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Could not summarize the conversation (%@); the full history is still sent.", why]];
                continuation(NO);
                return;
            }
            NSDictionary *entry = @{
                @"role": @"user",
                @"content": [@"Summary of the conversation so far (the messages before this are no longer sent to the model):\n\n" stringByAppendingString:summary],
                @"compacted": @"1",
            };
            [self->_messages insertObject:entry atIndex:MIN(insertAt, self->_messages.count)];
            [self saveTranscript];
            [self refreshTranscript];
            continuation(YES);
        });
    });
}

#pragma mark - Change review and undo

- (void)showChanges {
    LLMChangesViewController *list = [LLMChangesViewController new];
    list.toolContext = _toolContext;
    __weak typeof(self) weakSelf = self;
    list.revertRequested = ^(ISHLLMFileChange *change, UIViewController *presenter, void (^done)(void)) {
        [weakSelf revertChange:change presenter:presenter force:NO completion:done];
    };
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:list];
    ISHConfigureLLMSettingsNavigationController(navigationController);
    [[self ish_presentationViewController] presentViewController:navigationController animated:YES completion:nil];
}

- (void)undoLastChange {
    if ([self isBusy]) {
        [self appendLocalRole:@"assistant" content:@"Wait for the reply to finish before undoing a change."];
        return;
    }
    for (ISHLLMFileChange *change in _toolContext.changes.reverseObjectEnumerator) {
        if (!change.reverted) {
            [self revertChange:change presenter:[self ish_presentationViewController] force:NO completion:nil];
            return;
        }
    }
    [self appendLocalRole:@"assistant" content:@"No file change in this chat to undo."];
}

// A file edited again since the change asks first: reverting would throw the
// later edit away too. A revert is recorded in the transcript as the user's
// own message, so the model knows its change is gone and re-reads the file
// before touching it again (its read of it is stale now anyway).
- (void)revertChange:(ISHLLMFileChange *)change presenter:(UIViewController *)presenter force:(BOOL)force completion:(void (^)(void))completion {
    __weak typeof(self) weakSelf = self;
    ISHLLMRevertFileChange(change, _toolContext, force, ^(BOOL reverted, BOOL changedSince, NSString *message) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (changedSince) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Changed since"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *action) {
                if (completion != nil)
                    completion();
            }]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Revert Anyway" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
                [self revertChange:change presenter:presenter force:YES completion:completion];
            }]];
            [presenter presentViewController:alert animated:YES completion:nil];
            return;
        }
        if (reverted) {
            [self->_messages addObject:@{@"role": @"user",
                                         @"content": [NSString stringWithFormat:@"(I reverted your change to %@. %@)", change.path, message]}];
            [self saveTranscript];
            [self refreshTranscript];
        } else {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Could not revert"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
            [presenter presentViewController:alert animated:YES completion:nil];
        }
        if (completion != nil)
            completion();
    });
}

@end
