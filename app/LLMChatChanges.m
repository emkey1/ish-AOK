//
//  LLMChatChanges.m
//  iSH-AOK
//
//  The LLM Chat's change review: every file the model wrote or edited in
//  this chat, newest first, each as a unified diff that can be reverted.
//  Reached from the chat menu ("Changes…"); /undo reverts the newest.
//

#import "LLMChatInternal.h"
#import "LLMChatDiff.h"
#import "UserPreferences.h"

static NSString *ISHLLMChangeText(NSData *data) {
    if (data.length == 0)
        return @"";
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
        ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding] ?: @"";
}

// The diff of one change, or nil when it is too large to compute.
static NSArray<ISHLLMDiffLine *> *ISHLLMChangeDiff(ISHLLMFileChange *change) {
    return ISHLLMUnifiedDiff(ISHLLMDiffSplitLines(ISHLLMChangeText(change.before)),
                             ISHLLMDiffSplitLines(ISHLLMChangeText(change.after)), 3, 5000);
}

NSString *ISHLLMChangeSummary(ISHLLMFileChange *change) {
    NSArray<ISHLLMDiffLine *> *diff = ISHLLMChangeDiff(change);
    NSString *counts;
    if (diff == nil) {
        counts = NSLocalizedString(@"rewritten", @"file change summary: too large to diff");
    } else {
        NSUInteger removed = 0, added = 0;
        ISHLLMDiffCounts(diff, &removed, &added);
        counts = [NSString stringWithFormat:@"−%lu +%lu", (unsigned long) removed, (unsigned long) added];
    }
    NSString *when = [NSDateFormatter localizedStringFromDate:change.date dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterShortStyle];
    NSString *what = change.created ? NSLocalizedString(@"created", @"file change summary") : ([change.toolName isEqualToString:@"edit_file"] ? NSLocalizedString(@"edited", @"file change summary") : NSLocalizedString(@"overwritten", @"file change summary"));
    return [NSString stringWithFormat:@"%@ · %@ · %@%@", what, counts, when, change.reverted ? NSLocalizedString(@" · reverted", @"file change summary suffix") : @""];
}

#pragma mark - One change

@interface LLMChangeDiffViewController : UIViewController
@property (nonatomic, strong) ISHLLMFileChange *change;
@property (nonatomic, copy) void (^revertRequested)(ISHLLMFileChange *change);
@end

@implementation LLMChangeDiffViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.change.path.lastPathComponent;
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    _textView = [UITextView new];
    _textView.translatesAutoresizingMaskIntoConstraints = NO;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    _textView.textContainerInset = UIEdgeInsetsMake(12, 8, 12, 8);
    [self.view addSubview:_textView];
    [NSLayoutConstraint activateConstraints:@[
        [_textView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [_textView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_textView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    ]];
    [self render];
}

- (void)render {
    ISHLLMFileChange *change = self.change;
    UIFont *mono = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    UIFont *monoBold = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightSemibold];
    NSMutableAttributedString *text = [NSMutableAttributedString new];
    void (^append)(NSString *, UIFont *, UIColor *, UIColor *) = ^(NSString *line, UIFont *font, UIColor *color, UIColor *background) {
        NSMutableDictionary *attributes = [@{NSFontAttributeName: font, NSForegroundColorAttributeName: color} mutableCopy];
        if (background != nil)
            attributes[NSBackgroundColorAttributeName] = background;
        [text appendAttributedString:[[NSAttributedString alloc] initWithString:[line stringByAppendingString:@"\n"] attributes:attributes]];
    };
    append(change.path, monoBold, UIColor.labelColor, nil);
    append(ISHLLMChangeSummary(change), mono, UIColor.secondaryLabelColor, nil);
    append(@"", mono, UIColor.labelColor, nil);
    NSArray<ISHLLMDiffLine *> *diff = ISHLLMChangeDiff(change);
    if (diff == nil) {
        append(NSLocalizedString(@"Too many lines changed to show as a diff. The new content:", @"file change diff view"), mono, UIColor.secondaryLabelColor, nil);
        append(ISHLLMChangeText(change.after), mono, UIColor.labelColor, nil);
    } else if (diff.count == 0) {
        append(NSLocalizedString(@"(No change to the content.)", @"file change diff view"), mono, UIColor.secondaryLabelColor, nil);
    }
    UIColor *removedBackground = [UIColor.systemRedColor colorWithAlphaComponent:0.16];
    UIColor *addedBackground = [UIColor.systemGreenColor colorWithAlphaComponent:0.18];
    for (ISHLLMDiffLine *line in diff) {
        switch (line.kind) {
            case ISHLLMDiffLineHunkHeader:
                append(line.text, mono, UIColor.systemBlueColor, nil);
                break;
            case ISHLLMDiffLineContext:
                append([@" " stringByAppendingString:line.text], mono, UIColor.labelColor, nil);
                break;
            case ISHLLMDiffLineRemoved:
                append([@"-" stringByAppendingString:line.text], mono, UIColor.labelColor, removedBackground);
                break;
            case ISHLLMDiffLineAdded:
                append([@"+" stringByAppendingString:line.text], mono, UIColor.labelColor, addedBackground);
                break;
        }
    }
    _textView.attributedText = text;
    UIBarButtonItem *revert = [[UIBarButtonItem alloc] initWithTitle:change.reverted ? NSLocalizedString(@"Reverted", @"file change toolbar button, already reverted") : NSLocalizedString(@"Revert", @"file change toolbar button")
                                                               style:UIBarButtonItemStylePlain
                                                              target:self
                                                              action:@selector(revert:)];
    revert.enabled = change.revertible && !change.reverted;
    self.navigationItem.rightBarButtonItem = revert;
}

