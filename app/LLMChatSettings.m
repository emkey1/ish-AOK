//
//  LLMChatSettings.m
//  iSH-AOK
//
//  LLM Settings, the provider picker, and the chat and destination lists.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "UserPreferences.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "MarkdownRenderer.h"
#import "LLMChatInternal.h"
#import "LLMChatMCP.h"
#import "LLMChatAgent.h"
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

@interface UINavigationController (ISHLLMSettingsDismiss)
- (void)ish_dismissLLMSettings;
@end

void ISHConfigureLLMSettingsNavigationController(UINavigationController *navigationController) {
    if (@available(iOS 13.0, *)) {
        navigationController.modalPresentationStyle = UIModalPresentationFormSheet;
    } else {
        navigationController.modalPresentationStyle = UIModalPresentationPageSheet;
    }
    // A modal root has no back button -- it is the root -- so it needs its own
    // dismiss, and a swipe-down nobody knows about does not count. Every LLM
    // modal presentation funnels through here, so installing it once covers
    // all four call sites. Only when the root has not already provided one:
    // LLMSettingsViewController and LLMChatSessionListViewController do.
    UIViewController *root = navigationController.viewControllers.firstObject;
    if (root != nil && root.navigationItem.leftBarButtonItem == nil) {
        root.navigationItem.leftBarButtonItem =
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                          target:navigationController
                                                          action:@selector(ish_dismissLLMSettings)];
    }
}

@implementation UINavigationController (ISHLLMSettingsDismiss)
- (void)ish_dismissLLMSettings {
    UIViewController *presenter = self.presentingViewController;
    [(presenter ?: self) dismissViewControllerAnimated:YES completion:nil];
}
@end

// Section 1 of LLM Settings. The rows used to be compared as bare integers in
// two long if/else chains; naming them keeps adding a row from silently
// renumbering the ones after it.
typedef NS_ENUM(NSInteger, ISHLLMSettingsRow) {
    ISHLLMSettingsRowDestinations, // "Models": only where the chat's own model button is not at hand
    ISHLLMSettingsRowContextWindow,
    ISHLLMSettingsRowShellTools,
    ISHLLMSettingsRowToolPermissions,
    ISHLLMSettingsRowMCPServers,
    ISHLLMSettingsRowCommandTimeout,
    ISHLLMSettingsRowOutputLimit,
    ISHLLMSettingsRowToolRounds,
    ISHLLMSettingsRowHideThinking,
    ISHLLMSettingsRowCount,
};

@implementation LLMSettingsViewController

// Inset grouped like the pages it opens; a plain table cut its footer to
// one line.
- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"LLM Client", @"LLM settings screen title");
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                          target:self
                                                                                          action:@selector(done:)];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (void)done:(id)sender {
    (void) sender;
    if (self.navigationController.viewControllers.firstObject == self) {
        UIViewController *presenter = self.navigationController.presentingViewController ?: self.presentingViewController;
        if (presenter != nil)
            [presenter dismissViewControllerAnimated:YES completion:nil];
    } else {
        [self.navigationController popViewControllerAnimated:YES];
    }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return 2;
}

// Which model a chat talks to is chosen in one place, the chat's model
// button; opened from the chat, these settings leave it out.
- (NSArray<NSNumber *> *)visibleRows {
    NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
    for (NSInteger row = 0; row < ISHLLMSettingsRowCount; row++) {
        if (row == ISHLLMSettingsRowDestinations && self.hidesModels)
            continue;
        [rows addObject:@(row)];
    }
    return rows;
}

- (ISHLLMSettingsRow)settingsRowAtIndexPath:(NSIndexPath *)indexPath {
    NSArray<NSNumber *> *rows = [self visibleRows];
    return (NSUInteger) indexPath.row < rows.count ? (ISHLLMSettingsRow) rows[(NSUInteger) indexPath.row].integerValue : ISHLLMSettingsRowCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return 1;
    return (NSInteger) [self visibleRows].count;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return nil;
    NSString *thinkingNote = NSLocalizedString(@"Hide Thinking collapses a reasoning model's <think> blocks behind a “Thinking” line in the transcript; tap it to expand or copy the reasoning. The full text is always kept in the saved history.", @"LLM settings footer");
    NSString *models = self.hidesModels
        ? NSLocalizedString(@"Which model a chat talks to is chosen with the chat's model button, next to Chats.", @"LLM settings footer")
        : NSLocalizedString(@"Models lists the saved destinations -- provider, server, model and key -- that chats choose from with their model button.", @"LLM settings footer");
    return [NSString stringWithFormat:NSLocalizedString(@"%@ A chat is summarized when it reaches three quarters of its Context Window. Tools lets the model read, search and edit files and run commands in the iSH-AOK shell, as Tool Permissions allows (Apple Foundation Models: shell only; Gemini: none). %@ Chats are saved in /AOK/persist/llm-chats.", @"LLM settings footer; each %@ is another footer sentence"), models, thinkingNote];
}

