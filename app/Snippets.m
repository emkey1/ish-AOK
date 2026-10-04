//
//  Snippets.m
//  iSH-AOK
//

#import <CommonCrypto/CommonDigest.h>

#import "Snippets.h"
#import "GuestFileBridge.h"
#import "UserPreferences.h"

NSString *const kISHSnippetID = @"id";
NSString *const kISHSnippetName = @"name";
NSString *const kISHSnippetText = @"text";
NSString *const kISHSnippetRun = @"run";

NSString *const kISHSnippetsGuestPath = @"/AOK/persist/snippets.json";

// A library big enough to be somebody's entire working set is still a few KiB.
// The cap is here so that a corrupted or wrongly-pointed file cannot be read
// into memory wholesale, not to limit anyone.
static const NSUInteger kISHSnippetsMaxBytes = 1 * 1024 * 1024;

// Declared rather than left implicit because the sheet calls it too, and a
// method that only exists inside an @implementation is not visible from another
// one in the same file.
@interface ISHSnippetStore ()
+ (void)exportToGuest;
@end

#pragma mark - Model helpers

static NSString *ISHSnippetString(NSDictionary<NSString *, id> *snippet, NSString *key) {
    id value = snippet[key];
    return [value isKindOfClass:NSString.class] ? value : @"";
}

// Tolerant about `run` because the mirror is a file people hand-edit: JSON true
// is what we write, but somebody typing yes/true/1 into vi means the same thing
// and should not silently get the opposite.
static BOOL ISHSnippetRuns(NSDictionary<NSString *, id> *snippet) {
    id value = snippet[kISHSnippetRun];
    if ([value isKindOfClass:NSNumber.class])
        return [value boolValue];
    if ([value isKindOfClass:NSString.class]) {
        NSString *text = [(NSString *)value lowercaseString];
        return [text isEqualToString:@"yes"] || [text isEqualToString:@"true"] || [text isEqualToString:@"1"];
    }
    return NO;
}

// The single line shown under a snippet's name, and the name a snippet falls
// back to when it has none.
static NSString *ISHSnippetFirstLine(NSString *text) {
    NSRange newline = [text rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet];
    NSString *first = newline.location == NSNotFound ? text : [text substringToIndex:newline.location];
    first = [first stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (newline.location != NSNotFound && first.length > 0)
        return [first stringByAppendingString:@" …"];
    return first;
}

static NSString *ISHSnippetsDigest(NSData *_Nullable data) {
    if (data == nil)
        return @"";
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    NSMutableString *hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        [hex appendFormat:@"%02x", hash[i]];
    return hex;
}

#pragma mark - JSON mirror

NSData *ISHSnippetsEncodeJSON(NSArray<NSDictionary<NSString *, id> *> *snippets) {
    NSMutableArray<NSDictionary<NSString *, id> *> *plain = [NSMutableArray arrayWithCapacity:snippets.count];
    for (NSDictionary<NSString *, id> *snippet in snippets) {
        if (![snippet isKindOfClass:NSDictionary.class])
            continue;
        [plain addObject:@{
            kISHSnippetID: ISHSnippetString(snippet, kISHSnippetID),
            kISHSnippetName: ISHSnippetString(snippet, kISHSnippetName),
            kISHSnippetText: ISHSnippetString(snippet, kISHSnippetText),
            kISHSnippetRun: @(ISHSnippetRuns(snippet)),
        }];
    }
    // Pretty-printed with sorted keys: this file's whole reason to exist is
    // that a person reads it in vi and a repository diffs it, and neither is
    // served by one long line whose key order moves between writes.
    NSJSONWritingOptions options = NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys;
    NSData *body = [NSJSONSerialization dataWithJSONObject:plain options:options error:NULL];
    if (body == nil)
        return nil;
    NSMutableData *data = [body mutableCopy];
    [data appendBytes:"\n" length:1];   // a text file ends with a newline
    return data;
}

NSArray<NSDictionary<NSString *, id> *> *ISHSnippetsDecodeJSON(NSData *data) {
    if (data.length == 0)
        return nil;
    id document = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![document isKindOfClass:NSArray.class])
        return nil;
    NSMutableArray<NSDictionary<NSString *, id> *> *snippets = [NSMutableArray array];
    for (id entry in (NSArray *)document) {
        if (![entry isKindOfClass:NSDictionary.class])
            continue;
        NSString *text = ISHSnippetString(entry, kISHSnippetText);
        if (text.length == 0)
            continue;   // nothing to put on the command line is not a snippet
        // Both of these are optional in a hand-written file. Generating the id
        // rather than refusing the entry is what lets someone add a snippet
        // with three lines of JSON and no ceremony.
        NSString *identifier = ISHSnippetString(entry, kISHSnippetID);
        if (identifier.length == 0)
            identifier = NSUUID.UUID.UUIDString;
        NSString *name = ISHSnippetString(entry, kISHSnippetName);
        if (name.length == 0)
            name = ISHSnippetFirstLine(text);
        [snippets addObject:@{
            kISHSnippetID: identifier,
            kISHSnippetName: name,
            kISHSnippetText: text,
            kISHSnippetRun: @(ISHSnippetRuns(entry)),
        }];
    }
    return snippets;
}