- (void)revert:(id)sender {
    (void) sender;
    if (self.revertRequested != nil)
        self.revertRequested(self.change);
}

- (void)changeDidUpdate {
    [self render];
}

@end

#pragma mark - The list

@implementation LLMChangesViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Changes", @"file changes screen title");
    // Done comes from ISHConfigureLLMSettingsNavigationController, as for
    // every LLM Chat modal.
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (NSArray<ISHLLMFileChange *> *)newestFirst {
    return [[self.toolContext.changes reverseObjectEnumerator] allObjects];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return (NSInteger) MAX((NSUInteger) 1, self.toolContext.changes.count);
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return NSLocalizedString(@"Every file the model wrote or edited in this chat, newest first. Reverting puts the file back as it was before that change, and tells the model. The copies are kept while this chat is open.", @"file changes footer");
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSArray<ISHLLMFileChange *> *changes = [self newestFirst];
    if (changes.count == 0) {
        cell.textLabel.text = NSLocalizedString(@"No file changes in this chat yet.", @"file changes empty state");
        cell.textLabel.textColor = UIColor.secondaryLabelColor;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        return cell;
    }
    ISHLLMFileChange *change = changes[(NSUInteger) indexPath.row];
    cell.textLabel.text = change.path;
    cell.textLabel.font = [UIFont monospacedSystemFontOfSize:UIFont.labelFontSize - 2 weight:UIFontWeightRegular];
    cell.textLabel.lineBreakMode = NSLineBreakByTruncatingHead;
    cell.detailTextLabel.text = ISHLLMChangeSummary(change);
    cell.detailTextLabel.textColor = change.reverted ? UIColor.tertiaryLabelColor : UIColor.secondaryLabelColor;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSArray<ISHLLMFileChange *> *changes = [self newestFirst];
    if (changes.count == 0)
        return;
    LLMChangeDiffViewController *detail = [LLMChangeDiffViewController new];
    detail.change = changes[(NSUInteger) indexPath.row];
    __weak typeof(self) weakSelf = self;
    __weak LLMChangeDiffViewController *weakDetail = detail;
    detail.revertRequested = ^(ISHLLMFileChange *change) {
        typeof(self) self = weakSelf;
        if (self.revertRequested != nil)
            self.revertRequested(change, self, ^{
                [weakDetail changeDidUpdate];
                [weakSelf.tableView reloadData];
            });
    };
    [self.navigationController pushViewController:detail animated:YES];
}

@end