// At the text size of the Workspace window this page is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    if (indexPath.section == 0) {
        cell.textLabel.text = NSLocalizedString(@"Back to Chat", @"LLM settings row");
        cell.detailTextLabel.text = NSLocalizedString(@"Done", @"LLM settings row detail");
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }
    BOOL onDevice = ISHLLMUsesAppleFoundationModels();
    switch ([self settingsRowAtIndexPath:indexPath]) {
        case ISHLLMSettingsRowDestinations: {
            NSUInteger count = ISHLLMDestinations().count;
            cell.textLabel.text = NSLocalizedString(@"Models", @"LLM settings row");
            cell.detailTextLabel.text = count == 1
                ? ISHLLMDestinationLabel(ISHLLMActiveDestination())
                : [NSString stringWithFormat:NSLocalizedString(@"%@ · %lu saved", @"LLM settings row detail; %@ is the active destination, %lu the number saved"), ISHLLMDestinationLabel(ISHLLMActiveDestination()), (unsigned long) count];
            break;
        }
        case ISHLLMSettingsRowContextWindow:
            cell.textLabel.text = NSLocalizedString(@"Context Window", @"LLM settings row");
            cell.detailTextLabel.text = ISHLLMContextWindowSetting() > 0
                ? [NSString stringWithFormat:NSLocalizedString(@"%@ tokens", @"LLM settings context window value; %@ is a count like 64K"), ISHLLMFormattedTokenCountShort(ISHLLMContextWindowSetting())]
                : NSLocalizedString(@"Automatic", @"LLM settings context window value");
            if (onDevice)
                cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowShellTools:
            cell.textLabel.text = NSLocalizedString(@"Tools", @"LLM settings row");
            cell.detailTextLabel.text = UserPreferences.shared.llmToolsEnabled ? NSLocalizedString(@"On", @"LLM settings switch state") : NSLocalizedString(@"Off", @"LLM settings switch state");
            cell.accessoryType = UserPreferences.shared.llmToolsEnabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowToolPermissions:
            cell.textLabel.text = NSLocalizedString(@"Tool Permissions", @"LLM settings row");
            cell.detailTextLabel.text = [NSString stringWithFormat:NSLocalizedString(@"Edit %@ · Shell %@", @"LLM settings row detail; each %@ is Allow, Ask or Deny"),
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryEdit)),
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryShell))];
            break;
        case ISHLLMSettingsRowMCPServers: {
            NSUInteger on = 0, total = ISHLLMMCPServers().count;
            for (NSDictionary *server in ISHLLMMCPServers())
                on += [server[@"enabled"] boolValue];
            cell.textLabel.text = NSLocalizedString(@"MCP Servers", @"LLM settings row");
            cell.detailTextLabel.text = total == 0 ? NSLocalizedString(@"None", @"LLM settings row detail: no MCP servers") : [NSString stringWithFormat:NSLocalizedString(@"%lu on", @"LLM settings row detail: MCP servers enabled"), (unsigned long) on];
            break;
        }
        case ISHLLMSettingsRowCommandTimeout:
            cell.textLabel.text = NSLocalizedString(@"Command Timeout", @"LLM settings row");
            cell.detailTextLabel.text = ISHLLMToolTimeoutTitle(ISHLLMToolTimeoutSeconds());
            break;
        case ISHLLMSettingsRowOutputLimit:
            cell.textLabel.text = NSLocalizedString(@"Output Limit", @"LLM settings row");
            cell.detailTextLabel.text = [NSString stringWithFormat:NSLocalizedString(@"%ld KB", @"LLM settings output limit value"), (long) ISHLLMToolOutputLimitKB()];
            break;
        case ISHLLMSettingsRowToolRounds:
            cell.textLabel.text = NSLocalizedString(@"Tool Call Rounds", @"LLM settings row");
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld", (long) ISHLLMToolMaxRounds()];
            break;
        case ISHLLMSettingsRowHideThinking:
            cell.textLabel.text = NSLocalizedString(@"Hide Thinking", @"LLM settings row");
            cell.detailTextLabel.text = UserPreferences.shared.llmHideThinking ? NSLocalizedString(@"On", @"LLM settings switch state") : NSLocalizedString(@"Off", @"LLM settings switch state");
            cell.accessoryType = UserPreferences.shared.llmHideThinking ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowCount:
            break;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 0) {
        [self done:nil];
        return;
    }
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    switch ([self settingsRowAtIndexPath:indexPath]) {
        case ISHLLMSettingsRowDestinations: {
            LLMDestinationListViewController *destinations = [LLMDestinationListViewController new];
            __weak typeof(self) weakSelf = self;
            destinations.destinationsChanged = ^{ [weakSelf.tableView reloadData]; };
            [self.navigationController pushViewController:destinations animated:YES];
            return;
        }
        case ISHLLMSettingsRowCount:
            return;
        case ISHLLMSettingsRowShellTools:
            [self toggleShellToolsFromView:cell];
            return;
        case ISHLLMSettingsRowToolPermissions:
            [self.navigationController pushViewController:[LLMToolPermissionsViewController new] animated:YES];
            return;
        case ISHLLMSettingsRowMCPServers:
            [self.navigationController pushViewController:[LLMMCPServersViewController new] animated:YES];
            return;
        case ISHLLMSettingsRowCommandTimeout:
            [self pickToolTimeoutFromView:cell];
            return;
        case ISHLLMSettingsRowOutputLimit:
            [self pickToolOutputLimitFromView:cell];
            return;
        case ISHLLMSettingsRowToolRounds:
            [self pickToolMaxRoundsFromView:cell];
            return;
        case ISHLLMSettingsRowContextWindow:
            if (!ISHLLMUsesAppleFoundationModels())
                [self pickContextWindowFromView:cell];
            return;
        case ISHLLMSettingsRowHideThinking:
            UserPreferences.shared.llmHideThinking = !UserPreferences.shared.llmHideThinking;
            [tableView reloadData];
            return;
    }
}