#pragma mark - Store

@implementation ISHSnippetStore

+ (NSArray<NSDictionary<NSString *, id> *> *)snippets {
    NSMutableArray<NSDictionary<NSString *, id> *> *valid = [NSMutableArray array];
    for (id entry in UserPreferences.shared.snippets) {
        // The id is what editing and deleting key on, so an entry without one
        // is dropped rather than shown and then failing to act on.
        if ([entry isKindOfClass:NSDictionary.class] && ISHSnippetString(entry, kISHSnippetID).length > 0)
            [valid addObject:entry];
    }
    return valid;
}

+ (void)setSnippets:(NSArray<NSDictionary<NSString *, id> *> *)snippets {
    UserPreferences.shared.snippets = snippets ?: @[];
}

+ (NSDictionary<NSString *, id> *)snippetWithName:(NSString *)name text:(NSString *)text run:(BOOL)run {
    return @{
        kISHSnippetID: NSUUID.UUID.UUIDString,
        kISHSnippetName: name ?: @"",
        kISHSnippetText: text ?: @"",
        kISHSnippetRun: @(run),
    };
}

+ (void)saveSnippet:(NSDictionary<NSString *, id> *)snippet {
    NSString *identifier = ISHSnippetString(snippet, kISHSnippetID);
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, id> *> *snippets = [[self snippets] mutableCopy];
    BOOL replaced = NO;
    for (NSUInteger i = 0; i < snippets.count; i++) {
        if ([ISHSnippetString(snippets[i], kISHSnippetID) isEqualToString:identifier]) {
            snippets[i] = snippet;
            replaced = YES;
            break;
        }
    }
    if (!replaced)
        [snippets addObject:snippet];
    [self setSnippets:snippets];
    [self exportToGuest];
}

+ (void)deleteSnippetWithID:(NSString *)identifier {
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, id> *> *snippets = [[self snippets] mutableCopy];
    for (NSUInteger i = 0; i < snippets.count; i++) {
        if ([ISHSnippetString(snippets[i], kISHSnippetID) isEqualToString:identifier]) {
            [snippets removeObjectAtIndex:i];
            [self setSnippets:snippets];
            [self exportToGuest];
            return;
        }
    }
}

// Writes the library out to the mirror and records what was written, so the
// next sync can tell our own bytes from somebody's edit. Deliberately silent:
// the sheet works whether or not the guest is up, and a user who never opens
// the file should never hear about it.
+ (void)exportToGuest {
    NSData *data = ISHSnippetsEncodeJSON([self snippets]);
    if (data == nil)
        return;
    ISHGuestFileBridge *bridge = ISHGuestFileBridge.sharedBridge;
    if (![bridge isGuestAvailable])
        return;
    [bridge writeData:data toGuestPath:kISHSnippetsGuestPath completion:^(BOOL ok, NSError *error) {
        if (ok)
            UserPreferences.shared.snippetsSyncedDigest = ISHSnippetsDigest(data);
    }];
}