- (void)toggleShellToolsFromView:(UIView *)sourceView {
    (void) sourceView;
    if (UserPreferences.shared.llmToolsEnabled) {
        UserPreferences.shared.llmToolsEnabled = NO;
        [self.tableView reloadData];
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Enable tools?", @"alert title")
        message:[NSString stringWithFormat:NSLocalizedString(@"The model will be able to read, search and change files and run shell commands in the iSH-AOK Linux environment — to work on code, fetch web pages with curl/wget, or run programs. By default it reads files freely and asks you before each edit or command; Tool Permissions changes that. Output is capped at %ld KB and commands are killed after %@ (both adjustable below). Only enable this with a model and server you trust.", @"enable tools alert message; %@ is a duration"),
            (long) ISHLLMToolOutputLimitKB(), ISHLLMToolTimeoutTitle(ISHLLMToolTimeoutSeconds())]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Enable", @"alert button, enable tools") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        UserPreferences.shared.llmToolsEnabled = YES;
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

// Preset pickers for the shell-tool limits. Action sheets need a popover anchor
// on iPad, so both take the tapped cell as the source view.
- (void)pickToolTimeoutFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Command Timeout", @"action sheet title")
        message:NSLocalizedString(@"A command that runs longer than this is killed and its partial output is returned to the model.", @"command timeout action sheet message")];
    NSInteger current = ISHLLMToolTimeoutSeconds();
    for (NSNumber *choice in @[@15, @30, @60, @120, @300, @600, @900]) {
        NSInteger seconds = choice.integerValue;
        NSString *title = ISHLLMToolTimeoutTitle(seconds);
        if (seconds == current)
            title = [title stringByAppendingString:@" ✓"];
        [sheet addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolTimeoutSeconds = seconds;
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (void)pickToolOutputLimitFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Output Limit", @"action sheet title")
        message:NSLocalizedString(@"Command output beyond this is truncated before being returned to the model. Larger limits use more of the model's context window.", @"output limit action sheet message")];
    NSInteger current = ISHLLMToolOutputLimitKB();
    for (NSNumber *choice in @[@16, @64, @128, @256]) {
        NSInteger kb = choice.integerValue;
        NSString *title = [NSString stringWithFormat:NSLocalizedString(@"%ld KB%@", @"output limit choice; %@ is a checkmark when selected"), (long) kb, kb == current ? @" ✓" : @""];
        [sheet addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolOutputLimitKB = kb;
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (void)pickContextWindowFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Context Window", @"action sheet title")
        message:NSLocalizedString(@"How many tokens this model can really use. A chat is summarized when it reaches three quarters of it. Automatic takes what the server reports, or assumes 64K when it reports nothing (as proxies often do). Set it lower than the model's advertised window if long chats turn into nonsense.", @"context window action sheet message")];
    NSInteger current = ISHLLMContextWindowSetting();
    NSArray<NSNumber *> *choices = @[@0, @32768, @65536, @131072, @200000, @262144, @1000000];
    for (NSNumber *choice in choices) {
        NSInteger tokens = choice.integerValue;
        NSString *label = tokens == 0 ? NSLocalizedString(@"Automatic", @"context window choice") : ISHLLMFormattedTokenCountShort(tokens);
        [sheet addActionWithTitle:[label stringByAppendingString:tokens == current ? @" ✓" : @""] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            ISHLLMSetContextWindowSetting(tokens);
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (void)pickToolMaxRoundsFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Tool Call Rounds", @"action sheet title")
        message:NSLocalizedString(@"A model reply that keeps calling tools without giving a final answer is stopped after this many rounds in a row, so a stuck model can't loop forever. Each round is one request to the model, so higher values let longer multi-step tasks (installing something, then using it) finish without you having to nudge it to continue.", @"tool call rounds action sheet message")];
    NSInteger current = ISHLLMToolMaxRounds();
    for (NSNumber *choice in @[@6, @10, @15, @20, @30, @50]) {
        NSInteger rounds = choice.integerValue;
        NSString *title = [NSString stringWithFormat:@"%ld%@", (long) rounds, rounds == current ? @" ✓" : @""];
        [sheet addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolMaxRounds = rounds;
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

@end


#pragma mark - Chat list

NSString *ISHLLMDestinationNameForID(NSString *destinationID) {
    for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
        if ([ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:destinationID])
            return ISHLLMDestinationDisplayName(destination);
    }
    return @"";
}

NSString *ISHLLMRelativeDateDescription(double timestamp) {
    if (timestamp <= 0.0)
        return @"";
    // Just written (or a hair in the future, as a save a moment ago can be)
    // reads "in 0 sec." from the formatter.
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:timestamp];
    if (fabs(date.timeIntervalSinceNow) < 10)
        return NSLocalizedString(@"just now", @"chat list: updated moments ago");
    NSRelativeDateTimeFormatter *formatter = [NSRelativeDateTimeFormatter new];
    formatter.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleShort;
    return [formatter localizedStringForDate:date relativeToDate:NSDate.date];
}

@implementation LLMChatSessionListViewController {
    NSArray<NSDictionary<NSString *, id> *> *_sessions;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Chats", @"chat list screen title");
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                          target:self
                                                                                          action:@selector(done:)];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCompose
                                                                                          target:self
                                                                                          action:@selector(newChat:)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reload) name:ISHLLMAgentStateDidChangeNotification object:nil];
    [self reload];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)reload {
    _sessions = ISHLLMSessionEntriesByRecency();
    [self.tableView reloadData];
}

- (void)done:(id)sender {
    (void) sender;
    [self dismissViewControllerAnimated:YES completion:nil];
}

// An empty session id means "start a new chat" -- the chat view controller
// creates it, so the same busy check and transcript flush apply as when the
// New Chat menu item is used.
- (void)newChat:(id)sender {
    (void) sender;
    void (^selected)(NSString *) = self.sessionSelected;
    [self dismissViewControllerAnimated:YES completion:^{
        if (selected != nil)
            selected(@"");
    }];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return _sessions.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return NSLocalizedString(@"Each chat keeps its own history, destination and system prompt, and keeps working when you switch to another. Chats starting with ↳ are sub-agents' work. Swipe a chat to rename or delete it. Chats are saved in /AOK/persist/llm-chats.", @"chat list footer");
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSDictionary<NSString *, id> *entry = _sessions[indexPath.row];
    NSString *title = ISHLLMStringValue(entry, @"title");
    cell.textLabel.text = title.length > 0 ? title : NSLocalizedString(@"New Chat", @"title of a chat that has no name yet");

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    NSInteger count = [entry[@"count"] isKindOfClass:NSNumber.class] ? [entry[@"count"] integerValue] : 0;
    [parts addObject:count == 1 ? NSLocalizedString(@"1 message", @"chat list row detail") : [NSString stringWithFormat:NSLocalizedString(@"%ld messages", @"chat list row detail"), (long) count]];
    NSString *when = ISHLLMRelativeDateDescription([entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0);
    if (when.length > 0)
        [parts addObject:when];
    NSString *destination = ISHLLMDestinationNameForID(ISHLLMStringValue(entry, @"destination"));
    if (destination.length > 0)
        [parts addObject:destination];
    if (ISHLLMStringValue(entry, @"system").length > 0)
        [parts addObject:NSLocalizedString(@"system prompt", @"chat list row detail: chat has a system prompt")];
    // What its agent is doing, when it is doing anything.
    ISHLLMAgent *agent = [ISHLLMAgentManager.shared existingAgentForSessionID:ISHLLMStringValue(entry, @"id")];
    BOOL needsApproval = [agent pendingApprovalIncludingSubagents] != nil;
    if (needsApproval)
        [parts insertObject:NSLocalizedString(@"Needs approval", @"chat list row detail") atIndex:0];
    else if (agent.busy)
        [parts insertObject:[NSString stringWithFormat:NSLocalizedString(@"Working: %@", @"chat list row detail; %@ is what the chat is doing"), [agent statusLine].lowercaseString] atIndex:0];
    else if (agent.finishedUnseen)
        [parts insertObject:NSLocalizedString(@"New answer", @"chat list row detail") atIndex:0];
    cell.detailTextLabel.text = [parts componentsJoinedByString:@" · "];
    cell.detailTextLabel.numberOfLines = 1;
    if (@available(iOS 13.0, *))
        cell.detailTextLabel.textColor = needsApproval ? UIColor.systemOrangeColor : (agent.busy ? UIColor.systemBlueColor : UIColor.secondaryLabelColor);
    cell.accessoryType = [ISHLLMStringValue(entry, @"id") isEqualToString:self.currentSessionID]
        ? UITableViewCellAccessoryCheckmark
        : UITableViewCellAccessoryNone;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *sessionID = ISHLLMStringValue(_sessions[indexPath.row], @"id");
    void (^selected)(NSString *) = self.sessionSelected;
    [self dismissViewControllerAnimated:YES completion:^{
        if (selected != nil)
            selected(sessionID);
    }];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    NSDictionary<NSString *, id> *entry = _sessions[indexPath.row];
    NSString *sessionID = ISHLLMStringValue(entry, @"id");
    UIContextualAction *deleteAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                                              title:NSLocalizedString(@"Delete", @"swipe action, delete chat")
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        NSString *nextSessionID = ISHLLMDeleteSession(sessionID);
        [ISHLLMAgentManager.shared forgetSessionID:sessionID];
        completion(YES);
        [self reload];
        // Deleting the chat that is open leaves the chat view showing content
        // with no file behind it, so hand it the survivor immediately.
        if ([sessionID isEqualToString:self.currentSessionID]) {
            self.currentSessionID = nextSessionID;
            if (self.sessionSelected != nil)
                self.sessionSelected(nextSessionID);
        }
    }];
    UIContextualAction *renameAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal
                                                                              title:NSLocalizedString(@"Rename", @"swipe action, rename chat")
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        completion(YES);
        [self renameSessionWithID:sessionID currentTitle:ISHLLMStringValue(entry, @"title")];
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[deleteAction, renameAction]];
}

- (void)renameSessionWithID:(NSString *)sessionID currentTitle:(NSString *)currentTitle {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Rename Chat", @"alert title") message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = currentTitle;
        textField.placeholder = NSLocalizedString(@"Chat name", @"rename chat text field placeholder");
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Rename", @"alert button") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *title = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
        BOOL isCustom = title.length > 0;
        if (!isCustom)
            title = ISHLLMSessionTitleFromMessages(ISHLLMLoadSessionMessages(sessionID));
        ISHLLMUpdateSessionEntry(sessionID, @{@"title": title.length > 0 ? title : @"New Chat", @"titleIsCustom": @(isCustom)});
        [self reload];
        // Renaming the open chat has to reach the toolbar label too.
        if ([sessionID isEqualToString:self.currentSessionID] && self.sessionSelected != nil)
            self.sessionSelected(sessionID);
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

#pragma mark - Destination list and editor

// Edits ONE saved destination, active or not. Writes go through
// ISHLLMSaveDestination, which re-activates the entry if it is the selected
// one, so editing the destination you are chatting with takes effect at once.
typedef NS_ENUM(NSInteger, ISHLLMDestinationEditorRow) {
    ISHLLMDestinationEditorRowName,
    ISHLLMDestinationEditorRowPreset,
    ISHLLMDestinationEditorRowServerURL,
    ISHLLMDestinationEditorRowModel,
    ISHLLMDestinationEditorRowAPIKey,
    ISHLLMDestinationEditorRowCount,
};

@implementation LLMDestinationEditorViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Destination", @"destination editor screen title");
}

// Section 1: what can be asked of the server itself.
typedef NS_ENUM(NSInteger, ISHLLMDestinationActionRow) {
    ISHLLMDestinationActionRowChooseModel,
    ISHLLMDestinationActionRowTest,
    ISHLLMDestinationActionRowCount,
};

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return 2;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    return section == 0 ? (NSInteger) ISHLLMDestinationEditorRowCount : (NSInteger) ISHLLMDestinationActionRowCount;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return NSLocalizedString(@"A preset fills in the provider, server URL and model; each stays editable. The API key is kept in the Keychain.", @"destination editor footer");
    return NSLocalizedString(@"Choose Model lists the models the server offers. Test Connection sends it a one-line request.", @"destination editor footer");
}