+ (void)syncWithGuest:(void (^)(BOOL))completion {
    void (^finish)(BOOL) = ^(BOOL changed) {
        if (completion == nil)
            return;
        if (NSThread.isMainThread)
            completion(changed);
        else
            dispatch_async(dispatch_get_main_queue(), ^{ completion(changed); });
    };

    ISHGuestFileBridge *bridge = ISHGuestFileBridge.sharedBridge;
    if (![bridge isGuestAvailable]) {
        finish(NO);
        return;
    }
    [bridge readFileAtGuestPath:kISHSnippetsGuestPath
                       maxBytes:kISHSnippetsMaxBytes
                     completion:^(NSData *data, NSError *error) {
        if (data == nil) {
            // No mirror yet, or it could not be read. Seed it from what we
            // have; a read that failed for some other reason costs one
            // harmless write and the next sync sorts itself out.
            [self exportToGuest];
            finish(NO);
            return;
        }
        NSString *digest = ISHSnippetsDigest(data);
        if ([digest isEqualToString:UserPreferences.shared.snippetsSyncedDigest]) {
            // Byte for byte what we left there, so the guest has said nothing
            // since. Push whatever changed on this side instead.
            NSData *current = ISHSnippetsEncodeJSON([self snippets]);
            if (current != nil && ![ISHSnippetsDigest(current) isEqualToString:digest])
                [self exportToGuest];
            finish(NO);
            return;
        }
        NSArray<NSDictionary<NSString *, id> *> *imported = ISHSnippetsDecodeJSON(data);
        if (imported == nil) {
            // Not parseable. Somebody is mid-edit, or the path holds something
            // that was never ours; either way exporting over it would delete
            // work to fix a problem they are probably already fixing.
            finish(NO);
            return;
        }
        [self setSnippets:imported];
        UserPreferences.shared.snippetsSyncedDigest = digest;
        finish(YES);
    }];
}

@end

#pragma mark - Editor

@protocol ISHSnippetEditorDelegate <NSObject>
- (void)snippetEditor:(UIViewController *)editor didSaveSnippet:(NSDictionary<NSString *, id> *)snippet;
@end

@interface ISHSnippetEditorViewController : UIViewController
- (instancetype)initWithSnippet:(nullable NSDictionary<NSString *, id> *)snippet;
@property (nonatomic, weak) id<ISHSnippetEditorDelegate> delegate;
@end

@implementation ISHSnippetEditorViewController {
    NSDictionary<NSString *, id> *_Nullable _original;   // nil when adding
    UITextField *_nameField;
    UITextView *_textView;
    UISwitch *_runSwitch;
    NSLayoutConstraint *_bottomConstraint;
}

- (instancetype)initWithSnippet:(NSDictionary<NSString *, id> *)snippet {
    self = [super initWithNibName:nil bundle:nil];
    if (self != nil)
        _original = [snippet copy];
    return self;
}