// At the text size of the Workspace window this page is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    if (indexPath.section == 1) {
        cell.textLabel.text = indexPath.row == ISHLLMDestinationActionRowChooseModel ? NSLocalizedString(@"Choose Model…", @"destination editor row") : NSLocalizedString(@"Test Connection", @"destination editor row");
        cell.textLabel.textColor = self.view.tintColor;
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }
    switch ((ISHLLMDestinationEditorRow) indexPath.row) {
        case ISHLLMDestinationEditorRowName:
            cell.textLabel.text = NSLocalizedString(@"Name", @"destination editor row");
            cell.detailTextLabel.text = ISHLLMDestinationDisplayName(self.destination);
            break;
        case ISHLLMDestinationEditorRowPreset:
            cell.textLabel.text = NSLocalizedString(@"Provider", @"destination editor row");
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationProvider);
            break;
        case ISHLLMDestinationEditorRowServerURL:
            cell.textLabel.text = NSLocalizedString(@"Server URL", @"destination editor row");
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationURL);
            break;
        case ISHLLMDestinationEditorRowModel:
            cell.textLabel.text = NSLocalizedString(@"Model", @"destination editor row");
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationModel);
            break;
        case ISHLLMDestinationEditorRowAPIKey:
            cell.textLabel.text = NSLocalizedString(@"API Key", @"destination editor row");
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationAPIKey).length > 0 ? NSLocalizedString(@"Set", @"destination editor API key state") : NSLocalizedString(@"Not set", @"destination editor API key state");
            break;
        case ISHLLMDestinationEditorRowCount:
            break;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 1) {
        UIView *cell = [tableView cellForRowAtIndexPath:indexPath];
        if (indexPath.row == ISHLLMDestinationActionRowChooseModel)
            [self chooseModelFromView:cell];
        else
            [self testConnection];
        return;
    }
    if (indexPath.row == ISHLLMDestinationEditorRowPreset) {
        [self pickPresetFromView:[tableView cellForRowAtIndexPath:indexPath]];
        return;
    }
    NSString *field = nil;
    NSString *title = nil;
    switch ((ISHLLMDestinationEditorRow) indexPath.row) {
        case ISHLLMDestinationEditorRowName: field = kISHLLMDestinationName; title = NSLocalizedString(@"Name", @"alert title, edit destination name"); break;
        case ISHLLMDestinationEditorRowServerURL: field = kISHLLMDestinationURL; title = NSLocalizedString(@"Server URL", @"alert title, edit server URL"); break;
        case ISHLLMDestinationEditorRowModel: field = kISHLLMDestinationModel; title = NSLocalizedString(@"Model", @"alert title, edit model"); break;
        case ISHLLMDestinationEditorRowAPIKey: field = kISHLLMDestinationAPIKey; title = NSLocalizedString(@"API Key", @"alert title, edit API key"); break;
        default: return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    NSString *current = ISHLLMStringValue(self.destination, field);
    BOOL secure = [field isEqualToString:kISHLLMDestinationAPIKey];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = current;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        textField.secureTextEntry = secure;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Save", @"alert button") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSMutableDictionary<NSString *, NSString *> *updated = [self.destination mutableCopy];
        updated[field] = alert.textFields.firstObject.text ?: @"";
        [self commitDestination:updated];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)pickPresetFromView:(UIView *)sourceView {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Provider", @"action sheet title") message:NSLocalizedString(@"Fills in the server URL and a default model.", @"provider preset action sheet message")];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        [alert addActionWithTitle:preset[@"name"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSMutableDictionary<NSString *, NSString *> *updated = [self.destination mutableCopy];
            NSString *previousProvider = ISHLLMStringValue(updated, kISHLLMDestinationProvider);
            updated[kISHLLMDestinationProvider] = preset[@"name"] ?: @"Custom";
            updated[kISHLLMDestinationURL] = preset[@"url"] ?: @"";
            if (preset[@"model"].length > 0)
                updated[kISHLLMDestinationModel] = preset[@"model"];
            // A destination still carrying its provider as its name follows the
            // new provider; a name the user chose is kept.
            NSString *name = ISHLLMStringValue(updated, kISHLLMDestinationName);
            if (name.length == 0 || [name isEqualToString:previousProvider])
                updated[kISHLLMDestinationName] = updated[kISHLLMDestinationProvider];
            [self commitDestination:updated];
        }];
    }
    [alert addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sourceView];
}

- (void)commitDestination:(NSDictionary<NSString *, NSString *> *)destination {
    self.destination = destination;
    ISHLLMSaveDestination(destination);
    [self.tableView reloadData];
    if (self.destinationSaved != nil)
        self.destinationSaved();
}

// Both requests go to this destination, not the one selected elsewhere:
// built inside its scope, and sent from a background thread in it too.
- (void)inDestination:(dispatch_block_t)block {
    ISHLLMRunWithDestination(self.destination, block);
}

- (void)inBackgroundDestination:(dispatch_block_t)block {
    NSDictionary<NSString *, NSString *> *destination = self.destination;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ISHLLMRunWithDestination(destination, block);
    });
}

- (void)showResult:(NSString *)message title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"alert button") style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)chooseModelFromView:(UIView *)sourceView {
    [self inDestination:^{
        if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            [self showResult:[NSString stringWithFormat:NSLocalizedString(@"The on-device model is the system's own.\n%@", @"alert message; %@ is an availability message"), ISHLLMAppleFoundationModelsUnavailableMessage()] title:@"Apple Foundation Models"];
            return;
        }
        NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
        if (url == nil) {
            [self showResult:NSLocalizedString(@"Invalid models URL.", @"alert message") title:NSLocalizedString(@"Model Query Failed", @"alert title")];
            return;
        }
        NSString *apiKey = ISHLLMCurrentAPIKey();
        NSString *endpoint = ISHLLMModelsEndpoint();
        void (^present)(NSData *, NSInteger, NSError *) = ^(NSData *data, NSInteger statusCode, NSError *error) {
            __block NSArray<NSString *> *models = nil;
            [self inDestination:^{
                models = ISHLLMModelIdentifiersFromResponseData(data); // Gemini and OpenAI list them differently
            }];
            [self presentModelPicker:models statusCode:statusCode error:error endpoint:endpoint fromView:sourceView];
        };
        if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
            [self inBackgroundDestination:^{
                NSInteger statusCode = 0;
                NSError *error = nil;
                NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
                dispatch_async(dispatch_get_main_queue(), ^{
                    present(data, statusCode, error);
                });
            }];
            return;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        ISHLLMApplyAuthHeaders(request, apiKey);
        [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                present(data, http.statusCode, error);
            });
        }] resume];
    }];
}