// Shell text, so every one of iOS's helpful text behaviours is wrong here.
// Smart quotes in particular turn a perfectly good command into one the shell
// cannot parse, and they do it invisibly.
static void ISHSnippetConfigureAsShellInput(id<UITextInputTraits> input) {
    input.autocorrectionType = UITextAutocorrectionTypeNo;
    input.autocapitalizationType = UITextAutocapitalizationTypeNone;
    input.spellCheckingType = UITextSpellCheckingTypeNo;
    input.smartQuotesType = UITextSmartQuotesTypeNo;
    input.smartDashesType = UITextSmartDashesTypeNo;
    input.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.title = _original == nil ? NSLocalizedString(@"New Snippet", @"Snippet editor title") : NSLocalizedString(@"Edit Snippet", @"Snippet editor title");
    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                      target:self
                                                      action:@selector(cancel:)];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemSave
                                                      target:self
                                                      action:@selector(save:)];

    _nameField = [UITextField new];
    _nameField.placeholder = NSLocalizedString(@"Name", @"Snippet editor name field placeholder");
    _nameField.borderStyle = UITextBorderStyleRoundedRect;
    _nameField.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _nameField.adjustsFontForContentSizeCategory = YES;
    _nameField.text = ISHSnippetString(_original ?: @{}, kISHSnippetName);
    // Shell rules here too, autocapitalisation included. A snippet name is not
    // prose: it is usually the command itself, or a fragment of one, which is
    // why an unnamed snippet falls back to its own first line. Capitalising it
    // is wrong more often than right, and iOS re-arms the shift every time the
    // field goes back to empty -- so a name that starts lowercase is a small
    // fight rather than a choice. Shift still works for anyone who wants a
    // capital; none of this prevents one.
    ISHSnippetConfigureAsShellInput(_nameField);

    UILabel *runLabel = [UILabel new];
    runLabel.text = NSLocalizedString(@"Run on tap", @"Snippet editor switch label");
    runLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    runLabel.adjustsFontForContentSizeCategory = YES;

    _runSwitch = [UISwitch new];
    _runSwitch.on = ISHSnippetRuns(_original ?: @{});

    UIStackView *runRow = [[UIStackView alloc] initWithArrangedSubviews:@[runLabel, _runSwitch]];
    runRow.axis = UILayoutConstraintAxisHorizontal;
    runRow.alignment = UIStackViewAlignmentCenter;

    UILabel *hint = [UILabel new];
    hint.text = NSLocalizedString(@"Off, tapping the snippet puts it on the command line and stops there. "
                 "On, it runs immediately.", @"Snippet editor hint under Run on tap");
    hint.numberOfLines = 0;
    hint.textColor = UIColor.secondaryLabelColor;
    hint.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    hint.adjustsFontForContentSizeCategory = YES;

    _textView = [UITextView new];
    _textView.font = [UIFont monospacedSystemFontOfSize:UIFont.systemFontSize weight:UIFontWeightRegular];
    _textView.text = ISHSnippetString(_original ?: @{}, kISHSnippetText);
    _textView.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    _textView.layer.cornerRadius = 8;
    _textView.textContainerInset = UIEdgeInsetsMake(8, 4, 8, 4);
    ISHSnippetConfigureAsShellInput(_textView);

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_nameField, runRow, hint, _textView]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    _bottomConstraint = [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        _bottomConstraint,
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(keyboardWillChangeFrame:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // A new snippet starts in the name field; an existing one is usually opened
    // to change the command, so put the caret where the work is.
    if (_original == nil)
        [_nameField becomeFirstResponder];
    else
        [_textView becomeFirstResponder];
}

// The sheet does not shrink for the keyboard on its own, and the text view is
// the bottom-most thing in the stack, so without this the caret ends up
// underneath the keyboard the moment the command runs past one line.
- (void)keyboardWillChangeFrame:(NSNotification *)note {
    CGRect end = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect overlap = CGRectIntersection(self.view.bounds, [self.view convertRect:end fromView:nil]);
    CGFloat inset = CGRectIsNull(overlap) ? 0 : CGRectGetHeight(overlap);
    _bottomConstraint.constant = -16 - MAX(inset - self.view.safeAreaInsets.bottom, 0);
    [self.view layoutIfNeeded];
}

- (void)cancel:(id)sender {
    [self.navigationController popViewControllerAnimated:YES];
}

- (void)save:(id)sender {
    NSString *text = _textView.text ?: @"";
    if ([text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length == 0) {
        // Nothing to put on the command line. Say so rather than saving a row
        // that would do nothing when tapped.
        [_textView becomeFirstResponder];
        UINotificationFeedbackGenerator *feedback = [UINotificationFeedbackGenerator new];
        [feedback notificationOccurred:UINotificationFeedbackTypeError];
        return;
    }
    NSString *name = [(_nameField.text ?: @"") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (name.length == 0)
        name = ISHSnippetFirstLine(text);
    NSString *identifier = ISHSnippetString(_original ?: @{}, kISHSnippetID);
    NSDictionary<NSString *, id> *snippet = @{
        kISHSnippetID: identifier.length > 0 ? identifier : NSUUID.UUID.UUIDString,
        kISHSnippetName: name,
        kISHSnippetText: text,
        kISHSnippetRun: @(_runSwitch.isOn),
    };
    [self.delegate snippetEditor:self didSaveSnippet:snippet];
    [self.navigationController popViewControllerAnimated:YES];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

@end

#pragma mark - Sheet

@interface ISHSnippetsViewController () <ISHSnippetEditorDelegate>
@end

@implementation ISHSnippetsViewController {
    NSArray<NSDictionary<NSString *, id> *> *_snippets;
    UIView *_emptyView;
}

+ (void)presentFromViewController:(UIViewController *)presenter
                         delegate:(id<ISHSnippetsDelegate>)delegate {
    ISHSnippetsViewController *snippets = [[self alloc] initWithStyle:UITableViewStyleInsetGrouped];
    snippets.delegate = delegate;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:snippets];
    nav.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController *sheet = nav.sheetPresentationController;
    if (sheet != nil) {
        sheet.detents = @[[UISheetPresentationControllerDetent mediumDetent],
                          [UISheetPresentationControllerDetent largeDetent]];
        sheet.prefersGrabberVisible = YES;
        // Same reason the file browser sets this: a drag in the list should
        // scroll the list, not grow the sheet out of the medium detent that
        // makes it usable over a live terminal.
        sheet.prefersScrollingExpandsWhenScrolledToEdge = NO;
    }
    [presenter presentViewController:nav animated:YES completion:nil];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Snippets", @"Snippets list title");
    // A sheet with no delegate cannot insert (see -useSnippet:). Say so rather
    // than let the first tap be the explanation.
    if (self.delegate == nil)
        self.navigationItem.prompt = NSLocalizedString(@"No terminal window open — tap a snippet to read or edit it.", @"Snippets list prompt when no terminal can receive a snippet");
    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(done:)];
    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                                      target:self
                                                      action:@selector(addSnippet:)],
        self.editButtonItem,
    ];
    [self.tableView registerClass:UITableViewCell.class forCellReuseIdentifier:@"snippet"];

    UILabel *empty = [UILabel new];
    empty.numberOfLines = 0;
    empty.textAlignment = NSTextAlignmentCenter;
    empty.textColor = UIColor.secondaryLabelColor;
    empty.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    empty.adjustsFontForContentSizeCategory = YES;
    empty.translatesAutoresizingMaskIntoConstraints = NO;
    empty.text = [NSString stringWithFormat:
        NSLocalizedString(@"No snippets yet.\n\nSave the commands you retype constantly, then tap one to put it "
         "on the command line.\n\nThey live in %@ inside the guest, so you can edit them with "
         "vi and keep them in git.", @"Snippets list empty state; %@ is a file path"), kISHSnippetsGuestPath];
    // A bare label as backgroundView runs edge to edge, which reads as a bug at
    // phone width. Centre it in a container with real margins instead.
    _emptyView = [UIView new];
    [_emptyView addSubview:empty];
    [NSLayoutConstraint activateConstraints:@[
        [empty.centerYAnchor constraintEqualToAnchor:_emptyView.centerYAnchor],
        [empty.leadingAnchor constraintEqualToAnchor:_emptyView.leadingAnchor constant:32],
        [empty.trailingAnchor constraintEqualToAnchor:_emptyView.trailingAnchor constant:-32],
    ]];

    _snippets = [ISHSnippetStore snippets];
    // The guest may have a newer library than the one we last saw -- somebody
    // edited the file, or another root wrote it. Ask before showing, and again
    // it is free when the guest is down.
    __weak typeof(self) weakSelf = self;
    [ISHSnippetStore syncWithGuest:^(BOOL libraryChanged) {
        if (libraryChanged)
            [weakSelf reload];
    }];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self reload];   // coming back from the editor
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    if (self.isBeingDismissed || self.navigationController.isBeingDismissed) {
        if ([self.delegate respondsToSelector:@selector(snippetsDidDismiss:)])
            [self.delegate snippetsDidDismiss:self];
    }
}

- (void)reload {
    _snippets = [ISHSnippetStore snippets];
    self.tableView.backgroundView = _snippets.count == 0 ? _emptyView : nil;
    [self.tableView reloadData];
}

#pragma mark Actions

- (void)done:(id)sender {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)addSnippet:(id)sender {
    [self editSnippet:nil];
}

- (void)editSnippet:(nullable NSDictionary<NSString *, id> *)snippet {
    ISHSnippetEditorViewController *editor = [[ISHSnippetEditorViewController alloc] initWithSnippet:snippet];
    editor.delegate = self;
    [self.navigationController pushViewController:editor animated:YES];
}

// The one thing the sheet exists for. `execute` is the snippet's own setting
// unless the caller overrides it from the context menu, and the sheet closes
// either way: the command line is underneath, and staying open to admire the
// list is not what anybody wants next.
- (void)useSnippet:(NSDictionary<NSString *, id> *)snippet execute:(BOOL)execute {
    NSString *text = ISHSnippetString(snippet, kISHSnippetText);
    if (text.length == 0)
        return;
    // Nowhere to insert into -- opened from the Workspace desktop with no
    // terminal window on it. The library is still worth having open, so a tap
    // opens the snippet for reading and editing instead of dismissing the sheet
    // and doing nothing, which is indistinguishable from the feature being
    // broken. -viewDidLoad says so in the prompt.
    if (self.delegate == nil) {
        [self editSnippet:snippet];
        return;
    }
    [self.delegate snippets:self insertText:text execute:execute];
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)deleteSnippetAtIndexPath:(NSIndexPath *)indexPath {
    if ((NSUInteger)indexPath.row >= _snippets.count)
        return;
    [ISHSnippetStore deleteSnippetWithID:ISHSnippetString(_snippets[indexPath.row], kISHSnippetID)];
    _snippets = [ISHSnippetStore snippets];
    [self.tableView deleteRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationAutomatic];
    self.tableView.backgroundView = _snippets.count == 0 ? _emptyView : nil;
}