- (void)presentModelPicker:(NSArray<NSString *> *)models statusCode:(NSInteger)statusCode error:(NSError *)error
                  endpoint:(NSString *)endpoint fromView:(UIView *)sourceView {
    if (error != nil) {
        [self showResult:error.localizedDescription title:NSLocalizedString(@"Model Query Failed", @"alert title")];
        return;
    }
    if (models.count == 0) {
        [self showResult:statusCode > 0 ? [NSString stringWithFormat:NSLocalizedString(@"No models found. HTTP %ld", @"alert message"), (long) statusCode] : NSLocalizedString(@"No models found.", @"alert message") title:NSLocalizedString(@"Model Query Failed", @"alert title")];
        return;
    }
    NSString *current = ISHLLMStringValue(self.destination, kISHLLMDestinationModel);
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Choose Model", @"action sheet title")
                                                         message:[NSString stringWithFormat:NSLocalizedString(@"%lu models returned by %@", @"choose model message; %@ is the server endpoint"), (unsigned long) models.count, endpoint]];
    NSUInteger limit = MIN(models.count, (NSUInteger) 80);
    for (NSUInteger i = 0; i < limit; i++) {
        NSString *model = models[i];
        [sheet addActionWithTitle:[model isEqualToString:current] ? [model stringByAppendingString:@" ✓"] : model style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSMutableDictionary<NSString *, NSString *> *updated = [self.destination mutableCopy];
            updated[kISHLLMDestinationModel] = model;
            [self commitDestination:updated];
        }];
    }
    if (models.count > limit)
        [sheet addActionWithTitle:[NSString stringWithFormat:NSLocalizedString(@"Showing first %lu of %lu", @"choose model action sheet note"), (unsigned long) limit, (unsigned long) models.count] style:UIAlertActionStyleDefault handler:nil];
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self source:sourceView];
}

- (void)testConnection {
    [self inDestination:^{
        if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            [self showResult:ISHLLMAppleFoundationModelsUnavailableMessage() title:@"Apple Foundation Models"];
            return;
        }
        NSString *model = [ISHLLMCurrentModel() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSURL *url = ISHLLMProbeURL();
        if (model.length == 0 || url == nil) {
            [self showResult:NSLocalizedString(@"Set a valid server URL and model first.", @"alert message") title:NSLocalizedString(@"Test Failed", @"alert title")];
            return;
        }
        NSString *apiKey = ISHLLMCurrentAPIKey();
        if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
            [self showResult:ISHLLMMissingAPIKeyMessage() title:NSLocalizedString(@"Test Failed", @"alert title")];
            return;
        }
        NSData *bodyData = [NSJSONSerialization dataWithJSONObject:ISHLLMProbeBody(model, @"Reply with exactly: ok", 8) options:0 error:nil];
        void (^report)(NSData *, NSInteger, NSError *) = ^(NSData *data, NSInteger statusCode, NSError *error) {
            if (error != nil) {
                [self showResult:error.localizedDescription title:NSLocalizedString(@"Test Failed", @"alert title")];
                return;
            }
            NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
            [self showResult:[NSString stringWithFormat:@"HTTP %ld\n%@", (long) statusCode, raw.length > 240 ? [raw substringToIndex:240] : raw]
                       title:statusCode >= 200 && statusCode < 300 ? NSLocalizedString(@"Connection OK", @"alert title") : NSLocalizedString(@"Test Failed", @"alert title")];
        };
        if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
            [self inBackgroundDestination:^{
                NSInteger statusCode = 0;
                NSError *error = nil;
                NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
                dispatch_async(dispatch_get_main_queue(), ^{
                    report(data, statusCode, error);
                });
            }];
            return;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"POST";
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        ISHLLMApplyAuthHeaders(request, apiKey);
        request.HTTPBody = bodyData;
        [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                report(data, http.statusCode, error);
            });
        }] resume];
    }];
}

@end

@implementation LLMDestinationListViewController {
    NSArray<NSDictionary<NSString *, NSString *> *> *_destinations;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Destinations", @"destination list screen title");
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                                                                           target:self
                                                                                           action:@selector(addDestination:)];
    [self reload];
}

// Presented modally from the chat and pushed from LLM Settings; only the
// modal presentation needs its own dismiss control.
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    if (self.navigationController.viewControllers.firstObject == self && self.navigationItem.leftBarButtonItem == nil) {
        self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                              target:self
                                                                                              action:@selector(done:)];
    }
    [self reload];
}

- (void)reload {
    _destinations = ISHLLMDestinations();
    [self.tableView reloadData];
}

- (void)done:(id)sender {
    (void) sender;
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)notifyChanged {
    if (self.destinationsChanged != nil)
        self.destinationsChanged();
}

- (void)addDestination:(id)sender {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Add Destination", @"action sheet title") message:NSLocalizedString(@"Start from a provider preset.", @"add destination action sheet message")];
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
            [self reload];
            [self notifyChanged];
            [self editDestination:destination]; // straight into the editor for the key
        }];
    }
    [alert addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sender];
}

- (void)editDestination:(NSDictionary<NSString *, NSString *> *)destination {
    LLMDestinationEditorViewController *editor = [LLMDestinationEditorViewController new];
    editor.destination = destination;
    __weak typeof(self) weakSelf = self;
    editor.destinationSaved = ^{
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        [self reload];
        [self notifyChanged];
    };
    [self.navigationController pushViewController:editor animated:YES];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return _destinations.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return NSLocalizedString(@"Tap a destination to chat with it; tap the arrow to edit it. The selected destination is what the chat, Test Connection and Query Models all use. Swipe to duplicate or delete.", @"destination list footer");
}