#pragma mark ISHSnippetEditorDelegate

- (void)snippetEditor:(UIViewController *)editor didSaveSnippet:(NSDictionary<NSString *, id> *)snippet {
    [ISHSnippetStore saveSnippet:snippet];
}

#pragma mark Table

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return _snippets.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"snippet" forIndexPath:indexPath];
    NSDictionary<NSString *, id> *snippet = _snippets[indexPath.row];
    BOOL runs = ISHSnippetRuns(snippet);

    UIListContentConfiguration *content = cell.defaultContentConfiguration;
    content.text = ISHSnippetString(snippet, kISHSnippetName);
    content.secondaryText = ISHSnippetFirstLine(ISHSnippetString(snippet, kISHSnippetText));
    content.secondaryTextProperties.font =
        [UIFont monospacedSystemFontOfSize:UIFont.smallSystemFontSize weight:UIFontWeightRegular];
    content.secondaryTextProperties.numberOfLines = 1;
    content.secondaryTextProperties.color = UIColor.secondaryLabelColor;
    // Which of the two things a tap will do, visible before the tap rather than
    // after it. A snippet that runs is the one worth being sure about.
    content.image = [UIImage systemImageNamed:runs ? @"play.circle.fill" : @"text.cursor"];
    content.imageProperties.tintColor = runs ? UIColor.systemOrangeColor : UIColor.secondaryLabelColor;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDetailButton;
    cell.accessibilityHint = runs ? NSLocalizedString(@"Runs this command", @"Accessibility hint for a snippet that runs on tap") : NSLocalizedString(@"Puts this command on the command line", @"Accessibility hint for a snippet that inserts on tap");
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if ((NSUInteger)indexPath.row >= _snippets.count)
        return;
    NSDictionary<NSString *, id> *snippet = _snippets[indexPath.row];
    // In editing mode a tap means "change this", which is what the rest of iOS
    // does and what the reorder grips already put the user in the mood for.
    if (self.isEditing)
        [self editSnippet:snippet];
    else
        [self useSnippet:snippet execute:ISHSnippetRuns(snippet)];
}