// At the text size of the Workspace window this page is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSDictionary<NSString *, NSString *> *destination = _destinations[indexPath.row];
    cell.textLabel.text = ISHLLMDestinationDisplayName(destination);
    cell.detailTextLabel.text = ISHLLMDestinationSubtitle(destination);
    if (@available(iOS 13.0, *))
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    BOOL isActive = [ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID)];
    cell.accessoryType = UITableViewCellAccessoryDetailDisclosureButton;
    if (isActive) {
        cell.imageView.image = [UIImage systemImageNamed:@"checkmark.circle.fill"];
    } else {
        // Keeps the titles aligned whether or not the row is the selected one.
        cell.imageView.image = [UIImage systemImageNamed:@"circle"];
        if (@available(iOS 13.0, *))
            cell.imageView.tintColor = UIColor.tertiaryLabelColor;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    ISHLLMActivateDestination(_destinations[indexPath.row]);
    [self reload];
    [self notifyChanged];
}

- (void)tableView:(UITableView *)tableView accessoryButtonTappedForRowWithIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    [self editDestination:_destinations[indexPath.row]];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    NSDictionary<NSString *, NSString *> *destination = _destinations[indexPath.row];
    NSString *destinationID = ISHLLMStringValue(destination, kISHLLMDestinationID);
    UIContextualAction *deleteAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                                              title:NSLocalizedString(@"Delete", @"swipe action, delete destination")
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        BOOL deleted = ISHLLMDeleteDestinationWithID(destinationID);
        completion(deleted);
        if (!deleted) {
            [self presentMessage:NSLocalizedString(@"The last destination can't be deleted. Edit it, or add another one first.", @"alert message")];
            return;
        }
        [self reload];
        [self notifyChanged];
    }];
    // Duplicating is the cheap way to have the same server twice with two
    // models, which is the common multi-destination setup for a local server.
    UIContextualAction *duplicateAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal
                                                                                 title:NSLocalizedString(@"Duplicate", @"swipe action, duplicate destination")
                                                                               handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        NSMutableDictionary<NSString *, NSString *> *copy = [destination mutableCopy];
        copy[kISHLLMDestinationID] = NSUUID.UUID.UUIDString;
        copy[kISHLLMDestinationName] = [ISHLLMDestinationDisplayName(destination) stringByAppendingString:@" copy"];
        ISHLLMSaveDestination(copy);
        completion(YES);
        [self reload];
        [self notifyChanged];
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[deleteAction, duplicateAction]];
}

- (void)presentMessage:(NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"alert button") style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

#pragma mark - Tool permissions

// LLM Settings -> Tool Permissions: the three category defaults, then the
// shell rules in the order they are tried (first match wins). See
// LLMChatPermissions.h for what each one means.
typedef NS_ENUM(NSInteger, ISHLLMPermissionsSection) {
    ISHLLMPermissionsSectionCategories,
    ISHLLMPermissionsSectionRules,
    ISHLLMPermissionsSectionActions,
    ISHLLMPermissionsSectionCount,
};

@implementation LLMToolPermissionsViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Tool Permissions", @"tool permissions screen title");
    self.navigationItem.rightBarButtonItem = self.editButtonItem;
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return ISHLLMPermissionsSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    switch ((ISHLLMPermissionsSection) section) {
        case ISHLLMPermissionsSectionCategories: return 4;
        case ISHLLMPermissionsSectionRules: return (NSInteger) ISHLLMShellRules().count + 1;
        case ISHLLMPermissionsSectionActions: return 1;
        case ISHLLMPermissionsSectionCount: break;
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMPermissionsSectionCategories)
        return NSLocalizedString(@"When the model wants to", @"tool permissions section header");
    if (section == ISHLLMPermissionsSectionRules)
        return NSLocalizedString(@"Shell command rules", @"tool permissions section header");
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMPermissionsSectionCategories)
        return NSLocalizedString(@"Allow runs it without asking, Ask shows you each one first, Deny refuses it and tells the model so. File edits outside the chat's working directory always ask unless edits are denied.", @"tool permissions footer");
    if (section == ISHLLMPermissionsSectionRules)
        return NSLocalizedString(@"Each part of a command line (split at ; & | && || and newlines) takes the first rule its text matches, or the Shell Commands setting when none does; the strictest part decides. * matches anything, so \"git status *\" also matches a bare \"git status\". A rule cannot allow a part that writes to a file with > or a line with $( ) or backquotes: those get the Shell Commands setting. Drag to reorder in Edit mode.", @"tool permissions footer");
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    switch ((ISHLLMPermissionsSection) indexPath.section) {
        case ISHLLMPermissionsSectionCategories: {
            ISHLLMToolCategory category = (ISHLLMToolCategory) indexPath.row;
            cell.textLabel.text = ISHLLMToolCategoryTitle(category);
            cell.detailTextLabel.text = ISHLLMPermissionActionTitle(ISHLLMCategoryAction(category));
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        }
        case ISHLLMPermissionsSectionRules: {
            NSArray<NSDictionary<NSString *, id> *> *rules = ISHLLMShellRules();
            if ((NSUInteger) indexPath.row >= rules.count) {
                cell.textLabel.text = NSLocalizedString(@"Add Rule…", @"tool permissions row");
                cell.textLabel.textColor = self.view.tintColor;
                break;
            }
            NSDictionary<NSString *, id> *rule = rules[(NSUInteger) indexPath.row];
            cell.textLabel.text = rule[kISHLLMShellRulePattern];
            cell.textLabel.font = [UIFont monospacedSystemFontOfSize:UIFont.labelFontSize - 1.0 weight:UIFontWeightRegular];
            cell.detailTextLabel.text = ISHLLMPermissionActionTitle((ISHLLMPermissionAction) [rule[kISHLLMShellRuleAction] integerValue]);
            break;
        }
        case ISHLLMPermissionsSectionActions:
            cell.textLabel.text = NSLocalizedString(@"Restore Default Rules", @"tool permissions row");
            cell.textLabel.textColor = self.view.tintColor;
            break;
        case ISHLLMPermissionsSectionCount:
            break;
    }
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    switch ((ISHLLMPermissionsSection) indexPath.section) {
        case ISHLLMPermissionsSectionCategories: {
            ISHLLMToolCategory category = (ISHLLMToolCategory) indexPath.row;
            [self pickActionWithTitle:ISHLLMToolCategoryTitle(category) current:ISHLLMCategoryAction(category) sourceView:cell handler:^(ISHLLMPermissionAction action) {
                ISHLLMSetCategoryAction(category, action);
            }];
            return;
        }
        case ISHLLMPermissionsSectionRules: {
            NSArray<NSDictionary<NSString *, id> *> *rules = ISHLLMShellRules();
            [self editRuleAtIndex:(NSUInteger) indexPath.row existing:(NSUInteger) indexPath.row < rules.count ? rules[(NSUInteger) indexPath.row] : nil sourceView:cell];
            return;
        }
        case ISHLLMPermissionsSectionActions: {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Restore the default rules?", @"alert title")
                message:NSLocalizedString(@"Your shell command rules are replaced by the defaults, which allow only commands that look at things (ls, cat, git status, ...).", @"restore rules alert message")
                preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"alert button") style:UIAlertActionStyleCancel handler:nil]];
            [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Restore", @"alert button, restore default rules") style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
                ISHLLMSetShellRules(nil);
                [self.tableView reloadData];
            }]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        case ISHLLMPermissionsSectionCount:
            return;
    }
}

- (void)pickActionWithTitle:(NSString *)title current:(ISHLLMPermissionAction)current sourceView:(UIView *)sourceView
                    handler:(void (^)(ISHLLMPermissionAction action))handler {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:title message:nil];
    for (NSNumber *choice in @[@(ISHLLMPermissionAllow), @(ISHLLMPermissionAsk), @(ISHLLMPermissionDeny)]) {
        ISHLLMPermissionAction action = (ISHLLMPermissionAction) choice.integerValue;
        NSString *label = ISHLLMPermissionActionTitle(action);
        if (action == current)
            label = [label stringByAppendingString:@" ✓"];
        [sheet addActionWithTitle:label style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *alertAction) {
            handler(action);
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

// Pattern first, then what it does: one alert with a text field, then the
// same Allow/Ask/Deny sheet the categories use.
- (void)editRuleAtIndex:(NSUInteger)index existing:(NSDictionary<NSString *, id> *)existing sourceView:(UIView *)sourceView {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:existing != nil ? NSLocalizedString(@"Edit Rule", @"alert title") : NSLocalizedString(@"Add Rule", @"alert title")
        message:NSLocalizedString(@"A command pattern, e.g. \"make *\" or \"rm *\".", @"add rule alert message")
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = existing[kISHLLMShellRulePattern];
        textField.placeholder = @"make *";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        textField.font = [UIFont monospacedSystemFontOfSize:UIFont.labelFontSize weight:UIFontWeightRegular];
    }];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Next", @"alert button") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *pattern = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (pattern.length == 0)
            return;
        ISHLLMPermissionAction current = existing != nil ? (ISHLLMPermissionAction) [existing[kISHLLMShellRuleAction] integerValue] : ISHLLMPermissionAllow;
        [self pickActionWithTitle:pattern current:current sourceView:sourceView handler:^(ISHLLMPermissionAction chosen) {
            NSMutableArray *rules = [ISHLLMShellRules() mutableCopy];
            NSDictionary *rule = @{kISHLLMShellRulePattern: pattern, kISHLLMShellRuleAction: @(chosen)};
            if (existing != nil && index < rules.count)
                rules[index] = rule;
            else
                [rules insertObject:rule atIndex:0]; // new rules win over older ones
            ISHLLMSetShellRules(rules);
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    return indexPath.section == ISHLLMPermissionsSectionRules && (NSUInteger) indexPath.row < ISHLLMShellRules().count;
}

- (BOOL)tableView:(UITableView *)tableView canMoveRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self tableView:tableView canEditRowAtIndexPath:indexPath];
}

- (NSIndexPath *)tableView:(UITableView *)tableView targetIndexPathForMoveFromRowAtIndexPath:(NSIndexPath *)source
       toProposedIndexPath:(NSIndexPath *)proposed {
    (void) tableView;
    NSInteger last = (NSInteger) ISHLLMShellRules().count - 1;
    if (proposed.section < ISHLLMPermissionsSectionRules)
        return [NSIndexPath indexPathForRow:0 inSection:ISHLLMPermissionsSectionRules];
    if (proposed.section > ISHLLMPermissionsSectionRules || proposed.row > last)
        return [NSIndexPath indexPathForRow:last inSection:ISHLLMPermissionsSectionRules];
    (void) source;
    return proposed;
}

- (void)tableView:(UITableView *)tableView moveRowAtIndexPath:(NSIndexPath *)source toIndexPath:(NSIndexPath *)destination {
    (void) tableView;
    NSMutableArray *rules = [ISHLLMShellRules() mutableCopy];
    NSDictionary *rule = rules[(NSUInteger) source.row];
    [rules removeObjectAtIndex:(NSUInteger) source.row];
    [rules insertObject:rule atIndex:(NSUInteger) destination.row];
    ISHLLMSetShellRules(rules);
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    if (editingStyle != UITableViewCellEditingStyleDelete)
        return;
    NSMutableArray *rules = [ISHLLMShellRules() mutableCopy];
    [rules removeObjectAtIndex:(NSUInteger) indexPath.row];
    ISHLLMSetShellRules(rules);
    [tableView deleteRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationAutomatic];
}

@end