- (void)tableView:(UITableView *)tableView accessoryButtonTappedForRowWithIndexPath:(NSIndexPath *)indexPath {
    if ((NSUInteger)indexPath.row < _snippets.count)
        [self editSnippet:_snippets[indexPath.row]];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
      trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    __weak typeof(self) weakSelf = self;
    UIContextualAction *deleteAction =
        [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                title:NSLocalizedString(@"Delete", @"Swipe action that deletes a snippet")
                                              handler:^(UIContextualAction *action, UIView *view, void (^done)(BOOL)) {
        [weakSelf deleteSnippetAtIndexPath:indexPath];
        done(YES);
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[deleteAction]];
}

- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView
      contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
                                          point:(CGPoint)point {
    if ((NSUInteger)indexPath.row >= _snippets.count)
        return nil;
    NSDictionary<NSString *, id> *snippet = _snippets[indexPath.row];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil
                                                   previewProvider:nil
                                                    actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggested) {
        // Both verbs, always, whichever way the snippet is set: the menu is how
        // you run a snippet you normally insert, and how you insert one you
        // normally run so you can look at it first.
        UIAction *insert = [UIAction actionWithTitle:NSLocalizedString(@"Insert", @"Snippet context menu: put on the command line")
                                                image:[UIImage systemImageNamed:@"text.cursor"]
                                           identifier:nil
                                              handler:^(UIAction *a) { [weakSelf useSnippet:snippet execute:NO]; }];
        UIAction *run = [UIAction actionWithTitle:NSLocalizedString(@"Run", @"Snippet context menu: run the command")
                                             image:[UIImage systemImageNamed:@"play.circle.fill"]
                                        identifier:nil
                                           handler:^(UIAction *a) { [weakSelf useSnippet:snippet execute:YES]; }];
        UIAction *edit = [UIAction actionWithTitle:NSLocalizedString(@"Edit", @"Snippet context menu")
                                              image:[UIImage systemImageNamed:@"pencil"]
                                         identifier:nil
                                            handler:^(UIAction *a) { [weakSelf editSnippet:snippet]; }];
        UIAction *duplicate = [UIAction actionWithTitle:NSLocalizedString(@"Duplicate", @"Snippet context menu")
                                                   image:[UIImage systemImageNamed:@"plus.square.on.square"]
                                              identifier:nil
                                                 handler:^(UIAction *a) {
            NSString *name = [NSString stringWithFormat:NSLocalizedString(@"%@ copy", @"Name of a duplicated snippet; %@ is the original name"), ISHSnippetString(snippet, kISHSnippetName)];
            [ISHSnippetStore saveSnippet:[ISHSnippetStore snippetWithName:name
                                                                     text:ISHSnippetString(snippet, kISHSnippetText)
                                                                      run:ISHSnippetRuns(snippet)]];
            [weakSelf reload];
        }];
        UIAction *copy = [UIAction actionWithTitle:NSLocalizedString(@"Copy", @"Snippet context menu: copy the text")
                                              image:[UIImage systemImageNamed:@"doc.on.doc"]
                                         identifier:nil
                                            handler:^(UIAction *a) {
            UIPasteboard.generalPasteboard.string = ISHSnippetString(snippet, kISHSnippetText);
        }];
        UIAction *deleteAction = [UIAction actionWithTitle:NSLocalizedString(@"Delete", @"Snippet context menu")
                                                image:[UIImage systemImageNamed:@"trash"]
                                           identifier:nil
                                              handler:^(UIAction *a) { [weakSelf deleteSnippetAtIndexPath:indexPath]; }];
        deleteAction.attributes = UIMenuElementAttributesDestructive;
        return [UIMenu menuWithTitle:@"" children:@[insert, run, edit, duplicate, copy, deleteAction]];
    }];
}

- (BOOL)tableView:(UITableView *)tableView canMoveRowAtIndexPath:(NSIndexPath *)indexPath {
    return YES;
}

- (void)tableView:(UITableView *)tableView
      moveRowAtIndexPath:(NSIndexPath *)source
             toIndexPath:(NSIndexPath *)destination {
    if ((NSUInteger)source.row >= _snippets.count || (NSUInteger)destination.row >= _snippets.count)
        return;
    NSMutableArray<NSDictionary<NSString *, id> *> *reordered = [_snippets mutableCopy];
    NSDictionary<NSString *, id> *moved = reordered[source.row];
    [reordered removeObjectAtIndex:source.row];
    [reordered insertObject:moved atIndex:destination.row];
    _snippets = reordered;
    [ISHSnippetStore setSnippets:reordered];
    [ISHSnippetStore exportToGuest];
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView
            editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    // No red minus badges: deleting is the swipe and the context menu, and the
    // editing mode here is for dragging the order around.
    return UITableViewCellEditingStyleNone;
}

- (BOOL)tableView:(UITableView *)tableView shouldIndentWhileEditingRowAtIndexPath:(NSIndexPath *)indexPath {
    return NO;
}

@end
